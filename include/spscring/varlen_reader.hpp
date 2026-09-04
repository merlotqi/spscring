#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <spscring/atomic_backoff.hpp>
#include <spscring/control_block.hpp>
#include <spscring/message_meta.hpp>
#include <spscring/ring_view.hpp>
#include <spscring/varlen_header.hpp>

namespace spscring {

// Consumer side of a variable-length SPSC ring. Pairs with varlen_writer.
//
// The consumer chases the producer's commit_seq: a slot is readable once the
// writer bumped the counter after filling it. Wrap padding slots (zero payload
// size, slot_size == bytes to ring end) are skipped transparently.
class varlen_reader final : public ring_view {
 public:
  explicit varlen_reader(control_block* header) : ring_view(header) {}

  // Handler receives (payload pointer, payload size, message meta, reserved).
  // When it returns true the message is consumed; returning false keeps it.
  template <typename Handler>
  bool read(Handler&& handler) noexcept(noexcept(handler(std::declval<std::uint8_t*&>(),
                                                         std::declval<std::uint32_t&>(),
                                                         std::declval<message_meta&>(),
                                                         std::declval<std::uint64_t&>()))) {
    control_block& hdr = *header_;
    for (;;) {
      const std::uint64_t read_pos = hdr.rb_meta.read_pos.load(std::memory_order_relaxed);
      const std::uint32_t commit_count = hdr.rb_meta.commit_seq.load(std::memory_order_acquire);
      const std::uint64_t read_index = read_pos % hdr.data_capacity;

      auto* slot = reinterpret_cast<const varlen_slot_header*>(data_ + read_index);
      const std::uint64_t write_pos = hdr.rb_meta.write_pos.load(std::memory_order_acquire);
      const std::uint64_t used = write_pos > read_pos ? write_pos - read_pos : 0;
      if (used == 0 || slot->slot_size == 0) {
        return false;  // Nothing reserved yet at this position.
      }
      if (used < slot->slot_size) {
        return false;  // Producer still filling the slot.
      }
      if (commit_count == 0) {
        return false;  // No committed messages at all.
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
        __builtin_memcpy(out, payload, size);
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
    const std::uint64_t read_index = read_pos % header_->data_capacity;
    auto* slot = reinterpret_cast<const varlen_slot_header*>(data_ + read_index);
    return slot->payload_size;
  }
};

}  // namespace spscring
