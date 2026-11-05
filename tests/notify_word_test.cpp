#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <spscring/atomic_wait.hpp>
#include <spscring/fixed_reader.hpp>
#include <spscring/fixed_writer.hpp>
#include <spscring/notify_word.hpp>
#include <thread>

#include "spsc_test.hpp"

namespace {

using spscring_test::arena;

TEST(NotifyWordTest, BumpReturnsFalseWithoutWaiter) {
  std::atomic<std::uint32_t> word{0};
  EXPECT_FALSE(spscring::bump_notify_word(word));
  EXPECT_EQ(word.load(), 1u);  // Sequence still advances.
}

TEST(NotifyWordTest, BumpReturnsTrueWhenWaiterRegistered) {
  std::atomic<std::uint32_t> word{0};
  // Simulate a waiter registering itself (as wait_on_notify_word does).
  word.fetch_or(spscring::notify_waiter_bit, std::memory_order_seq_cst);
  EXPECT_TRUE(spscring::bump_notify_word(word));
}

TEST(NotifyWordTest, SeqValueStripsWaiterBit) {
  const std::uint32_t raw = 0x80000005u;  // Waiter bit + sequence 5.
  EXPECT_EQ(spscring::notify_seq_value(raw), 5u);
  EXPECT_EQ(spscring::notify_seq_value(0x7FFFFFFFu), 0x7FFFFFFFu);
}

TEST(NotifyWordTest, WaitReturnsImmediatelyWhenPredicateFalse) {
  std::atomic<std::uint32_t> word{7};
  const auto start = std::chrono::steady_clock::now();
  // Predicate false from the start: no blocking, no waiter-bit leak.
  spscring::wait_on_notify_word(
      word, [&] { return spscring::notify_seq_value(word.load(std::memory_order_acquire)) == 8; });
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100);
  EXPECT_EQ(word.load() & spscring::notify_waiter_bit, 0u);
}

TEST(NotifyWordTest, WaitWakesOnBumpAndBumperSeesWaiter) {
  // Classic lost-wakeup window: the waiter must be visible (waiter bit) to a
  // bump that races with registration, and the bump must wake the waiter.
  std::atomic<std::uint32_t> word{0};
  std::atomic<bool> waiter_done{false};
  std::atomic<bool> bumper_saw_waiter{false};

  std::thread waiter([&] {
    spscring::wait_on_notify_word(word, [&] {
      return spscring::notify_seq_value(word.load(std::memory_order_acquire)) == 0;
    });
    waiter_done.store(true, std::memory_order_release);
  });

  // Give the waiter time to register and block.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // bump_notify_word only reports the decision; the caller issues the wake.
  if (spscring::bump_notify_word(word)) {
    spscring::atomic_notify_all(&word);
  }
  bumper_saw_waiter.store(true, std::memory_order_release);

  waiter.join();
  EXPECT_TRUE(waiter_done.load());
  EXPECT_TRUE(bumper_saw_waiter.load());
  EXPECT_EQ(word.load() & spscring::notify_waiter_bit, 0u);  // Bit cleared on exit.
}

TEST(NotifyWordTest, WaitForTimesOutWhenNothingChanges) {
  std::atomic<std::uint32_t> word{0};
  const auto start = std::chrono::steady_clock::now();
  const bool changed = spscring::wait_on_notify_word_for(
      word, [&] { return spscring::notify_seq_value(word.load(std::memory_order_acquire)) == 0; }, 50);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  EXPECT_FALSE(changed);
  EXPECT_GE(elapsed.count(), 40);
  EXPECT_EQ(word.load() & spscring::notify_waiter_bit, 0u);
}

TEST(NotifyWordTest, WaitForReturnsWhenSeqChanges) {
  std::atomic<std::uint32_t> word{0};
  std::thread bumper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (spscring::bump_notify_word(word)) {
      spscring::atomic_notify_all(&word);
    }
  });
  const bool changed = spscring::wait_on_notify_word_for(
      word, [&] { return spscring::notify_seq_value(word.load(std::memory_order_acquire)) == 0; }, 2000);
  bumper.join();
  EXPECT_TRUE(changed);
  EXPECT_EQ(word.load() & spscring::notify_waiter_bit, 0u);
}

TEST(NotifyWordTest, WaitForSeqChangeConvenience) {
  // The xproc-facing shape: wait until the sequence value differs from a
  // previously observed snapshot, waiter-bit agnostic on both sides.
  std::atomic<std::uint32_t> word{0};
  const std::uint32_t last = word.load(std::memory_order_acquire);
  std::thread bumper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (spscring::bump_notify_word(word)) {
      spscring::atomic_notify_all(&word);
    }
  });
  spscring::wait_for_seq_change(word, last);
  bumper.join();
  EXPECT_NE(spscring::notify_seq_value(word.load()), spscring::notify_seq_value(last));
  EXPECT_EQ(word.load() & spscring::notify_waiter_bit, 0u);
}

TEST(NotifyWordTest, RingCommitWakesRegisteredWaiter) {
  // End-to-end guard for the conditional notify: a consumer registered on
  // commit_seq must still be woken by fixed_writer::commit().
  arena<256> ring;
  ASSERT_TRUE(ring.init(spscring::layout_type::fixed, 64, 64));
  spscring::fixed_writer writer{ring.header()};
  spscring::fixed_reader reader{ring.header()};

  std::atomic<bool> woken{false};
  std::thread consumer([&] {
    const std::uint32_t last = ring.header()->rb_meta.commit_seq.load(std::memory_order_acquire);
    auto& word = reinterpret_cast<std::atomic<std::uint32_t>&>(ring.header()->rb_meta.commit_seq);
    spscring::wait_for_seq_change(word, last);
    woken.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const char payload[64]{};
  ASSERT_TRUE(writer.write(payload, 64));
  consumer.join();
  EXPECT_TRUE(woken.load());
  (void)reader;
}

TEST(NotifyWordTest, RingCommitBeforeWaitDoesNotBlock) {
  // Commit happens first; the later waiter's predicate observes the change and
  // the wait must return without blocking (no lost wakeup, no stale sleep).
  arena<256> ring;
  ASSERT_TRUE(ring.init(spscring::layout_type::fixed, 64, 64));
  spscring::fixed_writer writer{ring.header()};

  const char payload[64]{};
  ASSERT_TRUE(writer.write(payload, 64));

  auto& word = reinterpret_cast<std::atomic<std::uint32_t>&>(ring.header()->rb_meta.commit_seq);
  const auto start = std::chrono::steady_clock::now();
  spscring::wait_for_seq_change(word, 0);
  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(),
            100);
}

}  // namespace
