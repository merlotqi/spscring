#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <spscring/atomic_backoff.hpp>
#include <spscring/control_block.hpp>
#include <spscring/ring_view.hpp>

namespace spscring {

// Consumer side of a fixed-size SPSC ring. Pairs with fixed_writer; reads are
// non-copying (pointer + size view into the ring), valid until read_advance().
class fixed_reader final : public ring_view {
 public:
  explicit fixed_reader(control_block* header) : ring_view(header) {}

  // Returns a pointer to the oldest unconsumed slot, or nullptr when empty.
  // The view stays valid until read_advance() publishes the consumption.
  //
  // Readiness is gated on commit_pos (the producer's published front), never
  // on write_pos (the reservation front): a reserved-but-unfinished slot is
  // not observable here.
  const void* try_read(std::uint32_t* out_size = nullptr) noexcept {
    control_block& hdr = *header_;
    const std::uint64_t item_size = hdr.fixed_item_size;
    const std::uint64_t capacity = hdr.data_capacity;
    const std::uint64_t read = hdr.rb_meta.read_pos.load(std::memory_order_relaxed);
    const std::uint64_t committed = hdr.rb_meta.commit_pos.load(std::memory_order_acquire);

    if (committed <= read) {
      return nullptr;  // Nothing published yet.
    }

    if (out_size != nullptr) {
      *out_size = static_cast<std::uint32_t>(item_size);
    }
    return data_ + (read % capacity);
  }

  // Blocking read with exponential backoff (pause -> yield -> futex).
  const void* spin_until_read(std::uint32_t* out_size, atomic_backoff& backoff) noexcept {
    for (;;) {
      if (const void* slot = try_read(out_size)) {
        return slot;
      }
      backoff.pause();
    }
  }

  // Publishes consumption of n slots; wakes producers blocked on space.
  void read_advance(std::size_t n = 1) noexcept {
    control_block& hdr = *header_;
    hdr.rb_meta.read_pos.fetch_add(static_cast<std::uint64_t>(n) * hdr.fixed_item_size, std::memory_order_seq_cst);
    // Bump the producer wakeup word so producers waiting on it (futex /
    // WaitOnAddress compare the value, they do not loop on the notify alone)
    // escape, then wake them.
    hdr.rb_meta.read_wake_seq.fetch_add(1, std::memory_order_seq_cst);
    atomic_notify_all(&hdr.rb_meta.read_wake_seq);
  }

  // Copies out the oldest slot; returns false when the ring is empty.
  bool read(void* out, std::uint32_t size) noexcept {
    const void* slot = try_read(nullptr);
    if (slot == nullptr) {
      return false;
    }
    std::memcpy(out, slot, size);
    read_advance(1);
    return true;
  }

  std::size_t item_size() const noexcept { return static_cast<std::size_t>(header_->fixed_item_size); }
};

}  // namespace spscring
