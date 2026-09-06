// In-process thread-to-thread messaging with spscring.
//
// spscring is shared-memory-first, but the sync state lives entirely in the
// control block — the ring only needs "one contiguous buffer holding a
// control_block followed by a data region". When both ends are threads of the
// same process that buffer is a plain, 64-byte-aligned heap allocation: no
// shm_open, no file mapping, no handle handoff. Swap the arena below for an
// mmap / CreateFileMapping segment and the same writer/reader code runs
// unchanged across processes.
//
// Covers:
//   - fixed ring: constant-size frames, zero-copy both ways
//     (producer try_reserve/commit, consumer try_read view)
//   - varlen ring: variable-length messages + message_meta
//   - full/empty handling with futex-style wait on the notify words
//     (commit_seq / read_wake_seq), instead of burning CPU forever

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <spscring/spscring.hpp>
#include <thread>

namespace {

// control_block is alignas(64). Plain new[] for std::byte only guarantees
// __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16 bytes), so allocate explicitly
// 64-byte aligned.
struct aligned_arena {
  std::byte* const base;
  const std::size_t total;

  explicit aligned_arena(std::size_t data_capacity)
      : base(static_cast<std::byte*>(
            ::operator new(sizeof(spscring::control_block) + data_capacity, std::align_val_t(64)))),
        total(sizeof(spscring::control_block) + data_capacity) {}

  ~aligned_arena() { ::operator delete(base, std::align_val_t(64)); }

  aligned_arena(const aligned_arena&) = delete;
  aligned_arena& operator=(const aligned_arena&) = delete;

  spscring::control_block* header() const { return reinterpret_cast<spscring::control_block*>(base); }
};

// ---------------------------------------------------------------------------
// 1. Fixed ring channel: constant-size frames, zero-copy both ways.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kFrameSize = 64;
constexpr std::uint32_t kFrameCount = 16;
constexpr std::uint64_t kFixedMessages = 200000;

void run_fixed_channel() {
  constexpr std::uint32_t kCapacity = kFrameSize * kFrameCount;
  aligned_arena arena(kCapacity);
  spscring::control_block* hdr = arena.header();
  if (!spscring::init_control_block(*hdr, kCapacity, spscring::layout_type::fixed, kFrameSize, kFrameSize)) {
    std::printf("fixed: invalid geometry\n");
    return;
  }

  spscring::fixed_writer writer{hdr};
  spscring::fixed_reader reader{hdr};
  std::atomic<bool> producer_done{false};
  std::uint64_t mismatches = 0;

  std::thread consumer([&] {
    spscring::atomic_backoff backoff;  // per-thread backoff state
    std::uint64_t drained = 0;
    while (true) {
      std::uint32_t size = 0;
      if (const void* slot = reader.try_read(&size)) {
        // Zero-copy view into the ring: valid until read_advance().
        std::uint64_t tag = 0;
        std::memcpy(&tag, slot, sizeof(tag));
        if (tag != drained) {
          ++mismatches;
        }
        ++drained;
        reader.read_advance(1);  // bumps read_wake_seq + wakes producers
      } else if (producer_done.load(std::memory_order_acquire)) {
        break;  // producer finished and the ring is drained
      } else {
        // Ring empty: spin briefly, then sleep until commit_seq moves
        // (every producer commit bumps it and wakes all waiters).
        const std::uint32_t seq = hdr->rb_meta.commit_seq.load(std::memory_order_acquire);
        backoff.wait(&hdr->rb_meta.commit_seq, seq, 2);
      }
    }
    std::printf("fixed: consumer drained %llu/%llu frames%s\n", (unsigned long long)drained,
                (unsigned long long)kFixedMessages, mismatches ? "  <-- MISMATCH" : "");
  });

  spscring::atomic_backoff backoff;
  std::uint64_t i = 0;
  while (i < kFixedMessages) {
    char* slot = static_cast<char*>(writer.try_reserve());
    if (slot == nullptr) {
      // Ring full: wait until the consumer frees room (read_wake_seq bump).
      const std::uint32_t wake = hdr->rb_meta.read_wake_seq.load(std::memory_order_acquire);
      backoff.wait(&hdr->rb_meta.read_wake_seq, wake, 2);
      continue;
    }
    const std::uint64_t tag = i;
    std::memcpy(slot, &tag, sizeof(tag));  // fill in place
    writer.commit();                        // +1 commit_seq, notify consumers
    ++i;
  }
  producer_done.store(true, std::memory_order_release);
  std::printf("fixed: producer wrote %llu frames\n", (unsigned long long)kFixedMessages);
  consumer.join();
}
// ---------------------------------------------------------------------------
// 2. Varlen ring channel: variable-length messages + message_meta.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kVarCapacity = 4096;
constexpr std::uint64_t kVarMessages = 100000;

void run_varlen_channel() {
  aligned_arena arena(kVarCapacity);
  spscring::control_block* hdr = arena.header();
  if (!spscring::init_control_block(*hdr, kVarCapacity, spscring::layout_type::varlen, 8)) {
    std::printf("varlen: invalid geometry\n");
    return;
  }

  spscring::varlen_writer writer{hdr};
  spscring::varlen_reader reader{hdr};
  std::atomic<bool> producer_done{false};
  std::uint64_t mismatches = 0;

  std::thread consumer([&] {
    spscring::atomic_backoff backoff;
    std::uint64_t drained = 0;
    char buffer[512];
    while (true) {
      std::uint32_t size = 0;
      spscring::message_meta meta{};
      if (reader.read(buffer, sizeof(buffer), &size, &meta)) {
        if (meta.user_data != drained) {
          ++mismatches;
        }
        ++drained;
      } else if (producer_done.load(std::memory_order_acquire)) {
        break;
      } else {
        const std::uint32_t seq = hdr->rb_meta.commit_seq.load(std::memory_order_acquire);
        backoff.wait(&hdr->rb_meta.commit_seq, seq, 2);
      }
    }
    std::printf("varlen: consumer drained %llu/%llu messages%s\n", (unsigned long long)drained,
                (unsigned long long)kVarMessages, mismatches ? "  <-- MISMATCH" : "");
  });

  spscring::atomic_backoff backoff;
  std::uint64_t i = 0;
  while (i < kVarMessages) {
    const std::uint32_t len = static_cast<std::uint32_t>(1 + (i % 31));  // 1..31 bytes
    spscring::message_meta meta{};
    meta.user_data = i;  // logical sequence number, travels with the payload

    const spscring::reserve_result res = writer.try_reserve(len, meta);
    if (res.status == spscring::reserve_status::full) {
      const std::uint32_t wake = hdr->rb_meta.read_wake_seq.load(std::memory_order_acquire);
      backoff.wait(&hdr->rb_meta.read_wake_seq, wake, 2);
      continue;
    }
    if (res.status != spscring::reserve_status::ok) {
      std::printf("varlen: unexpected reserve status %d\n", static_cast<int>(res.status));
      break;
    }
    std::memset(res.payload, static_cast<int>('a' + (i % 26)), len);
    writer.commit(res.position);
    ++i;
  }
  producer_done.store(true, std::memory_order_release);
  std::printf("varlen: producer wrote %llu messages\n", (unsigned long long)kVarMessages);

  consumer.join();
}

}  // namespace

int main() {
  std::printf("=== fixed ring, in-process threads ===\n");
  run_fixed_channel();
  std::printf("=== varlen ring, in-process threads ===\n");
  run_varlen_channel();
  return 0;
}