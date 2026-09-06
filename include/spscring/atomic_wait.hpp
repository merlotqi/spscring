#pragma once

// Dispatches to the platform wait/notify primitives:
//   Linux  - futex (cross-process on shared mappings)
//   Win32  - WaitOnAddress / WakeByAddress (fallback: polling)
//   Darwin - os_sync_wait_on_address, or __ulock with re-check loop
//   other  - C++11 std::atomic_wait-style polling (atomic_poll)

#include <atomic>
#include <cstdint>
#include <spscring/internal/platform.hpp>
#include <thread>

#if defined(__linux__)
#include <spscring/internal/atomic_wait_futex.hpp>
#elif defined(_WIN32)
#include <spscring/internal/atomic_wait_win32.hpp>
#elif defined(__APPLE__)
#include <spscring/internal/atomic_wait_darwin.hpp>
#endif

namespace spscring {

// Portable fallback for platforms without a futex-like primitive: pure polling
// with an exponential CPU-pause backoff. Used by no-wait code paths everywhere,
// and as the block target on unsupported platforms.
template <typename T>
inline void atomic_poll(const std::atomic<T>* atomic, T old) {
  std::uint32_t iteration = 0;
  while (atomic->load(std::memory_order_acquire) == old) {
    ++iteration;
    const std::uint32_t exp = iteration < 9 ? iteration - 1 : 8;
    for (std::uint32_t i = 0; i < (1u << exp); ++i) {
      SPSCRING_CPU_PAUSE();
    }
    if (iteration % 32 == 0) {
      std::this_thread::yield();
    }
  }
}

template <typename T>
inline void atomic_wait(const std::atomic<T>* atomic, T old) {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  // Platform primitives re-check the value internally (futex returns on
  // spurious wakeups; Win32/Darwin wrap the load in a loop).
  namespace sync = spscring::sync;
  sync::atomic_wait(atomic, old);
#else
  atomic_poll(atomic, old);
#endif
}

template <typename T>
inline bool atomic_wait_for(const std::atomic<T>* atomic, T old, int timeout_ms) {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  namespace sync = spscring::sync;
  return sync::atomic_wait_for(atomic, old, timeout_ms);
#else
  // Negative timeout means "wait indefinitely" (futex semantics).
  if (timeout_ms < 0) {
    atomic_poll(atomic, old);
    return true;
  }
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(timeout_ms);
  while (atomic->load(std::memory_order_acquire) == old) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    atomic_poll(atomic, old);
  }
  return true;
#endif
}

template <typename T>
inline void atomic_notify_one(const std::atomic<T>* atomic) {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  namespace sync = spscring::sync;
  sync::atomic_notify_one(atomic);
#endif
}

template <typename T>
inline void atomic_notify_all(const std::atomic<T>* atomic) {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  namespace sync = spscring::sync;
  sync::atomic_notify_all(atomic);
#endif
}

// Notifies all waiters only when some thread is actually blocked on *atomic.
// Returns true when a wake syscall was issued, false when there was nothing to
// wake. On Linux/Win32/Darwin the wake primitives are no-ops without waiters,
// so this always notifies; the predicate exists so the C++11 fallback can skip
// the syscall. Safe against lost wakeups: a thread that decides to sleep
// rechecks the value after a raced notify and would then see the update.
template <typename T>
inline bool atomic_notify_all_if_waiters(const std::atomic<T>* atomic) {
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  namespace sync = spscring::sync;
  return sync::atomic_notify_all_if_waiters(atomic);
#else
  return atomic->load(std::memory_order_acquire);
#endif
}

}  // namespace spscring
