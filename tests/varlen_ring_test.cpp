#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <spscring/message_meta.hpp>
#include <spscring/varlen_reader.hpp>
#include <spscring/varlen_writer.hpp>

#include "spsc_test.hpp"

namespace {

constexpr std::uint32_t kCapacity = 512;

using varlen_arena = spscing_test::arena<kCapacity>;

varlen_arena& make_ring() {
  static varlen_arena arena{};
  return arena;
}

TEST(VarlenRingTest, MessageTooLargeFails) {
  varlen_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{arena.header()};
  const auto status = writer.try_reserve(kCapacity).status;
  EXPECT_EQ(status, spscring::reserve_status::message_too_large);
}

TEST(VarlenRingTest, WriteThenReadPreservesPayloadAndMeta) {
  varlen_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{arena.header()};
  spscring::varlen_reader reader{arena.header()};

  const std::string sent = "hello varlen ring";
  spscring::message_meta meta{};
  meta.user_data = 42;
  meta.schema_id = 7;
  meta.flags = spscring::flag_priority_high;

  EXPECT_TRUE(writer.write(static_cast<std::uint32_t>(sent.size()), sent.data(), meta));

  std::vector<char> recv(sent.size());
  spscring::message_meta recv_meta{};
  std::uint32_t recv_size = 0;
  EXPECT_TRUE(reader.read(recv.data(), static_cast<std::uint32_t>(recv.size()), &recv_size, &recv_meta));
  EXPECT_EQ(recv_size, sent.size());
  EXPECT_EQ(std::memcmp(recv.data(), sent.data(), sent.size()), 0);
  EXPECT_EQ(recv_meta.user_data, 42u);
  EXPECT_EQ(recv_meta.schema_id, 7u);
  EXPECT_EQ(recv_meta.flags, spscring::flag_priority_high);

  EXPECT_FALSE(reader.read(recv.data(), static_cast<std::uint32_t>(recv.size()), &recv_size, &recv_meta));
}

TEST(VarlenRingTest, MixedSizesAndFifoOrder) {
  varlen_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{arena.header()};
  spscring::varlen_reader reader{arena.header()};

  std::vector<std::string> sent;
  for (std::uint32_t i = 1; i <= 10; ++i) {
    sent.emplace_back(i, static_cast<char>('a' + i));
  }
  for (const auto& msg : sent) {
    ASSERT_TRUE(writer.write(static_cast<std::uint32_t>(msg.size()), msg.data()));
  }

  for (const auto& expected : sent) {
    std::vector<char> recv(expected.size());
    std::uint32_t recv_size = 0;
    ASSERT_TRUE(reader.read(recv.data(), static_cast<std::uint32_t>(recv.size()), &recv_size));
    EXPECT_EQ(recv_size, expected.size());
    EXPECT_EQ(std::memcmp(recv.data(), expected.data(), expected.size()), 0);
  }
}

TEST(VarlenRingTest, SmallSegmentWrapUsesDummyHeader) {
  // 64 bytes capacity with 8-byte alignment: header (24) + 40-byte payload fits
  // exactly once; the next reserve must wrap via the dummy path.
  spscing_test::arena<64> small{};
  ASSERT_TRUE(small.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{small.header()};
  spscring::varlen_reader reader{small.header()};

  const std::uint32_t payload = 40;  // 24 + 40 == 64 == capacity
  char sent[40]{};
  std::memcpy(sent, "wrap", 4);
  ASSERT_TRUE(writer.write(payload, sent));

  // Ring is now exactly full; nothing fits, not even the smallest message.
  EXPECT_EQ(writer.try_reserve(1).status, spscring::reserve_status::full);

  char recv[40]{};
  std::uint32_t recv_size = 0;
  ASSERT_TRUE(reader.read(recv, payload, &recv_size));
  EXPECT_EQ(recv_size, payload);
  EXPECT_EQ(std::memcmp(recv, sent, payload), 0);

  // After the drain the whole ring is usable again (the wrap padding that was
  // committed at the end has been consumed along with the real message).
  EXPECT_TRUE(writer.write(payload, sent));
}

TEST(VarlenRingTest, TwoPhaseReserveCommit) {
  varlen_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{arena.header()};
  spscring::varlen_reader reader{arena.header()};

  const spscring::reserve_result res = writer.try_reserve(4);
  ASSERT_EQ(res.status, spscring::reserve_status::ok);

  // Not visible before commit.
  char recv[16];
  std::uint32_t recv_size = 0;
  EXPECT_FALSE(reader.read(recv, sizeof(recv), &recv_size));

  std::memcpy(res.payload, "abcd", 4);
  writer.commit(res.position);

  EXPECT_TRUE(reader.read(recv, sizeof(recv), &recv_size));
  EXPECT_EQ(recv_size, 4u);
  EXPECT_EQ(std::memcmp(recv, "abcd", 4), 0);
}

TEST(VarlenRingTest, SPSCThreadedStress) {
  varlen_arena& arena = make_ring();
  ASSERT_TRUE(arena.init(spscring::layout_type::varlen, 8));

  spscring::varlen_writer writer{arena.header()};
  spscring::varlen_reader reader{arena.header()};

  constexpr std::uint64_t kMessages = 5000;
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      const std::string msg = "m" + std::to_string(i);
      spscring::message_meta meta{};
      meta.user_data = i;
      while (!writer.write(static_cast<std::uint32_t>(msg.size()), msg.data(), meta)) {
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::vector<std::uint64_t> received;
  std::thread consumer([&] {
    std::vector<char> recv(128);
    std::uint32_t recv_size = 0;
    spscring::message_meta meta{};
    while (true) {
      if (reader.read(recv.data(), static_cast<std::uint32_t>(recv.size()), &recv_size, &meta)) {
        received.push_back(meta.user_data);
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
  for (std::uint64_t i = 0; i < kMessages; ++i) {
    EXPECT_EQ(received[i], i);
  }
}

}  // namespace
