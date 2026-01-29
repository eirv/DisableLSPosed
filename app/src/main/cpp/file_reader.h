#pragma once

#include <dirent.h>
#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <concepts>
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
struct FixedString {
  using value_type = T;

  consteval FixedString() = default;

  consteval FixedString(const value_type (&data)[N]) { std::copy_n(data, N - 1, data_); }

  consteval auto operator*() const { return operator[](0); }

  consteval auto operator[](size_t index) const {
    static_assert(N > 1);
    return data_[index];
  }

  [[nodiscard]] consteval auto data() const { return data_; }

  [[nodiscard]] consteval auto size() const {
    if constexpr (N == 0) {
      return 0;
    } else {
      return N - 1;
    }
  }

  [[nodiscard]] consteval auto empty() const -> bool { return size() == 0; }

  value_type data_[N != 0 ? N - 1 : N]{};
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

  BaseReader(int fd, bool owned)
      : fd_{fd}, owned_{owned}, buffer_{Buffer::template make_buffer<kBufferSize + kReservedBytes>()} {}

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

  [[nodiscard]] auto IsValid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] auto GetFd() const noexcept { return fd_; }

  void Reduce() {
    auto rem = buf_end_ - buf_pos_;
    memmove(&buffer_[0], &buffer_[buf_pos_], rem);
    buf_end_ = rem;
    buf_pos_ = 0;
  }

  [[nodiscard]] auto begin() { return iterator{static_cast<Derived*>(this)}; }
  [[nodiscard]] auto end() { return iterator{}; }

 protected:
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
    if constexpr (is_string_view_v<value_type>) {
      return AlignUp<void*>(kBufferSize + sizeof(typename value_type::value_type)) - kBufferSize;
    } else {
      return 0;
    }
  }();

  int fd_;
  bool owned_;
  bool eof_{};
  size_t buf_pos_{};
  size_t buf_end_{};
  Buffer::template type<kBufferSize + kReservedBytes> buffer_;
};
}  // namespace internal

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
  template <size_t kBufferSize = kDefaultBufferSize>
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
      if (base_) raw_munmap(base_, kDefaultBufferSize);
      base_ = std::exchange(other.base_, nullptr);
    }
    return *this;
  }

  MMapBuffer() {
    auto base = raw_mmap(nullptr, kDefaultBufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reinterpret_cast<uintptr_t>(base) < -4095UL) [[likely]] {
      base_ = static_cast<uint8_t*>(base);
    }
  }

  ~MMapBuffer() {
    if (base_) [[likely]] {
      raw_munmap(base_, kDefaultBufferSize);
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
  static constexpr auto LF = internal::FixedString<char>{};
  static constexpr auto CR = internal::FixedString{"\r"};
  static constexpr auto CRLF = internal::FixedString{"\r\n"};
  static constexpr auto SPACE = internal::FixedString{" "};
};

struct UTF16 {
  static constexpr auto LF = internal::FixedString<char16_t>{};
  static constexpr auto CR = internal::FixedString{u"\r"};
  static constexpr auto CRLF = internal::FixedString{u"\r\n"};
  static constexpr auto SPACE = internal::FixedString{u" "};
};

struct UTF32 {
  static constexpr auto LF = internal::FixedString<char32_t>{};
  static constexpr auto CR = internal::FixedString{U"\r"};
  static constexpr auto CRLF = internal::FixedString{U"\r\n"};
  static constexpr auto SPACE = internal::FixedString{U" "};
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
 * for (auto line : FileReader<DefaultStackBuffer, UTF8::SPACE>{"..."}) {}
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
template <internal::BufferPolicy Buffer = DefaultBuffer, internal::FixedString kDelimiter = UTF8::LF>
  requires(Buffer::size % sizeof(typename decltype(kDelimiter)::value_type) == 0)
class FileReader : public internal::BaseReader<FileReader<Buffer, kDelimiter>,
                                               std::basic_string_view<typename decltype(kDelimiter)::value_type>,
                                               Buffer> {
 public:
  using char_type = decltype(kDelimiter)::value_type;
  using string_view_type = std::basic_string_view<char_type>;

  explicit FileReader(int fd) : FileReader::BaseReader{fd, false} {}

  explicit FileReader(const char* pathname) : FileReader::BaseReader{raw_open(pathname, O_RDONLY | O_CLOEXEC), true} {}

  FileReader(int dirfd, const char* pathname)
      : FileReader::BaseReader{raw_openat(dirfd, pathname, O_RDONLY | O_CLOEXEC), true} {}

  auto operator++() { return NextLine(); }

  auto NextLine() -> std::optional<string_view_type> {
    return this->NextImpl([] [[gnu::always_inline]] (
                              const uint8_t* buf,
                              size_t available) -> std::optional<std::pair<string_view_type, size_t>> {
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
          pos = sv.find(GetDefaultDelimiter());
        } else if constexpr (kDelimiter.size() == 1) {
          pos = sv.find(*kDelimiter);
        } else {
          pos = sv.find(kDelimiter.data(), 0, kDelimiter.size());
        }
        if (pos != string_view_type::npos) [[likely]] {
          next = reinterpret_cast<const char_type*>(buf) + pos;
        }
      } else if constexpr (kDelimiter.empty()) {
        next = memchr(buf, GetDefaultDelimiter(), available);
      } else if constexpr (kDelimiter.size() == 1) {
        next = memchr(buf, *kDelimiter, available);
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
  static auto OnBufferFull(const uint8_t* buf, size_t sz) -> std::optional<string_view_type> {
    return string_view_type{reinterpret_cast<const char_type*>(buf),
                            reinterpret_cast<const char_type*>(buf + internal::AlignDown<char_type>(sz))};
  }

  static auto OnEOF(const uint8_t* buf, size_t sz) -> std::optional<string_view_type> {
    sz = internal::AlignDown<char_type>(sz);
    if (sz == 0) return {};
    return string_view_type{reinterpret_cast<const char_type*>(buf), reinterpret_cast<const char_type*>(buf + sz)};
  }

  static auto ReadFromFD(int fd, void* buf, size_t sz) { return raw_read(fd, buf, sz); }

  static consteval auto GetDefaultDelimiter() {
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
  }

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

template <internal::BufferPolicy Buffer = DefaultBuffer>
  requires(Buffer::size > offsetof(kernel_dirent64, d_name) && Buffer::size % sizeof(uint64_t) == 0)
class DirReader : public internal::BaseReader<DirReader<Buffer>, DirEntry, Buffer> {
 public:
  explicit DirReader(int fd) : DirReader::BaseReader{fd, false} {}

  explicit DirReader(const char* pathname) : DirReader::BaseReader{raw_open(pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  DirReader(int dirfd, const char* pathname)
      : DirReader::BaseReader{raw_openat(dirfd, pathname, O_DIRECTORY | O_CLOEXEC), true} {}

  auto operator++() { return NextEntry(); }

  auto NextEntry() -> std::optional<DirEntry> {
    return this->NextImpl(
        [] [[gnu::always_inline]] (uint8_t* buf, size_t available) -> std::optional<std::pair<DirEntry, size_t>> {
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
  static auto OnBufferFull(const uint8_t*, size_t) -> std::optional<DirEntry> { return {}; }

  static auto OnEOF(const uint8_t*, size_t) -> std::optional<DirEntry> { return {}; }

  static auto ReadFromFD(int fd, void* buf, size_t sz) {
    return raw_getdents64(fd, static_cast<kernel_dirent64*>(buf), static_cast<int>(sz));
  }

  friend class DirReader::BaseReader;
};
}  // namespace io