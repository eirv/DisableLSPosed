#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

  auto get_line() const -> std::string;
  auto get_line(std::span<char> buffer) const -> std::string_view;
};

class MapsParser {
 public:
  using value_type = VmaEntry;
  using iterator = internal::Iterator<MapsParser>;

  explicit MapsParser(uint32_t query_flags = 0);

  MapsParser(MapsParser&& other) noexcept
      : maps_reader_{std::move(other.maps_reader_)},
        status_{other.status_},
        name_buffer_{std::move(other.name_buffer_)},
        query_buffer_{std::move(other.query_buffer_)} {}

  auto operator=(MapsParser&& other) noexcept -> auto& {
    if (this != &other) {
      maps_reader_ = std::move(other.maps_reader_);
      status_ = other.status_;
      name_buffer_ = std::move(other.name_buffer_);
      query_buffer_ = std::move(other.query_buffer_);
    }
    return *this;
  }

  MapsParser(const MapsParser&) = delete;
  void operator=(const MapsParser&) = delete;

  auto operator++() { return NextEntry(); }
  auto operator++(int) { return operator++(); }

  auto IsValid() const noexcept { return maps_reader_.IsValid(); }
  operator bool() const noexcept { return IsValid(); }

  auto begin() { return iterator{this}; }
  auto end() { return iterator{}; }

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

}  // namespace io::proc
