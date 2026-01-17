#include "maps_parser.h"

#include <linux/fs.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include "linux_syscall_support.h"

namespace io::proc {

namespace {
auto procmap_query_failed_ = bool{};
#ifndef __LP64__
auto name_offset_ = size_t{};
#endif

template <typename T>
constexpr size_t kNameOffset = 25 + sizeof(T) * 6;

template <typename T>
  requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
auto FastParseHex(const char** p) {
  T value{};
  for (auto s = *p;;) {
    auto c = static_cast<unsigned char>(*s);

    T digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<T>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<T>(c - 'a' + 10);
    } else {
      *p = s + 1;
      return value;
    }

    value = (value << 4) | digit;
    ++s;
  }
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

  while (auto line = maps_reader_.NextLine()) {
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

    auto query_flags =
        static_cast<uint32_t>(reinterpret_cast<procmap_query*>(query_buffer_.data())->query_flags) & kVmaAllQueryFlags;
    if (query_flags != 0 && (query_flags & kVmaAllFlags) != vma_flags) continue;

    auto vma_offset = FastParseHex<uint64_t>(&current);
    auto dev_major = FastParseHex<uint32_t>(&current);
    auto dev_minor = FastParseHex<uint32_t>(&current);
    auto inode = static_cast<uint64_t>(strtoull(current, const_cast<char**>(&current), 10));

#ifdef __LP64__
    auto name = line->size() > kNameOffset<void*> ? line->substr(kNameOffset<void*>) : std::string_view{};
#else
    auto name = std::string_view{};
    if (name_offset_) [[likely]] {
      if (line->size() > name_offset_) {
        name = line->substr(name_offset_);
      }
    } else if (line->size() > kNameOffset<void*>) {
      auto data = line->data();
      auto pos = std::max(kNameOffset<void*>, static_cast<size_t>(current - data));
      if (data[pos] == ' ' && line->size() > kNameOffset<uint64_t> && data[kNameOffset<uint64_t> - 1] == ' ')
          [[likely]] {
        name_offset_ = kNameOffset<uint64_t>;
      } else {
        name_offset_ = kNameOffset<uint32_t>;
      }
      name = line->substr(name_offset_);
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

  status_ = Status::kCompleted;
  return {};
}

auto VmaEntry::get_line() const -> std::string {
  std::array<char, kNameOffset<uint64_t> + PATH_MAX + 1> buf;

  size_t size = snprintf(buf.data(),
                         kNameOffset<uint64_t>,
                         "%08lx-%08lx %c%c%c%c %08llx %02x:%02x %llu",
                         static_cast<unsigned long>(vma_start),
                         static_cast<unsigned long>(vma_end),
                         vma_flags & kVmaRead ? 'r' : '-',
                         vma_flags & kVmaWrite ? 'w' : '-',
                         vma_flags & kVmaExec ? 'x' : '-',
                         vma_flags & kVmaShared ? 's' : 'p',
                         static_cast<unsigned long long>(vma_offset),
                         dev_major,
                         dev_minor,
                         static_cast<unsigned long long>(inode));

  buf[size] = ' ';

  if (name.empty()) {
    buf[++size] = '\0';
  } else {
#ifdef __LP64__
    constexpr auto name_offset = kNameOffset<void*>;
#else
    auto name_offset = std::max(name_offset_, kNameOffset<void*>);
#endif
    if (auto space_size = static_cast<ssize_t>(name_offset) - static_cast<ssize_t>(size); space_size > 0) [[likely]] {
      memset(&buf[size], ' ', space_size);
    }

    auto prefix_size = std::max(size + 1, name_offset);
    auto name_size = std::min(name.size(), buf.size() - 1 - prefix_size);

    memcpy(&buf[prefix_size], name.data(), name_size);

    size = prefix_size + name_size;
    buf[size] = '\0';
  }

  return {buf.data(), size};
}

}  // namespace io::proc
