#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <spscring/atomic_backoff.hpp>
#include <spscring/control_block.hpp>
#include <spscring/internal/varlen_header.hpp>
#include <spscring/message_meta.hpp>
#include <spscring/ring_view.hpp>

namespace spscring {

// Consumer side of a variable-length SPSC ring. Pairs with varlen_writer.
//
// The consumer chases the producer's published front (commit_pos), never the
// reservation front: slots between commit_pos and write_pos are reserved but
// possibly still being written. Wrap padding slots (payload_size == 0) are
// skipped transparently; note that this makes zero-length payloads
// indistinguishable from padding, so they are never delivered to the handler.
class varlen_reader final : public ring_view {
 public:
  explicit varlen_reader(control_block* header) : ring_view(header) {}

  // Handler receives (payload pointer, payload size, message meta, reserved).
  // When it returns true the message is consumed; returning false keeps it.
  template <typename Handler>
  bool read(Handler&& handler) noexcept(noexcept(handler(std::declval<std::uint8_t*&>(), std::declval<std::uint32_t&>(),
                                                         std::declval<message_meta&>(),
                                                         std::declval<std::uint64_t&>()))) {
    control_block& hdr = *header_;
    for (;;) {
      const std::uint64_t read_pos = hdr.rb_meta.read_pos.load(std::memory_order_relaxed);
      // Chase the producer's published front, never the reservation front:
      // slots between commit_pos and write_pos are reserved but possibly
      // still being written.
      const std::uint64_t committed = hdr.rb_meta.commit_pos.load(std::memory_order_acquire);
      const std::uint64_t used = committed > read_pos ? committed - read_pos : 0;
      const std::uint64_t read_index = read_pos % hdr.data_capacity;

      auto* slot = reinterpret_cast<const varlen_slot_header*>(data_ + read_index);
      if (used == 0 || slot->slot_size == 0) {
        return false;  // Nothing published yet at this position.
      }
      if (used < slot->slot_size) {
        return false;  // Should not happen (commit covers whole slots); be safe.
      }

      if (slot->payload_size == 0) {
        // Wrap padding committed by the producer to reach the ring start:
        // skip it without consulting the handler.
        hdr.rb_meta.read_pos.store(read_pos + slot->slot_size, std::memory_order_seq_cst);
        atomic_notify_all(&hdr.rb_meta.read_wake_seq);
        continue;
      }

      const std::uint32_t payload_size = slot->payload_size;
      const message_meta meta = slot->meta;
      std::uint8_t* payload = data_ + read_index + varlen_slot_header_size;
      std::uint32_t payload_arg = payload_size;
      std::uint64_t reserved = 0;

      if (handler(payload, payload_arg, const_cast<message_meta&>(meta), reserved)) {
        hdr.rb_meta.read_pos.store(read_pos + slot->slot_size, std::memory_order_seq_cst);
        atomic_notify_all(&hdr.rb_meta.read_wake_seq);
        return true;
      }
      // Handler declined: message stays available for the next read() call.
      return false;
    }
  }

  // Copies the next message out. Returns false when the ring is empty.
  bool read(void* out, std::uint32_t max_size, std::uint32_t* out_size = nullptr, message_meta* out_meta = nullptr) {
    bool ok = read([&](std::uint8_t*& payload, std::uint32_t& size, message_meta& meta, std::uint64_t& reserved) {
      (void)reserved;
      if (out_meta != nullptr) {
        *out_meta = meta;
      }
      if (out_size != nullptr) {
        *out_size = size;
      }
      if (out != nullptr && size > 0) {
        if (size > max_size) {
          return false;  // Caller buffer too small: keep the message.
        }
        std::memcpy(out, payload, size);
      }
      return true;
    });
    return ok;
  }

  // Blocking read with exponential backoff (pause -> yield -> futex).
  template <typename Handler>
  bool spin_until_read(Handler&& handler, atomic_backoff& backoff) {
    for (;;) {
      if (read(std::forward<Handler>(handler))) {
        return true;
      }
      backoff.pause();
    }
  }

  std::size_t next_payload_size() const noexcept {
    const std::uint64_t read_pos = header_->rb_meta.read_pos.load(std::memory_order_relaxed);
    if (header_->rb_meta.commit_pos.load(std::memory_order_acquire) <= read_pos) {
      return 0;  // Ring empty.
    }
    const std::uint64_t read_index = read_pos % header_->data_capacity;
    auto* slot = reinterpret_cast<const varlen_slot_header*>(data_ + read_index);
    return slot->payload_size;
  }
};

}  // namespace spscring
