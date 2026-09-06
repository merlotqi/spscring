#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <spscring/atomic_backoff.hpp>
#include <spscring/control_block.hpp>
#include <spscring/internal/varlen_header.hpp>
#include <spscring/message_meta.hpp>
#include <spscring/reserve_result.hpp>
#include <spscring/ring_view.hpp>

namespace spscring {

// Producer side of a variable-length SPSC ring. Two-phase publication:
//
//   1. reserve:   CAS-advance write_pos from head to head + slot_size and
//                 write the payload + header
//   2. commit:    bump commit_seq, making the slot visible to the consumer
//
// Because every slot is self-describing (slot_size lives in the header), the
// consumer can chase the producer across the wrap without per-position checks.
class varlen_writer final : public ring_view {
 public:
  explicit varlen_writer(control_block* header) : ring_view(header) {}

  // Reserves room for payload_size bytes. Returns reserve_status::full when
  // fewer than header+payload bytes are available anywhere in the ring.
  reserve_result try_reserve(std::uint32_t payload_size, const message_meta& meta = {}) noexcept {
    const std::uint32_t align = header_->data_alignment;
    const std::uint32_t needed = align_up_u32(varlen_slot_header_size + payload_size, align);
    if (needed > header_->data_capacity) {
      return {reserve_status::message_too_large, nullptr, 0};
    }

    control_block& hdr = *header_;
    std::uint64_t head = hdr.rb_meta.write_pos.load(std::memory_order_relaxed);

    for (;;) {
      const std::uint64_t read = hdr.rb_meta.read_pos.load(std::memory_order_acquire);
      const std::uint64_t used = head - read;
      if (used + needed > hdr.data_capacity) {
        return {reserve_status::full, nullptr, 0};
      }

      const std::uint64_t head_index = head % hdr.data_capacity;
      const std::uint64_t wrap_padding = hdr.data_capacity - head_index;

      if (wrap_padding < needed) {
        // Not enough contiguous room before the ring end: commit a dummy
        // header that skips the consumer to index 0, then retry from there.
        const std::uint64_t new_head = head + wrap_padding;
        if (hdr.rb_meta.write_pos.compare_exchange_strong(head, new_head, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
          auto* dummy = reinterpret_cast<varlen_slot_header*>(data_ + head_index);
          *dummy = varlen_slot_header{static_cast<std::uint32_t>(wrap_padding), 0, message_meta{}};
          // Publish the padding slot immediately (nothing else is in flight
          // between reserve and commit within this call).
          hdr.rb_meta.commit_pos.store(new_head, std::memory_order_release);
          hdr.rb_meta.commit_seq.fetch_add(1, std::memory_order_seq_cst);
          atomic_notify_all(&hdr.rb_meta.commit_seq);
          head = new_head;
          continue;  // Re-check capacity from the start; the CAS consumed space.
        }
        backoff_.pause();
        head = hdr.rb_meta.write_pos.load(std::memory_order_relaxed);
        continue;
      }

      const std::uint64_t new_head = head + needed;
      if (!hdr.rb_meta.write_pos.compare_exchange_strong(head, new_head, std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
        backoff_.pause();
        continue;
      }

      auto* slot = reinterpret_cast<varlen_slot_header*>(data_ + head_index);
      *slot = varlen_slot_header{needed, payload_size, meta};

      return {reserve_status::ok, data_ + head_index + varlen_slot_header_size, head};
    }
  }

  // Two-step variant: reserve, then copy the payload into the returned pointer.
  reserve_result reserve_and_copy(std::uint32_t payload_size, const void* data, const message_meta& meta = {}) {
    reserve_result result = try_reserve(payload_size, meta);
    if (result.status == reserve_status::ok && payload_size > 0 && data != nullptr) {
      std::memcpy(result.payload, data, payload_size);
    }
    return result;
  }

  // Makes the slot at `position` (from a successful try_reserve) visible by
  // advancing commit_pos (release) to the end of that slot, then waking
  // consumers via the 32-bit notify counter.
  void commit(std::uint64_t position) noexcept {
    control_block& hdr = *header_;
    // The slot header at `position` was written by this same producer thread,
    // so a relaxed load of its size is safe. commit_pos publishes both the
    // header and the payload writes to the consumer (acquire side).
    const auto* slot = reinterpret_cast<const varlen_slot_header*>(data_ + (position % hdr.data_capacity));
    hdr.rb_meta.commit_pos.store(position + slot->slot_size, std::memory_order_release);
    hdr.rb_meta.commit_seq.fetch_add(1, std::memory_order_seq_cst);
    atomic_notify_all(&hdr.rb_meta.commit_seq);
  }

  // Single-shot convenience: reserve + copy + commit.
  bool write(std::uint32_t payload_size, const void* data, const message_meta& meta = {}) {
    reserve_result result = reserve_and_copy(payload_size, data, meta);
    if (result.status != reserve_status::ok) {
      return false;
    }
    commit(result.position);
    return true;
  }

 private:
  atomic_backoff backoff_;
};

}  // namespace spscring
