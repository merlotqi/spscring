// spscing basic usage: fixed ring + varlen ring on plain heap storage.
//
// The same code works unchanged when the arena lives in a POSIX shm segment or
// a Windows file mapping — only the storage acquisition differs.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <spscring/spscring.hpp>
#include <string>

namespace {

// Any contiguous buffer works: heap, stack, or a shared memory mapping.
// control_block is alignas(64), so the arena base must be 64-byte aligned —
// plain new[] for std::byte only guarantees __STDCPP_DEFAULT_NEW_ALIGNMENT__.
struct arena_t {
  std::byte* const bytes;

  explicit arena_t(std::size_t data_capacity)
      : bytes(static_cast<std::byte*>(
            ::operator new(sizeof(spscring::control_block) + data_capacity, std::align_val_t(64)))) {}

  ~arena_t() { ::operator delete(bytes, std::align_val_t(64)); }

  arena_t(const arena_t&) = delete;
  arena_t& operator=(const arena_t&) = delete;

  spscring::control_block* header() { return reinterpret_cast<spscring::control_block*>(bytes); }
};

void fixed_ring_demo() {
  constexpr std::uint32_t kItemSize = 64;
  constexpr std::uint32_t kItemCount = 8;
  arena_t arena(kItemSize * kItemCount);

  if (!spscring::init_control_block(*arena.header(), kItemSize * kItemCount, spscring::layout_type::fixed, 64,
                                    kItemSize)) {
    std::cerr << "invalid ring geometry\n";
    return;
  }

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  const char* sent = "hello from the fixed ring";
  if (!writer.write(sent, kItemSize)) {
    std::cerr << "ring full\n";
    return;
  }

  std::uint32_t size = 0;
  if (const void* slot = reader.try_read(&size)) {
    std::string received(static_cast<const char*>(slot), size);
    reader.read_advance(1);
    std::cout << "fixed ring received: " << received.c_str() << "\n";
  }
}

void varlen_ring_demo() {
  constexpr std::uint32_t kCapacity = 512;
  arena_t arena(kCapacity);

  if (!spscring::init_control_block(*arena.header(), kCapacity, spscring::layout_type::varlen, 8)) {
    std::cerr << "invalid ring geometry\n";
    return;
  }

  spscring::varlen_writer writer{arena.header()};
  spscring::varlen_reader reader{arena.header()};

  const std::string sent = "hello from the varlen ring";
  spscring::message_meta meta{};
  meta.user_data = 1;
  if (!writer.write(static_cast<std::uint32_t>(sent.size()), sent.data(), meta)) {
    std::cerr << "ring full\n";
    return;
  }

  char buffer[128];
  std::uint32_t received_size = 0;
  spscring::message_meta received_meta{};
  if (reader.read(buffer, sizeof(buffer), &received_size, &received_meta)) {
    std::cout << "varlen ring received: " << std::string(buffer, received_size)
              << " (user_data=" << received_meta.user_data << ")\n";
  }
}

}  // namespace

int main() {
  fixed_ring_demo();
  varlen_ring_demo();
  return 0;
}
