#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "file_reader_internal.h"

namespace io {

template <size_t kDefaultBufferSize>
struct StackBuffer {
  template <size_t kBufferSize = kDefaultBufferSize>
  using type = std::array<uint8_t, kBufferSize>;

  static constexpr auto size = kDefaultBufferSize;

  template <size_t kBufferSize = kDefaultBufferSize>
  static constexpr auto make_buffer() -> type<kBufferSize> {
    return {};
  }

  StackBuffer() = delete;
};

template <size_t kDefaultBufferSize>
struct HeapBuffer {
  template <size_t>
  using type = std::unique_ptr<uint8_t[]>;

  static constexpr auto size = kDefaultBufferSize;

  template <size_t kBufferSize = kDefaultBufferSize>
  static constexpr auto make_buffer() -> type<kBufferSize> {
    return std::make_unique<uint8_t[]>(kBufferSize);
  }

  HeapBuffer() = delete;
};

template <size_t kDefaultBufferSize>
struct MMapBuffer {
  template <size_t kBufferSize = kDefaultBufferSize>
  using type = MMapBuffer<kBufferSize>;

  static constexpr auto size = kDefaultBufferSize;

  template <size_t kBufferSize = kDefaultBufferSize>
    requires(kBufferSize > 0)
  static constexpr auto make_buffer() -> type<kBufferSize> {
    return {};
  }

  auto operator[](size_t index) const { return base_[index]; }
  auto operator[](size_t index) -> auto& { return base_[index]; }

  MMapBuffer(MMapBuffer&& other) noexcept : base_{std::exchange(other.base_, nullptr)} {}

  auto operator=(MMapBuffer&& other) noexcept -> auto& {
    if (this != &other) {
      if (base_) internal::posix::munmap(base_, kDefaultBufferSize);
      base_ = std::exchange(other.base_, nullptr);
    }
    return *this;
  }

  MMapBuffer() {
    auto base =
        internal::posix::mmap(nullptr, kDefaultBufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reinterpret_cast<uintptr_t>(base) < -4095UL) [[likely]] {
      base_ = static_cast<uint8_t*>(base);
    }
  }

  ~MMapBuffer() {
    if (base_) [[likely]] {
      internal::posix::munmap(base_, kDefaultBufferSize);
    }
  }

  MMapBuffer(const MMapBuffer&) = delete;
  void operator=(const MMapBuffer&) = delete;

 private:
  uint8_t* base_{};
};

using DefaultStackBuffer = StackBuffer<16 * 1024>;
using DefaultHeapBuffer = HeapBuffer<32 * 1024>;
using DefaultMMapBuffer = MMapBuffer<64 * 1024>;
using DefaultBuffer = DefaultStackBuffer;

struct UTF8 {
  static constexpr auto LF = internal::StringView<char>{};
  static constexpr auto CR = internal::StringView{"\r"};
  static constexpr auto CRLF = internal::StringView{"\r\n"};
  static constexpr auto SPACE = internal::StringView{" "};
};

struct UTF16 {
  static constexpr auto LF = internal::StringView<char16_t>{};
  static constexpr auto CR = internal::StringView{u"\r"};
  static constexpr auto CRLF = internal::StringView{u"\r\n"};
  static constexpr auto SPACE = internal::StringView{u" "};
};

struct UTF32 {
  static constexpr auto LF = internal::StringView<char32_t>{};
  static constexpr auto CR = internal::StringView{U"\r"};
  static constexpr auto CRLF = internal::StringView{U"\r\n"};
  static constexpr auto SPACE = internal::StringView{U" "};
};

static constexpr auto UTF8 = UTF8::LF;
static constexpr auto UTF16 = UTF16::LF;
static constexpr auto UTF32 = UTF32::LF;

/**
 * @brief A high-performance, flexible file reader designed for range-based loops.
 *
 * The FileReader class allows iterating over a file line-by-line (or token-by-token)
 * with zero-copy semantics where possible. It supports customizable memory allocation
 * strategies (Stack vs. Heap) and flexible delimiter/encoding handling.
 *
 * The type of the yielded `line` (e.g., std::string_view, std::wstring_view) is
 * automatically deduced based on the provided delimiter's character type.
 *
 * @tparam Buffer Controls how the internal buffer is allocated.
 * Options: DefaultStackBuffer, StackBuffer<Size>, DefaultHeapBuffer, HeapBuffer<Size>.
 * @tparam kDelimiter Defines the separator and the character encoding.
 * Can be a predefined constant (e.g., UTF8::LF) or a string literal.
 *
 * @example **Basic Usage (Buffering Strategies)**
 * @code
 * // 1. Default Stack Buffer (Fastest, suitable for typical line lengths)
 * for (auto line : FileReader<DefaultStackBuffer>{"/path/to/file"}) {
 * // line is std::string_view
 * }
 *
 * // 2. Custom Size Stack Buffer (e.g., 1024 bytes)
 * for (auto line : FileReader<StackBuffer<1024>>{"/path/to/file"}) {}
 *
 * // 3. Default Heap Buffer (Suitable for large files or limited stack environments)
 * for (auto line : FileReader<DefaultHeapBuffer>{"/path/to/file"}) {}
 *
 * // 4. Custom Size Heap Buffer
 * for (auto line : FileReader<HeapBuffer<4096>>{"/path/to/file"}) {}
 * @endcode
 *
 * @example **Custom Delimiters & Encodings**
 * @code
 * // Unix style (LF), explicitly specified
 * for (auto line : FileReader<DefaultStackBuffer, UTF8>{"..."}) {}
 * for (auto line : FileReader<DefaultStackBuffer, UTF8::LF>{"..."}) {}
 *
 * // Windows style (CRLF)
 * for (auto line : FileReader<DefaultStackBuffer, UTF8::CRLF>{"..."}) {}
 *
 * // Mac style (CR)
 * for (auto line : FileReader<DefaultStackBuffer, UTF8::CR>{"..."}) {}
 *
 * // Custom separator (e.g., Space)
 * for (auto line : FileReader<DefaultStackBuffer, " ">{"..."}) {}  // Equivalent to UTF8::SPACE
 * @endcode
 *
 * @example **Type Deduction via Literals**
 * The return type of `line` changes based on the delimiter type:
 * @code
 * // 1. char -> std::string_view
 * FileReader<DefaultStackBuffer, "\n">       // Equivalent to UTF8 and UTF8::LF
 * FileReader<DefaultStackBuffer, "\r\n">     // Equivalent to UTF8::CRLF
 *
 * // 2. wchar_t -> std::wstring_view
 * FileReader<DefaultStackBuffer, L"\n">
 *
 * // 3. char8_t (C++20) -> std::u8string_view
 * FileReader<DefaultStackBuffer, u8"\n">
 *
 * // 4. char16_t -> std::u16string_view
 * FileReader<DefaultStackBuffer, u"\r\n">    // Equivalent to UTF16::CRLF
 *
 * // 5. char32_t -> std::u32string_view
 * FileReader<DefaultStackBuffer, U"\n">      // Equivalent to UTF32 and UTF32::LF
 * @endcode
 *
 * @warning The returned string_view points to the internal buffer. Do not store
 * the view itself outside the loop iteration, as the buffer content changes or
 * gets overwritten as reading progresses.
 */
template <internal::BufferPolicy Buffer = DefaultBuffer, internal::StringView kDelimiter = UTF8::LF>
  requires(Buffer::size % sizeof(typename decltype(kDelimiter)::value_type) == 0)
class FileReader : public internal::BaseReader<FileReader<Buffer, kDelimiter>,
                                               std::basic_string_view<typename decltype(kDelimiter)::value_type>,
                                               Buffer> {
 public:
  using char_type = decltype(kDelimiter)::value_type;
  using string_view_type = std::basic_string_view<char_type>;

  using FileReader::BaseReader::BaseReader;

  explicit FileReader(int fd) : FileReader::BaseReader{fd >= 0 ? fd : -EBADF, false} {}

  explicit FileReader(const char* pathname)
      : FileReader::BaseReader{internal::posix::open(pathname, O_RDONLY | O_CLOEXEC), true} {}

  FileReader(int dirfd, const char* pathname)
      : FileReader::BaseReader{internal::posix::openat(dirfd, pathname, O_RDONLY | O_CLOEXEC), true} {}

  auto operator++() { return NextLine(); }

  [[nodiscard]] auto NextLine() -> std::optional<string_view_type> {
    return this->NextImpl([] [[gnu::always_inline]] (
                              const uint8_t* buf,
                              size_t available) static -> std::optional<std::pair<string_view_type, size_t>> {
      if (!available) [[unlikely]] {
        return {};
      }

      if constexpr (sizeof(char_type) > 1) {
        available = internal::AlignDown<char_type>(available);
        if (!available) [[unlikely]] {
          return {};
        }
      }

      const void* next = nullptr;
      if constexpr (!std::is_same_v<char_type, char>) {
        auto sv = string_view_type{reinterpret_cast<const char_type*>(buf),
                                   reinterpret_cast<const char_type*>(buf + available)};
        auto pos = string_view_type::npos;
        if constexpr (kDelimiter.empty()) {
          pos = sv.find(kDefaultDelimiter);
        } else if constexpr (kDelimiter.size() == 1) {
          pos = sv.find(*kDelimiter);
        } else {
          pos = sv.find(kDelimiter.data(), 0, kDelimiter.size());
        }
        if (pos != string_view_type::npos) [[likely]] {
          next = reinterpret_cast<const char_type*>(buf) + pos;
        }
      } else if constexpr (kDelimiter.empty()) {
        next = std::memchr(buf, kDefaultDelimiter, available);
      } else if constexpr (kDelimiter.size() == 1) {
        next = std::memchr(buf, *kDelimiter, available);
      } else {
        next = memmem(buf, available, kDelimiter.data(), kDelimiter.size());
      }

      if (next) [[likely]] {
        auto len = static_cast<size_t>(static_cast<const char_type*>(next) - reinterpret_cast<const char_type*>(buf));
        return std::pair{string_view_type{reinterpret_cast<const char_type*>(buf), len},
                         (len + std::max<size_t>(kDelimiter.size(), 1)) * sizeof(char_type)};
      }
      return {};
    });
  }

 private:
  static constexpr auto kDefaultDelimiter = [] consteval {
    if constexpr (std::is_same_v<char_type, char>) {
      return '\n';
    } else if constexpr (std::is_same_v<char_type, wchar_t>) {
      return L'\n';
    } else if constexpr (std::is_same_v<char_type, char8_t>) {
      return u8'\n';
    } else if constexpr (std::is_same_v<char_type, char16_t>) {
      return u'\n';
    } else if constexpr (std::is_same_v<char_type, char32_t>) {
      return U'\n';
    }
  }();

  static auto OnBufferFull(const uint8_t* buf, size_t sz) -> std::optional<string_view_type> {
    return string_view_type{reinterpret_cast<const char_type*>(buf),
                            reinterpret_cast<const char_type*>(buf + internal::AlignDown<char_type>(sz))};
  }

  static auto OnEOF(const uint8_t* buf, size_t sz) -> std::optional<string_view_type> {
    sz = internal::AlignDown<char_type>(sz);
    if (sz == 0) return {};
    return string_view_type{reinterpret_cast<const char_type*>(buf), reinterpret_cast<const char_type*>(buf + sz)};
  }

  static auto ReadFromFD(int fd, void* buf, size_t sz) { return internal::posix::read(fd, buf, sz); }

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
  internal::posix::dirent* entry;

  [[nodiscard]] auto inode() const { return entry->d_ino; }
  [[nodiscard]] auto type() const { return static_cast<DirEntryType>(entry->d_type); }

  [[nodiscard]] auto name() const {
    return std::string_view{entry->d_name,
                            strnlen(entry->d_name, entry->d_reclen - offsetof(internal::posix::dirent, d_name))};
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

class PosixDirReader {
 public:
  using value_type = DirEntry;
  using iterator = internal::Iterator<const PosixDirReader>;

  explicit PosixDirReader(DIR* dir) : dir_{dir}, owned_{false} {}

  explicit PosixDirReader(int fd) : owned_{true} {
    if (fd < 0) {
      error_ = EBADF;
      return;
    }
#ifdef __APPLE__
    if (__builtin_available(macOS 26.4, *)) {
      dir_ = fdopendir(fd);
      fd_owned_ = false;
    } else {
      dir_ = fdopendir(internal::posix::dup(fd));
    }
#else
    dir_ = fdopendir(internal::posix::dup(fd));
#endif
    if (dir_ == nullptr) [[unlikely]] {
      error_ = errno;
    }
  }

  explicit PosixDirReader(const char* pathname) : dir_{opendir(pathname)}, owned_{true} {
    if (dir_ == nullptr) [[unlikely]] {
      error_ = errno;
    }
  }

  PosixDirReader(int dirfd, const char* pathname) : owned_{true} {
    if (auto fd = internal::posix::openat(dirfd, pathname, O_DIRECTORY | O_CLOEXEC); fd >= 0) [[likely]] {
      dir_ = fdopendir(fd);
      if (dir_ == nullptr) [[unlikely]] {
        error_ = errno;
      }
    } else {
      error_ = -fd;
    }
  }

  PosixDirReader(PosixDirReader&& other) noexcept
      : dir_{std::exchange(other.dir_, nullptr)},
        error_{std::exchange(other.error_, 0)},
        owned_{other.owned_},
        fd_owned_{other.fd_owned_} {}

  auto operator=(PosixDirReader&& other) noexcept -> auto& {
    if (this != &other) {
      dir_ = std::exchange(other.dir_, nullptr);
      error_ = std::exchange(other.error_, 0);
      owned_ = other.owned_;
      fd_owned_ = other.fd_owned_;
    }
    return *this;
  }

  PosixDirReader(const PosixDirReader&) = delete;
  void operator=(const PosixDirReader&) = delete;

  [[nodiscard]] auto NextEntry() const -> std::optional<DirEntry> {
    if (auto entry = readdir(dir_)) [[likely]] {
      return DirEntry{reinterpret_cast<internal::posix::dirent*>(entry)};
    }
    return {};
  }

  [[nodiscard]] auto IsValid() const noexcept { return dir_ != nullptr; }
  [[nodiscard]] auto GetFd() const noexcept { return dirfd(dir_); }
  [[nodiscard]] auto GetError() const noexcept { return error_; }

  [[nodiscard]] auto begin() const { return iterator{this}; }
  [[nodiscard]] auto end() const { return iterator{}; }

  operator bool() const noexcept { return IsValid(); }

  auto operator++() const { return NextEntry(); }
  auto operator++(int) const { return operator++(); }

  ~PosixDirReader() {
    if (!dir_) [[unlikely]] {
      return;
    }
#ifdef __APPLE__
    if (__builtin_available(macOS 26.4, *)) {
      if (fd_owned_) [[likely]] {
        closedir(dir_);
      } else if (owned_) [[likely]] {
        fdclosedir(dir_);
      }
    } else {
      closedir(dir_);
    }
#else
    if (owned_) [[likely]] {
      closedir(dir_);
    }
#endif
  }

 private:
  DIR* dir_{};
  int error_{};
  bool owned_;
  bool fd_owned_{true};
};

#ifdef __linux__
template <internal::BufferPolicy Buffer = DefaultBuffer>
  requires(Buffer::size > offsetof(internal::posix::dirent, d_name) && Buffer::size % sizeof(uint64_t) == 0)
class DirReader : public internal::BaseReader<DirReader<Buffer>, DirEntry, Buffer> {
 public:
  using DirReader::BaseReader::BaseReader;

  explicit DirReader(int fd) : DirReader::BaseReader{fd >= 0 ? fd : -EBADF, false} {}

  explicit DirReader(const char* pathname)
      : DirReader::BaseReader{internal::posix::open(pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  DirReader(int dirfd, const char* pathname)
      : DirReader::BaseReader{internal::posix::openat(dirfd, pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  auto operator++() { return NextEntry(); }

  auto NextEntry() -> std::optional<DirEntry> {
    return this->NextImpl(
        [] [[gnu::always_inline]] (uint8_t* buf, size_t available) -> std::optional<std::pair<DirEntry, size_t>> {
          if (available < offsetof(internal::posix::dirent, d_name)) [[unlikely]] {
            return {};
          }

          auto dir = reinterpret_cast<internal::posix::dirent*>(buf);
          if (available < dir->d_reclen) [[unlikely]] {
            return {};
          }

          return std::pair{DirEntry{dir}, dir->d_reclen};
        });
  }

 private:
  static auto OnBufferFull(const uint8_t*, size_t) -> std::optional<DirEntry> { return {}; }

  static auto OnEOF(const uint8_t*, size_t) -> std::optional<DirEntry> { return {}; }

  static auto ReadFromFD(int fd, void* buf, size_t sz) {
    return internal::posix::getdents(fd, static_cast<internal::posix::dirent*>(buf), sz);
  }

  friend class DirReader::BaseReader;
};
#else
using DirReader = PosixDirReader;
#endif
}  // namespace io