// Throughput benchmarks for the fixed-size SPSC ring.
//
// The xproc-derived scenario (single producer, single consumer, 64-byte
// items over a 1 MiB ring) is preserved; the manual capacity rewind of the
// original is no longer needed because capacity % item_size is now enforced.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <spscring/spscring.hpp>
#include <thread>

namespace {

constexpr std::uint32_t kItemSize = 64;
constexpr std::size_t kCapacity = 1u << 20;  // 1 MiB
constexpr std::uint64_t kItemsPerBatch = 1000000;

struct fixed_ring_fixture {
  std::unique_ptr<std::byte[]> storage{std::make_unique<std::byte[]>(sizeof(spscring::control_block) + kCapacity)};

  fixed_ring_fixture() {
    auto* header = reinterpret_cast<spscring::control_block*>(storage.get());
    spscring::init_control_block(*header, kCapacity, spscring::layout_type::fixed, 64, kItemSize);
  }

  spscring::control_block* header() { return reinterpret_cast<spscring::control_block*>(storage.get()); }
};

// Single-threaded reserve+commit loop (upper bound for the hot path). The
// consumer side drains in the same thread so the batch never stalls on a full
// ring.
void BM_FixedReserveCommit(benchmark::State& state) {
  fixed_ring_fixture fixture;
  spscring::fixed_writer writer{fixture.header()};
  spscring::fixed_reader reader{fixture.header()};

  char payload[kItemSize]{};
  char recv[kItemSize];
  for (auto _ : state) {
    for (std::uint64_t i = 0; i < kItemsPerBatch; ++i) {
      void* slot = writer.try_reserve();
      while (slot == nullptr) {
        reader.read(recv, kItemSize);  // make room
        slot = writer.try_reserve();
      }
      std::memcpy(slot, payload, kItemSize);
      writer.commit();
      reader.read(recv, kItemSize);  // keep the ring mostly empty
    }
  }
  state.SetItemsProcessed(state.iterations() * kItemsPerBatch);
}
BENCHMARK(BM_FixedReserveCommit)->Unit(benchmark::kMillisecond);

// Two threads: producer fills, consumer drains. Measures end-to-end SPSC
// throughput including cross-core cache-line transfers.
void BM_FixedThreadedThroughput(benchmark::State& state) {
  for (auto _ : state) {
    fixed_ring_fixture fixture;
    spscring::fixed_writer writer{fixture.header()};
    spscring::fixed_reader reader{fixture.header()};

    std::thread consumer([&] {
      std::uint64_t consumed = 0;
      char recv[kItemSize];
      while (consumed < kItemsPerBatch) {
        if (reader.read(recv, kItemSize)) {
          ++consumed;
        } else {
          benchmark::DoNotOptimize(consumed);
        }
      }
    });

    char payload[kItemSize]{};
    for (std::uint64_t i = 0; i < kItemsPerBatch; ++i) {
      while (!writer.write(payload, kItemSize)) {
        // Retry; the consumer drains concurrently.
      }
    }
    consumer.join();
  }
  state.SetItemsProcessed(state.iterations() * kItemsPerBatch);
}
BENCHMARK(BM_FixedThreadedThroughput)->Unit(benchmark::kMillisecond);

// Non-copying consumer: read the slot pointer in place and advance.
void BM_FixedZeroCopyConsume(benchmark::State& state) {
  fixed_ring_fixture fixture;
  spscring::fixed_writer writer{fixture.header()};
  spscring::fixed_reader reader{fixture.header()};

  // Pre-fill the ring so the consumer always has data.
  char payload[kItemSize]{};
  while (writer.write(payload, kItemSize)) {
  }

  for (auto _ : state) {
    for (std::uint64_t i = 0; i < kItemsPerBatch; ++i) {
      const void* slot = reader.try_read(nullptr);
      if (slot == nullptr) {
        // Refill (single-threaded benchmark: producer role alternates).
        while (writer.write(payload, kItemSize)) {
        }
        slot = reader.try_read(nullptr);
      }
      benchmark::DoNotOptimize(slot);
      reader.read_advance(1);
    }
  }
  state.SetItemsProcessed(state.iterations() * kItemsPerBatch);
}
BENCHMARK(BM_FixedZeroCopyConsume)->Unit(benchmark::kMillisecond);

}  // namespace
