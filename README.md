# spscring

Header-only, dependency-free **single-producer / single-consumer (SPSC) lock-free
ring buffers** in C++17, designed for cross-process shared-memory IPC.

Extracted from the [xproc](https://github.com/merlotqi/xproc) project, where it
powers the inter-process message channel.

## Features

- **Two layouts**
  - *fixed ring* — constant-size frames, one CAS per reserve, wrap handled with
    a dummy write
  - *varlen ring* — variable-length messages with a self-describing slot header
    (`slot_size` + payload), supporting arbitrary sizes up to the ring capacity
- **Shared-memory first**: the `control_block` is a pinned standard-layout ABI
  (`static_assert`-ed offsets) that lives at the start of the mapped segment;
  writers and readers only exchange atomic counters, never pointers
- **Syscall-free fast path**: 64-bit monotonic logical byte offsets (no ABA),
  two-phase publication (`reserve` → `commit` via release/acquire)
- **Blocking without burning CPU**: exponential backoff
  (`pause` → `yield` → platform wait):
  | OS | primitive |
  |---|---|
  | Linux | `futex(FUTEX_WAIT_PRIVATE)` |
  | Windows | `WaitOnAddress` / `WakeByAddress*` (polling fallback) |
  | macOS 14.4+ | `os_sync_wait_on_address` (cross-process, `SHARED` flag) |
  | macOS < 14.4 | `__ulock_wait` with 50 ms re-check loop |
- **Zero allocations, zero dependencies, header-only** — C++17, builds on
  GCC / Clang / MSVC, x86-64 / ARM64

## Quick start

```cpp
#include <spscring/spscring.hpp>

#include <cstring>
#include <memory>

int main() {
  // 1 KiB of storage: control block + data region, any contiguous buffer works
  // (heap, stack, shared memory).
  const std::size_t capacity = 1024;
  const std::size_t item_size = 64;
  auto storage = std::make_unique<std::byte[]>(sizeof(spscring::control_block) + capacity);

  auto* header = reinterpret_cast<spscring::control_block*>(storage.get());
  spscring::init_control_block(*header, capacity, spscring::layout_type::fixed,
                               64, static_cast<std::uint32_t>(item_size));

  spscring::fixed_writer writer{header};
  spscring::fixed_reader reader{header};

  const char msg[] = "hello ring";
  writer.write(msg, static_cast<std::uint32_t>(sizeof(msg)));   // returns false when full

  std::uint32_t size = 0;
  if (const void* slot = reader.try_read(&size)) {              // non-copying view
    char buffer[64];
    std::memcpy(buffer, slot, size);
    reader.read_advance(1);                                     // publish consumption
  }
}
```

For variable-length messages use `varlen_writer` / `varlen_reader` (two-phase
`try_reserve` → `commit`, or the one-shot `write(payload_size, data, meta)`).

A complete build-and-run example lives in [`examples/`](examples/).

## Integration

### CMake (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(spscring
  GIT_REPOSITORY https://github.com/merlotqi/spscring.git
  GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(spscring)

target_link_libraries(my_target PRIVATE spscring::spscring)
```

### Subproject / vendored

Add this repository as a submodule and call `add_subdirectory(spscring)`, or
simply add `include/` to your target's include path — there is nothing else to
build.

## Building & testing

```bash
cmake -S . -B build -DSPSCRING_BUILD_TESTS=ON -DSPSCRING_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Options: `SPSCRING_BUILD_TESTS` (default `ON` when top-level),
`SPSCRING_BUILD_BENCHMARKS` (default `OFF`),
`SPSCRING_BUILD_EXAMPLES` (default `ON` when top-level).

## Documentation

- [`docs/design.md`](docs/design.md) — memory layout, algorithms, memory
  ordering, ABI contract, blocking strategy

## License

MIT — see [LICENSE](LICENSE).
