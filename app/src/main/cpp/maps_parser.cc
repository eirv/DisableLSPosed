#include "maps_parser.h"

#include <linux/fs.h>

#include <algorithm>
#include <cerrno>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "linux_syscall_support.h"

#ifndef PROCMAP_QUERY
#define PROCMAP_QUERY 0xc0686611

enum procmap_query_flags {
  PROCMAP_QUERY_VMA_READABLE = 0x01,
  PROCMAP_QUERY_VMA_WRITABLE = 0x02,
  PROCMAP_QUERY_VMA_EXECUTABLE = 0x04,
  PROCMAP_QUERY_VMA_SHARED = 0x08,
  PROCMAP_QUERY_COVERING_OR_NEXT_VMA = 0x10,
  PROCMAP_QUERY_FILE_BACKED_VMA = 0x20,
};

struct procmap_query {
  uint64_t size;
  uint64_t query_flags;
  uint64_t query_addr;
  uint64_t vma_start;
  uint64_t vma_end;
  uint64_t vma_flags;
  uint64_t vma_page_size;
  uint64_t vma_offset;
  uint64_t inode;
  uint32_t dev_major;
  uint32_t dev_minor;
  uint32_t vma_name_size;
  uint32_t build_id_size;
  uint64_t vma_name_addr;
  uint64_t build_id_addr;
};
#endif

namespace io::proc {
namespace {
template <typename T>
constexpr size_t kNameOffset = 25 + sizeof(T) * 6;
constexpr size_t kMaxPrefixSize = 95;

auto procmap_query_failed_ = bool{};
#ifndef __LP64__
auto name_offset_ = [] -> size_t {
  auto smaps_rollup = raw_open("/proc/self/smaps_rollup", O_RDONLY | O_CLOEXEC);
  if (smaps_rollup < 0) [[unlikely]] {
    return 0;
  }

  std::array<char, kNameOffset<uint64_t> + 1> buffer;

  ssize_t nread;
  do {
    nread = raw_read(smaps_rollup, buffer.data(), buffer.size());
  } while (nread != -EINTR);
  raw_close(smaps_rollup);

  if (nread == buffer.size()) [[likely]] {
    if (buffer[kNameOffset<uint64_t>] == '[') [[likely]] {
      return kNameOffset<uint64_t>;
    } else if (buffer[kNameOffset<uint32_t>] == '[') [[likely]] {
      return kNameOffset<uint32_t>;
    }
  }
  return 0;
}();
#endif

template <std::unsigned_integral T>
auto FastParseHex(const char** p) {
  T value{};
  for (auto s = *p;;) {
    auto c = static_cast<unsigned char>(*s);

    T digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<T>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<T>(c - 'a') + T{10};
    } else {
      *p = s + 1;
      return value;
    }

    value = (value << 4) | digit;
    ++s;
  }
}

template <class Buffer>
auto ParseVmaEntry(FileReader<Buffer>& reader, uint32_t query_flags) -> std::optional<VmaEntry> {
  while (auto line = reader.NextLine()) {
    if (line->empty()) [[unlikely]] {
      break;
    }

    auto current = line->data();

    auto vma_start = FastParseHex<uintptr_t>(&current);
    auto vma_end = FastParseHex<uintptr_t>(&current);

    if (!vma_start || !vma_end) [[unlikely]] {
      break;
    }

    auto vma_flags = uint32_t{};
    if (current[0] == 'r') vma_flags |= kVmaRead;
    if (current[1] == 'w') vma_flags |= kVmaWrite;
    if (current[2] == 'x') vma_flags |= kVmaExec;
    if (current[3] == 's') vma_flags |= kVmaShared;
    current += 5;

    if (query_flags != 0 && (query_flags & kVmaAllFlags) != vma_flags) continue;

    auto vma_offset = FastParseHex<uint64_t>(&current);
    auto dev_major = FastParseHex<uint32_t>(&current);
    auto dev_minor = FastParseHex<uint32_t>(&current);
    auto inode = static_cast<uint64_t>(strtoull(current, const_cast<char**>(&current), 10));

    auto name_offset = static_cast<size_t>(current - line->data() + 1);
#ifdef __LP64__
    name_offset = std::max(name_offset, kNameOffset<void*>);
    auto name = line->size() > name_offset ? line->substr(name_offset) : std::string_view{};
#else
    auto name = std::string_view{};
    if (name_offset_) [[likely]] {
      name_offset = std::max({name_offset, name_offset_, kNameOffset<void*>});
      if (line->size() > name_offset) {
        name = line->substr(name_offset);
      }
    } else if (name_offset >= kNameOffset<uint64_t>) [[unlikely]] {
      name = line->substr(name_offset);
    } else if (line->size() > std::max(name_offset, kNameOffset<void*>)) {
      auto data = line->data();
      if (line->size() > kNameOffset<uint64_t> && data[kNameOffset<uint64_t> - 1] == ' ' &&
          data[kNameOffset<uint64_t>] != ' ') [[likely]] {
        name_offset = kNameOffset<uint64_t>;
      } else {
        name_offset = kNameOffset<uint32_t>;
      }
      name = line->substr(name_offset);
      name_offset_ = name_offset;
    }
#endif

    if (query_flags & kVmaQueryFileBackedVma && (name.empty() || name[0] != '/')) [[unlikely]] {
      continue;
    }

    return VmaEntry{
        .vma_start = vma_start,
        .vma_end = vma_end,
        .vma_flags = vma_flags,
        .vma_offset = vma_offset,
        .dev_major = dev_major,
        .dev_minor = dev_minor,
        .inode = inode,
        .name = name,
    };
  }
  return {};
}
}  // namespace

MapsParser::MapsParser(uint32_t query_flags) : maps_reader_{"/proc/self/maps"} {
  auto query = reinterpret_cast<procmap_query*>(query_buffer_.data());
  query->size = sizeof(procmap_query);
  query->query_flags = query_flags | PROCMAP_QUERY_COVERING_OR_NEXT_VMA;
  query->vma_name_addr = reinterpret_cast<uintptr_t>(name_buffer_.data());

  static_assert(sizeof(name_buffer_) == PATH_MAX);
  static_assert(sizeof(query_buffer_) == sizeof(procmap_query));
}

auto MapsParser::NextEntry() -> std::optional<VmaEntry> {
  if (status_ == Status::kCompleted) [[unlikely]] {
    return {};
  }

  if (!procmap_query_failed_ && status_ == Status::kTryIoctl) [[unlikely]] {
    auto query = reinterpret_cast<procmap_query*>(query_buffer_.data());
    query->vma_name_size = name_buffer_.size();
    name_buffer_[0] = '\0';

    int r;
    do {
      r = raw_ioctl(maps_reader_.GetFd(), PROCMAP_QUERY, query);
    } while (r == -EINTR);

    if (r == 0) [[likely]] {
      query->query_addr = query->vma_end;
      auto name_size = static_cast<size_t>(query->vma_name_size);
      if (name_size) --name_size;
      return VmaEntry{
          .vma_start = static_cast<uintptr_t>(query->vma_start),
          .vma_end = static_cast<uintptr_t>(query->vma_end),
          .vma_flags = static_cast<uint32_t>(query->vma_flags),
          .vma_offset = query->vma_offset,
          .dev_major = query->dev_major,
          .dev_minor = query->dev_minor,
          .inode = query->inode,
          .name = std::string_view{name_buffer_.data(), name_size},
      };
    } else if (r == -ENOENT) [[likely]] {
      status_ = Status::kCompleted;
      return {};
    } else {
      // EACCES: The ioctl operation was blocked by SELinux policies.
      // ENODEV: The kernel does not support the PROCMAP_QUERY feature.
      if (r == -EACCES || r == -ENODEV) [[likely]] {
        procmap_query_failed_ = true;
      }
      status_ = Status::kParseText;
    }
  }

  auto query_flags =
      static_cast<uint32_t>(reinterpret_cast<procmap_query*>(query_buffer_.data())->query_flags) & kVmaAllQueryFlags;
  auto result = ParseVmaEntry(maps_reader_, query_flags);

  if (!result) [[unlikely]] {
    status_ = Status::kCompleted;
  }
  return result;
}

SMapsParser::SMapsParser(uint32_t query_flags)
    : smaps_reader_{"/proc/self/smaps"}, query_flags_{query_flags}, completed_{} {}

auto SMapsParser::NextEntry() -> std::optional<SVmaEntry> {
  if (completed_) [[unlikely]] {
    return {};
  }

  smaps_reader_.Reduce();

  while (auto vma = ParseVmaEntry(smaps_reader_, 0)) {
    if (query_flags_ != 0 && ((query_flags_ & kVmaAllFlags) != vma->vma_flags ||
                              (query_flags_ & kVmaQueryFileBackedVma && (vma->name.empty() || vma->name[0] != '/')))) {
      while (auto line = smaps_reader_.NextLine()) {
        if (line->starts_with("VmFlags:")) break;
      }
      smaps_reader_.Reduce();
      continue;
    }
    SVmaEntry entry{.base = *vma};
    while (auto field = smaps_reader_.NextLine()) {
      if (field->starts_with("VmFlags:")) [[unlikely]] {
        entry.vm_flags = std::move(*field);
        return entry;
      } else {
        entry.fields.emplace_back(std::move(*field));
      }
    }
    break;
  }

  completed_ = true;
  return {};
}

auto VmaEntry::get_line() const -> std::string {
  std::array<char, kMaxPrefixSize + PATH_MAX + 1> buffer;
  return std::string{get_line(buffer)};
}

auto VmaEntry::get_line(std::span<char> buffer) const -> std::string_view {
  if (buffer.empty()) [[unlikely]] {
    return {};
  }

  auto perms = std::array{(vma_flags & kVmaRead) ? 'r' : '-',
                          (vma_flags & kVmaWrite) ? 'w' : '-',
                          (vma_flags & kVmaExec) ? 'x' : '-',
                          (vma_flags & kVmaShared) ? 's' : 'p'};
  auto len = snprintf(buffer.data(),
                      buffer.size(),
                      "%08lx-%08lx %.4s %08llx %02x:%02x %llu",
                      static_cast<unsigned long>(vma_start),
                      static_cast<unsigned long>(vma_end),
                      perms.data(),
                      static_cast<unsigned long long>(vma_offset),
                      dev_major,
                      dev_minor,
                      static_cast<unsigned long long>(inode));
  if (len <= 0) [[unlikely]] {
    return {};
  }

  auto cursor = static_cast<size_t>(len);
  if (cursor >= buffer.size()) [[unlikely]] {
    return {buffer.data(), buffer.size() - 1};
  } else {
    buffer[cursor++] = ' ';
  }

  if (!name.empty()) {
    if (cursor >= buffer.size()) [[unlikely]] {
      return {buffer.data(), buffer.size()};
    }

#ifdef __LP64__
    constexpr auto name_offset = kNameOffset<void*>;
#else
    auto name_offset = std::max(name_offset_, kNameOffset<void*>);
#endif

    if (cursor < name_offset) [[likely]] {
      auto padding = std::min(name_offset - cursor, buffer.size() - cursor);
      memset(&buffer[cursor], ' ', padding);
      cursor += padding;
    }

    if (cursor < buffer.size()) [[likely]] {
      auto copy_len = std::min(name.size(), buffer.size() - cursor);
      memcpy(&buffer[cursor], name.data(), copy_len);
      cursor += copy_len;
    }
  }

  if (cursor < buffer.size()) [[likely]] {
    buffer[cursor] = '\0';
  }

  return {buffer.data(), cursor};
}

auto SVmaEntry::get_field(std::string_view name) const -> std::optional<size_t> {
  auto field = get_field_string(name);
  if (!field.empty()) {
    auto begin = field.data();
    auto end = begin + field.size();

    auto tmp = const_cast<char*>(begin);
    auto value = strtoul(begin, &tmp, 10);

    if (tmp != begin && tmp <= end && (tmp == end || *tmp == ' ')) [[likely]] {
      return static_cast<size_t>(value);
    }
  }
  return {};
}

auto SVmaEntry::get_field_string(std::string_view name) const -> std::string_view {
  for (const auto& field : fields) {
    if (!field.starts_with(name)) continue;
    if (name.size() == field.size() || field[name.size()] != ':') continue;
    for (auto data = field.data() + name.size() + 1, end = field.data() + field.size(); data < end; ++data) {
      if (*data == ' ') continue;
      return {data, end};
    }
  }
  return {};
}

auto SVmaEntry::has_vm_flag(std::string_view vm_flag) const -> bool {
  if (vm_flags.size() <= 9 /* "VmFlags: " */) [[unlikely]] {
    return false;
  }
  if (auto pos = vm_flags.find(vm_flag, 9); pos != std::string_view::npos) {
    if (vm_flags[pos - 1] != ' ') return false;
    auto end_pos = pos + vm_flag.size();
    return end_pos == vm_flags.size() || vm_flags[end_pos] == ' ';
  }
  return false;
}

auto SVmaEntry::get_lines() const -> std::string {
  auto result = base.get_line() + '\n';
  for (auto field : fields) {
    result += field;
    result += '\n';
  }
  result += vm_flags;
  return result;
}

auto SVmaEntry::get_lines(std::span<char> buffer) const -> std::string_view {
  auto cursor = base.get_line(buffer).size();

  auto print = [&](const std::string_view& sv) {
    if (cursor >= buffer.size()) [[unlikely]] {
      return;
    }
    auto copy_len = std::min(buffer.size() - cursor, sv.size());
    memcpy(&buffer[cursor], sv.data(), copy_len);
    cursor += copy_len;
  };
  auto new_line = [&] {
    if (cursor < buffer.size()) [[likely]] {
      buffer[cursor++] = '\n';
    }
  };

  new_line();
  for (auto field : fields) {
    print(field);
    new_line();
  }
  print(vm_flags);

  if (cursor < buffer.size()) [[likely]] {
    buffer[cursor] = '\0';
  }

  return {buffer.data(), cursor};
}
}  // namespace io::proc