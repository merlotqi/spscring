#pragma once

// Conditional wait/notify protocol for the ring's two futex words
// (meta::commit_seq and meta::read_wake_seq).
//
// Problem: commit()/read_advance() used to issue a wake syscall (futex /
// WaitOnAddress / os_sync_wake) on every message even when no peer was
// blocked — measured at ~470 ns per syscall on Apple M2, roughly half the
// per-message cost of a fixed-ring round trip.
//
// Protocol (folly::EventCount style): each 32-bit notify word packs a waiter
// flag into bit 31 and a free-running wake sequence into bits 0..30.
//
//   Waiter:  set the bit (fetch_or), re-check the wake predicate, then block
//            in a futex wait with the full word (bit + sequence) as the
//            expected value; clear the bit on exit if it set it.
//   Bumper:  fetch_add(1) publishes the state change AND returns the previous
//            word: the waiter bit in that previous word says whether anyone
//            may be blocked, so the wake syscall happens only then.
//
// No lost wakeups: both sides operate on one word, and RMWs on the same word
// are totally coherently ordered.
//   - If the waiter's fetch_or precedes the bumper's fetch_add in coherence
//     order, the fetch_add reads the bit and the bumper wakes.
//   - If it follows it, the waiter's marked value already contains the
//     bumper's sequence bump, so the waiter's predicate re-check (which runs
//     after registration) observes the published state and never sleeps.
//   - In between, the kernel's futex value check sees the changed word and
//     returns EWOULDBLOCK instead of blocking.
//
// SPSC discipline means at most one waiter per word in practice; the helpers
// tolerate racing registrars by only clearing a bit they observed unset.
//
// Counter overflow: bits 0..30 wrap after 2^31 bumps; the carry sets a
// spurious waiter bit. This is benign (one extra wake syscall; the next
// exiting waiter clears it) and cannot cause lost wakeups — a set bit only
// makes bumpers more eager to wake.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <spscring/atomic_wait.hpp>

namespace spscring {

// Waiter-present flag inside a notify word. Layout is part of the shared
// memory ABI (see control_block.hpp): the words stay 32-bit futex words, only
// their packed semantics change (ABI version_minor bumped accordingly).
inline constexpr std::uint32_t notify_waiter_bit = 0x80000000u;
inline constexpr std::uint32_t notify_seq_mask = 0x7FFFFFFFu;

// Wake-sequence value of a notify word with the waiter flag stripped. Use for
// diagnostics / metrics; raw values may carry the waiter bit.
inline constexpr std::uint32_t notify_seq_value(std::uint32_t raw) noexcept {
  return raw & notify_seq_mask;
}

// Bumps the wake sequence by one and reports whether a waiter is (or was)
// registered on this word. Callers issue atomic_notify_all only when this
// returns true. seq_cst keeps the historical ordering of the unconditional
// bump it replaces; the RMW read is what makes the waiter bit observable.
inline bool bump_notify_word(std::atomic<std::uint32_t>& word) noexcept {
  return (word.fetch_add(1, std::memory_order_seq_cst) & notify_waiter_bit) != 0;
}

// Blocks while pred() is true. pred() must observe the wake-side state change
// (e.g. an advanced commit_pos, or a changed notify word) when it becomes
// true; it is re-checked after registering the waiter bit and after every
// wakeup, which closes the lost-wakeup window.
template <typename Pred>
void wait_on_notify_word(std::atomic<std::uint32_t>& word, Pred&& pred) {
  bool registered_by_us = false;
  for (;;) {
    if (!pred()) {
      if (registered_by_us) {
        word.fetch_and(~notify_waiter_bit, std::memory_order_relaxed);
      }
      return;
    }
    const std::uint32_t marked = word.fetch_or(notify_waiter_bit, std::memory_order_acq_rel);
    registered_by_us = registered_by_us || (marked & notify_waiter_bit) == 0;
    if (!pred()) {
      if (registered_by_us) {
        word.fetch_and(~notify_waiter_bit, std::memory_order_relaxed);
      }
      return;
    }
    // Kernel re-checks the full word (bit + sequence): a bump that raced with
    // registration makes this return immediately instead of blocking.
    (void)atomic_wait_for(&word, marked, -1);
  }
}

// Timed variant: waits while pred() is true, up to timeout_ms (negative =
// indefinitely, mirroring futex semantics). Returns true when pred() became
// false (progress observed), false on timeout. Always clears the waiter bit
// if this call set it.
template <typename Pred>
bool wait_on_notify_word_for(std::atomic<std::uint32_t>& word, Pred&& pred, int timeout_ms) {
  if (timeout_ms < 0) {
    wait_on_notify_word(word, pred);
    return true;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool registered_by_us = false;
  for (;;) {
    if (!pred()) {
      if (registered_by_us) {
        word.fetch_and(~notify_waiter_bit, std::memory_order_relaxed);
      }
      return true;
    }
    const std::uint32_t marked = word.fetch_or(notify_waiter_bit, std::memory_order_acq_rel);
    registered_by_us = registered_by_us || (marked & notify_waiter_bit) == 0;
    if (!pred()) {
      if (registered_by_us) {
        word.fetch_and(~notify_waiter_bit, std::memory_order_relaxed);
      }
      return true;
    }
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining_ms <= 0) {
      if (registered_by_us) {
        word.fetch_and(~notify_waiter_bit, std::memory_order_relaxed);
      }
      return false;
    }
    (void)atomic_wait_for(&word, marked, static_cast<int>(remaining_ms));
  }
}

// Convenience: block until the word's wake-sequence value differs from
// `last_seen` (a raw snapshot that may or may not carry the waiter bit).
// Equivalent to the historical atomic_wait(&word, last) usage, but registers
// the waiter bit so conditional bumpers keep waking it.
inline void wait_for_seq_change(std::atomic<std::uint32_t>& word, std::uint32_t last_seen) {
  const std::uint32_t last = notify_seq_value(last_seen);
  wait_on_notify_word(word, [&word, last] { return notify_seq_value(word.load(std::memory_order_acquire)) == last; });
}

// Timed convenience overload; see wait_for_seq_change.
inline bool wait_for_seq_change_for(std::atomic<std::uint32_t>& word, std::uint32_t last_seen, int timeout_ms) {
  const std::uint32_t last = notify_seq_value(last_seen);
  return wait_on_notify_word_for(
      word, [&word, last] { return notify_seq_value(word.load(std::memory_order_acquire)) == last; }, timeout_ms);
}

}  // namespace spscring
