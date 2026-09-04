# Contributing

Thanks for your interest in **spscring**! This guide covers how to build, test,
and submit changes.

## Repo layout

```
cmake/                      Reusable build helpers (version, warnings, sanitizers, packaging)
include/spscring/           Header-only library (public API)
include/spscring/internal/  Implementation-detail headers — not a public API
tests/                      gtest unit tests
benchmarks/                 google/benchmark throughput benchmarks
examples/                   Runnable sample (basic_usage.cpp)
docs/                       Design notes (ABI, algorithms, memory ordering)
.github/workflows/          CI
```

## Requirements

- C++17 compatible compiler — GCC ≥ 8, Clang ≥ 8, or MSVC 2019+
- CMake ≥ 3.14
- Network access on first build (tests/benchmarks pull gtest / google-benchmark
  via FetchContent)

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DSPSCRING_BUILD_TESTS=ON -DSPSCRING_BUILD_BENCHMARKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build options (see the top-level `CMakeLists.txt`):

| option | default | meaning |
|---|---|---|
| `SPSCRING_BUILD_TESTS` | `ON` (top-level) | build the gtest suite |
| `SPSCRING_BUILD_BENCHMARKS` | `OFF` | build the benchmarks |
| `SPSCRING_BUILD_EXAMPLES` | `ON` (top-level) | build the example binary |
| `SPSCRING_SANITIZE` | (empty) | comma-separated sanitizers, e.g. `address,undefined` |

Sanitized build (as CI runs):

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DSPSCRING_SANITIZE=address,undefined
cmake --build build-san -j && ctest --test-dir build-san --output-on-failure
```

## Adding a unit test

- The suite lives in [`tests/`](tests/). Register a new test file in
  `tests/CMakeLists.txt` via `spscring_gtest_executable(...)`.
- Cover the new behaviour with at least one focused test. Ring behaviour is
  order-sensitive: exercise the wrap boundary, two-phase
  `try_reserve`→`commit`, and the empty/full transitions explicitly.
- Threaded-stress tests must be deterministic enough for CI: bounded message
  counts, `join()` before asserting, and no long poll loops.
- Run the whole suite (`ctest --test-dir build --output-on-failure`) and the
  sanitized build before opening a PR.

## Code style

- Follow the checked-in [`.clang-format`](.clang-format) (Google base, 120-col
  limit). Format before committing:
  ```bash
  clang-format -i $(find include tests benchmarks examples -name '*.hpp' -o -name '*.cpp')
  ```
- Put implementation-detail headers (things no external consumer includes) in
  `include/spscring/internal/`. Public headers must not depend on internal ones
  in their signatures.
- Header-only and dependency-free: any new public file must carry no link-time
  dependency and no allocations on the ring hot path.

## ABI rules

`control_block` is shared between producers and consumers, possibly across
processes on mapped memory. It must remain `standard-layout` and trivially
copyable; its field offsets are pinned by `static_assert` in
`control_block.hpp`. Any layout change is a breaking ABI change: bump the
version fields (`version_major`/`version_minor` in
[`cmake/spscring-version.cmake`](cmake/spscring-version.cmake) currently drives
the package version; keep the in-segment version in sync) and note it in
[`CHANGELOG.md`](CHANGELOG.md).

## Committing & branching

- Branch from `main`, name it `feat/…`, `fix/…`, or `chore/…`.
- Prefer small, reviewable commits. Use a conventional `type(scope): summary`
  subject line.
- Update [`CHANGELOG.md`](CHANGELOG.md) under **Unreleased** with the change.

## Submitting

- Pull request against `main`.
- CI runs Linux (GCC + Clang, Debug + Release), sanitizers, macOS, and Windows.
  All green is required.
- Verify the library still installs and can be consumed via
  `find_package(spscring CONFIG)`:
  ```bash
  cmake --install build --prefix /tmp/spscring-install
  ```

## License

spscring is MIT licensed. By contributing you agree to license your changes
under the same terms (see [`LICENSE`](LICENSE)).