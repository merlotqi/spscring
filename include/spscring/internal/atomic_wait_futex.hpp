#pragma once

// Linux futex-based atomic wait/notify (cross-process capable: futexes operate
// on the physical page, so MAP_SHARED mappings work).
#if defined(__linux__)

#include <atomic>
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128
#endif
#ifndef FUTEX_WAKE_PRIVATE
#define FUTEX_WAKE_PRIVATE 129
#endif

namespace spscring {
namespace sync {

namespace details {

inline long futex_syscall(int* addr, int op, int val, const timespec* timeout = nullptr) {
  return ::syscall(SYS_futex, addr, op, val, timeout, nullptr, 0);
}

}  // namespace details

// Blocks the calling thread while *atomic == expected. 32-bit values only (the
// futex word size). Spurious wakeups are handled by the caller's re-check loop.
template <typename T>
inline void atomic_wait(const std::atomic<T>* atomic, T expected) {
  static_assert(sizeof(T) == 4, "atomic_wait(futex): only 32-bit atomics are supported");
  auto* addr = reinterpret_cast<int*>(const_cast<std::atomic<T>*>(atomic));
  while (atomic->load(std::memory_order_acquire) == expected) {
    details::futex_syscall(addr, FUTEX_WAIT_PRIVATE, static_cast<int>(expected), nullptr);
  }
}

// Blocks with a timeout; returns false when the call timed out, true when the
// value may have changed. The caller must always re-check the value.
template <typename T>
inline bool atomic_wait_for(const std::atomic<T>* atomic, T expected, int timeout_ms) {
  static_assert(sizeof(T) == 4, "atomic_wait_for(futex): only 32-bit atomics are supported");
  auto* addr = reinterpret_cast<int*>(const_cast<std::atomic<T>*>(atomic));
  if (atomic->load(std::memory_order_acquire) != expected) {
    return true;
  }
  timespec ts{};
  ts.tv_sec = timeout_ms / 1000;
  ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
  const long rc = details::futex_syscall(addr, FUTEX_WAIT_PRIVATE, static_cast<int>(expected), &ts);
  return !(rc != 0 && errno == ETIMEDOUT);
}

template <typename T>
inline void atomic_notify_one(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) == 4, "atomic_notify_one(futex): only 32-bit atomics are supported");
  auto* addr = reinterpret_cast<int*>(const_cast<std::atomic<T>*>(atomic));
  details::futex_syscall(addr, FUTEX_WAKE_PRIVATE, 1);
}

template <typename T>
inline void atomic_notify_all(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) == 4, "atomic_notify_all(futex): only 32-bit atomics are supported");
  auto* addr = reinterpret_cast<int*>(const_cast<std::atomic<T>*>(atomic));
  details::futex_syscall(addr, FUTEX_WAKE_PRIVATE, INT32_MAX);
}

template <typename T>
inline bool atomic_notify_all_if_waiters(const std::atomic<T>*) {
  // Waking without waiters is a cheap no-op syscall on Linux; no need for the
  // recheck dance Windows requires. Returns true for interface parity.
  return true;
}

}  // namespace sync
}  // namespace spscring

#endif  // __linux__
