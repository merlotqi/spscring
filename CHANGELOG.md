# Changelog

All notable changes to **spscring** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `cmake/` module suite: centralized version (`spscring-version`), cross-compiler
  warnings (`spscring-warnings`), and sanitizer wiring (`spscring-sanitizers`).
- `find_package(spscring CONFIG)` support via a generated `spscring-config.cmake`
  (with `spscring::spscring` and plain `spscring` target aliases) and a
  `spscring-config-version.cmake` (SameMajorVersion compatibility).
- `pkg-config` support via an installed `spscring.pc`.

### Changed
- First-party build targets (tests, benchmarks, examples) now compile with a
  common, portable warning set.
- The pre-existing ASan/UBSan wiring is now routed through
  `spscring_enable_sanitizers` in `cmake/spscring-sanitizers.cmake`.

### Build / housekeeping
- Moved the implementation-detail headers under `include/spscring/internal/`
  (public API surface remains unchanged).

## [0.1.0] - 2026-09-04

Initial extraction from the
[xproc](https://github.com/merlotqi/xproc) project, where the ring powers the
inter-process message channel.

### Added
- **Fixed ring** (`fixed_writer` / `fixed_reader`): constant-size frames, one
  CAS per reserve, wrap handled with a dummy write.
- **Variable-length ring** (`varlen_writer` / `varlen_reader`): self-describing
  `varlen_slot_header` (slot size + payload size + `message_meta`), wrap handled
  via zero-size padding headers.
- **Three-frontier protocol** — `write_pos` (reserve), `commit_pos`
  (release), `read_pos` (consume); consumers never observe un-committed slots.
- **Shared-memory-first ABI**: `control_block` is a pinned standard-layout type
  (offset-aligned with `static_assert`) with a `0x53505343` ("SPSC") magic and
  explicit version fields.
- **Blocking primitives** with exponential backoff
  (`pause` → `yield` → wait): Linux `futex`, Win32 `WaitOnAddress` /
  `WakeByAddress*`, macOS `os_sync_wait_on_address` (14.4+, `SHARED`) with the
  `__ulock_wait` fallback.
- 21 unit tests (writes/reads, wrap, pad headers, two-phase commit, threaded
  stress, ABI layout, geometry validation), two throughput benchmarks, one
  runnable example (`examples/basic_usage.cpp`), and a CI matrix (Linux
  GCC/Clang × Debug/Release, ASan/UBSan, macOS, Windows).

### Fixed (vs. xproc upstream)
- `ring_view` now has a virtual destructor, fixing undefined behaviour where the
  upstream code deleted derived writers through base pointers.
- The fixed ring now **enforces** `data_capacity % fixed_item_size == 0`
  (constructor `abort()`); the upstream silently relied on the invariant and its
  benchmark had to rewind capacity manually.
- Reader handler signatures were unified across the fixed/varlen readers.
- The unused `ringbuffer_error` module was dropped; validation failures return
  `bool`.

### License
- Relicensed under MIT (upstream is GPL-3.0).

[Unreleased]: https://github.com/merlotqi/spscring/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/merlotqi/spscring/releases/tag/v0.1.0