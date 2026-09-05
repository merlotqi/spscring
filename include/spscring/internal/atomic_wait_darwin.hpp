#pragma once

// Darwin atomic wait/notify — two-tier implementation.
//
// Tier 1 (macOS 14.4+): os_sync_wait_on_address / os_sync_wake_by_address_*,
//   Apple's documented futex equivalent. OS_SYNC_WAIT_ON_ADDRESS_SHARED
//   provides true cross-process synchronization on MAP_SHARED memory.
//   Supports 4-byte and 8-byte values.
// Tier 2 (macOS < 14.4): __ulock_wait / __ulock_wake, private libsystem
//   syscalls (UL_COMPARE_AND_WAIT). They operate on virtual addresses only, so
//   cross-process MAP_SHARED requires a periodic timeout re-check loop.
//   32-bit values only.
#ifdef __APPLE__

#include <Availability.h>
#include <os/os_sync_wait_on_address.h>
#include <stdint.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>

// ---- Tier 1: os_sync_wait_on_address (macOS 14.4+) ----

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 140400 || defined(SPSCRING_USE_OS_SYNC)

// Compile-time: target is macOS 14.4+, use os_sync directly.
#define SPSCRING_ATOMIC_WAIT_TIER1 1

#elif __has_builtin(__builtin_available)

// Runtime: check availability at call time.
#define SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME 1

#endif

// ---- Tier 2: __ulock_wait fallback ----

// Opcode/flag values per the xnu kernel header bsd/sys/ulock.h: the operation
// code lives in bits [7:0]. Beware third-party snippets showing
// UL_COMPARE_AND_WAIT as 0x00000100 — that value is rejected with EINVAL by
// current libsystem (verified empirically on macOS 26/arm64). Wake reuses the
// wait opcode; wake-all ORs in the ULF_WAKE_ALL flag bit (0x00000100).
#define SPSCRING_ULOCK_WAIT_OP 0x00000001      // UL_COMPARE_AND_WAIT (32-bit word)
#define SPSCRING_ULOCK_WAKE_OP 0x00000001      // UL_WAKE
#define SPSCRING_ULOCK_WAKE_ALL_OP 0x00000101  // UL_WAKE | ULF_WAKE_ALL

// __ulock_wait's timeout parameter is MICROSECONDS (uint32_t) — see the
// "timeout is specified in microseconds" declaration in xnu bsd/sys/ulock.h.
// The Tier 2 wait loops block in 50 ms slices: __ulock_wait keys on the
// process-local virtual address, so cross-process MAP_SHARED waiters must wake
// periodically and re-check the shared value themselves.
//
// Note: Darwin wait primitives fault with EFAULT when the wait address sits on
// a never-touched zero-fill page. Ring control blocks are always initialized
// (init_control_block writes the header) before anyone waits, so this is not
// an issue for the library — but keep it in mind for raw callers.
#define SPSCRING_ULOCK_TIMEOUT_US 50000u  // 50 ms

extern "C" int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint32_t timeout_us);
extern "C" int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);

namespace spscring {
namespace sync {

namespace details {

// ---- Tier 1 ----

#if defined(SPSCRING_ATOMIC_WAIT_TIER1) || defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)

inline int os_wait(const std::atomic<uint32_t>* atomic, uint32_t old) {
  return os_sync_wait_on_address(const_cast<std::atomic<uint32_t>*>(atomic), static_cast<uint64_t>(old),
                                 sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED);
}

inline int os_wake_one(const std::atomic<uint32_t>* atomic) {
  return os_sync_wake_by_address_any(const_cast<std::atomic<uint32_t>*>(atomic), sizeof(uint32_t),
                                     OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

inline int os_wake_all(const std::atomic<uint32_t>* atomic) {
  return os_sync_wake_by_address_all(const_cast<std::atomic<uint32_t>*>(atomic), sizeof(uint32_t),
                                     OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

// Timed variant of os_wait. Returns true when the deadline expired (the caller
// reports a timeout), false when the value may have changed or the syscall was
// interrupted / failed transiently — the caller must re-check the atomic
// either way.
inline bool os_wait_timed_out(const std::atomic<uint32_t>* atomic, uint32_t old, uint64_t timeout_ns) {
  const int rc = os_sync_wait_on_address_with_timeout(const_cast<std::atomic<uint32_t>*>(atomic),
                                                      static_cast<uint64_t>(old), sizeof(uint32_t),
                                                      OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                      OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns);
  return rc < 0 && errno == ETIMEDOUT;
}

#endif  // Tier 1

// ---- Tier 2 ----

inline int ulock_wait(void* addr, uint64_t val, uint32_t timeout_us) {
  return __ulock_wait(SPSCRING_ULOCK_WAIT_OP, addr, val, timeout_us);
}

inline int ulock_wake_one(void* addr) { return __ulock_wake(SPSCRING_ULOCK_WAKE_OP, addr, 0); }

inline int ulock_wake_all(void* addr) { return __ulock_wake(SPSCRING_ULOCK_WAKE_ALL_OP, addr, 0); }

// ---- Runtime tier selection ----

#if defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)

inline bool has_os_sync() {
  // macOS 14.4+: os_sync_wait_on_address is available.
  // __builtin_available generates a single branch on the cached OS version.
  if (__builtin_available(macOS 14.4, *)) {
    return true;
  }
  return false;
}

#endif  // runtime tier selection

}  // namespace details

// ---------------------------------------------------------------------------
// atomic_wait — block until *atomic != old
// ---------------------------------------------------------------------------

template <typename T>
inline void atomic_wait(const std::atomic<T>* atomic, T old) {
  static_assert(sizeof(T) == 4, "atomic_wait(darwin): only 32-bit atomics are supported");

#if defined(SPSCRING_ATOMIC_WAIT_TIER1)
  // Compile-time: macOS 14.4+ target, use os_sync exclusively.
  while (atomic->load(std::memory_order_acquire) == old) {
    details::os_wait(atomic, static_cast<uint32_t>(old));
  }

#elif defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)
  // Runtime check: prefer os_sync when available, fall back to ulock.
  if (details::has_os_sync()) {
    while (atomic->load(std::memory_order_acquire) == old) {
      details::os_wait(atomic, static_cast<uint32_t>(old));
    }
  } else {
    while (atomic->load(std::memory_order_acquire) == old) {
      (void)details::ulock_wait(const_cast<std::atomic<T>*>(atomic), static_cast<uint64_t>(static_cast<uint32_t>(old)),
                                SPSCRING_ULOCK_TIMEOUT_US);
    }
  }

#else
  // No os_sync available, use ulock with timeout re-check.
  while (atomic->load(std::memory_order_acquire) == old) {
    (void)details::ulock_wait(const_cast<std::atomic<T>*>(atomic), static_cast<uint64_t>(static_cast<uint32_t>(old)),
                              SPSCRING_ULOCK_TIMEOUT_US);
  }
#endif
}

// ---------------------------------------------------------------------------
// atomic_notify_one / atomic_notify_all — wake waiters
// ---------------------------------------------------------------------------

template <typename T>
inline void atomic_notify_one(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) == 4, "atomic_notify_one(darwin): only 32-bit atomics are supported");

#if defined(SPSCRING_ATOMIC_WAIT_TIER1)
  details::os_wake_one(atomic);

#elif defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)
  if (details::has_os_sync()) {
    details::os_wake_one(atomic);
  } else {
    details::ulock_wake_one(const_cast<std::atomic<T>*>(atomic));
  }

#else
  details::ulock_wake_one(const_cast<std::atomic<T>*>(atomic));
#endif
}

template <typename T>
inline void atomic_notify_all(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) == 4, "atomic_notify_all(darwin): only 32-bit atomics are supported");

#if defined(SPSCRING_ATOMIC_WAIT_TIER1)
  details::os_wake_all(atomic);

#elif defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)
  if (details::has_os_sync()) {
    details::os_wake_all(atomic);
  } else {
    details::ulock_wake_all(const_cast<std::atomic<T>*>(atomic));
  }

#else
  details::ulock_wake_all(const_cast<std::atomic<T>*>(atomic));
#endif
}

template <typename T>
inline bool atomic_notify_all_if_waiters(const std::atomic<T>*) {
  return true;
}

// ---------------------------------------------------------------------------
// atomic_wait_for — block until *atomic != old, or the timeout expires.
// Matches the Linux futex semantics: returns true if the value MAY have
// changed (woken or spurious — the caller must re-check), false on timeout.
// ---------------------------------------------------------------------------

template <typename T>
inline bool atomic_wait_for(const std::atomic<T>* atomic, T old, int timeout_ms) {
  static_assert(sizeof(T) == 4, "atomic_wait_for(darwin): only 32-bit atomics are supported");
  if (timeout_ms < 0) {
    // Negative timeout = wait indefinitely.
    atomic_wait(atomic, old);
    return true;
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

#if defined(SPSCRING_ATOMIC_WAIT_TIER1)
  // Compile-time: macOS 14.4+ target, os_sync supports absolute timeouts.
  for (;;) {
    if (atomic->load(std::memory_order_acquire) != old) {
      return true;
    }
    const auto remaining_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline -
                                                                                   std::chrono::steady_clock::now());
    if (remaining_ns.count() <= 0) {
      return false;  // Timed out.
    }
    if (details::os_wait_timed_out(atomic, static_cast<uint32_t>(old),
                                   static_cast<uint64_t>(remaining_ns.count()))) {
      return false;  // Kernel reported ETIMEDOUT.
    }
    // Woken or interrupted — re-check the value.
  }

#elif defined(SPSCRING_ATOMIC_WAIT_TIER1_RUNTIME)
  if (details::has_os_sync()) {
    for (;;) {
      if (atomic->load(std::memory_order_acquire) != old) {
        return true;
      }
      const auto remaining_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
          deadline - std::chrono::steady_clock::now());
      if (remaining_ns.count() <= 0) {
        return false;
      }
      if (details::os_wait_timed_out(atomic, static_cast<uint32_t>(old),
                                     static_cast<uint64_t>(remaining_ns.count()))) {
        return false;
      }
    }
  }
  // Fall through to the ulock loop below when os_sync is unavailable.

#endif

  // Tier 2 (and runtime fallback): block in 50 ms ulock slices until the
  // deadline passes. __ulock_wait cannot watch MAP_SHARED addresses from other
  // processes, so wake-ups rely on the periodic re-check.
  for (;;) {
    if (atomic->load(std::memory_order_acquire) != old) {
      return true;
    }
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
    if (remaining_ms <= 0) {
      return false;  // Timed out.
    }
    (void)details::ulock_wait(const_cast<std::atomic<T>*>(atomic),
                              static_cast<uint64_t>(static_cast<uint32_t>(old)), SPSCRING_ULOCK_TIMEOUT_US);
  }
}

}  // namespace sync
}  // namespace spscring

#endif  // __APPLE__
