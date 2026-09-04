#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include <spscring/control_block.hpp>
#include <spscring/message_meta.hpp>
#include <spscring/reserve_result.hpp>
#include <spscring/internal/varlen_header.hpp>

namespace {

TEST(ControlBlockTest, PinnedAbiLayout) {
  using spscring::control_block;
  // SPSCRING_CACHE_LINE_SIZE is a preprocessor macro (64), not a namespace
  // entity — used directly below.

  static_assert(sizeof(spscring::meta) == 3 * SPSCRING_CACHE_LINE_SIZE);
  static_assert(offsetof(control_block, rb_meta) == SPSCRING_CACHE_LINE_SIZE);
  static_assert(offsetof(control_block, data_capacity) == 4 * SPSCRING_CACHE_LINE_SIZE);
  static_assert(sizeof(control_block) == 5 * SPSCRING_CACHE_LINE_SIZE);
  static_assert(std::is_standard_layout_v<control_block>);

  EXPECT_EQ(sizeof(spscring::meta), 3u * SPSCRING_CACHE_LINE_SIZE);
  EXPECT_EQ(offsetof(control_block, rb_meta), static_cast<std::size_t>(SPSCRING_CACHE_LINE_SIZE));
  EXPECT_EQ(offsetof(control_block, data_capacity), 4u * SPSCRING_CACHE_LINE_SIZE);
  EXPECT_EQ(sizeof(control_block), 5u * SPSCRING_CACHE_LINE_SIZE);
  EXPECT_EQ(spscring::expected_magic, 0x53505343u);  // "SPSC"
}

TEST(ControlBlockTest, InitAndValidateRoundTrip) {
  spscring::control_block header{};
  EXPECT_TRUE(spscring::init_control_block(header, 1024, spscring::layout_type::varlen, 8));
  EXPECT_TRUE(spscring::validate_control_block(header));
  EXPECT_EQ(header.magic, 0x53505343u);
  EXPECT_EQ(header.header_size, sizeof(spscring::control_block));
  EXPECT_EQ(header.layout_type, static_cast<std::uint32_t>(spscring::layout_type::varlen));
  EXPECT_EQ(header.data_capacity, 1024u);
  EXPECT_EQ(header.data_alignment, 8u);
  EXPECT_EQ(header.rb_meta.write_pos.load(), 0u);
  EXPECT_EQ(header.rb_meta.commit_seq.load(), 0u);
}

TEST(ControlBlockTest, InitRejectsBadGeometry) {
  spscring::control_block header{};
  // Zero alignment.
  EXPECT_FALSE(spscring::init_control_block(header, 1024, spscring::layout_type::fixed, 0, 64));
  // Non-power-of-two alignment.
  EXPECT_FALSE(spscring::init_control_block(header, 1024, spscring::layout_type::fixed, 24, 64));
  // Alignment beyond 64.
  EXPECT_FALSE(spscring::init_control_block(header, 1024, spscring::layout_type::fixed, 128, 64));
  // Zero capacity.
  EXPECT_FALSE(spscring::init_control_block(header, 0, spscring::layout_type::fixed, 8, 64));
  // Capacity not a multiple of alignment.
  EXPECT_FALSE(spscring::init_control_block(header, 1023, spscring::layout_type::fixed, 8, 64));
}

TEST(ControlBlockTest, ValidateRejectsCorruption) {
  spscring::control_block header{};
  ASSERT_TRUE(spscring::init_control_block(header, 1024, spscring::layout_type::varlen, 8));

  header.magic = 0xdeadbeef;
  EXPECT_FALSE(spscring::validate_control_block(header));

  // std::atomic members make assignment deleted; re-initialize in place.
  new (&header) spscring::control_block{};
  ASSERT_TRUE(spscring::init_control_block(header, 1024, spscring::layout_type::varlen, 8));
  header.version_major = 1;
  EXPECT_FALSE(spscring::validate_control_block(header));

  new (&header) spscring::control_block{};
  ASSERT_TRUE(spscring::init_control_block(header, 1024, spscring::layout_type::varlen, 8));
  header.header_size = 128;
  EXPECT_FALSE(spscring::validate_control_block(header));
}

TEST(ControlBlockTest, VarlenHeaderLayout) {
  // 4-byte slot_size + 4-byte payload_size + 24-byte message_meta.
  EXPECT_EQ(spscring::varlen_slot_header_size, 32u);
  EXPECT_EQ(offsetof(spscring::varlen_slot_header, meta), 8u);
  EXPECT_EQ(sizeof(spscring::message_meta), 24u);
}

TEST(ControlBlockTest, VarlenValidationNormalizesLegacyAlignment) {
  spscring::control_block header{};
  header.magic = spscring::expected_magic;
  header.version_major = spscring::version_major;
  header.header_size = sizeof(spscring::control_block);
  header.layout_type = static_cast<std::uint32_t>(spscring::layout_type::varlen);
  header.data_capacity = 512;
  header.data_alignment = 0;  // legacy segments
  EXPECT_TRUE(spscring::validate_varlen_control_block(header));
  EXPECT_EQ(header.data_alignment, 8u);
}

TEST(ReserveResultTest, BoolConversion) {
  spscring::reserve_result ok{};
  ok.status = spscring::reserve_status::ok;
  EXPECT_TRUE(static_cast<bool>(ok));

  spscring::reserve_result full{};
  full.status = spscring::reserve_status::full;
  EXPECT_FALSE(static_cast<bool>(full));

  EXPECT_STREQ(spscring::reserve_status_cstr(spscring::reserve_status::message_too_large),
               "message_too_large");
}

}  // namespace
