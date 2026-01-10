#include "maps_parser.h"

#include <linux/fs.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <optional>
#include <string_view>
#include <type_traits>

#include "linux_syscall_support.h"

namespace io::proc {

namespace {
template <int kBit>
constexpr size_t kNameOffset = 25 + (kBit / 8) * 6;

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

MapsParser::MapsParser(uint32_t query_vma_flags) : maps_reader_{"/proc/self/maps"} {
  auto query = reinterpret_cast<procmap_query*>(query_buffer_.data());
  query->size = sizeof(procmap_query);
  query->query_flags = query_vma_flags | PROCMAP_QUERY_COVERING_OR_NEXT_VMA;

  static_assert(sizeof(name_buffer_) == PATH_MAX);
  static_assert(sizeof(query_buffer_) == sizeof(procmap_query));
}

auto MapsParser::NextEntry() -> std::optional<VmaEntry> {
  if (status_ == Status::kCompleted) [[unlikely]] {
    return {};
  }

  if (status_ == Status::kTryIoctl) [[unlikely]] {
    auto query = reinterpret_cast<procmap_query*>(query_buffer_.data());
    query->vma_name_addr = reinterpret_cast<uintptr_t>(name_buffer_.data());
    query->vma_name_size = name_buffer_.size();
    name_buffer_[0] = '\0';

    int r;
    do {
      r = raw_ioctl(maps_reader_.GetFd(), PROCMAP_QUERY, query);
    } while (r == -EINTR);

    if (r == 0) [[likely]] {
      query->query_addr = query->vma_end;
      return VmaEntry{
          .vma_start = static_cast<uintptr_t>(query->vma_start),
          .vma_end = static_cast<uintptr_t>(query->vma_end),
          .vma_flags = static_cast<uint32_t>(query->vma_flags),
          .vma_offset = query->vma_offset,
          .dev_major = query->dev_major,
          .dev_minor = query->dev_minor,
          .inode = query->inode,
          .name = std::string_view{name_buffer_.data(), static_cast<size_t>(query->vma_name_size)},
      };
    } else if (r == -ENOENT) [[likely]] {
      status_ = Status::kCompleted;
      return {};
    } else {
      status_ = Status::kParseText;
    }
  }

  for (std::optional<std::string_view> line; (line = maps_reader_.NextLine()).has_value();) {
    auto tmp = line->data();

    auto vma_start = FastParseHex<uintptr_t>(&tmp);
    auto vma_end = FastParseHex<uintptr_t>(&tmp);

    auto vma_flags = uint32_t{};
    if (tmp[0] == 'r') vma_flags |= kVmaRead;
    if (tmp[1] == 'w') vma_flags |= kVmaWrite;
    if (tmp[2] == 'x') vma_flags |= kVmaExec;
    if (tmp[3] == 's') vma_flags |= kVmaShared;
    tmp += 5;

    auto query_flags =
        static_cast<uint32_t>(reinterpret_cast<procmap_query*>(query_buffer_.data())->query_flags) & kVmaAllFlags;
    if (query_flags != 0 && query_flags != vma_flags) continue;

    auto vma_offset = FastParseHex<uint64_t>(&tmp);
    auto dev_major = FastParseHex<uint32_t>(&tmp);
    auto dev_minor = FastParseHex<uint32_t>(&tmp);
    auto inode = FastParseHex<uint64_t>(&tmp);

#ifdef __LP64__
    auto name = line->size() > kNameOffset<64> ? line->substr(kNameOffset<64>) : std::string_view{};
#else
    static auto name_offset = size_t{};
    auto name = std::string_view{};
    if (name_offset) [[likely]] {
      if (line->size() > name_offset) {
        name = line->substr(name_offset);
      }
    } else if (line->size() > kNameOffset<32>) {
      auto pos = std::max(kNameOffset<32>, static_cast<size_t>(tmp - line->data()) - 1);
      name_offset = line->data()[pos] == ' ' ? kNameOffset<64> : kNameOffset<32>;
      name = line->substr(name_offset);
    }
#endif

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

}  // namespace io::proc
