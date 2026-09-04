# spscring design notes

This document describes the memory layout, algorithms, memory-ordering rules,
and the on-disk (in-segment) ABI contract of the spscring ring buffers.

## 1. Segment layout

A ring lives in one contiguous block of memory: the **control block** followed
by the **data region**. The block can be a heap allocation, a stack buffer, or
a shared-memory mapping — the ring never owns storage and never allocates.

```
offset  content                              size
0       control_block                        320 B (5 cache lines)
320     data region                          data_capacity bytes
```

### Control block (cross-process ABI)

| field            | offset | type / value                          |
|------------------|--------|---------------------------------------|
| magic            | 0      | `0x53505343` ("SPSC")                 |
| version_major    | 4      | 0                                     |
| version_minor    | 6      | 1                                     |
| header_size      | 8      | `sizeof(control_block)`               |
| layout_type      | 12     | `fixed` (0) / `varlen` (1)            |
| reserved0        | 16     | 0                                     |
| rb_meta          | 64     | 3 cache lines (see below)             |
| data_capacity    | 256    | bytes                                 |
| data_alignment   | 264    | power of two, 1..64                   |
| fixed_item_size  | 268    | fixed rings: slot size; else 0        |
| reserved         | 272    | 40 bytes                              |

`offsetof` pins: `rb_meta == 64`, `data_capacity == 256`, `sizeof == 320`,
`std::is_standard_layout && std::is_trivially_copyable`. Any change is a
breaking ABI change and must bump the version.

### meta (ring fronts + wait words)

| field         | line | type             | written by | role                          |
|---------------|------|------------------|------------|-------------------------------|
| write_pos     | 0    | atomic\<uint64\> | producer   | reservation front             |
| read_pos      | 1    | atomic\<uint64\> | consumer   | consumption front             |
| read_wake_seq | 1    | atomic\<uint32\> | consumer   | producer wakeup (futex word)  |
| commit_pos    | 2    | atomic\<uint64\> | producer   | publication front             |
| commit_seq    | 2    | atomic\<uint32\> | producer   | consumer wakeup (futex word)  |

- The three **fronts** are monotonic logical byte offsets, never wrapped; the
  physical index is `pos % data_capacity`. 64-bit offsets make the ABA problem
  practically impossible.
  - `write_pos` — space is *reserved* here before the slot is filled.
  - `commit_pos` — advanced (release) after a slot is fully written; consumers
    only chase this, so a reserved-but-unfinished slot is never observable.
  - `read_pos` — advanced after a message is consumed.
- The 32-bit sequence words exist purely to block/wake: they double as futex
  words on Linux and `WaitOnAddress` words on Windows. Cache-line ownership
  follows the writer: line 0 producer, line 1 consumer, line 2 producer.

## 2. Fixed ring

Every message occupies exactly `fixed_item_size` bytes; `data_capacity` must be
an integer multiple of `fixed_item_size` (enforced by `init_control_block` and
asserted by the writer constructor), so slot placement stays contiguous and the
wrap needs no special-casing.

Producer fast path (`fixed_writer::try_reserve`):
1. load `write_pos` relaxed, `read_pos` acquire
2. if `write % cap == read % cap && write > read` → ring full, return null
3. store `write_pos + item_size` relaxed (single writer: no CAS needed)
4. return pointer to the slot

Publication (`commit`): `commit_pos.store(reserved_end, release)` — this is
what makes the slot observable — followed by `commit_seq.fetch_add` (seq_cst)
and a notify-all on `commit_seq` for blocked consumers.

Consumer (`fixed_reader::try_read`): load `read_pos` relaxed / `commit_pos`
acquire; if `commit_pos <= read_pos` the ring is empty; otherwise return the
in-place slot view. `read_advance(n)` adds `n * item_size` to `read_pos`
(seq_cst) and notifies producers via `read_wake_seq`.

## 3. Variable-length ring

Slots are self-describing:

```
[ varlen_slot_header (24 B) ][ payload, aligned to data_alignment ]
slot_size = align_up(24 + payload_size, data_alignment)
```

- `slot_size == 0` marks **wrap padding**: when a producer cannot fit a slot
  before the ring end, it commits a dummy header spanning the tail; the
  consumer skips it like any other slot.
- Producer: CAS `write_pos` from `head` to `head + needed` (acq_rel), fill the
  slot, then `commit` (bump `commit_seq`, notify). A failed CAS backs off and
  retries; a lost race simply reloads `head`.
- Consumer: chases the producer with the same slot headers. A slot is
  considered readable only after `commit_seq` has advanced (the header may
  still be mid-write otherwise); the payload is handed to the caller's handler
  as a non-owning view, valid until `read_pos` advances.

## 4. Memory ordering summary

| operation                                | ordering                        | pairs with                        |
|------------------------------------------|---------------------------------|-----------------------------------|
| producer store/CAS `write_pos`           | acq_rel (CAS) / relaxed (store) | consumer load acquire             |
| payload writes before `commit`           | release (via seq_cst bump)      | consumer acquire load `commit_seq`|
| consumer `read_pos` update               | seq_cst RMW                     | producer acquire load             |
| consumer notify on `read_wake_seq`       | seq_cst RMW                     | producer acquire re-check         |

The relaxed `write_pos` store in the fixed producer is safe because the single
producer is the only writer of that word; consumers only read it with acquire.

## 5. Blocking and wakeup

`atomic_backoff` escalates: CPU `pause` (exponential 1→256) → `yield` →
platform wait. Platform mapping of the wait words:

| OS      | wait primitive                                | cross-process |
|---------|-----------------------------------------------|---------------|
| Linux   | `futex(FUTEX_WAIT_PRIVATE)`                   | yes (physical page based) |
| Win32   | `WaitOnAddress` / `WakeByAddress*`            | yes (address based) |
| macOS ≥14.4 | `os_sync_wait_on_address(SHARED)`         | yes (`_SHARED` flag) |
| macOS <14.4 | `__ulock_wait` + 50 ms re-check loop      | degraded (VA based) |
| other   | polling (`atomic_poll`)                       | n/a |

Consumers block on `commit_seq`, producers on `read_wake_seq`. Notify calls are
cheap no-ops when nobody waits; the `atomic_notify_all_if_waiters` predicate
exists for parity on platforms where the C++11 fallback would need a recheck.

## 6. Validation contract

`init_control_block` rejects invalid geometry (alignment not a power of two in
[1, 64], zero capacity, capacity not a multiple of alignment). A peer that
attaches to an existing segment must call `validate_control_block` and then
compare `data_capacity` / `layout_type` / `fixed_item_size` against its own
expectations — the library cannot know what the peer intended.

## 7. Provenance and deviations from xproc

Extracted from `xproc/include/xproc/ringbuffer` and `xproc/include/xproc/sync`.
Deliberate deviations:

1. `ring_view` gained a virtual destructor (xproc deletes derived writers via
   base pointers — undefined behaviour without it).
2. The fixed ring now **enforces** `data_capacity % fixed_item_size == 0`; the
   original relied on that invariant silently and its benchmark had to rewind
   capacity manually.
3. Read-callback signatures were unified across fixed/varlen readers.
4. The unused `ringbuffer_error` module was dropped; init/validation failures
   return `bool` instead.
5. `reserve_for` (blocking reserve with timeout) was dropped pending tests; the
   timeout-aware `atomic_wait_for` is available for callers who want to build
   their own.
