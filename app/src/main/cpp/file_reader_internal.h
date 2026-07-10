#pragma once

#include <dirent.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

// https://github.com/eirv/linux-syscall-support
#if __has_include("linux_syscall_support_errno.h")

#include "linux_syscall_support.h"

namespace io::internal::posix {
static constexpr auto kHasLSS = true;
using dirent = kernel_dirent64;
}  // namespace io::internal::posix
#else

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <syscall.h>
#endif

namespace io::internal::posix {
static constexpr auto kHasLSS = false;
#ifdef __APPLE__
using dirent = ::dirent;
#else
using dirent = ::dirent64;
#endif

auto raw_open(auto&&...) -> int { std::unreachable(); }
auto raw_openat(auto&&...) -> int { std::unreachable(); }
auto raw_dup(auto&&...) -> int { std::unreachable(); }
auto raw_close(auto&&...) -> int { std::unreachable(); }
auto raw_read(auto&&...) -> ssize_t { std::unreachable(); }
auto raw_getdents64(auto&&...) -> ssize_t { std::unreachable(); }
auto raw_ioctl(auto&&...) -> int { std::unreachable(); }
auto raw_mmap(auto&&...) -> void* { std::unreachable(); }
auto raw_munmap(auto&&...) -> int { std::unreachable(); }
}  // namespace io::internal::posix
#endif

namespace io::internal {

namespace posix {
[[gnu::always_inline]] inline auto open(const char* path, int flags, mode_t mode = 0) -> int {
  if constexpr (kHasLSS) {
    return raw_open(path, flags, mode);
  } else {
    if (auto result = ::open(path, flags, mode); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
[[gnu::always_inline]] inline auto openat(int dirfd, const char* path, int flags, mode_t mode = 0) -> int {
  if constexpr (kHasLSS) {
    return raw_openat(dirfd, path, flags, mode);
  } else {
    if (auto result = ::openat(dirfd, path, flags, mode); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
[[gnu::always_inline]] inline auto dup(int fd) -> int {
  if constexpr (kHasLSS) {
    return raw_dup(fd);
  } else {
    if (auto result = ::dup(fd); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
[[gnu::always_inline]] inline auto close(int fd) -> void {
  if constexpr (kHasLSS) {
    raw_close(fd);
  } else {
    ::close(fd);
  }
}
[[gnu::always_inline]] inline auto read(int fd, void* buf, size_t count) -> ssize_t {
  if constexpr (kHasLSS) {
    return raw_read(fd, buf, count);
  } else {
    if (auto result = ::read(fd, buf, count); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
#ifdef __linux__
[[gnu::always_inline]] inline auto getdents(int fd, dirent* dirp, size_t count) -> ssize_t {
  if constexpr (kHasLSS) {
    return raw_getdents64(fd, dirp, count);
  } else {
    if (auto result = static_cast<ssize_t>(syscall(__NR_getdents64, fd, dirp, count)); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
#endif
[[gnu::always_inline]] inline auto ioctl(int fd, unsigned long op, void* arg) -> int {
  if constexpr (kHasLSS) {
    return raw_ioctl(fd, op, arg);
  } else {
    if (auto result = ::ioctl(fd, op, arg); result >= 0) [[likely]] {
      return result;
    }
    return -errno;
  }
}
[[gnu::always_inline]] inline auto mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) -> void* {
  if constexpr (kHasLSS) {
    return raw_mmap(addr, length, prot, flags, fd, offset);
  } else {
    if (auto result = ::mmap(addr, length, prot, flags, fd, offset); result != MAP_FAILED) [[likely]] {
      return result;
    }
    return reinterpret_cast<void*>(-errno);
  }
}
[[gnu::always_inline]] inline auto munmap(void* addr, size_t length) -> void {
  if constexpr (kHasLSS) {
    raw_munmap(addr, length);
  } else {
    ::munmap(addr, length);
  }
}
}  // namespace posix

template <class, template <class, class...> class>
struct is_instance : public std::false_type {};

template <class... Ts, template <class, class...> class U>
struct is_instance<U<Ts...>, U> : public std::true_type {};

template <class T, template <class, class...> class U>
inline constexpr bool is_instance_v = is_instance<T, U>::value;

template <typename T>
concept IndexableAddressable = requires(T t, std::size_t i) {
  { &t[i] } -> std::same_as<uint8_t*>;
};

template <typename T>
concept BufferPolicy = requires {
  { T::size } -> std::convertible_to<std::size_t>;

  typename T::template type<T::size>;

  { T::template make_buffer<T::size>() } -> std::same_as<typename T::template type<T::size>>;
} && T::size > 0 && IndexableAddressable<typename T::template type<T::size>>;

template <typename A, std::integral T>
static constexpr auto AlignDown(T p) -> T {
  if constexpr (sizeof(A) == 1) {
    return p;
  } else {
    return p & ~(static_cast<T>(sizeof(A)) - T{1});
  }
}

template <typename A, std::integral T>
static constexpr auto AlignUp(T p) -> T {
  if constexpr (sizeof(A) == 1) {
    return p;
  } else {
    return AlignDown<A>(p + static_cast<T>(sizeof(A)) - T{1});
  }
}

template <typename T = char, size_t N = 0>
struct StringView {
  using value_type = T;

  consteval StringView() = default;

  consteval StringView(const value_type (&data)[N]) { std::copy_n(data, N - 1, data_.data()); }

  [[nodiscard]] consteval auto operator*() const { return *data_; }
  [[nodiscard]] consteval auto operator[](size_t index) const { return data_[index]; }

  [[nodiscard]] consteval auto data() const { return data_.data(); }
  [[nodiscard]] consteval auto size() const { return data_.size(); }
  [[nodiscard]] consteval auto empty() const { return data_.empty(); }

  std::array<value_type, N != 0 ? N - 1 : N> data_{};
};

template <class Reader>
class Iterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = Reader::value_type;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  Iterator() = default;

  explicit Iterator(Reader* reader) : reader_{reader} { operator++(); }

  auto operator*() const noexcept -> const_reference { return current_; }
  auto operator*() noexcept -> reference { return current_; }
  auto operator->() const noexcept -> const_pointer { return &current_; }
  auto operator->() noexcept -> pointer { return &current_; }

  auto operator++() noexcept -> auto& {
    if (!reader_) return *this;
    if (auto ret = reader_->operator++()) [[likely]] {
      current_ = std::move(*ret);
    } else {
      reader_ = nullptr;
      current_ = {};
    }
    return *this;
  }

  void operator++(int) noexcept { operator++(); }

  auto operator==(const Iterator& other) const noexcept -> bool { return reader_ == other.reader_; }

 private:
  Reader* reader_{};
  value_type current_{};
};

template <class Derived, class T, class Buffer>
class BaseReader {
 public:
  using value_type = T;
  using iterator = Iterator<Derived>;

  BaseReader() = default;

  BaseReader(BaseReader&& other) noexcept
      : fd_{std::exchange(other.fd_, -1)},
        owned_{std::exchange(other.owned_, false)},
        buf_pos_{other.buf_pos_},
        buf_end_{other.buf_end_},
        buffer_{std::move(other.buffer_)} {}

  auto operator=(BaseReader&& other) noexcept -> auto& {
    if (this != &other) {
      if (fd_ >= 0 && owned_) posix::close(fd_);
      fd_ = std::exchange(other.fd_, -1);
      owned_ = std::exchange(other.owned_, false);
      buf_pos_ = other.buf_pos_;
      buf_end_ = other.buf_end_;
      buffer_ = std::move(other.buffer_);
    }
    return *this;
  }

  BaseReader(const BaseReader&) = delete;
  void operator=(const BaseReader&) = delete;

  ~BaseReader() {
    if (fd_ >= 0 && owned_) [[likely]] {
      posix::close(fd_);
    }
  }

  operator bool() const noexcept { return IsValid(); }

  auto operator++(int) { return static_cast<Derived*>(this)->operator++(); }

  [[nodiscard]] auto IsValid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] auto GetFd() const noexcept { return fd_ >= 0 ? fd_ : -1; }
  [[nodiscard]] auto GetError() const noexcept { return fd_ < 0 ? -fd_ : 0; }

  void Reduce() {
    auto rem = buf_end_ - buf_pos_;
    std::memmove(&buffer_[0], &buffer_[buf_pos_], rem);
    buf_end_ = rem;
    buf_pos_ = 0;
  }

  [[nodiscard]] auto begin() { return iterator{static_cast<Derived*>(this)}; }
  [[nodiscard]] auto end() const { return iterator{}; }

 protected:
  BaseReader(int fd, bool owned)
      : fd_{fd}, owned_{owned}, buffer_{Buffer::template make_buffer<kBufferSize + kReservedBytes>()} {}

  auto NextImpl(auto&& parse_func) -> std::optional<value_type> {
    if (eof_ || fd_ < 0) [[unlikely]] {
      return {};
    }

    for (;;) {
      auto available = buf_end_ - buf_pos_;

      if (auto res = parse_func(&buffer_[buf_pos_], available)) [[likely]] {
        auto [val, consumed] = *res;
        buf_pos_ += consumed;
        if (buf_pos_ == buf_end_) buf_pos_ = buf_end_ = 0;
        return val;
      }

      if (buf_pos_ > 0 && buf_pos_ < buf_end_) [[likely]] {
        Reduce();
      } else if (buf_pos_ == buf_end_) {
        buf_pos_ = buf_end_ = 0;
      }

      auto space = kBufferSize - buf_end_;
      if (space == 0) [[unlikely]] {
        return Derived::OnBufferFull(&buffer_[0], std::exchange(buf_end_, 0));
      }

      ssize_t n;
      do {
        n = Derived::ReadFromFD(fd_, &buffer_[buf_end_], space);
      } while (n == -EINTR);

      if (n <= 0) [[unlikely]] {
        eof_ = true;
        return Derived::OnEOF(&buffer_[0], buf_end_);
      }

      buf_end_ += static_cast<size_t>(n);
    }
  }

 private:
  static constexpr size_t kBufferSize = Buffer::size;

  // String is not null-terminated by default.
  // One extra byte is reserved for user to add null terminator if required.
  static constexpr auto kReservedBytes = [] consteval -> size_t {
    if constexpr (is_instance_v<value_type, std::basic_string_view>) {
      return AlignUp<void*>(kBufferSize + sizeof(typename value_type::value_type)) - kBufferSize;
    } else {
      return 0;
    }
  }();

  int fd_{-EBADF};
  bool owned_{};
  bool eof_{};
  size_t buf_pos_{};
  size_t buf_end_{};
  Buffer::template type<kBufferSize + kReservedBytes> buffer_;
};
}  // namespace io::internal