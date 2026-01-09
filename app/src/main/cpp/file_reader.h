#pragma once

#include <dirent.h>

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
template <class Reader>
class BaseIterator {
 public:
  using iterator_category = std::input_iterator_tag;
  using value_type = typename Reader::value_type;
  using difference_type = std::ptrdiff_t;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = value_type*;
  using const_pointer = const value_type*;

  BaseIterator() = default;
  explicit BaseIterator(Reader* reader) : reader_{reader} { operator++(); }

  auto operator*() const noexcept -> const_reference { return current_; }
  auto operator*() noexcept -> reference { return current_; }
  auto operator->() const noexcept -> const_pointer { return &current_; }
  auto operator->() noexcept -> pointer { return &current_; }

  auto operator++() noexcept -> auto& {
    if (!reader_) return *this;
    auto ret = reader_->operator++();
    if (!ret) {
      reader_ = nullptr;
      current_ = {};
    } else {
      current_ = std::move(*ret);
    }
    return *this;
  }

  auto operator++(int) noexcept {
    auto tmp = *this;
    ++(*this);
    return tmp;
  }

  auto operator==(const BaseIterator& other) const noexcept -> bool { return reader_ == other.reader_; }

 private:
  Reader* reader_{};
  value_type current_{};
};

template <class Derived, class T, size_t kBufferSize, bool kUseHeap>
class BaseReader {
 public:
  using value_type = T;
  using iterator = BaseIterator<Derived>;

  explicit BaseReader(int fd, bool owned) : fd_{fd}, owned_{owned} {
    if constexpr (kUseHeap) {
      buffer_ = std::make_unique<char[]>(kBufferSize);
    }
  }

  BaseReader(BaseReader&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)),
        owned_(std::exchange(other.owned_, false)),
        buf_pos_(other.buf_pos_),
        buf_end_(other.buf_end_),
        buffer_(std::move(other.buffer_)) {}

  BaseReader& operator=(BaseReader&& other) noexcept {
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
  BaseReader& operator=(const BaseReader&) = delete;

  ~BaseReader() {
    if (fd_ >= 0 && owned_) raw_close(fd_);
  }

  operator bool() const noexcept { return IsValid(); }

  auto operator++(int) { return operator++(); }

  auto IsValid() const noexcept { return fd_ >= 0; }
  auto GetFd() const noexcept { return fd_; }

  auto begin() { return iterator{static_cast<Derived*>(this)}; }
  auto end() { return iterator{}; }

 protected:
  template <typename Parser>
  auto NextImpl(Parser&& parse_func) -> std::optional<T> {
    if (fd_ < 0) return {};

    for (;;) {
      auto available = buf_end_ - buf_pos_;

      if (auto res = parse_func(&buffer_[buf_pos_], available)) {
        auto [val, consumed] = *res;
        buf_pos_ += consumed;
        if (buf_pos_ == buf_end_) buf_pos_ = buf_end_ = 0;
        return val;
      }

      if (buf_pos_ > 0 && buf_pos_ < buf_end_) {
        auto rem = buf_end_ - buf_pos_;
        memmove(&buffer_[0], &buffer_[buf_pos_], rem);
        buf_end_ = rem;
        buf_pos_ = 0;
      } else if (buf_pos_ == buf_end_) {
        buf_pos_ = buf_end_ = 0;
      }

      auto space = kBufferSize - buf_end_;
      if (space == 0) {
        return static_cast<Derived*>(this)->OnBufferFull(&buffer_[0], buf_end_);
      }

      ssize_t n;
      do {
        n = static_cast<Derived*>(this)->ReadFromFD(fd_, &buffer_[buf_end_], space);
      } while (n == -EINTR);

      if (n <= 0) {
        return static_cast<Derived*>(this)->OnEOF(&buffer_[0], buf_end_);
      }

      buf_end_ += static_cast<size_t>(n);
    }
  }

 private:
  int fd_;
  bool owned_;
  size_t buf_pos_{};
  size_t buf_end_{};
  std::conditional_t<kUseHeap, std::unique_ptr<char[]>, std::array<char, kBufferSize>> buffer_;
};

constexpr size_t kDefaultBufferSize = 16 * 1024;

template <size_t kBufferSize>
constexpr bool kUseHeap = kBufferSize > kDefaultBufferSize;
}  // namespace internal

template <auto kBufferSize = internal::kDefaultBufferSize, auto kUseHeap = internal::kUseHeap<kBufferSize>>
class FileReader
    : public internal::BaseReader<FileReader<kBufferSize, kUseHeap>, std::string_view, kBufferSize, kUseHeap> {
 public:
  explicit FileReader(int fd) : FileReader{fd, false} {}
  explicit FileReader(const char* pathname) : FileReader{raw_open(pathname, O_RDONLY | O_CLOEXEC, 0), true} {}

  FileReader(int dirfd, const char* pathname)
      : FileReader{raw_openat(dirfd, pathname, O_RDONLY | O_CLOEXEC, 0), true} {}

  auto operator++() { return NextLine(); }

  auto NextLine() -> std::optional<std::string_view> {
    return this->NextImpl(+[](char* buf, size_t available) -> std::optional<std::pair<std::string_view, size_t>> {
      if (available == 0) return {};

      if (auto nl = static_cast<char*>(memchr(buf, '\n', available))) {
        *nl = '\0';
        auto len = static_cast<size_t>(nl - buf);
        return std::pair{std::string_view{buf, len}, len + 1};
      }
      return {};
    });
  }

 private:
  FileReader(int fd, bool owned)
      : internal::BaseReader<FileReader, std::string_view, kBufferSize, kUseHeap>{fd, owned} {}

  auto OnBufferFull(char* buf, size_t sz) -> std::optional<std::string_view> { return std::string_view{buf, sz}; }

  auto OnEOF(char* buf, size_t sz) -> std::optional<std::string_view> {
    if (sz == 0) return {};
    buf[sz] = '\0';
    return std::string_view{buf, sz};
  }

  auto ReadFromFD(int fd, void* buf, size_t sz) { return raw_read(fd, buf, sz); }

  friend class internal::BaseReader<FileReader, std::string_view, kBufferSize, kUseHeap>;
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
class DirReader : public internal::BaseReader<DirReader<kBufferSize, kUseHeap>, DirEntry, kBufferSize, kUseHeap> {
 public:
  explicit DirReader(int fd) : DirReader{fd, false} {}
  explicit DirReader(const char* pathname) : DirReader{raw_open(pathname, O_DIRECTORY | O_CLOEXEC, 0), true} {}

  DirReader(int dirfd, const char* pathname)
      : DirReader{raw_openat(dirfd, pathname, O_DIRECTORY | O_CLOEXEC, 0), true} {}

  auto operator++() { return NextEntry(); }

  auto NextEntry() -> std::optional<DirEntry> {
    return this->NextImpl(+[](char* buf, size_t available) -> std::optional<std::pair<DirEntry, size_t>> {
      if (available < offsetof(kernel_dirent64, d_name)) return {};

      auto dir = reinterpret_cast<kernel_dirent64*>(buf);
      if (available < dir->d_reclen) return {};

      return std::pair{DirEntry{dir}, dir->d_reclen};
    });
  }

 private:
  DirReader(int fd, bool owned) : internal::BaseReader<DirReader, DirEntry, kBufferSize, kUseHeap>{fd, owned} {}

  auto OnBufferFull(char*, size_t) { return std::optional<DirEntry>{}; }

  auto OnEOF(char*, size_t) { return std::optional<DirEntry>{}; }

  auto ReadFromFD(int fd, void* buf, size_t sz) { return raw_getdents64(fd, static_cast<kernel_dirent64*>(buf), sz); }

  friend class internal::BaseReader<DirReader, DirEntry, kBufferSize, kUseHeap>;
};

}  // namespace io
