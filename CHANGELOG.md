# Changelog

All notable changes to **spscring** are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `notify_word.hpp`: conditional wait/notify protocol for the ring's futex
  words. The waiter flag lives in bit 31 of `commit_seq` / `read_wake_seq`;
  producers/consumers bump via `bump_notify_word` (returns whether a wake
  syscall is needed) and block via `wait_on_notify_word` / `wait_for_seq_change`.
- Single-outstanding-reservation guard on `fixed_writer` and `varlen_writer`:
  a second `try_reserve` before `commit()` is rejected (returns `null` /
  `reserve_status::pending_commit`), and `commit()` is a no-op unless it
  matches the outstanding reservation — misuse can no longer move
  `commit_pos` backward or skip an uncommitted slot.
- `is_valid_fixed_ring`: structural validation for fixed-ring control blocks
  (layout, non-zero item size, capacity multiple) to check before
  constructing a `fixed_writer` instead of hitting the constructor's abort.
- `reserve_status::pending_commit` (and its `reserve_status_cstr` spelling).

### Changed
- `commit()` / `read_advance()` (fixed and varlen, including wrap-padding
  commits) wake the peer **only when a waiter is registered** on the notify
  word. Benchmarks on Apple M2: fixed-ring round trip 1.05M → 18.0M msg/s,
  threaded throughput 1.53M → 24.0M msg/s, varlen write 1.52M → 19.3M msg/s
  (13–17x). Callers that block on the notify words must register via
  `wait_for_seq_change` (or set the waiter bit themselves) to keep receiving
  wakes.
- In-segment ABI: `version_minor` bumped 1 → 2 (notify words now pack a
  waiter flag into bit 31; struct offsets and sizes are unchanged, and
  diagnostic readers should mask with `notify_seq_value`).
- Project version 0.2.0.

### Fixed
- `fixed_writer` constructor abort on a misconfigured ring is now documented
  alongside `is_valid_fixed_ring`, the supported pre-flight check.

### Build / housekeeping
- Moved the implementation-detail headers under `include/spscring/internal/`
  (public API surface remains unchanged).
- `cmake/` module suite: centralized version (`spscring-version`), cross-compiler
  warnings (`spscring-warnings`), and sanitizer wiring (`spscring-sanitizers`).
- `find_package(spscring CONFIG)` support via a generated `spscring-config.cmake`
  (with `spscring::spscring` and plain `spscring` target aliases) and a
  `spscring-config-version.cmake` (SameMajorVersion compatibility).
- `pkg-config` support via an installed `spscring.pc`.
- First-party build targets (tests, benchmarks, examples) now compile with a
  common, portable warning set.
- The pre-existing ASan/UBSan wiring is now routed through
  `spscring_enable_sanitizers` in `cmake/spscring-sanitizers.cmake`.

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