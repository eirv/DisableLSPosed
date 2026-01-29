#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "file_reader.h"

namespace io::proc {
static constexpr uint32_t kVmaRead = 0x01;
static constexpr uint32_t kVmaWrite = 0x02;
static constexpr uint32_t kVmaExec = 0x04;
static constexpr uint32_t kVmaShared = 0x08;
static constexpr uint32_t kVmaAllFlags = kVmaRead | kVmaWrite | kVmaExec | kVmaShared;

static constexpr uint32_t kVmaQueryFileBackedVma = 0x20;
static constexpr uint32_t kVmaAllQueryFlags = kVmaAllFlags | kVmaQueryFileBackedVma;

struct VmaEntry {
  uintptr_t vma_start;
  uintptr_t vma_end;
  uint32_t vma_flags;
  uint64_t vma_offset;
  uint32_t dev_major;
  uint32_t dev_minor;
  uint64_t inode;
  std::string_view name;

  [[nodiscard]] auto get_line() const -> std::string;

  [[nodiscard]] auto get_line(std::span<char> buffer) const -> std::string_view;
};

class MapsParser {
 public:
  using value_type = VmaEntry;
  using iterator = internal::Iterator<MapsParser>;

  explicit MapsParser(uint32_t query_flags = 0);

  MapsParser(MapsParser&& other) noexcept
      : maps_reader_{std::move(other.maps_reader_)},
        status_{other.status_},
        name_buffer_{other.name_buffer_},
        query_buffer_{other.query_buffer_} {}

  auto operator=(MapsParser&& other) noexcept -> auto& {
    if (this != &other) {
      maps_reader_ = std::move(other.maps_reader_);
      status_ = other.status_;
      name_buffer_ = other.name_buffer_;
      query_buffer_ = other.query_buffer_;
    }
    return *this;
  }

  MapsParser(const MapsParser&) = delete;
  void operator=(const MapsParser&) = delete;

  auto operator++() { return NextEntry(); }
  auto operator++(int) { return operator++(); }

  [[nodiscard]] auto IsValid() const noexcept { return maps_reader_.IsValid(); }
  operator bool() const noexcept { return IsValid(); }

  [[nodiscard]] auto begin() { return iterator{this}; }
  [[nodiscard]] auto end() { return iterator{}; }

  auto NextEntry() -> std::optional<VmaEntry>;

 private:
  enum class Status {
    kTryIoctl,
    kParseText,
    kCompleted,
  };

  FileReader<DefaultHeapBuffer> maps_reader_;
  Status status_{Status::kTryIoctl};

  std::array<char, 0x1000> name_buffer_{};
  std::array<uint64_t, 13> query_buffer_{};
};

struct SVmaEntry {
  VmaEntry base;
  std::vector<std::string_view> fields;
  std::string_view vm_flags;

  [[nodiscard]] auto get_field(std::string_view name) const -> std::optional<size_t>;

  [[nodiscard]] auto get_field_string(std::string_view name) const -> std::string_view;

  [[nodiscard]] auto has_vm_flag(std::string_view vm_flag) const -> bool;

  [[nodiscard]] auto get_lines() const -> std::string;

  [[nodiscard]] auto get_lines(std::span<char> buffer) const -> std::string_view;
};

class SMapsParser {
 public:
  using value_type = SVmaEntry;
  using iterator = internal::Iterator<SMapsParser>;

  explicit SMapsParser(uint32_t query_flags = 0);

  SMapsParser(SMapsParser&& other) noexcept
      : smaps_reader_{std::move(other.smaps_reader_)}, query_flags_{other.query_flags_} {}

  auto operator=(SMapsParser&& other) noexcept -> auto& {
    if (this != &other) {
      smaps_reader_ = std::move(other.smaps_reader_);
      query_flags_ = other.query_flags_;
    }
    return *this;
  }

  SMapsParser(const SMapsParser&) = delete;
  void operator=(const SMapsParser&) = delete;

  auto operator++() { return NextEntry(); }
  auto operator++(int) { return operator++(); }

  [[nodiscard]] auto IsValid() const noexcept { return smaps_reader_.IsValid(); }
  operator bool() const noexcept { return IsValid(); }

  [[nodiscard]] auto begin() { return iterator{this}; }
  [[nodiscard]] auto end() { return iterator{}; }

  auto NextEntry() -> std::optional<SVmaEntry>;

 private:
  FileReader<DefaultHeapBuffer> smaps_reader_;
  uint32_t query_flags_;
  bool completed_;
};

struct Field {
  static constexpr std::string_view kSize = "Size";
  static constexpr std::string_view kKernelPageSize = "KernelPageSize";
  static constexpr std::string_view kMMUPageSize = "MMUPageSize";
  static constexpr std::string_view kRss = "Rss";
  static constexpr std::string_view kPss = "Pss";
  static constexpr std::string_view kPssDirty = "Pss_Dirty";
  static constexpr std::string_view kSharedClean = "Shared_Clean";
  static constexpr std::string_view kSharedDirty = "Shared_Dirty";
  static constexpr std::string_view kPrivateClean = "Private_Clean";
  static constexpr std::string_view kPrivateDirty = "Private_Dirty";
  static constexpr std::string_view kReferenced = "Referenced";
  static constexpr std::string_view kAnonymous = "Anonymous";
  static constexpr std::string_view kLazyFree = "LazyFree";
  static constexpr std::string_view kAnonHugePages = "AnonHugePages";
  static constexpr std::string_view kShmemPmdMapped = "ShmemPmdMapped";
  static constexpr std::string_view kFilePmdMapped = "FilePmdMapped";
  static constexpr std::string_view kSharedHugetlb = "Shared_Hugetlb";
  static constexpr std::string_view kPrivateHugetlb = "Private_Hugetlb";
  static constexpr std::string_view kSwap = "Swap";
  static constexpr std::string_view kSwapPss = "SwapPss";
  static constexpr std::string_view kLocked = "Locked";
  static constexpr std::string_view kTHPeligible = "THPeligible";
};

struct VmFlag {
  static constexpr std::string_view kRead = "rd";
  static constexpr std::string_view kWrite = "wr";
  static constexpr std::string_view kExec = "ex";
  static constexpr std::string_view kShared = "sh";
  static constexpr std::string_view kMayRead = "mr";
  static constexpr std::string_view kMayWrite = "mw";
  static constexpr std::string_view kMayExec = "me";
  static constexpr std::string_view kMayShare = "ms";
  static constexpr std::string_view kGrowsDown = "gd";
  static constexpr std::string_view kPfnMap = "pf";
  static constexpr std::string_view kLocked = "lo";
  static constexpr std::string_view kIO = "io";
  static constexpr std::string_view kSeqRead = "sr";
  static constexpr std::string_view kRandRead = "rr";
  static constexpr std::string_view kDontCopy = "dc";
  static constexpr std::string_view kDontExpand = "de";
  static constexpr std::string_view kLockOnFault = "lf";
  static constexpr std::string_view kAccount = "ac";
  static constexpr std::string_view kNoReserve = "nr";
  static constexpr std::string_view kHugeTlb = "ht";
  static constexpr std::string_view kSync = "sf";
  static constexpr std::string_view kWipeOnFork = "wf";
  static constexpr std::string_view kDontDump = "dd";
  static constexpr std::string_view kMixedMap = "mm";
  static constexpr std::string_view kHugePage = "hg";
  static constexpr std::string_view kNoHugePage = "nh";
  static constexpr std::string_view kMergeable = "mg";
  static constexpr std::string_view kUffdMissing = "um";
  static constexpr std::string_view kUffdWp = "uw";
  static constexpr std::string_view kSealed = "sl";
};
}  // namespace io::proc