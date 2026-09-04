#pragma once

#include <cstdint>
#include <spscring/atomic_wait.hpp>
#include <spscring/internal/platform.hpp>
#include <thread>

namespace spscring {

// Exponential backoff for lock-free contention: CPU pause hints, escalating to
// thread yields, optionally finishing with a timed atomic wait.
class atomic_backoff {
 public:
  explicit atomic_backoff(std::uint32_t max_pauses = 256) noexcept : max_pauses_(max_pauses) {}

  void pause() noexcept {
    if (pause_count_ < yield_threshold_) {
      const std::uint32_t exp = pause_count_ < 8 ? pause_count_ : 8;
      for (std::uint32_t i = 0; i < (1u << exp); ++i) {
        SPSCRING_CPU_PAUSE();
      }
    } else {
      std::this_thread::yield();
    }
    ++pause_count_;
  }

  // Escalates to a timed wait on a 32-bit atomic (futex / WaitOnAddress /
  // os_sync_wait_on_address). Callers use this when pause() alone is not
  // enough — e.g. waiting for a peer process.
  template <typename T>
  bool wait(const std::atomic<T>* atomic, T old, int timeout_ms = 1) noexcept {
    static_assert(sizeof(T) == 4, "atomic_backoff::wait supports 32-bit atomics only");
    (void)max_pauses_;
    return atomic_wait_for(atomic, old, timeout_ms);
  }

  void reset() noexcept { pause_count_ = 0; }

 private:
  static constexpr std::uint32_t yield_threshold_ = 16;

  std::uint32_t pause_count_{0};
  std::uint32_t max_pauses_;
};

}  // namespace spscring
