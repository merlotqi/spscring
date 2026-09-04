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
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
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

#define SPSCRING_ULOCK_WAIT_OP 0x00000100      // UL_COMPARE_AND_WAIT
#define SPSCRING_ULOCK_WAKE_OP 0x00000100      // UL_WAKE
#define SPSCRING_ULOCK_WAKE_ALL 0x10000100     // UL_WAKE | UL_WAKE_ALL bit
#define SPSCRING_ULOCK_TIMEOUT_NS 50000000ULL  // 50 ms

extern "C" int __ulock_wait(uint32_t operation, void* addr, uint64_t value, uint64_t timeout);
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

#endif  // Tier 1

// ---- Tier 2 ----

inline int ulock_wait(void* addr, uint64_t val, uint64_t timeout) {
  return __ulock_wait(SPSCRING_ULOCK_WAIT_OP, addr, val, timeout);
}

inline int ulock_wake_one(void* addr) { return __ulock_wake(SPSCRING_ULOCK_WAKE_OP, addr, 0); }

inline int ulock_wake_all(void* addr) { return __ulock_wake(SPSCRING_ULOCK_WAKE_ALL, addr, 0); }

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
      (void)details::ulock_wait(const_cast<std::atomic<T>*>(atomic),
                                static_cast<uint64_t>(static_cast<uint32_t>(old)), SPSCRING_ULOCK_TIMEOUT_NS);
    }
  }

#else
  // No os_sync available, use ulock with timeout re-check.
  while (atomic->load(std::memory_order_acquire) == old) {
    (void)details::ulock_wait(const_cast<std::atomic<T>*>(atomic),
                              static_cast<uint64_t>(static_cast<uint32_t>(old)), SPSCRING_ULOCK_TIMEOUT_NS);
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

}  // namespace sync
}  // namespace spscring

#endif  // __APPLE__
