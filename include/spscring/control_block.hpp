#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <spscring/platform.hpp>

namespace spscing {

// 0: fixed-size frames, 1: variable-length messages.
enum class layout_type : std::uint32_t {
  fixed = 0,
  varlen = 1,
};

// SPSC ring indices and wait words. write_pos/read_pos are monotonic logical
// byte offsets (uint64: they never wrap in practice); the data region index is
// pos % data_capacity.
//
//   write_pos     - advanced by the producer's compare-and-swap in try_reserve.
//   read_pos      - advanced by the consumer after a message is consumed.
//   commit_seq    - bumped by the producer after each message becomes readable
//                   (32-bit: Linux futex compare value). Wait word for consumers.
//   read_wake_seq - bumped by the consumer whenever read_pos advances (including
//                   wrap padding); wakes producers blocked on space.
//
// Each counter occupies its own cache line; wait words must stay 32-bit so the
// futex path can block on them directly.
struct meta {
  SPSCRING_ALIGNAS_CACHE_LINE std::atomic<std::uint64_t> write_pos{0};
  std::uint8_t padding1[SPSCRING_CACHE_LINE_SIZE - sizeof(std::atomic<std::uint64_t>)];

  SPSCRING_ALIGNAS_CACHE_LINE std::atomic<std::uint64_t> read_pos{0};
  std::uint8_t padding2[SPSCRING_CACHE_LINE_SIZE - sizeof(std::atomic<std::uint64_t>)];

  SPSCRING_ALIGNAS_CACHE_LINE std::atomic<std::uint32_t> commit_seq{0};
  std::uint8_t padding3[SPSCRING_CACHE_LINE_SIZE - sizeof(std::atomic<std::uint32_t>)];

  SPSCRING_ALIGNAS_CACHE_LINE std::atomic<std::uint32_t> read_wake_seq{0};
  std::uint8_t padding4[SPSCRING_CACHE_LINE_SIZE - sizeof(std::atomic<std::uint32_t>)];
};

// The control block is the cross-process ABI: it lives at the start of a shared
// memory segment (or arena) and must stay standard-layout and trivially
// copyable. The data region begins at header + header_size.
struct SPSCRING_ALIGNAS_CACHE_LINE control_block {
  std::uint32_t magic{0};
  std::uint16_t version_major{0};
  std::uint32_t version_minor{0};
  std::uint32_t header_size{0};
  std::uint32_t layout_type{0};  // layout_type::fixed / layout_type::varlen
  std::uint32_t reserved0{0};

  meta rb_meta;

  std::uint64_t data_capacity{0};
  std::uint32_t data_alignment{0};
  std::uint32_t fixed_item_size{0};  // informational for fixed rings; 0 otherwise
  std::uint64_t reserved[6]{0, 0, 0, 0, 0, 0};
};

inline constexpr std::uint32_t expected_magic = 0x53505343;  // "SPSC"
inline constexpr std::uint16_t version_major = 0;
inline constexpr std::uint32_t version_minor = 1;

// The data region starts at header + sizeof(control_block), which is a 64-byte
// boundary; slot alignment above 64 bytes is therefore not supported.
inline bool is_valid_data_alignment(std::uint32_t alignment) noexcept {
  return alignment != 0 && (alignment & (alignment - 1)) == 0 && alignment <= 64;
}

// Validates the geometry and (re)initializes a control block in place.
// Returns false when the geometry is invalid:
//   - data_alignment must be a power of two in [1, 64]
//   - data_capacity must be non-zero and a multiple of data_alignment
inline bool init_control_block(control_block& header, std::uint64_t data_capacity, layout_type layout,
                               std::uint32_t data_alignment, std::uint32_t fixed_item_size = 0) noexcept {
  if (!is_valid_data_alignment(data_alignment)) {
    return false;
  }
  if (data_capacity == 0 || data_capacity % data_alignment != 0) {
    return false;
  }

  header.magic = expected_magic;
  header.version_major = version_major;
  header.version_minor = version_minor;
  header.header_size = sizeof(control_block);
  header.layout_type = static_cast<std::uint32_t>(layout);
  header.reserved0 = 0;
  header.rb_meta.write_pos.store(0, std::memory_order_relaxed);
  header.rb_meta.read_pos.store(0, std::memory_order_relaxed);
  header.rb_meta.commit_seq.store(0, std::memory_order_relaxed);
  header.rb_meta.read_wake_seq.store(0, std::memory_order_relaxed);
  header.data_capacity = data_capacity;
  header.data_alignment = data_alignment;
  header.fixed_item_size = fixed_item_size;
  for (std::uint64_t& value : header.reserved) {
    value = 0;
  }
  return true;
}

// Structural sanity check for a control block found in mapped memory. Peer
// processes that expect a specific capacity/layout/item size should compare
// those fields themselves after this check passes.
inline bool validate_control_block(const control_block& header) noexcept {
  return header.magic == expected_magic && header.version_major == version_major &&
         header.header_size == sizeof(control_block) && is_valid_data_alignment(header.data_alignment) &&
         header.data_capacity != 0 && header.data_capacity % header.data_alignment == 0;
}

// ABI pins: changing any of these is a breaking change for mapped segments and
// requires bumping version_major/minor.
static_assert(std::is_standard_layout_v<control_block>,
              "control_block must stay standard-layout (shared-memory ABI)");
static_assert(std::is_trivially_copyable_v<control_block>,
              "control_block must stay trivially copyable (shared-memory ABI)");
static_assert(sizeof(meta) == 4 * SPSCRING_CACHE_LINE_SIZE, "meta must be exactly four cache lines");
static_assert(offsetof(control_block, rb_meta) == SPSCRING_CACHE_LINE_SIZE,
              "rb_meta must start on the second cache line");
static_assert(offsetof(control_block, data_capacity) == 5 * SPSCRING_CACHE_LINE_SIZE,
              "data_capacity must start on the sixth cache line");
static_assert(sizeof(control_block) == 6 * SPSCRING_CACHE_LINE_SIZE,
              "the data region must start on a 64-byte boundary");

}  // namespace spscing
