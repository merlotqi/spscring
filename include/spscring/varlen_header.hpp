#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <spscring/control_block.hpp>
#include <spscring/message_meta.hpp>

namespace spscring {

// Slot layout for variable-length messages.
//
//   [ varlen_slot_header ][ payload (aligned to data_alignment) ]
//
// Total slot size is align_up(sizeof(varlen_slot_header) + payload_bytes).
// A zero size classifies wrap padding: a writer that hit the ring end without
// enough room commits a padding slot up to the ring boundary so the consumer
// skips to index 0.
struct varlen_slot_header {
  std::uint32_t slot_size{0};  // Total slot size in bytes (header + payload, aligned).
  std::uint32_t payload_size{0};
  message_meta meta;
};

inline constexpr std::uint32_t varlen_slot_header_size = static_cast<std::uint32_t>(sizeof(varlen_slot_header));

static_assert(offsetof(varlen_slot_header, meta) == 8,
              "varlen_slot_header must be tightly packed (shared-memory ABI)");

// Rounds up to the next multiple of alignment (alignment must be a power of 2).
inline std::uint32_t align_up_u32(std::uint32_t value, std::uint32_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

// Validates a control block for varlen use; also normalizes a zero alignment
// (legacy segments) to the default of 8. Returns false on structural problems.
inline bool validate_varlen_control_block(control_block& header) noexcept {
  if (!validate_control_block(header)) {
    return false;
  }
  if (header.layout_type != static_cast<std::uint32_t>(layout_type::varlen)) {
    return false;
  }
  if (header.data_alignment == 0) {
    header.data_alignment = 8;  // Legacy default.
  }
  return true;
}

}  // namespace spscring
