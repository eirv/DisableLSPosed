#pragma once

#include <cerrno>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>

#include "linux_syscall_support.h"

template <size_t kBufferSize = 16 * 1024>
class FileReader {
 public:
  class Iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = std::string_view;
    using difference_type = std::ptrdiff_t;
    using reference = const std::string_view&;
    using pointer = const std::string_view*;

    auto operator*() const -> reference { return current_; }
    auto operator->() const -> pointer { return &current_; }

    auto operator++() -> auto& {
      if (!fr_) return *this;
      auto sv = fr_->NextLine();
      if (!sv) {
        fr_ = nullptr;
      } else {
        current_ = *sv;
      }
      return *this;
    }

    auto operator++(int) {
      auto tmp = *this;
      ++(*this);
      return tmp;
    }

    auto operator==(const Iterator& other) const { return fr_ == other.fr_; }
    auto operator!=(const Iterator& other) const { return !(*this == other); }

   private:
    Iterator() {}
    explicit Iterator(FileReader* fr) : fr_{fr} { ++(*this); }

    FileReader* fr_{};
    std::string_view current_{};

    friend class FileReader;
  };

  explicit FileReader(const char* pathname) : fd_{raw_open(pathname, O_RDONLY | O_CLOEXEC, 0)} {}

  explicit FileReader(int fd) : fd_{fd} {}

  FileReader(const FileReader&) = delete;
  FileReader& operator=(const FileReader&) = delete;

  ~FileReader() {
    if (fd_ >= 0) raw_close(fd_);
  }

  auto NextLine() -> std::optional<std::string_view> {
    if (fd_ < 0) return {};

    for (;;) {
      auto available = buf_end_ - buf_pos_;

      if (available > 0) {
        if (auto nl = static_cast<char*>(memchr(buffer_ + buf_pos_, '\n', available))) {
          auto len = static_cast<size_t>(nl - (buffer_ + buf_pos_));

          *nl = '\0';

          std::string_view sv{buffer_ + buf_pos_, len};
          buf_pos_ = (nl - buffer_) + 1;

          if (buf_pos_ == buf_end_) buf_pos_ = buf_end_ = 0;

          return sv;
        }
      }

      if (buf_pos_ > 0 && buf_pos_ < buf_end_) {
        auto rem = buf_end_ - buf_pos_;
        memmove(buffer_, buffer_ + buf_pos_, rem);
        buf_end_ = rem;
        buf_pos_ = 0;
      } else if (buf_pos_ == buf_end_) {
        buf_pos_ = buf_end_ = 0;
      }

      auto space = kBufferSize - buf_end_;
      if (space == 0) {
        buffer_[buf_end_] = '\0';
        std::string_view sv{buffer_, buf_end_};
        buf_pos_ = buf_end_ = 0;
        return sv;
      }

      ssize_t n;
      do {
        n = raw_read(fd_, buffer_ + buf_end_, space);
      } while (n == -EINTR);

      if (n <= 0) {
        if (buf_end_ == 0) return {};
        buffer_[buf_end_] = '\0';
        std::string_view sv{buffer_, buf_end_};
        buf_pos_ = buf_end_ = 0;
        return sv;
      }

      buf_end_ += static_cast<size_t>(n);
    }
  }

  auto begin() { return Iterator{this}; }
  auto end() { return Iterator{}; }

 private:
  int fd_;
  size_t buf_pos_{};
  size_t buf_end_{};
  char buffer_[kBufferSize];
};
