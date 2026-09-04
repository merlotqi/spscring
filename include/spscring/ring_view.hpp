#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <spscring/control_block.hpp>

namespace spscing {

// Non-owning view over a ring: a control block followed by a data region in one
// contiguous memory block (shared memory segment, heap arena, or stack buffer).
// Writer/reader classes derive from this view; upper layers commonly hold them
// through base-class pointers, hence the virtual destructor.
class ring_view {
 public:
  explicit ring_view(control_block* header)
      : header_(header), data_(reinterpret_cast<std::uint8_t*>(header) + header->header_size) {}

  virtual ~ring_view() = default;

  ring_view(const ring_view&) = delete;
  ring_view& operator=(const ring_view&) = delete;

  control_block* header() noexcept { return header_; }
  const control_block* header() const noexcept { return header_; }

  std::size_t capacity() const { return header_->data_capacity; }

  std::size_t used_bytes() const {
    const auto write = header_->rb_meta.write_pos.load(std::memory_order_acquire);
    const auto read = header_->rb_meta.read_pos.load(std::memory_order_acquire);
    const auto used = write >= read ? (write - read) : 0;
    const auto cap = static_cast<std::uint64_t>(header_->data_capacity);
    return static_cast<std::size_t>(used > cap ? cap : used);
  }

  std::size_t available_bytes() const { return capacity() - used_bytes(); }

  double fill_ratio() const {
    const auto cap = capacity();
    if (cap == 0) {
      return 0.0;
    }
    return static_cast<double>(used_bytes()) / static_cast<double>(cap);
  }

 protected:
  std::size_t map_pos(std::uint64_t pos) const { return static_cast<std::size_t>(pos % header_->data_capacity); }

  std::uint8_t* get_ptr(std::uint64_t pos) { return data_ + map_pos(pos); }

  const std::uint8_t* get_ptr(std::uint64_t pos) const { return data_ + map_pos(pos); }

  std::uint64_t bytes_to_end(std::uint64_t pos) const { return header_->data_capacity - map_pos(pos); }

  std::uint32_t align_size(std::uint32_t size) const noexcept {
    const std::uint32_t align = header_->data_alignment;
    return (size + align - 1) & ~(align - 1);
  }

  control_block* header_;
  std::uint8_t* data_;
};

}  // namespace spscing
