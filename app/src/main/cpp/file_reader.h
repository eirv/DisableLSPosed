#pragma once

#include <dirent.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "linux_syscall_support.h"

namespace io {

namespace internal {
template <typename T>
struct is_string_view : std::false_type {};

template <typename CharT, typename Traits>
struct is_string_view<std::basic_string_view<CharT, Traits>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_view_v = is_string_view<T>::value;

template <typename T = char, size_t N = 0>
struct FixedString {
  using value_type = T;

  consteval FixedString() = default;

  consteval FixedString(const value_type (&data)[N]) { std::copy_n(data, N - 1, data_); }

  consteval auto operator*() const { return operator[](0); }

  consteval auto operator[](size_t index) const {
    static_assert(N > 1);
    return data_[index];
  }

  consteval auto data() const { return data_; }

  consteval auto size() const {
    if constexpr (N == 0) {
      return 0;
    } else {
      return N - 1;
    }
  }

  consteval auto empty() const -> bool { return size() == 0; }

  value_type data_[N != 0 ? N - 1 : N]{};
};

template <class Reader>
class Iterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = typename Reader::value_type;
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

template <class Derived, class T, size_t kBufferSize, bool kUseHeap>
class BaseReader {
 public:
  using value_type = T;
  using iterator = Iterator<Derived>;

  BaseReader(int fd, bool owned) : fd_{fd}, owned_{owned} {
    if constexpr (kUseHeap) {
      buffer_ = std::make_unique<char[]>(kBufferSize + kReservedBytes);
    }
  }

  BaseReader(BaseReader&& other) noexcept
      : fd_{std::exchange(other.fd_, -1)},
        owned_{std::exchange(other.owned_, false)},
        buf_pos_{other.buf_pos_},
        buf_end_{other.buf_end_},
        buffer_{std::move(other.buffer_)} {}

  auto operator=(BaseReader&& other) noexcept -> auto& {
    if (this != &other) {
      if (fd_ >= 0 && owned_) raw_close(fd_);
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
      raw_close(fd_);
    }
  }

  operator bool() const noexcept { return IsValid(); }

  auto operator++(int) { return static_cast<Derived*>(this)->operator++(); }

  auto IsValid() const noexcept { return fd_ >= 0; }
  auto GetFd() const noexcept { return fd_; }

  auto begin() { return iterator{static_cast<Derived*>(this)}; }
  auto end() { return iterator{}; }

 protected:
  template <typename Parser>
  auto NextImpl(Parser&& parse_func) -> std::optional<value_type> {
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
        auto rem = buf_end_ - buf_pos_;
        memmove(&buffer_[0], &buffer_[buf_pos_], rem);
        buf_end_ = rem;
        buf_pos_ = 0;
      } else if (buf_pos_ == buf_end_) {
        buf_pos_ = buf_end_ = 0;
      }

      auto space = kBufferSize - buf_end_;
      if (space == 0) [[unlikely]] {
        return static_cast<Derived*>(this)->OnBufferFull(&buffer_[0], std::exchange(buf_end_, 0));
      }

      ssize_t n;
      do {
        n = static_cast<Derived*>(this)->ReadFromFD(fd_, &buffer_[buf_end_], space);
      } while (n == -EINTR);

      if (n <= 0) [[unlikely]] {
        eof_ = true;
        return static_cast<Derived*>(this)->OnEOF(&buffer_[0], buf_end_);
      }

      buf_end_ += static_cast<size_t>(n);
    }
  }

 private:
  // String is not null-terminated by default.
  // One extra byte is reserved for user to add null terminator if required.
  static constexpr size_t kReservedBytes =
      is_string_view_v<value_type> ?
          __builtin_align_up(kBufferSize + sizeof(typename value_type::value_type), sizeof(void*)) - kBufferSize :
          0;

  int fd_;
  bool owned_;
  bool eof_{};
  size_t buf_pos_{};
  size_t buf_end_{};
  std::conditional_t<kUseHeap, std::unique_ptr<char[]>, std::array<char, kBufferSize + kReservedBytes>> buffer_;
};

constexpr size_t kDefaultBufferSize = 16 * 1024;

template <size_t kBufferSize>
constexpr bool kUseHeap = kBufferSize > kDefaultBufferSize;
}  // namespace internal

template <auto kBufferSize = internal::kDefaultBufferSize,
          auto kUseHeap = internal::kUseHeap<kBufferSize>,
          internal::FixedString kDelimiter = {}>
  requires(kBufferSize > 0 &&
           __builtin_align_down(kBufferSize, sizeof(typename decltype(kDelimiter)::value_type)) == kBufferSize)
class FileReader : public internal::BaseReader<FileReader<kBufferSize, kUseHeap, kDelimiter>,
                                               std::basic_string_view<typename decltype(kDelimiter)::value_type>,
                                               kBufferSize,
                                               kUseHeap> {
 public:
  using char_type = typename decltype(kDelimiter)::value_type;
  using string_view_type = std::basic_string_view<char_type>;

  explicit FileReader(int fd) : FileReader::BaseReader{fd, false} {}
  explicit FileReader(const char* pathname) : FileReader::BaseReader{raw_open(pathname, O_RDONLY | O_CLOEXEC), true} {}

  FileReader(int dirfd, const char* pathname)
      : FileReader::BaseReader{raw_openat(dirfd, pathname, O_RDONLY | O_CLOEXEC), true} {}

  auto operator++() { return NextLine(); }

  auto NextLine() -> std::optional<string_view_type> {
    return this->NextImpl(+[](char* buf, size_t available) -> std::optional<std::pair<string_view_type, size_t>> {
      if (!available) [[unlikely]] {
        return {};
      }

      if constexpr (sizeof(char_type) == 2) {
        available &= ~1;
        if (!available) [[unlikely]] {
          return {};
        }
      } else if constexpr (sizeof(char_type) >= 4) {
        if (auto aligned = __builtin_align_down(available, sizeof(char_type))) [[likely]] {
          available = aligned;
        } else {
          return {};
        }
      }

      void* next = nullptr;
      if constexpr (std::is_same_v<char_type, char>) {
        if constexpr (kDelimiter.empty()) {
          next = memchr(buf, '\n', available);
        } else if constexpr (kDelimiter.size() == sizeof(char_type)) {
          next = memchr(buf, *kDelimiter, available);
        } else {
          next = memmem(buf, available, kDelimiter.data(), kDelimiter.size());
        }
      } else {
        auto sv = string_view_type{reinterpret_cast<char_type*>(buf), reinterpret_cast<char_type*>(buf + available)};
        auto pos = string_view_type::npos;
        if constexpr (kDelimiter.empty()) {
          pos = sv.find(static_cast<char_type>('\n'));
        } else if constexpr (kDelimiter.size() == sizeof(char_type)) {
          pos = sv.find(*kDelimiter);
        } else {
          pos = sv.find(kDelimiter.data(), 0, kDelimiter.size());
        }
        if (pos != string_view_type::npos) [[likely]] {
          next = reinterpret_cast<char_type*>(buf) + pos;
        }
      }

      if (next) [[likely]] {
        // *next = '\0';
        auto len = static_cast<size_t>(reinterpret_cast<char_type*>(next) - reinterpret_cast<char_type*>(buf));
        return std::pair{string_view_type{reinterpret_cast<char_type*>(buf), len},
                         (len + std::max<size_t>(kDelimiter.size(), 1)) * sizeof(char_type)};
      }
      return {};
    });
  }

 private:
  auto OnBufferFull(char* buf, size_t sz) -> std::optional<string_view_type> {
    return string_view_type{reinterpret_cast<char_type*>(buf), reinterpret_cast<char_type*>(buf + sz)};
  }

  auto OnEOF(char* buf, size_t sz) -> std::optional<string_view_type> {
    if constexpr (sizeof(char_type) == 2) {
      sz &= ~1;
    } else if constexpr (sizeof(char_type) >= 4) {
      sz = __builtin_align_down(sz, sizeof(char_type));
    }
    if (sz == 0) return {};
    // buf[sz] = '\0';
    return string_view_type{reinterpret_cast<char_type*>(buf), reinterpret_cast<char_type*>(buf + sz)};
  }

  auto ReadFromFD(int fd, void* buf, size_t sz) { return raw_read(fd, buf, sz); }

  friend class FileReader::BaseReader;
};

enum class DirEntryType : uint8_t {
  kUnknown = DT_UNKNOWN,
  kFIFO = DT_FIFO,
  kCharacterDevice = DT_CHR,
  kDirectory = DT_DIR,
  kBlockDevice = DT_BLK,
  kRegularFile = DT_REG,
  kSymbolicLink = DT_LNK,
  kSocket = DT_SOCK,
};

struct DirEntry {
  kernel_dirent64* entry;

  [[nodiscard]] auto inode() const { return entry->d_ino; }
  [[nodiscard]] auto offset() const { return entry->d_off; }
  [[nodiscard]] auto type() const { return static_cast<DirEntryType>(entry->d_type); }
  [[nodiscard]] auto name() const {
    return std::string_view{entry->d_name, strnlen(entry->d_name, entry->d_reclen - offsetof(kernel_dirent64, d_name))};
  }

  [[nodiscard]] auto is_unknown() const { return type() == DirEntryType::kUnknown; }
  [[nodiscard]] auto is_fifo() const { return type() == DirEntryType::kFIFO; }
  [[nodiscard]] auto is_character_device() const { return type() == DirEntryType::kCharacterDevice; }
  [[nodiscard]] auto is_directory() const { return type() == DirEntryType::kDirectory; }
  [[nodiscard]] auto is_block_device() const { return type() == DirEntryType::kBlockDevice; }
  [[nodiscard]] auto is_regular_file() const { return type() == DirEntryType::kRegularFile; }
  [[nodiscard]] auto is_symbolic_link() const { return type() == DirEntryType::kSymbolicLink; }
  [[nodiscard]] auto is_socket() const { return type() == DirEntryType::kSocket; }
};

template <auto kBufferSize = internal::kDefaultBufferSize, auto kUseHeap = internal::kUseHeap<kBufferSize>>
  requires(kBufferSize > offsetof(kernel_dirent64, d_name))
class DirReader : public internal::BaseReader<DirReader<kBufferSize, kUseHeap>, DirEntry, kBufferSize, kUseHeap> {
 public:
  explicit DirReader(int fd) : DirReader::BaseReader{fd, false} {}
  explicit DirReader(const char* pathname) : DirReader::BaseReader{raw_open(pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  DirReader(int dirfd, const char* pathname)
      : DirReader::BaseReader{raw_openat(dirfd, pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  auto operator++() { return NextEntry(); }

  auto NextEntry() -> std::optional<DirEntry> {
    return this->NextImpl(+[](char* buf, size_t available) -> std::optional<std::pair<DirEntry, size_t>> {
      if (available < offsetof(kernel_dirent64, d_name)) [[unlikely]] {
        return {};
      }

      auto dir = reinterpret_cast<kernel_dirent64*>(buf);
      if (available < dir->d_reclen) [[unlikely]] {
        return {};
      }

      return std::pair{DirEntry{dir}, dir->d_reclen};
    });
  }

 private:
  auto OnBufferFull(char*, size_t) -> std::optional<DirEntry> { return {}; }

  auto OnEOF(char*, size_t) -> std::optional<DirEntry> { return {}; }

  auto ReadFromFD(int fd, void* buf, size_t sz) { return raw_getdents64(fd, static_cast<kernel_dirent64*>(buf), sz); }

  friend class DirReader::BaseReader;
};

}  // namespace io
