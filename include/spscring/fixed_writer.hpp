#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <spscring/atomic_backoff.hpp>
#include <spscring/control_block.hpp>
#include <spscring/ring_view.hpp>

namespace spscing {

// Producer side of a fixed-size SPSC ring. Every message occupies exactly
// `item_size` bytes, so wraps are handled with a dummy write and the fast path
// needs only one CAS.
//
// Requirements (enforced in the constructor / init_control_block):
//   - item_size > 0, data_alignment power of two in [1, 64]
//   - data_capacity % item_size == 0, i.e. the item count is an exact integer,
//     otherwise contiguous slot placement breaks at the wrap point
//
// Storage is owned by the caller; a control_block + data region pair.
class fixed_writer final : public ring_view {
 public:
  explicit fixed_writer(control_block* header) : ring_view(header) {
    // data_capacity must be an exact multiple of item_size.
    if (header_->fixed_item_size != 0 && header_->data_capacity % header_->fixed_item_size != 0) {
      std::abort();  // Misconfigured ring: cannot maintain slot invariants.
    }
  }

  // Reserves the next slot. Returns nullptr when the ring is full; the
  // spin_until_reserve overload blocks with exponential backoff instead.
  void* try_reserve() noexcept {
    control_block& hdr = *header_;
    const std::uint64_t item_size = hdr.fixed_item_size;
    const std::uint64_t capacity = hdr.data_capacity;
    const std::uint64_t write = hdr.rb_meta.write_pos.load(std::memory_order_relaxed);
    const std::uint64_t read = hdr.rb_meta.read_pos.load(std::memory_order_acquire);

    const std::uint64_t write_index = write % capacity;
    const std::uint64_t read_index = read % capacity;
    if (write_index == read_index && write > read) {
      return nullptr;  // Ring is full.
    }

    hdr.rb_meta.write_pos.store(write + item_size, std::memory_order_relaxed);
    return data_ + write_index;
  }

  // Blocking reserve with exponential backoff (pause -> yield -> futex).
  void* spin_until_reserve(atomic_backoff& backoff) noexcept {
    for (;;) {
      if (void* slot = try_reserve()) {
        return slot;
      }
      backoff.pause();
    }
  }

  // Publishes a reserved slot: seq-cst release so consumers observing
  // commit_seq also observe the payload writes.
  void commit() noexcept {
    control_block& hdr = *header_;
    hdr.rb_meta.commit_seq.fetch_add(1, std::memory_order_seq_cst);
    atomic_notify_all(&hdr.rb_meta.commit_seq);
  }

  // Fills the next slot and publishes it. Returns false when the ring is full.
  bool write(const void* data, std::uint32_t size) noexcept {
    void* slot = try_reserve();
    if (slot == nullptr) {
      return false;
    }
    if (size > 0) {
      __builtin_memcpy(slot, data, size);
    }
    commit();
    return true;
  }

  std::size_t item_size() const noexcept { return static_cast<std::size_t>(header_->fixed_item_size); }

  std::size_t item_count() const noexcept {
    return static_cast<std::size_t>(header_->data_capacity / header_->fixed_item_size);
  }
};

}  // namespace spscing
