// Regression + behavior tests for spscring::sync atomic wait/notify.
//
// Guards the platform backends (Linux futex / macOS os_sync + __ulock /
// Windows WaitOnAddress): atomic_wait_for must exist in the spscring::sync
// namespace for every supported platform and follow futex semantics — false
// only on timeout, true when the value may have changed.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <spscring/atomic_wait.hpp>
#include <thread>

namespace {

using spscring::sync::atomic_notify_all;
using spscring::sync::atomic_wait;
using spscring::sync::atomic_wait_for;

using steady = std::chrono::steady_clock;

std::int64_t elapsed_ms(const steady::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count();
}

TEST(AtomicWaitTest, WaitForTimesOutWhenValueUnchanged) {
  std::atomic<std::uint32_t> v{0};
  const auto t0 = steady::now();
  // 200 ms deadline; generous upper bound to tolerate slow CI machines.
  EXPECT_FALSE(atomic_wait_for(&v, 0u, 200));
  const auto dt = elapsed_ms(t0);
  EXPECT_GE(dt, 150);
  EXPECT_LT(dt, 2000);
}

TEST(AtomicWaitTest, WaitForReturnsTrueWhenNotified) {
  std::atomic<std::uint32_t> v{0};
  std::thread th([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v.store(1, std::memory_order_release);
    atomic_notify_all(&v);
  });
  const auto t0 = steady::now();
  EXPECT_TRUE(atomic_wait_for(&v, 0u, 5000));
  EXPECT_LT(elapsed_ms(t0), 2000);
  th.join();
}

TEST(AtomicWaitTest, WaitForReturnsTrueWhenValueChangedBeforeWait) {
  // The kernel compares the value at entry: a change that lands before the
  // wait (even without a notify) must return immediately. This is the race
  // the ring's seq-notify protocol relies on.
  std::atomic<std::uint32_t> v{0};
  v.store(42, std::memory_order_release);
  const auto t0 = steady::now();
  EXPECT_TRUE(atomic_wait_for(&v, 0u, 5000));
  EXPECT_LT(elapsed_ms(t0), 1000);
}

TEST(AtomicWaitTest, NegativeTimeoutWaitsIndefinitelyUntilNotified) {
  std::atomic<std::uint32_t> v{0};
  std::thread th([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v.store(7, std::memory_order_release);
    atomic_notify_all(&v);
  });
  const auto t0 = steady::now();
  EXPECT_TRUE(atomic_wait_for(&v, 0u, -1));
  EXPECT_LT(elapsed_ms(t0), 2000);
  th.join();
}

TEST(AtomicWaitTest, WaitBlocksUntilNotified) {
  std::atomic<std::uint32_t> v{0};
  std::thread th([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    v.store(3, std::memory_order_release);
    atomic_notify_all(&v);
  });
  atomic_wait(&v, 0u);
  EXPECT_EQ(v.load(), 3u);
  th.join();
}

TEST(AtomicWaitTest, NotifyWithoutWaitersIsHarmless) {
  std::atomic<std::uint32_t> v{0};
  atomic_notify_all(&v);
  EXPECT_EQ(v.load(), 0u);
}

}  // namespace
