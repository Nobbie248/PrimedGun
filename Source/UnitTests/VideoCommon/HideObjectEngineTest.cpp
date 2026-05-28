// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "VideoCommon/HideObjectEngine.h"

namespace
{
std::array<u8, 20> MakeIncrementingBytes()
{
  std::array<u8, 20> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<u8>(i + 1);
  return bytes;
}
}  // namespace

TEST(HideObjectEngine, BuildEntryFromPrefixUsesLargestSupportedWidth)
{
  const auto bytes = MakeIncrementingBytes();

  const auto one_byte = HideObjectEngine::BuildEntryFromPrefix(bytes.data(), 1);
  ASSERT_TRUE(one_byte);
  EXPECT_EQ(one_byte->type, HideObjectEngine::HideObjectType::Bits8);
  EXPECT_EQ(one_byte->value_upper, 0u);
  EXPECT_EQ(one_byte->value_lower, 0x01u);

  const auto three_bytes = HideObjectEngine::BuildEntryFromPrefix(bytes.data(), 3);
  ASSERT_TRUE(three_bytes);
  EXPECT_EQ(three_bytes->type, HideObjectEngine::HideObjectType::Bits24);
  EXPECT_EQ(three_bytes->value_upper, 0u);
  EXPECT_EQ(three_bytes->value_lower, 0x010203u);

  const auto nine_bytes = HideObjectEngine::BuildEntryFromPrefix(bytes.data(), 9);
  ASSERT_TRUE(nine_bytes);
  EXPECT_EQ(nine_bytes->type, HideObjectEngine::HideObjectType::Bits72);
  EXPECT_EQ(nine_bytes->value_upper, 0x01u);
  EXPECT_EQ(nine_bytes->value_lower, 0x0203040506070809ull);

  const auto full_width = HideObjectEngine::BuildEntryFromPrefix(bytes.data(), bytes.size());
  ASSERT_TRUE(full_width);
  EXPECT_EQ(full_width->type, HideObjectEngine::HideObjectType::Bits128);
  EXPECT_EQ(full_width->value_upper, 0x0102030405060708ull);
  EXPECT_EQ(full_width->value_lower, 0x090A0B0C0D0E0F10ull);
}

TEST(HideObjectEngine, CapturedEntriesAreDoubleBufferedAndDeduplicated)
{
  const auto bytes = MakeIncrementingBytes();
  auto& engine = HideObjectEngine::Engine::GetInstance();

  engine.SetCaptureEnabled(true);
  engine.CaptureVertexPrefix(bytes.data(), 16);
  engine.CaptureVertexPrefix(bytes.data(), 16);
  engine.CommitCapturedPrefixesForDraw(5);
  engine.CaptureVertexPrefix(bytes.data() + 4, 4);
  engine.CommitCapturedPrefixesForDraw(6);

  EXPECT_TRUE(engine.GetCapturedEntriesForDrawSequences({5, 6}).entries.empty());

  engine.OnFrameEnd();
  const auto captured = engine.GetCapturedEntriesForDrawSequences({5, 6, 5});

  EXPECT_EQ(captured.requested_draws, 2u);
  EXPECT_EQ(captured.matched_draws, 2u);
  EXPECT_EQ(captured.prefixes_seen, 3u);
  ASSERT_EQ(captured.entries.size(), 2u);
  EXPECT_EQ(captured.shorter_than_128bit, 1u);

  EXPECT_EQ(captured.entries[0].type, HideObjectEngine::HideObjectType::Bits128);
  EXPECT_EQ(captured.entries[0].value_upper, 0x0102030405060708ull);
  EXPECT_EQ(captured.entries[0].value_lower, 0x090A0B0C0D0E0F10ull);
  EXPECT_EQ(captured.entries[1].type, HideObjectEngine::HideObjectType::Bits32);
  EXPECT_EQ(captured.entries[1].value_lower, 0x05060708u);

  engine.SetCaptureEnabled(false);
}
