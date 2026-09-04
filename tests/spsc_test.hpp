#pragma once

// Test helpers: arena construction for header + data region pairs.

#include <cstddef>
#include <cstdint>
#include <spscring/control_block.hpp>
#include <utility>

namespace spscring_test {

// Contiguous arena holding a control_block followed by a data region, aligned
// to the cache line size so the pinned control-block offsets hold.
template <std::size_t DataCapacity>
struct arena {
  static constexpr std::size_t total_size = sizeof(spscring::control_block) + DataCapacity;

  alignas(64) std::uint8_t bytes[total_size];

  spscring::control_block* header() { return reinterpret_cast<spscring::control_block*>(bytes); }

  std::uint8_t* data() { return bytes + sizeof(spscring::control_block); }

  template <typename... Args>
  bool init(Args&&... args) {
    return spscring::init_control_block(*header(), DataCapacity, std::forward<Args>(args)...);
  }
};

}  // namespace spscring_test
