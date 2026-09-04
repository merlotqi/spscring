// Throughput benchmarks for the variable-length SPSC ring.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <spscring/spscring.hpp>
#include <thread>

namespace {

constexpr std::size_t kCapacity = 1u << 20;  // 1 MiB
constexpr std::uint64_t kMessagesPerBatch = 500000;

struct varlen_ring_fixture {
  std::unique_ptr<std::byte[]> storage{std::make_unique<std::byte[]>(sizeof(spscring::control_block) + kCapacity)};

  varlen_ring_fixture() {
    auto* header = reinterpret_cast<spscring::control_block*>(storage.get());
    spscring::init_control_block(*header, kCapacity, spscring::layout_type::varlen, 8);
  }

  spscring::control_block* header() { return reinterpret_cast<spscring::control_block*>(storage.get()); }
};

// Single-threaded varlen reserve+copy+commit with 64-byte payloads.
void BM_VarlenWrite64(benchmark::State& state) {
  for (auto _ : state) {
    varlen_ring_fixture fixture;
    spscring::varlen_writer writer{fixture.header()};
    spscring::varlen_reader reader{fixture.header()};

    char payload[64]{};
    // Drain alongside so the never-drained ring does not stall the batch.
    std::thread consumer([&] {
      char recv[64];
      std::uint32_t recv_size = 0;
      std::uint64_t consumed = 0;
      while (consumed < kMessagesPerBatch) {
        if (reader.read(recv, sizeof(recv), &recv_size)) {
          ++consumed;
        }
      }
    });

    for (std::uint64_t i = 0; i < kMessagesPerBatch; ++i) {
      while (!writer.write(sizeof(payload), payload)) {
      }
    }
    consumer.join();
  }
  state.SetItemsProcessed(state.iterations() * kMessagesPerBatch);
}
BENCHMARK(BM_VarlenWrite64)->Unit(benchmark::kMillisecond);

// Mixed payload sizes: 16, 64, 256, 1024 bytes.
void BM_VarlenMixedSizes(benchmark::State& state) {
  for (auto _ : state) {
    varlen_ring_fixture fixture;
    spscring::varlen_writer writer{fixture.header()};
    spscring::varlen_reader reader{fixture.header()};

    char payload[1024]{};
    std::uint64_t sent = 0;
    std::uint64_t consumed = 0;

    std::thread consumer([&] {
      char recv[1024];
      std::uint32_t recv_size = 0;
      while (consumed < kMessagesPerBatch) {
        if (reader.read(recv, sizeof(recv), &recv_size)) {
          ++consumed;
        }
      }
    });

    const std::uint32_t sizes[] = {16, 64, 256, 1024};
    while (sent < kMessagesPerBatch) {
      const std::uint32_t size = sizes[sent % 4];
      if (!writer.write(size, payload)) {
        continue;
      }
      ++sent;
    }
    consumer.join();
  }
  state.SetItemsProcessed(state.iterations() * kMessagesPerBatch);
}
BENCHMARK(BM_VarlenMixedSizes)->Unit(benchmark::kMillisecond);

}  // namespace
