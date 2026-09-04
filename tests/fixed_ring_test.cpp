#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <thread>
#include <vector>

#include <spscring/fixed_reader.hpp>
#include <spscring/fixed_writer.hpp>
#include <spscring/message_meta.hpp>

#include "spsc_test.hpp"

namespace {

constexpr std::uint32_t kItemSize = 64;
constexpr std::uint32_t kItemCount = 8;
constexpr std::uint32_t kCapacity = kItemSize * kItemCount;

using fixed_arena = spscring_test::arena<kCapacity>;

fixed_arena& make_ring() {
  static fixed_arena arena{};
  return arena;
}

TEST(FixedRingTest, InitRejectsNonMultipleCapacity) {
  spscring_test::arena<100> arena{};
  // item_size 32 does not divide capacity 100.
  EXPECT_FALSE(arena.init(spscring::layout_type::fixed, 8, 32));
}

TEST(FixedRingTest, WriteThenReadPreservesPayload) {
  fixed_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::fixed, 64, kItemSize));

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  char sent[kItemSize]{};
  std::snprintf(sent, sizeof(sent), "hello fixed ring");
  EXPECT_TRUE(writer.write(sent, kItemSize));

  std::uint32_t size = 0;
  const void* slot = reader.try_read(&size);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(size, kItemSize);
  EXPECT_EQ(std::memcmp(slot, sent, kItemSize), 0);
  reader.read_advance(1);

  EXPECT_EQ(reader.try_read(nullptr), nullptr);
}

TEST(FixedRingTest, FullRingRejectsWritesThenRecovers) {
  fixed_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::fixed, 64, kItemSize));

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  char sent[kItemSize]{};
  for (std::uint32_t i = 0; i < kItemCount; ++i) {
    sent[0] = static_cast<char>(i);
    EXPECT_TRUE(writer.write(sent, kItemSize)) << "write " << i;
  }
  EXPECT_FALSE(writer.write(sent, kItemSize));  // full

  // Consume one slot; exactly one new write must fit.
  char recv[kItemSize]{};
  EXPECT_TRUE(reader.read(recv, kItemSize));
  EXPECT_EQ(recv[0], '\0');
  EXPECT_TRUE(writer.write(sent, kItemSize));
  EXPECT_FALSE(writer.write(sent, kItemSize));  // full again

  // Drain: FIFO order 1..7 then the re-written 0.
  for (std::uint32_t i = 1; i < kItemCount; ++i) {
    EXPECT_TRUE(reader.read(recv, kItemSize)) << "read " << i;
    EXPECT_EQ(recv[0], static_cast<char>(i)) << "read " << i;
  }
  EXPECT_TRUE(reader.read(recv, kItemSize));
  EXPECT_EQ(recv[0], '\0');
  EXPECT_FALSE(reader.read(recv, kItemSize));
}

TEST(FixedRingTest, WrapAroundWorks) {
  fixed_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::fixed, 64, kItemSize));

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  // Write and consume 3 full passes; each message tags its logical index.
  for (std::uint64_t i = 0; i < 3 * kItemCount; ++i) {
    char sent[kItemSize]{};
    sent[0] = 'x';
    std::memcpy(sent + 1, &i, sizeof(i));
    EXPECT_TRUE(writer.write(sent, kItemSize));

    std::uint32_t size = 0;
    const void* slot = reader.try_read(&size);
    ASSERT_NE(slot, nullptr);
    std::uint64_t tag = 0;
    std::memcpy(&tag, static_cast<const char*>(slot) + 1, sizeof(tag));
    EXPECT_EQ(tag, i);
    reader.read_advance(1);
  }
}

TEST(FixedRingTest, RepeatedWrapPassesKeepOrder) {
  fixed_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::fixed, 64, kItemSize));

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  // Repeated full fill / drain cycles across the wrap boundary.
  char sent[kItemSize]{};
  char recv[kItemSize]{};
  for (std::uint32_t pass = 0; pass < 5; ++pass) {
    for (std::uint32_t i = 0; i < kItemCount; ++i) {
      sent[0] = static_cast<char>(i ^ pass);
      ASSERT_TRUE(writer.write(sent, kItemSize));
    }
    for (std::uint32_t i = 0; i < kItemCount; ++i) {
      ASSERT_TRUE(reader.read(recv, kItemSize));
      EXPECT_EQ(recv[0], static_cast<char>(i ^ pass));
    }
  }
}

TEST(FixedRingTest, SPSCThreadedStress) {
  fixed_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::fixed, 64, kItemSize));

  spscring::fixed_writer writer{arena.header()};
  spscring::fixed_reader reader{arena.header()};

  constexpr std::uint64_t kMessages = 20000;

  std::atomic<bool> producer_done{false};
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      char sent[kItemSize]{};
      std::memcpy(sent, &i, sizeof(i));
      while (!writer.write(sent, kItemSize)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<std::uint64_t> received;
  received.reserve(kMessages);
  std::thread consumer([&] {
    while (true) {
      std::uint32_t size = 0;
      if (const void* slot = reader.try_read(&size)) {
        std::uint64_t tag = 0;
        std::memcpy(&tag, slot, sizeof(tag));
        received.push_back(tag);
        reader.read_advance(1);
      } else if (producer_done.load(std::memory_order_acquire)) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  ASSERT_EQ(received.size(), kMessages);
  // FIFO order preserved.
  for (std::uint64_t i = 0; i < kMessages; ++i) {
    EXPECT_EQ(received[i], i);
  }
}

}  // namespace
