#pragma once

#include <cstdint>

namespace spscring {

// Per-message metadata stored in each slot header alongside the payload.
struct message_meta {
  std::uint64_t user_data{0};
  std::uint64_t timestamp_ns{0};
  std::uint32_t schema_id{0};
  std::uint32_t flags{0};
};

enum message_flags : std::uint32_t {
  flag_none = 0,
  flag_priority_high = 1u << 0,
  flag_compressed = 1u << 1,
  flag_response = 1u << 2,
  flag_cancel = 1u << 3,
};

static_assert(sizeof(message_meta) == 24, "message_meta layout is part of the shared-memory ABI");

}  // namespace spscring
