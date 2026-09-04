#pragma once

// Win32 atomic wait/notify via WaitOnAddress / WakeByAddress*.
//
// The APIs live in api-ms-win-core-synch-l1-2-0.dll (kernelbase), so they are
// resolved dynamically for Windows 7/8 compatibility; a polling fallback with
// exponential backoff is used when they are unavailable.
//
// WaitOnAddress compares bytewise over the given size, so it also works for
// 64-bit words; the ring only ever waits on 32-bit words.
#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cwchar>
#include <spscring/platform.hpp>

namespace spscring {
namespace details {

using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressFn = VOID(WINAPI*)(PVOID);

struct wait_api {
  WaitOnAddressFn wait_on_address{nullptr};
  WakeByAddressFn wake_one{nullptr};
  WakeByAddressFn wake_all{nullptr};
};

struct wait_tuning {
  std::uint32_t spin_count{1000};
  std::uint32_t yield_count{10};
  DWORD wait_timeout_ms{1};
  std::uint32_t polling_sleep_ms{1};
};

inline bool try_read_env_u32(const wchar_t* name, std::uint32_t* out) {
  if (out == nullptr) {
    return false;
  }
  wchar_t buf[32] = {};
  const DWORD len = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (len == 0 || len >= static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]))) {
    return false;
  }
  wchar_t* end = nullptr;
  const unsigned long parsed = std::wcstoul(buf, &end, 10);
  if (end == buf || *end != L'\0') {
    return false;
  }
  *out = static_cast<std::uint32_t>(parsed);
  return true;
}

inline std::uint32_t clamp_u32(std::uint32_t value, std::uint32_t min_value, std::uint32_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

inline wait_api load_wait_api() {
  wait_api api{};
  const HMODULE modules[] = {::GetModuleHandleW(L"kernel32.dll"), ::GetModuleHandleW(L"KernelBase.dll")};
  for (const HMODULE module : modules) {
    if (module == nullptr) {
      continue;
    }
    if (api.wait_on_address == nullptr) {
      api.wait_on_address = reinterpret_cast<WaitOnAddressFn>(::GetProcAddress(module, "WaitOnAddress"));
    }
    if (api.wake_one == nullptr) {
      api.wake_one = reinterpret_cast<WakeByAddressFn>(::GetProcAddress(module, "WakeByAddressSingle"));
    }
    if (api.wake_all == nullptr) {
      api.wake_all = reinterpret_cast<WakeByAddressFn>(::GetProcAddress(module, "WakeByAddressAll"));
    }
  }
  return api;
}

inline const wait_api& native_wait_api() {
  static const wait_api api = load_wait_api();
  return api;
}

inline wait_tuning load_wait_tuning() {
  wait_tuning tuning{};

  std::uint32_t value = 0;
  if (try_read_env_u32(L"SPSCRING_WIN32_WAIT_SPIN", &value)) {
    tuning.spin_count = clamp_u32(value, 0, 1000000);
  }
  if (try_read_env_u32(L"SPSCRING_WIN32_WAIT_YIELD", &value)) {
    tuning.yield_count = clamp_u32(value, 0, 100000);
  }
  if (try_read_env_u32(L"SPSCRING_WIN32_WAIT_TIMEOUT_MS", &value)) {
    tuning.wait_timeout_ms = static_cast<DWORD>(clamp_u32(value, 0, 60000));
  }
  if (try_read_env_u32(L"SPSCRING_WIN32_POLL_SLEEP_MS", &value)) {
    tuning.polling_sleep_ms = clamp_u32(value, 0, 60000);
  }

  return tuning;
}

inline const wait_tuning& native_wait_tuning() {
  static const wait_tuning tuning = load_wait_tuning();
  return tuning;
}

inline bool has_native_wait_api() {
  const wait_api& api = native_wait_api();
  return api.wait_on_address != nullptr && api.wake_one != nullptr && api.wake_all != nullptr;
}

template <typename T>
inline volatile void* wait_address(const std::atomic<T>* atomic) {
  return const_cast<volatile void*>(reinterpret_cast<volatile const void*>(atomic));
}

template <typename T>
inline void* wake_address(const std::atomic<T>* atomic) {
  return const_cast<void*>(reinterpret_cast<const void*>(atomic));
}

template <typename T>
inline void polling_wait(const std::atomic<T>* atomic, T old) {
  const wait_tuning& tuning = native_wait_tuning();
  std::uint32_t iteration = 0;
  while (true) {
    const T cur = atomic->load(std::memory_order_acquire);
    if (cur != old) {
      return;
    }
    ++iteration;
    if (iteration <= tuning.spin_count) {
      // Exponential backoff: 1, 2, 4, ..., capped at 256 pauses per iter.
      const std::uint32_t exp = (std::min)(iteration - 1, 8u);
      const std::uint32_t delay = 1u << exp;
      for (std::uint32_t i = 0; i < delay; ++i) {
        SPSCRING_CPU_PAUSE();
      }
    } else if (iteration <= tuning.spin_count + tuning.yield_count) {
      ::SwitchToThread();
    } else {
      ::Sleep(tuning.polling_sleep_ms);
    }
  }
}

}  // namespace details

namespace sync {

// atomic_wait is a pure blocking primitive: spin/yield backoff belongs in
// atomic_backoff. Callers should pause/yield before blocking.
template <typename T>
inline void atomic_wait(const std::atomic<T>* atomic, T old) {
  static_assert(sizeof(T) <= 8, "atomic_wait(win32): only 64-bit or smaller atomics are supported");

  if (!details::has_native_wait_api()) {
    details::polling_wait(atomic, old);
    return;
  }

  const details::wait_api& api = details::native_wait_api();
  const details::wait_tuning& tuning = details::native_wait_tuning();
  while (atomic->load(std::memory_order_acquire) == old) {
    api.wait_on_address(details::wait_address(atomic), &old, sizeof(T), tuning.wait_timeout_ms);
  }
}

template <typename T>
inline bool atomic_wait_for(const std::atomic<T>* atomic, T old, int timeout_ms) {
  static_assert(sizeof(T) <= 8, "atomic_wait_for(win32): only 64-bit or smaller atomics are supported");

  if (!details::has_native_wait_api()) {
    const DWORD start = ::GetTickCount();
    while (atomic->load(std::memory_order_acquire) == old) {
      if (static_cast<int>(::GetTickCount() - start) >= timeout_ms) {
        return false;
      }
      details::polling_wait(atomic, old);
    }
    return true;
  }

  const details::wait_api& api = details::native_wait_api();
  while (atomic->load(std::memory_order_acquire) == old) {
    const BOOL ok = api.wait_on_address(details::wait_address(atomic), &old, sizeof(T),
                                        static_cast<DWORD>(timeout_ms > 0 ? timeout_ms : 1));
    if (!ok && ::GetLastError() == ERROR_TIMEOUT) {
      return atomic->load(std::memory_order_acquire) != old;
    }
  }
  return true;
}

template <typename T>
inline void atomic_notify_one(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) <= 8, "atomic_notify_one(win32): only 64-bit or smaller atomics are supported");
  if (details::has_native_wait_api()) {
    details::native_wait_api().wake_one(details::wake_address(atomic));
  }
}

template <typename T>
inline void atomic_notify_all(const std::atomic<T>* atomic) {
  static_assert(sizeof(T) <= 8, "atomic_notify_all(win32): only 64-bit or smaller atomics are supported");
  if (details::has_native_wait_api()) {
    details::native_wait_api().wake_all(details::wake_address(atomic));
  }
}

// WakeByAddress* is a no-op when nobody waits on the address, so no recheck is
// required; returns true for interface parity with the C++11 fallback.
template <typename T>
inline bool atomic_notify_all_if_waiters(const std::atomic<T>*) {
  return true;
}

}  // namespace sync
}  // namespace spscring

#endif  // _WIN32
