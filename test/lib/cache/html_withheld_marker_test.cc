// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "lib/cache/html_withheld_marker.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

std::array<uint8_t, kHtmlParentHashBytes> Hash(uint8_t fill) {
  std::array<uint8_t, kHtmlParentHashBytes> h;
  h.fill(fill);
  return h;
}

std::span<const uint8_t> Bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

TEST(HtmlWithheldMarkerTest, RoundTripsEveryReason) {
  for (uint8_t r = 1; r <= kHtmlWithheldReasonMax; ++r) {
    HtmlWithheldMarker m;
    m.reason = static_cast<HtmlWithheldReason>(r);
    m.deriver_version = 0x01020304;
    m.parent_hash = Hash(0x42);
    const std::string payload = EncodeHtmlWithheldMarker(m);
    ASSERT_EQ(payload.size(), kHtmlWithheldMarkerBytes);
    EXPECT_EQ(static_cast<uint8_t>(payload[0]), kHtmlWithheldMarkerFormat);
    EXPECT_EQ(static_cast<uint8_t>(payload[2]), 0x04);  // little-endian
    const auto back = DecodeHtmlWithheldMarker(Bytes(payload));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->reason, m.reason);
    EXPECT_EQ(back->deriver_version, m.deriver_version);
    EXPECT_EQ(back->parent_hash, m.parent_hash);
    EXPECT_NE(HtmlWithheldReasonName(m.reason), "unknown");
  }
}

TEST(HtmlWithheldMarkerTest, AppliesOnlyToItsHashAndTheCurrentDeriver) {
  HtmlWithheldMarker m;
  m.deriver_version = kHtmlMarkdownDeriverVersion;
  m.parent_hash = Hash(0x11);
  const std::string payload = EncodeHtmlWithheldMarker(m);
  const auto h1 = Hash(0x11);
  const auto h2 = Hash(0x22);
  const auto zero = Hash(0);
  EXPECT_TRUE(HtmlWithheldMarkerApplies(Bytes(payload), h1));
  EXPECT_FALSE(HtmlWithheldMarkerApplies(Bytes(payload), h2));
  EXPECT_FALSE(HtmlWithheldMarkerApplies(Bytes(payload), zero));
  m.deriver_version = kHtmlMarkdownDeriverVersion - 1;
  EXPECT_FALSE(
      HtmlWithheldMarkerApplies(Bytes(EncodeHtmlWithheldMarker(m)), h1));
  m.deriver_version = kHtmlMarkdownDeriverVersion + 1;
  EXPECT_FALSE(
      HtmlWithheldMarkerApplies(Bytes(EncodeHtmlWithheldMarker(m)), h1));
}

TEST(HtmlWithheldMarkerTest, AnythingElseIsNotAMarker) {
  HtmlWithheldMarker m;
  m.parent_hash = Hash(0x11);
  const std::string good = EncodeHtmlWithheldMarker(m);
  EXPECT_FALSE(DecodeHtmlWithheldMarker(Bytes(good.substr(1))).has_value());
  EXPECT_FALSE(DecodeHtmlWithheldMarker(Bytes(good + "x")).has_value());
  std::string bad = good;
  bad[0] = 2;  // unknown format version
  EXPECT_FALSE(DecodeHtmlWithheldMarker(Bytes(bad)).has_value());
  bad = good;
  bad[1] = 0;  // no reason
  EXPECT_FALSE(DecodeHtmlWithheldMarker(Bytes(bad)).has_value());
  bad[1] = static_cast<char>(kHtmlWithheldReasonMax + 1);
  EXPECT_FALSE(DecodeHtmlWithheldMarker(Bytes(bad)).has_value());
  m.parent_hash = Hash(0);
  EXPECT_FALSE(
      DecodeHtmlWithheldMarker(Bytes(EncodeHtmlWithheldMarker(m))).has_value());
  EXPECT_FALSE(DecodeHtmlWithheldMarker({}).has_value());
  // The format version sits below every entry-metadata version (3 and up).
  EXPECT_LT(kHtmlWithheldMarkerFormat, 3);
}

}  // namespace
}  // namespace pagespeed
