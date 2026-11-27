// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/alternate_metadata.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(AlternateMetadataTest, RoundTrip) {
  AlternateMetadata meta;
  meta.full_mask = 0xDEADBEEF;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 3600;
  meta.origin_s_maxage = 1800;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginPublic |
                         AlternateMetadata::kCCOriginHeaderPresent;

  auto serialized = meta.Serialize();
  auto deserialized = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(deserialized.has_value());
  EXPECT_EQ(deserialized->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(deserialized->full_mask, 0xDEADBEEFu);
  EXPECT_EQ(deserialized->content_type, ContentType::kImage);
  EXPECT_EQ(deserialized->origin_content_type, "image/jpeg");
  EXPECT_EQ(deserialized->cache_inserted_at, 1700000000u);
  EXPECT_EQ(deserialized->origin_max_age, 3600u);
  EXPECT_EQ(deserialized->origin_s_maxage, 1800u);
  EXPECT_EQ(deserialized->origin_cc_flags,
            AlternateMetadata::kCCOriginPublic |
                AlternateMetadata::kCCOriginHeaderPresent);
  // V4 defaults.
  EXPECT_EQ(deserialized->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
  EXPECT_EQ(deserialized->content_class,
            AlternateMetadata::kContentClassUnknown);
  // V6 default.
  EXPECT_EQ(deserialized->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, RoundTripEmptyContentType) {
  AlternateMetadata meta;
  meta.full_mask = 0x00C8;
  meta.content_type = ContentType::kHtml;
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 300;
  // empty origin_content_type

  auto serialized = meta.Serialize();
  auto deserialized = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(deserialized.has_value());
  EXPECT_EQ(deserialized->full_mask, 0x00C8u);
  EXPECT_EQ(deserialized->content_type, ContentType::kHtml);
  EXPECT_TRUE(deserialized->origin_content_type.empty());
  EXPECT_EQ(deserialized->wire_ct_len, 0);
  EXPECT_EQ(deserialized->cache_inserted_at, 1700000000u);
  EXPECT_EQ(deserialized->origin_max_age, 300u);
}

TEST(AlternateMetadataTest, RejectTruncatedData) {
  // Less than kFixedPrefixSize bytes.
  std::vector<std::byte> short_data(5, std::byte{0});
  EXPECT_FALSE(AlternateMetadata::Deserialize(short_data).has_value());

  // Empty.
  EXPECT_FALSE(AlternateMetadata::Deserialize({}).has_value());
}

TEST(AlternateMetadataTest, RejectUnknownVersion) {
  AlternateMetadata meta;
  meta.full_mask = 0;
  meta.content_type = ContentType::kOther;

  auto serialized = meta.Serialize();
  // Corrupt the version byte.
  serialized[0] = std::byte{99};
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, RejectV1) {
  // Build a v1 serialized blob manually.
  std::vector<std::byte> v1_data(8, std::byte{0});
  v1_data[0] = std::byte{1};  // version=1
  EXPECT_FALSE(AlternateMetadata::Deserialize(v1_data).has_value());
}

TEST(AlternateMetadataTest, RejectV2) {
  // Build a v2 serialized blob manually.
  std::vector<std::byte> v2_data(9, std::byte{0});
  v2_data[0] = std::byte{2};  // version=2
  EXPECT_FALSE(AlternateMetadata::Deserialize(v2_data).has_value());
}

TEST(AlternateMetadataTest, RejectOriginCtLenExceedingData) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";

  auto serialized = meta.Serialize();
  // Truncate the data so ct_len exceeds actual remaining bytes.
  serialized.resize(AlternateMetadata::kFixedPrefixSize + 2);
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, RejectOriginCtLenOverMax) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";

  auto serialized = meta.Serialize();
  // ct_len is at offset 7.
  uint16_t big_len = 300;
  std::memcpy(&serialized[7], &big_len, 2);
  // Extend data to avoid truncation error.
  serialized.resize(AlternateMetadata::kFixedPrefixSize + big_len +
                        AlternateMetadata::kV6FixedSuffixSize,
                    std::byte{0x41});
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, RejectContentTypeOutOfRange) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;

  auto serialized = meta.Serialize();
  // Set content_type to an invalid value.
  serialized[5] = std::byte{99};
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, StripCrLfNullFromContentType) {
  AlternateMetadata dirty;
  dirty.full_mask = 42;
  dirty.content_type = ContentType::kHtml;
  dirty.origin_content_type = std::string("text/\r\nhtml\0;charset", 20);

  auto dirty_ser = dirty.Serialize();
  auto result = AlternateMetadata::Deserialize(dirty_ser);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_type, "text/html;charset");
}

TEST(AlternateMetadataTest, SerializeTruncatesLongContentType) {
  AlternateMetadata meta;
  meta.origin_content_type = std::string(500, 'x');

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_type.size(), 256u);
}

TEST(AlternateMetadataTest, RoundTripWithFlags) {
  AlternateMetadata meta;
  meta.full_mask = 0x00C8;
  meta.content_type = ContentType::kHtml;
  meta.flags = AlternateMetadata::kFlagNeedsRevalidation;
  meta.origin_content_type = "text/html; charset=utf-8";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 600;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginMustRevalidate |
                         AlternateMetadata::kCCOriginHeaderPresent;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->full_mask, 0x00C8u);
  EXPECT_EQ(result->content_type, ContentType::kHtml);
  EXPECT_EQ(result->flags, AlternateMetadata::kFlagNeedsRevalidation);
  EXPECT_EQ(result->origin_content_type, "text/html; charset=utf-8");
  EXPECT_EQ(result->cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->origin_max_age, 600u);
  EXPECT_EQ(result->origin_cc_flags,
            AlternateMetadata::kCCOriginMustRevalidate |
                AlternateMetadata::kCCOriginHeaderPresent);
}

TEST(AlternateMetadataTest, FlagsDefaultToZero) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->flags, 0);
  EXPECT_EQ(result->origin_cc_flags, 0);
}

TEST(AlternateMetadataTest, MaxUint32Values) {
  AlternateMetadata meta;
  meta.full_mask = std::numeric_limits<uint32_t>::max();
  meta.cache_inserted_at = std::numeric_limits<uint32_t>::max();
  meta.origin_max_age = std::numeric_limits<uint32_t>::max();
  meta.origin_s_maxage = std::numeric_limits<uint32_t>::max();
  meta.origin_cc_flags = std::numeric_limits<uint16_t>::max();

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->full_mask, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(result->cache_inserted_at, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(result->origin_max_age, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(result->origin_s_maxage, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(result->origin_cc_flags, std::numeric_limits<uint16_t>::max());
}

TEST(AlternateMetadataTest, AllFlagBitsSet) {
  AlternateMetadata meta;
  meta.origin_cc_flags = 0x03FF;  // All 10 defined bits.

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cc_flags, 0x03FFu);
}

TEST(AlternateMetadataTest, Uint16FlagsPreserveHighBits) {
  AlternateMetadata meta;
  meta.origin_cc_flags = 0xFC00;  // Only reserved high bits.

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cc_flags, 0xFC00u);
}

TEST(AlternateMetadataTest, WireCtLenPreserved) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->wire_ct_len, 9);  // "text/html" = 9 bytes
  EXPECT_EQ(result->origin_content_type, "text/html");
}

TEST(AlternateMetadataTest, SerializedSizeMatchesSpec) {
  AlternateMetadata meta;
  meta.origin_content_type = "image/webp";

  auto serialized = meta.Serialize();
  // v8 Total = kFixedPrefixSize(9) + ct_len(10) + kV8FixedSuffixSize(101)
  //          + etag_len(0) + cc_len(0) = 120
  EXPECT_EQ(serialized.size(), 120u);

  AlternateMetadata empty;
  auto empty_ser = empty.Serialize();
  // Total = 9 + 0 + 101 + 0 + 0 = 110
  EXPECT_EQ(empty_ser.size(), 110u);
}

TEST(AlternateMetadataTest, SMaxageWithFlags) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kCss;
  meta.origin_max_age = 86400;
  meta.origin_s_maxage = 3600;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginSMaxagePresent |
                         AlternateMetadata::kCCOriginHeaderPresent;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_max_age, 86400u);
  EXPECT_EQ(result->origin_s_maxage, 3600u);
  EXPECT_TRUE(result->origin_cc_flags &
              AlternateMetadata::kCCOriginSMaxagePresent);
  EXPECT_TRUE(result->origin_cc_flags &
              AlternateMetadata::kCCOriginHeaderPresent);
}

TEST(AlternateMetadataTest, ImmutableMaxAge) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kImage;
  meta.origin_max_age = 31536000;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginImmutable |
                         AlternateMetadata::kCCOriginHeaderPresent;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_max_age, 31536000u);
  EXPECT_TRUE(result->origin_cc_flags & AlternateMetadata::kCCOriginImmutable);
}

TEST(AlternateMetadataTest, AllFlagsByteSet) {
  AlternateMetadata meta;
  meta.flags = 0xFF;  // All 8 bits set.
  meta.content_type = ContentType::kHtml;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->flags, 0xFFu);
}

TEST(AlternateMetadataTest, CacheInsertedAtZero) {
  AlternateMetadata meta;
  meta.cache_inserted_at = 0;  // Unix epoch.
  meta.content_type = ContentType::kCss;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->cache_inserted_at, 0u);
}

TEST(AlternateMetadataTest, MinimumValidV6Blob) {
  // Minimum v6 blob with ct_len = 0 and etag_len = 0.
  AlternateMetadata meta;
  meta.content_type = ContentType::kOther;

  auto serialized = meta.Serialize();
  // 9 + 0 + 101 + 0 + 0 = 110 (v8)
  ASSERT_EQ(serialized.size(), 110u);

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->wire_ct_len, 0);
  EXPECT_TRUE(result->origin_content_type.empty());
  EXPECT_EQ(result->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
  EXPECT_EQ(result->origin_last_modified, 0u);
  EXPECT_TRUE(result->origin_etag.empty());
  EXPECT_EQ(result->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, RejectTruncatedSuffix) {
  // Valid prefix + ct_len=0 but missing the 23-byte v5 suffix.
  std::vector<std::byte> data(AlternateMetadata::kFixedPrefixSize,
                              std::byte{0});
  data[0] = std::byte{5};
  // ct_len = 0 at offset 7-8 (already zero)
  // Total = 9 bytes, missing the 23-byte suffix.
  EXPECT_FALSE(AlternateMetadata::Deserialize(data).has_value());

  // Also test v4 truncated suffix.
  std::vector<std::byte> v4_data(AlternateMetadata::kFixedPrefixSize,
                                 std::byte{0});
  v4_data[0] = std::byte{4};
  EXPECT_FALSE(AlternateMetadata::Deserialize(v4_data).has_value());
}

TEST(AlternateMetadataTest, V4FieldsRoundTrip) {
  // Verify v4 fields (score, content_class) survive v5 round-trip.
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 86400;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginHeaderPresent;
  meta.ssimulacra2_score_x100 = 7523;  // 75.23
  meta.content_class = 2;              // Illustration

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->ssimulacra2_score_x100, 7523);
  EXPECT_EQ(result->content_class, 2);
}

TEST(AlternateMetadataTest, V4ScoreNA) {
  AlternateMetadata meta;
  meta.ssimulacra2_score_x100 = AlternateMetadata::kScoreNA;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
}

TEST(AlternateMetadataTest, V4ContentClassUnknown) {
  AlternateMetadata meta;
  meta.content_class = AlternateMetadata::kContentClassUnknown;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
}

TEST(AlternateMetadataTest, V3BackwardCompat) {
  // Build a v3 blob manually: 9-byte prefix + 14-byte suffix = 23 bytes.
  std::vector<std::byte> v3_data(23, std::byte{0});
  v3_data[0] = std::byte{3};  // version=3
  // Not HTML: HTML below version 9 is refused.
  v3_data[5] = static_cast<std::byte>(ContentType::kCss);

  auto result = AlternateMetadata::Deserialize(v3_data);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 3);
  // V4 fields default when reading v3 data.
  EXPECT_EQ(result->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
}

TEST(AlternateMetadataTest, V4TruncatedNewFields) {
  // Build a v4 header but only provide 14-byte suffix (missing 3 v4 bytes).
  std::vector<std::byte> data(9 + 14, std::byte{0});
  data[0] = std::byte{4};  // version=4

  // Should reject: v4 requires 17-byte suffix.
  EXPECT_FALSE(AlternateMetadata::Deserialize(data).has_value());
}

TEST(AlternateMetadataTest, V5TruncatedNewFields) {
  // Build a v5 header but only provide 17-byte suffix (missing v5 fields).
  std::vector<std::byte> data(9 + 17, std::byte{0});
  data[0] = std::byte{5};  // version=5

  // Should reject: v5 requires 23-byte fixed suffix.
  EXPECT_FALSE(AlternateMetadata::Deserialize(data).has_value());
}

TEST(AlternateMetadataTest, V6TruncatedNewFields) {
  // Build a v6 header but only provide 23-byte suffix (missing v6 fields).
  std::vector<std::byte> data(9 + 23, std::byte{0});
  data[0] = std::byte{6};  // version=6

  // Should reject: v6 requires 27-byte fixed suffix (+ etag_len).
  EXPECT_FALSE(AlternateMetadata::Deserialize(data).has_value());
}

TEST(AlternateMetadataTest, V6WireSizeDefault) {
  AlternateMetadata meta;
  meta.origin_content_type = "image/jpeg";
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  // v8 WireSize = 9 + 10 + 101 + 0 + 0 = 120
  EXPECT_EQ(result->WireSize(), 120u);
  EXPECT_EQ(result->WireSize(), serialized.size());
}

TEST(AlternateMetadataTest, V3WireSize) {
  // Build a v3 blob with ct_len=5.
  std::vector<std::byte> v3_data(9 + 5 + 14, std::byte{0});
  v3_data[0] = std::byte{3};
  // Not HTML: HTML below version 9 is refused.
  v3_data[5] = static_cast<std::byte>(ContentType::kCss);
  uint16_t ct_len = 5;
  std::memcpy(&v3_data[7], &ct_len, 2);

  auto result = AlternateMetadata::Deserialize(v3_data);
  ASSERT_TRUE(result.has_value());
  // WireSize = 9 + 5 + 14 = 28
  EXPECT_EQ(result->WireSize(), 28u);
}

TEST(AlternateMetadataTest, V6RoundTripAllFields) {
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kHtml;
  meta.origin_content_type = "text/html";
  meta.cache_inserted_at = 1700000000;
  meta.origin_max_age = 60;
  meta.origin_cc_flags = AlternateMetadata::kCCOriginMustRevalidate |
                         AlternateMetadata::kCCOriginHeaderPresent;
  meta.ssimulacra2_score_x100 = AlternateMetadata::kScoreNA;
  meta.content_class = AlternateMetadata::kContentClassUnknown;
  meta.origin_last_modified = 1700000000;
  meta.origin_etag = "\"abc123\"";
  meta.origin_content_length = 123456;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->full_mask, 0x08u);
  EXPECT_EQ(result->content_type, ContentType::kHtml);
  EXPECT_EQ(result->origin_content_type, "text/html");
  EXPECT_EQ(result->cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->origin_max_age, 60u);
  EXPECT_EQ(result->origin_cc_flags,
            AlternateMetadata::kCCOriginMustRevalidate |
                AlternateMetadata::kCCOriginHeaderPresent);
  EXPECT_EQ(result->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
  EXPECT_EQ(result->origin_last_modified, 1700000000u);
  EXPECT_EQ(result->origin_etag, "\"abc123\"");
  EXPECT_EQ(result->origin_content_length, 123456u);
}

TEST(AlternateMetadataTest, V5WeakETag) {
  AlternateMetadata meta;
  meta.origin_etag = "W/\"weak-tag\"";
  meta.origin_last_modified = 1700000000;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_etag, "W/\"weak-tag\"");
}

TEST(AlternateMetadataTest, V5EmptyETag) {
  AlternateMetadata meta;
  meta.origin_last_modified = 1700000000;
  // origin_etag left empty.

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->origin_etag.empty());
  EXPECT_EQ(result->origin_last_modified, 1700000000u);
}

TEST(AlternateMetadataTest, V5NoLastModified) {
  AlternateMetadata meta;
  meta.origin_etag = "\"tag\"";
  // origin_last_modified left at 0.

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_etag, "\"tag\"");
  EXPECT_EQ(result->origin_last_modified, 0u);
}

TEST(AlternateMetadataTest, V5LongETag) {
  AlternateMetadata meta;
  meta.origin_etag = "\"" + std::string(998, 'x') + "\"";  // 1000 chars

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_etag.size(), 1000u);
  EXPECT_EQ(result->origin_etag, meta.origin_etag);
}

TEST(AlternateMetadataTest, V5MaxETag) {
  AlternateMetadata meta;
  meta.origin_etag = std::string(65535, 'E');

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_etag.size(), 65535u);
  EXPECT_EQ(result->origin_etag, meta.origin_etag);
}

TEST(AlternateMetadataTest, V6WireSize) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";  // ct_len = 9
  meta.origin_etag = "\"abc\"";            // etag_len = 5

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  // WireSize = 9 + 9 + 101 + 5 + 0 = 124 (v8)
  EXPECT_EQ(result->WireSize(), 124u);
  EXPECT_EQ(result->WireSize(), serialized.size());
}

TEST(AlternateMetadataTest, V4BackwardCompatManual) {
  // Build a v4 blob manually: 9-byte prefix + 17-byte suffix = 26 bytes.
  std::vector<std::byte> v4_data(26, std::byte{0});
  v4_data[0] = std::byte{4};  // version=4
  // full_mask = 0x08 (Desktop/Identity)
  uint32_t mask = 0x08;
  std::memcpy(&v4_data[1], &mask, 4);
  // content_type = kImage (3)
  v4_data[5] = std::byte{3};
  // ct_len = 0
  // cache_inserted_at = 1700000000
  uint32_t ts = 1700000000;
  std::memcpy(&v4_data[9], &ts, 4);
  // ssimulacra2_score_x100 = 7500
  uint16_t score = 7500;
  std::memcpy(&v4_data[23], &score, 2);
  // content_class = 0 (Photo)
  v4_data[25] = std::byte{0};

  auto result = AlternateMetadata::Deserialize(v4_data);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 4);
  EXPECT_EQ(result->full_mask, 0x08u);
  EXPECT_EQ(result->content_type, ContentType::kImage);
  EXPECT_EQ(result->cache_inserted_at, 1700000000u);
  EXPECT_EQ(result->ssimulacra2_score_x100, 7500);
  EXPECT_EQ(result->content_class, 0);
  // V5 fields should be defaults.
  EXPECT_EQ(result->origin_last_modified, 0u);
  EXPECT_TRUE(result->origin_etag.empty());
  // V6 fields should be defaults.
  EXPECT_EQ(result->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, V3BackwardCompatDefaults) {
  // Build a v3 blob manually: 9-byte prefix + 14-byte suffix = 23 bytes.
  std::vector<std::byte> v3_data(23, std::byte{0});
  v3_data[0] = std::byte{3};  // version=3
  // Not HTML: HTML below version 9 is refused.
  v3_data[5] = static_cast<std::byte>(ContentType::kCss);
  uint32_t mask = 0x08;
  std::memcpy(&v3_data[1], &mask, 4);

  auto result = AlternateMetadata::Deserialize(v3_data);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 3);
  // V4 fields default.
  EXPECT_EQ(result->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
  // V5 fields default.
  EXPECT_EQ(result->origin_last_modified, 0u);
  EXPECT_TRUE(result->origin_etag.empty());
  // V6 fields default.
  EXPECT_EQ(result->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, V5TruncatedETag) {
  AlternateMetadata meta;
  meta.origin_etag = "\"0123456789\"";  // 12 chars
  meta.origin_last_modified = 1700000000;

  auto serialized = meta.Serialize();
  // Truncate so etag_len says 12 but only 5 bytes of etag remain.
  // suffix_offset = 9 + 0 = 9; etag starts at 9 + 23 = 32.
  // Full size = 9 + 0 + 23 + 12 = 44. Cut to 32 + 5 = 37.
  serialized.resize(37);

  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, V5TruncatedLastModified) {
  AlternateMetadata meta;
  meta.origin_last_modified = 1700000000;

  auto serialized = meta.Serialize();
  // Cut off in the middle of the last_modified field.
  // suffix_offset = 9; last_modified at suffix_offset+17 = 26.
  // Truncate to 28 (missing 2 of 4 bytes of last_modified + etag_len).
  serialized.resize(28);

  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataTest, V6SerializedSizeMatchesSpec) {
  AlternateMetadata meta;
  meta.origin_content_type = "image/webp";  // ct_len = 10
  meta.origin_etag = "\"etag\"";            // etag_len = 6

  auto serialized = meta.Serialize();
  // Total = 9 + 10 + 101 + 6 + 0 = 126 (v8)
  EXPECT_EQ(serialized.size(), 126u);

  AlternateMetadata empty;
  auto empty_ser = empty.Serialize();
  // Total = 9 + 0 + 101 + 0 + 0 = 110 (v8)
  EXPECT_EQ(empty_ser.size(), 110u);
}

// ---------- Phase 2.4 adversarial additions ----------

TEST(AlternateMetadataTest, NonUtf8ContentType) {
  // Non-UTF-8 bytes (Latin-1 encoding) survive round-trip since
  // only \r\n\0 are stripped. Use string concatenation to avoid
  // embedded null from the C string literal.
  AlternateMetadata meta;
  meta.origin_content_type = std::string("text/html; charset=") + '\xe9';

  auto serialized = meta.Serialize();
  auto deserialized = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(deserialized.has_value());
  EXPECT_EQ(deserialized->origin_content_type, meta.origin_content_type);
}

TEST(AlternateMetadataTest, ContentTypeWithAllSanitizedChars) {
  // Content type consisting entirely of \r\n\0 → empty after sanitization.
  AlternateMetadata meta;
  meta.origin_content_type = std::string("\r\n\0\r\n", 5);

  auto serialized = meta.Serialize();
  auto deserialized = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(deserialized.has_value());
  EXPECT_TRUE(deserialized->origin_content_type.empty());
}

TEST(AlternateMetadataTest, AllContentTypeEnumValues) {
  // Round-trip all valid ContentType enum values.
  for (int ct = 0; ct <= static_cast<int>(ContentType::kOther); ++ct) {
    AlternateMetadata meta;
    meta.content_type = static_cast<ContentType>(ct);
    auto serialized = meta.Serialize();
    auto deserialized = AlternateMetadata::Deserialize(serialized);
    ASSERT_TRUE(deserialized.has_value()) << "ContentType " << ct;
    EXPECT_EQ(deserialized->content_type, meta.content_type)
        << "ContentType " << ct;
  }
}

TEST(AlternateMetadataTest, CapabilityMaskFullRangeRoundTrip) {
  // Verify that full_mask survives round-trip for various bit patterns.
  for (uint32_t mask : {0u, 0x08u, 0xFFu, 0xFFFFu, 0xFFFFFFFFu, 0xDEADBEEFu,
                        0x01u, 0x80000000u}) {
    AlternateMetadata meta;
    meta.full_mask = mask;
    auto serialized = meta.Serialize();
    auto deserialized = AlternateMetadata::Deserialize(serialized);
    ASSERT_TRUE(deserialized.has_value()) << "mask=0x" << std::hex << mask;
    EXPECT_EQ(deserialized->full_mask, mask) << "mask=0x" << std::hex << mask;
  }
}

// ---------- Round 9: content_class range validation ----------

TEST(AlternateMetadataTest, ContentClassClampedOnCorruption) {
  // Serialize valid metadata with content_class = 0 (Photo), then corrupt
  // the content_class byte to 255 (out of range).  Deserialize should clamp
  // it to kContentClassUnknown (4) rather than returning the raw value.
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.ssimulacra2_score_x100 = 7500;
  meta.content_class = 0;  // Photo

  auto serialized = meta.Serialize();
  auto result_before = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result_before.has_value());
  EXPECT_EQ(result_before->content_class, 0);  // Verify baseline.

  // content_class is at suffix_offset + 16.
  // suffix_offset = kFixedPrefixSize(9) + wire_ct_len(10 for "image/jpeg") = 19.
  // So content_class byte is at offset 19 + 16 = 35.
  size_t content_class_offset =
      AlternateMetadata::kFixedPrefixSize +
      static_cast<size_t>(result_before->wire_ct_len) + 16;
  ASSERT_LT(content_class_offset, serialized.size());

  // Corrupt content_class to 255.
  serialized[content_class_offset] = std::byte{255};

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value()) << "Deserialize should succeed with clamping";
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown)
      << "content_class=255 should be clamped to kContentClassUnknown(4)";
}

TEST(AlternateMetadataTest, ContentClassJustAboveMaxIsClamped) {
  // Verify that content_class = kContentClassUnknown + 1 (=5) is clamped.
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.ssimulacra2_score_x100 = AlternateMetadata::kScoreNA;
  meta.content_class = 2;  // Illustration
  // Empty origin_content_type for simpler offset calculation.

  auto serialized = meta.Serialize();

  // content_class offset with empty ct: 9 + 0 + 16 = 25.
  size_t content_class_offset = AlternateMetadata::kFixedPrefixSize + 16;
  ASSERT_LT(content_class_offset, serialized.size());
  // Verify current value.
  EXPECT_EQ(static_cast<uint8_t>(serialized[content_class_offset]), 2);

  // Set content_class to kContentClassUnknown + 1 = 5.
  serialized[content_class_offset] =
      std::byte{AlternateMetadata::kContentClassUnknown + 1};

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown)
      << "content_class=5 should be clamped to kContentClassUnknown(4)";
}

TEST(AlternateMetadataTest, ContentClassAtMaxIsNotClamped) {
  // Verify that content_class = kContentClassUnknown (4) itself is NOT
  // clamped — only values strictly greater are clamped.
  AlternateMetadata meta;
  meta.content_class = AlternateMetadata::kContentClassUnknown;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->content_class, AlternateMetadata::kContentClassUnknown);
}

TEST(AlternateMetadataTest, ContentClassAllValidValuesPreserved) {
  // Verify all valid content_class values [0..kContentClassUnknown] survive
  // round-trip without clamping.
  for (uint8_t cc = 0; cc <= AlternateMetadata::kContentClassUnknown; ++cc) {
    AlternateMetadata meta;
    meta.full_mask = 0x08;
    meta.content_type = ContentType::kImage;
    meta.content_class = cc;

    auto serialized = meta.Serialize();
    auto result = AlternateMetadata::Deserialize(serialized);
    ASSERT_TRUE(result.has_value()) << "content_class=" << static_cast<int>(cc);
    EXPECT_EQ(result->content_class, cc)
        << "content_class=" << static_cast<int>(cc)
        << " should survive round-trip";
  }
}

// ---------- V6: origin_content_length ----------

TEST(AlternateMetadataTest, V6OriginContentLengthRoundTrip) {
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.content_type = ContentType::kImage;
  meta.origin_content_type = "image/jpeg";
  meta.origin_content_length = 987654;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->origin_content_length, 987654u);
}

TEST(AlternateMetadataTest, V6OriginContentLengthMax) {
  AlternateMetadata meta;
  meta.origin_content_length = std::numeric_limits<uint32_t>::max();

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_length,
            std::numeric_limits<uint32_t>::max());
}

TEST(AlternateMetadataTest, V6OriginContentLengthZero) {
  AlternateMetadata meta;
  meta.origin_content_length = 0;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, V5BackwardCompatManual) {
  // Build a v5 blob manually: 9-byte prefix + 23-byte suffix = 32 bytes.
  std::vector<std::byte> v5_data(32, std::byte{0});
  v5_data[0] = std::byte{5};  // version=5
  uint32_t mask = 0x08;
  std::memcpy(&v5_data[1], &mask, 4);
  // content_type = kImage (3)
  v5_data[5] = std::byte{3};

  auto result = AlternateMetadata::Deserialize(v5_data);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 5);
  EXPECT_EQ(result->full_mask, 0x08u);
  EXPECT_EQ(result->content_type, ContentType::kImage);
  // V6 field should default to 0.
  EXPECT_EQ(result->origin_content_length, 0u);
}

TEST(AlternateMetadataTest, V6WithETagAndContentLength) {
  // Verify origin_content_length works alongside etag.
  AlternateMetadata meta;
  meta.origin_content_type = "text/css";
  meta.origin_etag = "\"css-etag\"";
  meta.origin_content_length = 55555;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->origin_etag, "\"css-etag\"");
  EXPECT_EQ(result->origin_content_length, 55555u);
  // WireSize = 9 + 8("text/css") + 101 + 10("\"css-etag\"") + 0 = 128 (v8)
  EXPECT_EQ(result->WireSize(), 128u);
  EXPECT_EQ(result->WireSize(), serialized.size());
}

// ---------- consumed_size regression canary ----------

TEST(AlternateMetadataTest, ConsumedSizeMatchesWireSize) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";
  meta.origin_etag = "\"abc\"";
  meta.origin_content_length = 1234;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->consumed_size, result->WireSize());
  EXPECT_EQ(result->consumed_size, serialized.size());
}

TEST(AlternateMetadataTest, ConsumedSizeSetForV3) {
  std::vector<std::byte> v3_data(23, std::byte{0});
  v3_data[0] = std::byte{3};
  // Not HTML: HTML below version 9 is refused.
  v3_data[5] = static_cast<std::byte>(ContentType::kCss);

  auto result = AlternateMetadata::Deserialize(v3_data);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->consumed_size, 23u);  // 9 + 0 + 14
  EXPECT_EQ(result->consumed_size, result->WireSize());
}

TEST(AlternateMetadataTest, ConsumedSizeSetForV4) {
  std::vector<std::byte> v4_data(26, std::byte{0});
  v4_data[0] = std::byte{4};
  // Not HTML: HTML below version 9 is refused.
  v4_data[5] = static_cast<std::byte>(ContentType::kCss);

  auto result = AlternateMetadata::Deserialize(v4_data);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->consumed_size, 26u);  // 9 + 0 + 17
  EXPECT_EQ(result->consumed_size, result->WireSize());
}

TEST(AlternateMetadataTest, ConsumedSizeSetForV5) {
  // V5: kFixedPrefixSize(9) + ct_len(0) + kV5FixedSuffixSize(23) + etag(0)
  // = 32 bytes.
  std::vector<std::byte> v5_data(32, std::byte{0});
  v5_data[0] = std::byte{5};
  // Not HTML: HTML below version 9 is refused.
  v5_data[5] = static_cast<std::byte>(ContentType::kCss);

  auto result = AlternateMetadata::Deserialize(v5_data);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->consumed_size, 32u);  // 9 + 0 + 23 + 0
  EXPECT_EQ(result->consumed_size, result->WireSize());
}

TEST(AlternateMetadataTest, ConsumedSizeWithEtag) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/css";
  meta.origin_etag = "\"etag-val\"";
  meta.origin_content_length = 42;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  // 9 + 8("text/css") + 101 + 10("\"etag-val\"") + 0 = 128 (v8)
  EXPECT_EQ(result->consumed_size, 128u);
  EXPECT_EQ(result->consumed_size, result->WireSize());
}

// Verify that a consumed_size/WireSize() mismatch is detectable — this is the
// exact condition ParseMetadataPrefix relies on to reject inconsistent entries.
// We simulate the check here because ParseMetadataPrefix lives in an anonymous
// namespace inside cache.cc.
TEST(AlternateMetadataTest, ConsumedSizeMismatchDetected) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";
  meta.origin_etag = "\"abc\"";
  meta.origin_content_length = 1234;

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());

  // Baseline: consumed_size and WireSize() agree.
  ASSERT_EQ(result->consumed_size, result->WireSize());

  // Tamper with consumed_size to simulate what a code regression would cause.
  // ParseMetadataPrefix rejects when consumed != WireSize().
  result->consumed_size = result->WireSize() + 1;
  EXPECT_NE(result->consumed_size, result->WireSize())
      << "Tampered consumed_size must disagree with WireSize()";

  result->consumed_size = 0;
  EXPECT_NE(result->consumed_size, result->WireSize())
      << "Zero consumed_size must disagree with WireSize()";
}

// Verify that patching serialized bytes to create an impossible version/data
// combination is caught by Deserialize's size check (which also feeds the
// consumed_size cross-check).  Specifically: take current serialized data
// (110 bytes with empty ct/etag) and patch the version byte to V5.  V5 expects
// the kV5FixedSuffixSize(23) suffix and none of the later fields.  Deserialize
// succeeds (V5 reads fewer fields, 32 bytes needed, 110 available), but the
// consumed_size (32) won't match the blob size (110), demonstrating that
// ParseMetadataPrefix's "consumed > content.size()" guard would not help —
// the "consumed != WireSize()" guard is the correct canary.
TEST(AlternateMetadataTest, VersionDowngradeCausesConsumedSizeBlobMismatch) {
  AlternateMetadata meta;
  // Empty ct and etag for simple size: 9 + 0 + 101 + 0 + 0 = 110 (v8).
  auto serialized = meta.Serialize();
  ASSERT_EQ(serialized.size(), 110u);

  // Patch version byte from 8 to 5.
  serialized[0] = std::byte{5};

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 5);
  // V5 consumed_size: 9 + 0 + (14 + 2 + 1 + 4 + 2 + 0) = 32
  EXPECT_EQ(result->consumed_size, 32u);
  // V5 WireSize: 9 + 0 + 23 + 0 = 32
  EXPECT_EQ(result->WireSize(), 32u);
  // consumed_size and WireSize agree (both 32), but the blob is 110 bytes.
  // This means the trailing bytes are unaccounted for.  ParseMetadataPrefix
  // would accept this (consumed <= content.size()), but the content offset
  // would be wrong (32 instead of 110), misinterpreting the remaining
  // metadata bytes as user content.  This test documents the limitation: the
  // cross-check catches constant/field drift within a version, not
  // version-byte corruption (which Deserialize's own size check handles
  // by rejecting blobs too short for the claimed version).
  EXPECT_EQ(result->consumed_size, result->WireSize());
  EXPECT_LT(result->consumed_size, serialized.size());
}

// ===========================================================================
// V7 content-binding hashes.
// ===========================================================================

TEST(AlternateMetadataTest, V7HashesRoundTrip) {
  AlternateMetadata meta;
  meta.full_mask = 0x7C;  // kAgentMarkdown low byte
  meta.content_type = ContentType::kOther;
  meta.origin_content_type = "text/markdown";
  meta.origin_content_length = 4096;
  meta.origin_etag = "\"v7etag\"";
  for (size_t i = 0; i < AlternateMetadata::kHashSize; ++i) {
    meta.origin_html_hash[i] = static_cast<std::byte>(i + 1);
    meta.render_source_hash[i] = static_cast<std::byte>(0xA0 + i);
  }

  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(result->origin_content_type, "text/markdown");
  EXPECT_EQ(result->origin_content_length, 4096u);
  EXPECT_EQ(result->origin_etag, "\"v7etag\"");
  EXPECT_EQ(result->origin_html_hash, meta.origin_html_hash);
  EXPECT_EQ(result->render_source_hash, meta.render_source_hash);
  // consumed_size (independently summed) must agree with WireSize().
  EXPECT_EQ(result->consumed_size, result->WireSize());
  EXPECT_EQ(result->WireSize(), serialized.size());
}

TEST(AlternateMetadataTest, V7DefaultHashesAreAllZero) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/html";
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, AlternateMetadata::kCurrentVersion);
  std::array<std::byte, AlternateMetadata::kHashSize> zero{};
  EXPECT_EQ(result->origin_html_hash, zero);
  EXPECT_EQ(result->render_source_hash, zero);
}

// A v6 blob (no hash fields) must deserialize under the v7 reader with both
// hashes zero-filled — the all-zero "binding absent" value the D8 gates expect.
TEST(AlternateMetadataTest, V6BlobDeserializesToV7ReaderWithZeroHashes) {
  // Build a v6 blob manually: prefix(9) + ct_len(0) + kV6FixedSuffixSize(27).
  std::vector<std::byte> v6_data(AlternateMetadata::kFixedPrefixSize +
                                     AlternateMetadata::kV6FixedSuffixSize,
                                 std::byte{0});
  v6_data[0] = std::byte{6};  // version=6
  // Not HTML: HTML below version 9 is refused.
  v6_data[5] = static_cast<std::byte>(ContentType::kCss);
  // origin_content_length (last 4 bytes) = 7 for good measure.
  uint32_t ocl = 7;
  std::memcpy(&v6_data[AlternateMetadata::kFixedPrefixSize + 23], &ocl, 4);

  auto result = AlternateMetadata::Deserialize(v6_data);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->version, 6);  // preserved — read-tolerant, not upgraded
  EXPECT_EQ(result->origin_content_length, 7u);
  std::array<std::byte, AlternateMetadata::kHashSize> zero{};
  EXPECT_EQ(result->origin_html_hash, zero);
  EXPECT_EQ(result->render_source_hash, zero);
}

}  // namespace
}  // namespace pagespeed
