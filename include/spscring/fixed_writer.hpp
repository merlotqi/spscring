#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <spscring/atomic_backoff.hpp>
#include <spscring/atomic_wait.hpp>
#include <spscring/control_block.hpp>
#include <spscring/notify_word.hpp>
#include <spscring/ring_view.hpp>

namespace spscring {

// Structural validation for a fixed ring's control block: fixed layout, a
// non-zero item size, and a capacity that is an exact multiple of the item
// size (init_control_block only checks alignment, not the item multiple).
// Call this before constructing a fixed_writer; the constructor aborts on a
// misconfigured ring because it cannot maintain the slot invariant otherwise.
inline bool is_valid_fixed_ring(const control_block& header) noexcept {
  return validate_control_block(header) && header.layout_type == static_cast<std::uint32_t>(layout_type::fixed) &&
         header.fixed_item_size != 0 && header.data_capacity % header.fixed_item_size == 0;
}

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

  // Reserves the next slot. Returns nullptr when the ring is full — or when a
  // previously reserved slot is still uncommitted (misuse guard: a second
  // outstanding reservation would overwrite reserved_end_ and corrupt the
  // first publication; commit() the pending slot first). The reservation is
  // invisible to the consumer until commit() publishes it.
  void* try_reserve() noexcept {
    if (pending_) {
      return nullptr;
    }
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

    reserved_end_ = write + item_size;
    hdr.rb_meta.write_pos.store(reserved_end_, std::memory_order_relaxed);
    pending_ = true;
    return data_ + write_index;
  }

  // Blocking reserve with exponential backoff (pause -> yield -> futex).
  // Misuse note: calling this while a reservation is already pending spins
  // forever (try_reserve refuses); commit the pending slot first.
  void* spin_until_reserve(atomic_backoff& backoff) noexcept {
    for (;;) {
      if (void* slot = try_reserve()) {
        return slot;
      }
      backoff.pause();
    }
  }

  // Publishes the slot filled after the last try_reserve: advances commit_pos
  // (release) so consumers chasing commit_pos observe the payload writes.
  // Wakes blocked consumers only when one is registered on the notify word
  // (see notify_word.hpp). Must be called once per successful reserve, in
  // reserve order; a no-op when no reservation is pending.
  void commit() noexcept {
    if (!pending_) {
      return;
    }
    control_block& hdr = *header_;
    hdr.rb_meta.commit_pos.store(reserved_end_, std::memory_order_release);
    pending_ = false;
    if (bump_notify_word(hdr.rb_meta.commit_seq)) {
      atomic_notify_all(&hdr.rb_meta.commit_seq);
    }
  }

  // Fills the next slot and publishes it. Returns false when the ring is full.
  bool write(const void* data, std::uint32_t size) noexcept {
    void* slot = try_reserve();
    if (slot == nullptr) {
      return false;
    }
    if (size > 0) {
      std::memcpy(slot, data, size);
    }
    commit();
    return true;
  }

  std::size_t item_size() const noexcept { return static_cast<std::size_t>(header_->fixed_item_size); }

  std::size_t item_count() const noexcept {
    return static_cast<std::size_t>(header_->data_capacity / header_->fixed_item_size);
  }

 private:
  // End position of the slot filled after the last try_reserve; published by
  // commit().
  std::uint64_t reserved_end_{0};
  // True between a successful try_reserve() and its commit().
  bool pending_{false};
};

}  // namespace spscring
