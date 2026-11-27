// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Entry-metadata format v8: the cross-version corpus, the new fields, the
// prefix budget, and the flags-byte contract.
//
// The corpus in testdata/alternate_metadata_fixtures.h is real output from the
// serializers that shipped at v3..v7, not transcribed layout.  To regenerate
// it: check out lib/classify/alternate_metadata.{h,cc} + content_type.h at the
// commit listed for each version in that file, compile them standalone against
// a generator that fills the field vector documented there, and paste the
// emitted bytes.  Nothing in this test depends on the current serializer being
// able to produce an old version — it cannot, and that is the point.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "test/lib/classify/testdata/alternate_metadata_fixtures.h"

namespace pagespeed {
namespace {

using testdata::kV3Full;
using testdata::kV3Minimal;
using testdata::kV4Full;
using testdata::kV4Minimal;
using testdata::kV5Full;
using testdata::kV5Minimal;
using testdata::kV6Full;
using testdata::kV6Minimal;
using testdata::kV7Full;
using testdata::kV7Minimal;

template <size_t N>
std::span<const std::byte> AsBytes(const unsigned char (&arr)[N]) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&arr[0]),
                                    N);
}

// ===========================================================================
// Cross-version corpus: every shipped version still reads.
// ===========================================================================

TEST(AlternateMetadataV8Test, CorpusFullBlobsAllParse) {
  struct Case {
    const char* name;
    std::span<const std::byte> bytes;
    uint8_t version;
  };
  const Case cases[] = {
      {"v3", AsBytes(kV3Full), 3}, {"v4", AsBytes(kV4Full), 4},
      {"v5", AsBytes(kV5Full), 5}, {"v6", AsBytes(kV6Full), 6},
      {"v7", AsBytes(kV7Full), 7},
  };

  for (const auto& c : cases) {
    SCOPED_TRACE(c.name);
    auto m = AlternateMetadata::Deserialize(c.bytes);
    ASSERT_TRUE(m.has_value()) << "a blob written by a shipped version must "
                                  "still read under the v8 reader";
    EXPECT_EQ(m->version, c.version);

    // Fields every version has.
    EXPECT_EQ(m->full_mask, 0x1234ABCDu);
    EXPECT_EQ(m->content_type, ContentType::kImage);
    EXPECT_EQ(m->flags, 0x81);
    EXPECT_EQ(m->origin_content_type, "image/jpeg");
    EXPECT_EQ(m->cache_inserted_at, 1700000000u);
    EXPECT_EQ(m->origin_max_age, 3600u);
    EXPECT_EQ(m->origin_s_maxage, 7200u);
    EXPECT_EQ(m->origin_cc_flags, 0x0355u);

    // v4 fields: present from v4, at their "absent" defaults before it.
    if (c.version >= 4) {
      EXPECT_EQ(m->ssimulacra2_score_x100, 9012);
      EXPECT_EQ(m->content_class, 1);
    } else {
      EXPECT_EQ(m->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
      EXPECT_EQ(m->content_class, AlternateMetadata::kContentClassUnknown);
    }

    // v5 fields.
    if (c.version >= 5) {
      EXPECT_EQ(m->origin_last_modified, 1699999000u);
      EXPECT_EQ(m->origin_etag, "\"abc-123\"");
    } else {
      EXPECT_EQ(m->origin_last_modified, 0u);
      EXPECT_TRUE(m->origin_etag.empty());
    }

    // v6 field.
    EXPECT_EQ(m->origin_content_length, c.version >= 6 ? 123456u : 0u);

    // v7 fields: all-zero is the reserved "no binding" value.
    if (c.version >= 7) {
      EXPECT_EQ(m->origin_html_hash[0], std::byte{0x01});
      EXPECT_EQ(m->origin_html_hash[31], std::byte{0x20});
      EXPECT_EQ(m->render_source_hash[0], std::byte{0xF0});
      EXPECT_EQ(m->render_source_hash[31], std::byte{0xD1});
    } else {
      for (size_t i = 0; i < AlternateMetadata::kHashSize; ++i) {
        EXPECT_EQ(m->origin_html_hash[i], std::byte{0});
        EXPECT_EQ(m->render_source_hash[i], std::byte{0});
      }
    }

    // v8 fields are absent in every pre-v8 blob, and absent means zero/empty.
    EXPECT_EQ(m->origin_epoch, 0u);
    EXPECT_TRUE(m->origin_cache_control.empty());
    EXPECT_EQ(m->wire_cc_len, 0u);
  }
}

TEST(AlternateMetadataV8Test, CorpusMinimalBlobsAllParse) {
  const std::span<const std::byte> blobs[] = {
      AsBytes(kV3Minimal), AsBytes(kV4Minimal), AsBytes(kV5Minimal),
      AsBytes(kV6Minimal), AsBytes(kV7Minimal),
  };
  uint8_t expected_version = 3;
  for (const auto& blob : blobs) {
    SCOPED_TRACE(static_cast<int>(expected_version));
    auto m = AlternateMetadata::Deserialize(blob);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->version, expected_version);
    EXPECT_EQ(m->full_mask, 0u);
    EXPECT_TRUE(m->origin_content_type.empty());
    EXPECT_TRUE(m->origin_etag.empty());
    EXPECT_EQ(m->origin_epoch, 0u);
    EXPECT_TRUE(m->origin_cache_control.empty());
    ++expected_version;
  }
}

// The offset a reader hands back for the content prefix is derived twice, and
// the two derivations must agree for every version in the corpus — that
// cross-check is what ParseMetadataPrefix rejects entries on.
TEST(AlternateMetadataV8Test, CorpusConsumedSizeMatchesWireSizeAndBlob) {
  const std::span<const std::byte> blobs[] = {
      AsBytes(kV3Full),    AsBytes(kV4Full),    AsBytes(kV5Full),
      AsBytes(kV6Full),    AsBytes(kV7Full),    AsBytes(kV3Minimal),
      AsBytes(kV4Minimal), AsBytes(kV5Minimal), AsBytes(kV6Minimal),
      AsBytes(kV7Minimal),
  };
  for (const auto& blob : blobs) {
    auto m = AlternateMetadata::Deserialize(blob);
    ASSERT_TRUE(m.has_value());
    SCOPED_TRACE(static_cast<int>(m->version));
    EXPECT_EQ(m->consumed_size, m->WireSize());
    EXPECT_EQ(m->consumed_size, blob.size())
        << "the whole fixture is metadata; a mismatch means the reader would "
           "take metadata bytes for content";
  }
}

// Reading an old entry and writing it back re-stamps it at the current
// version.  This is the upgrade path: no conversion pass, entries move forward
// when they are next written.
TEST(AlternateMetadataV8Test, CorpusReserializesAtCurrentVersion) {
  auto m = AlternateMetadata::Deserialize(AsBytes(kV5Full));
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->version, 5);

  auto rewritten = m->Serialize();
  ASSERT_FALSE(rewritten.empty());
  EXPECT_EQ(static_cast<uint8_t>(rewritten[0]),
            AlternateMetadata::kCurrentVersion);

  auto again = AlternateMetadata::Deserialize(rewritten);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->version, AlternateMetadata::kCurrentVersion);
  EXPECT_EQ(again->origin_etag, "\"abc-123\"");
  EXPECT_EQ(again->origin_content_type, "image/jpeg");
  EXPECT_EQ(again->flags, 0x81) << "including the bit nothing has claimed";
}

// ===========================================================================
// Unknown future version -> MISS, never a partial parse.
// ===========================================================================

TEST(AlternateMetadataV8Test, UnknownFutureVersionIsRejected) {
  AlternateMetadata meta;
  meta.origin_content_type = "text/css";
  auto serialized = meta.Serialize();
  ASSERT_TRUE(AlternateMetadata::Deserialize(serialized).has_value());

  // Every version above the current one, up to the width of the byte.
  for (int v = AlternateMetadata::kCurrentVersion + 1; v <= 255; ++v) {
    serialized[0] = static_cast<std::byte>(v);
    EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value())
        << "version " << v
        << " must read as unparseable (the caller turns that into a MISS), "
           "not as a best-effort parse of the fields this build knows";
  }
}

TEST(AlternateMetadataV8Test, VersionTenIsRejectedEvenWhenLongEnough) {
  // A v10 blob would be at least as long as a v9 one, so length alone cannot
  // be what rejects it.
  AlternateMetadata meta;
  meta.origin_cache_control = "public, max-age=600";
  auto serialized = meta.Serialize();
  serialized.resize(serialized.size() + 64, std::byte{0xAB});
  serialized[0] = std::byte{10};
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

// ===========================================================================
// v8 fields.
// ===========================================================================

TEST(AlternateMetadataV8Test, SerializesAtVersionNine) {
  AlternateMetadata meta;
  auto serialized = meta.Serialize();
  ASSERT_FALSE(serialized.empty());
  EXPECT_EQ(static_cast<uint8_t>(serialized[0]), 9);
  EXPECT_EQ(AlternateMetadata::kCurrentVersion, 9);
}

// Version 9 moved no byte: a version 8 blob is the version 9 blob with the
// first byte changed.
TEST(AlternateMetadataV8Test, VersionNineHasTheVersionEightLayout) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kCss;
  meta.origin_content_type = "text/css";
  meta.origin_etag = "\"abc\"";
  meta.origin_cache_control = "public, max-age=60";
  auto nine = meta.Serialize();
  auto eight = nine;
  eight[0] = std::byte{8};
  auto from_nine = AlternateMetadata::Deserialize(nine);
  auto from_eight = AlternateMetadata::Deserialize(eight);
  ASSERT_TRUE(from_nine.has_value());
  ASSERT_TRUE(from_eight.has_value());
  EXPECT_EQ(from_eight->consumed_size, from_nine->consumed_size);
  EXPECT_EQ(from_eight->consumed_size, nine.size());
  EXPECT_EQ(from_eight->origin_etag, from_nine->origin_etag);
  EXPECT_EQ(from_eight->origin_cache_control, from_nine->origin_cache_control);
}

// ===========================================================================
// Worker-written HTML below version 9 reads as a MISS.
// ===========================================================================

namespace {
std::vector<std::byte> StoredAt(uint8_t version, ContentType type,
                                uint8_t flags) {
  AlternateMetadata meta;
  meta.content_type = type;
  meta.flags = flags;
  meta.origin_content_type = "text/html";
  auto blob = meta.Serialize();
  blob[0] = static_cast<std::byte>(version);
  return blob;
}
}  // namespace

TEST(AlternateMetadataV8Test, WorkerWrittenHtmlBelowVersionNineIsRefused) {
  EXPECT_FALSE(AlternateMetadata::Deserialize(
                   StoredAt(8, ContentType::kHtml,
                            AlternateMetadata::kFlagWorkerProcessed))
                   .has_value());
  EXPECT_FALSE(AlternateMetadata::Deserialize(
                   StoredAt(8, ContentType::kHtml,
                            AlternateMetadata::kFlagWorkerProcessed |
                                AlternateMetadata::kFlagNeedsRevalidation))
                   .has_value());
}

TEST(AlternateMetadataV8Test, EveryOlderVersionOfHtmlIsRefused) {
  // The corpus blobs are images; re-type each one as HTML, written by the
  // worker or not.
  const std::span<const std::byte> blobs[] = {
      AsBytes(kV3Full), AsBytes(kV4Full), AsBytes(kV5Full),
      AsBytes(kV6Full), AsBytes(kV7Full),
  };
  for (const auto& blob : blobs) {
    std::vector<std::byte> html(blob.begin(), blob.end());
    ASSERT_TRUE(AlternateMetadata::Deserialize(html).has_value());
    html[5] = static_cast<std::byte>(ContentType::kHtml);
    html[6] |= std::byte{AlternateMetadata::kFlagWorkerProcessed};
    EXPECT_FALSE(AlternateMetadata::Deserialize(html).has_value())
        << "version " << static_cast<int>(static_cast<uint8_t>(html[0]));
    html[6] &= ~std::byte{AlternateMetadata::kFlagWorkerProcessed};
    EXPECT_FALSE(AlternateMetadata::Deserialize(html).has_value())
        << "version " << static_cast<int>(static_cast<uint8_t>(html[0]));
  }
}

TEST(AlternateMetadataV8Test, WorkerWrittenMarkdownBelowVersionNineIsRefused) {
  // The Markdown version of a page is stored as kOther under its own
  // sentinel mask.
  AlternateMetadata meta;
  meta.full_mask = static_cast<uint32_t>(SentinelId::kAgentMarkdown);
  meta.content_type = ContentType::kOther;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  meta.origin_content_type = "text/markdown";
  auto blob = meta.Serialize();
  EXPECT_TRUE(AlternateMetadata::Deserialize(blob).has_value());
  blob[0] = std::byte{8};
  EXPECT_FALSE(AlternateMetadata::Deserialize(blob).has_value());
  EXPECT_TRUE(AlternateMetadata::IsRetired(blob));
  blob[6] = std::byte{0};
  EXPECT_TRUE(AlternateMetadata::Deserialize(blob).has_value());
  EXPECT_FALSE(AlternateMetadata::IsRetired(blob));
}

TEST(AlternateMetadataV8Test, IsRetiredNeedsTheWholeHead) {
  auto blob =
      StoredAt(8, ContentType::kHtml, AlternateMetadata::kFlagWorkerProcessed);
  EXPECT_TRUE(AlternateMetadata::IsRetired(blob));
  EXPECT_FALSE(AlternateMetadata::IsRetired(std::span(blob).first(6)));
  EXPECT_FALSE(AlternateMetadata::IsRetired({}));
}

TEST(AlternateMetadataV8Test, OtherEntriesBelowVersionNineStayReadable) {
  // Worker-written CSS, JS, images and other types.
  for (ContentType t : {ContentType::kCss, ContentType::kJs,
                        ContentType::kImage, ContentType::kOther}) {
    EXPECT_TRUE(AlternateMetadata::Deserialize(
                    StoredAt(8, t, AlternateMetadata::kFlagWorkerProcessed))
                    .has_value())
        << static_cast<int>(t);
  }
}

TEST(AlternateMetadataV8Test, WorkerWrittenHtmlAtVersionNineIsReadable) {
  AlternateMetadata meta;
  meta.content_type = ContentType::kHtml;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed;
  auto read = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->version, 9);
}

TEST(AlternateMetadataV8Test, EpochRoundTrip) {
  const uint64_t values[] = {
      0u,                     // absent
      1u,                     // first generation
      0xFFFFFFFFull,          // just past what 32 bits could hold
      0x0123456789ABCDEFull,  // all bytes distinct — catches byte-order slips
      0xFFFFFFFFFFFFFFFFull,  // max
  };
  for (uint64_t v : values) {
    SCOPED_TRACE(v);
    AlternateMetadata meta;
    meta.origin_epoch = v;
    auto result = AlternateMetadata::Deserialize(meta.Serialize());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->origin_epoch, v);
  }
}

TEST(AlternateMetadataV8Test, EpochDefaultsToZeroMeaningAbsent) {
  AlternateMetadata meta;
  EXPECT_EQ(meta.origin_epoch, 0u);
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_epoch, 0u);
}

TEST(AlternateMetadataV8Test, EpochIsIndependentOfNeighbouringFields) {
  // The epoch sits between the v7 hashes and the Cache-Control length; an
  // off-by-one in either direction shows up as one of these disagreeing.
  AlternateMetadata meta;
  meta.origin_epoch = 0xAABBCCDD11223344ull;
  meta.origin_etag = "\"e\"";
  meta.origin_content_length = 0xDEADBEEFu;
  meta.origin_cache_control = "no-store";
  for (size_t i = 0; i < AlternateMetadata::kHashSize; ++i) {
    meta.origin_html_hash[i] = static_cast<std::byte>(0x11);
    meta.render_source_hash[i] = static_cast<std::byte>(0x22);
  }

  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_epoch, 0xAABBCCDD11223344ull);
  EXPECT_EQ(result->origin_content_length, 0xDEADBEEFu);
  EXPECT_EQ(result->origin_etag, "\"e\"");
  EXPECT_EQ(result->origin_cache_control, "no-store");
  EXPECT_EQ(result->origin_html_hash[31], std::byte{0x11});
  EXPECT_EQ(result->render_source_hash[0], std::byte{0x22});
}

TEST(AlternateMetadataV8Test, RawCacheControlRoundTrip) {
  const char* values[] = {
      "",
      "no-store",
      "public, max-age=31536000, immutable",
      "private=\"Set-Cookie\", max-age=600, stale-while-revalidate=30",
  };
  for (const char* v : values) {
    SCOPED_TRACE(v);
    AlternateMetadata meta;
    meta.origin_cache_control = v;
    auto serialized = meta.Serialize();
    auto result = AlternateMetadata::Deserialize(serialized);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->origin_cache_control, v);
    EXPECT_EQ(result->wire_cc_len, std::string(v).size());
    EXPECT_EQ(result->WireSize(), serialized.size());
    EXPECT_EQ(result->consumed_size, serialized.size());
  }
}

TEST(AlternateMetadataV8Test, RawCacheControlAtCapRoundTrips) {
  AlternateMetadata meta;
  meta.origin_cache_control =
      std::string(AlternateMetadata::kMaxOriginCcLen, 'x');
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cache_control.size(),
            AlternateMetadata::kMaxOriginCcLen);
  EXPECT_EQ(result->WireSize(), serialized.size());
}

TEST(AlternateMetadataV8Test, RawCacheControlOverCapIsStoredAbsentNotClipped) {
  AlternateMetadata meta;
  meta.origin_cache_control =
      std::string(AlternateMetadata::kMaxOriginCcLen + 1, 'x');
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->origin_cache_control.empty())
      << "a clipped directive list is a different caching policy from the one "
         "the origin sent; absent is the honest answer";
}

TEST(AlternateMetadataV8Test, RawCacheControlStripsControlCharacters) {
  AlternateMetadata meta;
  meta.origin_cache_control = std::string("max-age=60\r\nX-Injected: 1");
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cache_control, "max-age=60X-Injected: 1");
  // Stored length and the value handed back agree, so the content offset is
  // still right — the same invariant wire_ct_len exists for.
  EXPECT_EQ(result->wire_cc_len, result->origin_cache_control.size());
  EXPECT_EQ(result->WireSize(), serialized.size());
  EXPECT_EQ(result->consumed_size, serialized.size());
}

// The READ-side strip, which the write-side test above cannot reach: Serialize
// already cleans the bytes, so ParseV8Fields' strip is a no-op on anything this
// build wrote.  It exists for bytes this build did NOT write — an older or
// hostile writer, or a corrupted blob — and it is the header-splitting guard,
// so a regression that dropped it must not ship green.
//
// The invariant that matters is not just the stripped value: wire_cc_len stays
// the ON-WIRE length, so consumed_size and WireSize() still equal the blob size
// and the content offset survives a wire-dirty value.
TEST(AlternateMetadataV8Test, ReadSideStripsControlCharactersFromWireBytes) {
  AlternateMetadata meta;
  meta.origin_cache_control = "abcdef";  // 6 clean bytes, to be overwritten
  auto serialized = meta.Serialize();
  const size_t blob_size = serialized.size();

  // Patch CR/LF/NUL directly into the stored Cache-Control bytes, which is
  // exactly what a blob from a writer without the strip would look like.
  const char dirty[6] = {'a', '\r', '\n', 'b', '\0', 'c'};
  std::memcpy(&serialized[blob_size - 6], dirty, 6);

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cache_control, "abc")
      << "CR, LF and NUL must be stripped on the way out, not only on the way "
         "in";
  EXPECT_EQ(result->wire_cc_len, 6u)
      << "wire_cc_len is the on-wire length, not the stripped length";
  EXPECT_EQ(result->WireSize(), blob_size);
  EXPECT_EQ(result->consumed_size, blob_size)
      << "a wire-dirty value must not move the content offset";
}

TEST(AlternateMetadataV8Test, WriteSideStripsNulFromCacheControl) {
  AlternateMetadata meta;
  meta.origin_cache_control = std::string("a\r\nb\0c", 6);
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cache_control, "abc");
  EXPECT_EQ(result->wire_cc_len, 3u)
      << "on write the stored length is the sanitized length, so the two agree";
  EXPECT_EQ(result->WireSize(), serialized.size());
}

TEST(AlternateMetadataV8Test, CapAppliesAfterStripping) {
  // A header that is over the cap in RAW bytes but under it once control
  // characters are removed is STORED, not dropped.  The cap is a bound on what
  // is kept, not a filter on what arrives.
  std::string raw;
  raw.reserve(300);
  for (int i = 0; i < 100; ++i) raw += "a\r\n";  // 300 raw -> 100 stripped

  AlternateMetadata meta;
  meta.origin_cache_control = raw;
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_cache_control.size(), 100u);
  EXPECT_EQ(result->origin_cache_control, std::string(100, 'a'));
}

TEST(AlternateMetadataV8Test, OverCapAfterStrippingIsStoredAbsent) {
  // The other side of the same rule: still over the cap once stripped -> absent.
  std::string raw;
  for (int i = 0; i < 300; ++i) raw += "a\r\n";  // 900 raw -> 300 stripped

  AlternateMetadata meta;
  meta.origin_cache_control = raw;
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->origin_cache_control.empty());
}

TEST(AlternateMetadataV8Test, RejectsCacheControlLengthOverCapOnRead) {
  // A blob claiming a longer Cache-Control than the format permits is not
  // parsed, however long the blob itself is.
  AlternateMetadata meta;
  meta.origin_cache_control = "max-age=1";
  auto serialized = meta.Serialize();
  // cc_len is the last fixed field: it sits 2 + cc bytes from the end.
  size_t cc_len_offset = serialized.size() - 2 - 9;
  uint16_t over_cap = AlternateMetadata::kMaxOriginCcLen + 1;
  std::memcpy(&serialized[cc_len_offset], &over_cap, 2);
  serialized.resize(serialized.size() + over_cap, std::byte{'y'});
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

TEST(AlternateMetadataV8Test, RejectsCacheControlRunningPastTheEnd) {
  AlternateMetadata meta;
  meta.origin_cache_control = "max-age=1";
  auto serialized = meta.Serialize();
  size_t cc_len_offset = serialized.size() - 2 - 9;
  uint16_t claimed = 200;  // under the cap, but the bytes are not there
  std::memcpy(&serialized[cc_len_offset], &claimed, 2);
  EXPECT_FALSE(AlternateMetadata::Deserialize(serialized).has_value());
}

// ===========================================================================
// Flags byte.
// ===========================================================================

TEST(AlternateMetadataV8Test, OriginVariesAcceptBitDoesNotCollide) {
  EXPECT_EQ(AlternateMetadata::kFlagOriginVariesAccept, 0x04);
  EXPECT_EQ(AlternateMetadata::kFlagOriginVariesAccept &
                AlternateMetadata::kFlagNeedsRevalidation,
            0);
  EXPECT_EQ(AlternateMetadata::kFlagOriginVariesAccept &
                AlternateMetadata::kFlagWorkerProcessed,
            0);
  EXPECT_EQ(AlternateMetadata::kFlagsAssignedMask, 0x7F);
}

TEST(AlternateMetadataV8Test, OriginHeadersNotReproducibleBitDoesNotCollide) {
  EXPECT_EQ(AlternateMetadata::kFlagOriginHeadersNotReproducible, 0x08);
  EXPECT_EQ(AlternateMetadata::kFlagOriginHeadersNotReproducible &
                (AlternateMetadata::kFlagNeedsRevalidation |
                 AlternateMetadata::kFlagWorkerProcessed |
                 AlternateMetadata::kFlagOriginVariesAccept),
            0);
  // Allocated, so it is no longer one of the free bits — that is the whole
  // content of an allocation, and it is what a future allocator reads.
  EXPECT_NE(AlternateMetadata::kFlagsAssignedMask &
                AlternateMetadata::kFlagOriginHeadersNotReproducible,
            0);
}

TEST(AlternateMetadataV8Test, HeadersSidecarBitsAreDistinctAndAssigned) {
  EXPECT_EQ(AlternateMetadata::kFlagHeadersClassified, 0x10);
  EXPECT_EQ(AlternateMetadata::kFlagHeadersSidecarRequired, 0x20);
  EXPECT_EQ(AlternateMetadata::kFlagsAssignedMask, 0x7F);
  EXPECT_NE(AlternateMetadata::kFlagsAssignedMask &
                AlternateMetadata::kFlagHeadersClassified,
            0);
  EXPECT_NE(AlternateMetadata::kFlagsAssignedMask &
                AlternateMetadata::kFlagHeadersSidecarRequired,
            0);
}

TEST(AlternateMetadataV8Test, AnonymousRecordedBitIsDistinctAndAssigned) {
  EXPECT_EQ(AlternateMetadata::kFlagAnonymousRecorded, 0x40);
  EXPECT_EQ(AlternateMetadata::kFlagAnonymousRecorded &
                (AlternateMetadata::kFlagNeedsRevalidation |
                 AlternateMetadata::kFlagWorkerProcessed |
                 AlternateMetadata::kFlagOriginVariesAccept |
                 AlternateMetadata::kFlagOriginHeadersNotReproducible |
                 AlternateMetadata::kFlagHeadersClassified |
                 AlternateMetadata::kFlagHeadersSidecarRequired),
            0);
  EXPECT_NE(AlternateMetadata::kFlagsAssignedMask &
                AlternateMetadata::kFlagAnonymousRecorded,
            0);
  AlternateMetadata meta;
  meta.flags = AlternateMetadata::kFlagWorkerProcessed |
               AlternateMetadata::kFlagAnonymousRecorded;
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->flags, meta.flags);
}

TEST(AlternateMetadataV8Test, HeadersSidecarBitsSetClearAndRoundTrip) {
  const uint8_t both = AlternateMetadata::kFlagHeadersClassified |
                       AlternateMetadata::kFlagHeadersSidecarRequired;
  for (const uint8_t bits : {AlternateMetadata::kFlagHeadersClassified,
                             static_cast<uint8_t>(both)}) {
    AlternateMetadata meta;
    meta.flags = AlternateMetadata::kFlagWorkerProcessed | bits;
    auto result = AlternateMetadata::Deserialize(meta.Serialize());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->flags, meta.flags);

    // Clear on the read copy, rewrite: the other bits survive, these go.
    result->flags &= static_cast<uint8_t>(~both);
    auto cleared = AlternateMetadata::Deserialize(result->Serialize());
    ASSERT_TRUE(cleared.has_value());
    EXPECT_EQ(cleared->flags, AlternateMetadata::kFlagWorkerProcessed);
  }
  // Required alone is representable too (the byte is opaque).
  AlternateMetadata only;
  only.flags = AlternateMetadata::kFlagHeadersSidecarRequired;
  auto solo = AlternateMetadata::Deserialize(only.Serialize());
  ASSERT_TRUE(solo.has_value());
  EXPECT_EQ(solo->flags, AlternateMetadata::kFlagHeadersSidecarRequired);
}

TEST(AlternateMetadataV8Test, EveryFlagsByteValueRoundTrips) {
  for (int f = 0; f <= 0xFF; ++f) {
    AlternateMetadata meta;
    meta.flags = static_cast<uint8_t>(f);
    auto result = AlternateMetadata::Deserialize(meta.Serialize());
    ASSERT_TRUE(result.has_value()) << f;
    EXPECT_EQ(result->flags, f) << "the flags byte is opaque on the wire";
  }
}

TEST(AlternateMetadataV8Test, UnknownFlagBitsSurviveReadAndRewrite) {
  // A bit set by a peer this build does not know about must come back out the
  // other side.  Clearing it is indistinguishable, downstream, from never
  // having been told — and this is the contract v7 already had.
  const uint8_t unknown = static_cast<uint8_t>(
      ~AlternateMetadata::kFlagsAssignedMask & 0xFF);  // 0x80
  AlternateMetadata meta;
  meta.flags =
      static_cast<uint8_t>(AlternateMetadata::kFlagWorkerProcessed | unknown);

  auto first = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->flags & unknown, unknown);

  auto second = AlternateMetadata::Deserialize(first->Serialize());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->flags, meta.flags);
}

TEST(AlternateMetadataV8Test, OriginVariesAcceptRoundTripsBesideOtherFlags) {
  AlternateMetadata meta;
  meta.flags = AlternateMetadata::kFlagNeedsRevalidation |
               AlternateMetadata::kFlagWorkerProcessed |
               AlternateMetadata::kFlagOriginVariesAccept;
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->flags & AlternateMetadata::kFlagOriginVariesAccept);
  EXPECT_TRUE(result->flags & AlternateMetadata::kFlagNeedsRevalidation);
  EXPECT_TRUE(result->flags & AlternateMetadata::kFlagWorkerProcessed);
}

TEST(AlternateMetadataV8Test,
     OriginHeadersNotReproducibleRoundTripsBesideOtherFlags) {
  // Every assigned bit at once, because the two origin-state markers answer
  // independent questions about the same response and an entry can carry
  // both: an origin that negotiates on `Accept` may equally have sent a
  // header no serve can reproduce.
  AlternateMetadata meta;
  meta.flags = AlternateMetadata::kFlagsAssignedMask;
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->flags, AlternateMetadata::kFlagsAssignedMask);
  EXPECT_TRUE(result->flags &
              AlternateMetadata::kFlagOriginHeadersNotReproducible);

  // And alone: setting it does not drag another marker along.
  AlternateMetadata only;
  only.flags = AlternateMetadata::kFlagOriginHeadersNotReproducible;
  auto solo = AlternateMetadata::Deserialize(only.Serialize());
  ASSERT_TRUE(solo.has_value());
  EXPECT_EQ(solo->flags, AlternateMetadata::kFlagOriginHeadersNotReproducible);
}

// ===========================================================================
// Prefix budget.
// ===========================================================================

TEST(AlternateMetadataV8Test, BudgetArithmeticIsTheSumOfItsParts) {
  EXPECT_EQ(AlternateMetadata::kV8FixedSuffixSize,
            AlternateMetadata::kV7FixedSuffixSize + 8 + 2);
  EXPECT_EQ(
      AlternateMetadata::kMaxSerializedSizeAtBudget,
      AlternateMetadata::kFixedPrefixSize + AlternateMetadata::kMaxOriginCtLen +
          AlternateMetadata::kV8FixedSuffixSize +
          AlternateMetadata::kEtagBudget + AlternateMetadata::kMaxOriginCcLen);
  EXPECT_EQ(AlternateMetadata::kMaxSerializedSizeAtBudget, 750u);
  EXPECT_EQ(AlternateMetadata::kRecommendedMaxMetadataSize, 878u);
}

TEST(AlternateMetadataV8Test, WorstCaseAtBudgetSerializesToExactlyTheBudget) {
  AlternateMetadata meta;
  meta.origin_content_type =
      std::string(AlternateMetadata::kMaxOriginCtLen, 'c');
  meta.origin_etag = std::string(AlternateMetadata::kEtagBudget, 'e');
  meta.origin_cache_control =
      std::string(AlternateMetadata::kMaxOriginCcLen, 'v');
  meta.origin_epoch = 0xFFFFFFFFFFFFFFFFull;
  meta.flags = 0xFF;

  auto serialized = meta.Serialize();
  EXPECT_EQ(serialized.size(), AlternateMetadata::kMaxSerializedSizeAtBudget);
  EXPECT_LE(serialized.size(), AlternateMetadata::kRecommendedMaxMetadataSize);

  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_type.size(),
            AlternateMetadata::kMaxOriginCtLen);
  EXPECT_EQ(result->origin_etag.size(), AlternateMetadata::kEtagBudget);
  EXPECT_EQ(result->origin_cache_control.size(),
            AlternateMetadata::kMaxOriginCcLen);
  EXPECT_EQ(result->WireSize(), serialized.size());
  EXPECT_EQ(result->consumed_size, serialized.size());
}

TEST(AlternateMetadataV8Test, ReserveCoversDoublingTheEtagBudget) {
  // What the 128 B of reserve is for, asserted rather than described: an ETag
  // at the same 256 B class as the other two origin strings still fits the
  // ceiling without a config change.
  AlternateMetadata meta;
  meta.origin_content_type =
      std::string(AlternateMetadata::kMaxOriginCtLen, 'c');
  meta.origin_etag = std::string(256, 'e');
  meta.origin_cache_control =
      std::string(AlternateMetadata::kMaxOriginCcLen, 'v');
  EXPECT_LE(meta.Serialize().size(),
            AlternateMetadata::kRecommendedMaxMetadataSize);
}

TEST(AlternateMetadataV8Test, ContentTypeStaysCappedAndTruncates) {
  // Unchanged from v7 and restated here because the ETag and Cache-Control
  // caps deliberately behave differently: the content type IS clipped.
  AlternateMetadata meta;
  meta.origin_content_type =
      std::string(AlternateMetadata::kMaxOriginCtLen + 50, 'c');
  auto result = AlternateMetadata::Deserialize(meta.Serialize());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_content_type.size(),
            AlternateMetadata::kMaxOriginCtLen);
}

TEST(AlternateMetadataV8Test, LongEtagStillSerializesUncapped) {
  // The ETag is budgeted, not capped: a long one is stored whole and it is the
  // cache's size check that decides whether the entry is kept.
  AlternateMetadata meta;
  meta.origin_etag = std::string(4096, 'e');
  auto serialized = meta.Serialize();
  auto result = AlternateMetadata::Deserialize(serialized);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->origin_etag.size(), 4096u);
  EXPECT_GT(serialized.size(), AlternateMetadata::kRecommendedMaxMetadataSize);
}

// ===========================================================================
// The score field on gzip/brotli coded entries: the encoded origin size.
// ===========================================================================

constexpr uint32_t kGzipMask = 1u << 6;
constexpr uint32_t kBrotliMask = 2u << 6;

AlternateMetadata CodedMeta(ContentType type, uint32_t mask,
                            uint32_t origin_length) {
  AlternateMetadata meta;
  meta.content_type = type;
  meta.full_mask = mask;
  meta.origin_content_length = origin_length;
  return meta;
}

TEST(AlternateMetadataEncodedLengthTest, RoundTripsThroughTheV8Wire) {
  for (const ContentType type :
       {ContentType::kHtml, ContentType::kCss, ContentType::kJs}) {
    for (const uint32_t mask : {kGzipMask, kBrotliMask}) {
      for (const uint32_t origin : {1u, 37u, 1000u, 54321u, 3000000u}) {
        for (const double ratio : {0.0, 0.0004, 0.2, 0.5, 1.0, 1.1}) {
          const uint64_t encoded = static_cast<uint64_t>(ratio * origin);
          AlternateMetadata meta = CodedMeta(type, mask, origin);
          meta.SetEncodedOriginLength(encoded);
          const auto wire = meta.Serialize();
          const auto parsed = AlternateMetadata::Deserialize(wire);
          ASSERT_TRUE(parsed.has_value());
          // The current version keeps the version 8 layout.
          EXPECT_EQ(parsed->version, AlternateMetadata::kCurrentVersion);
          const auto got = parsed->EncodedOriginLength();
          ASSERT_TRUE(got.has_value()) << ratio << " of " << origin;
          // One part in 50000 of the origin, plus rounding.
          const double tolerance = origin / 50000.0 + 1.0;
          EXPECT_NEAR(static_cast<double>(*got), static_cast<double>(encoded),
                      tolerance)
              << ratio << " of " << origin;
        }
      }
    }
  }
}

TEST(AlternateMetadataEncodedLengthTest,
     IdentityAndImagesMeasureAgainstOrigin) {
  const AlternateMetadata css = CodedMeta(ContentType::kCss, 0, 4000);
  EXPECT_EQ(css.EncodedOriginLength(), 4000u);
  const AlternateMetadata image = CodedMeta(ContentType::kImage, 0, 9000);
  EXPECT_EQ(image.EncodedOriginLength(), 9000u);
  // Setting on an identity entry is a no-op; the score is not disturbed.
  AlternateMetadata scored = CodedMeta(ContentType::kImage, 0, 9000);
  scored.SetQualityScoreX100(8123);
  scored.SetEncodedOriginLength(10);
  EXPECT_EQ(scored.QualityScoreX100(), 8123);
  EXPECT_EQ(scored.EncodedOriginLength(), 9000u);
}

TEST(AlternateMetadataEncodedLengthTest, AbsentWhenNotRecorded) {
  // A coded entry that never carried the value (written by an earlier build).
  EXPECT_FALSE(CodedMeta(ContentType::kCss, kGzipMask, 4000)
                   .EncodedOriginLength()
                   .has_value());
  // Unknown origin length.
  AlternateMetadata no_origin = CodedMeta(ContentType::kCss, kGzipMask, 0);
  no_origin.SetEncodedOriginLength(10);
  EXPECT_FALSE(no_origin.EncodedOriginLength().has_value());
  // Larger than the field can express (more than 1.11 times the origin).
  AlternateMetadata big = CodedMeta(ContentType::kCss, kBrotliMask, 100);
  big.SetEncodedOriginLength(112);
  EXPECT_FALSE(big.EncodedOriginLength().has_value());
  big.SetEncodedOriginLength(110);
  EXPECT_EQ(big.EncodedOriginLength(), 110u);
  // Clearing drops an inherited value.
  big.ClearEncodedOriginLength();
  EXPECT_FALSE(big.EncodedOriginLength().has_value());
}

TEST(AlternateMetadataEncodedLengthTest, AValueAboveTheFourByteRangeIsAbsent) {
  AlternateMetadata big = CodedMeta(ContentType::kCss, kGzipMask, 4000000000u);
  big.SetEncodedOriginLength(4400000000ull);  // 1.1 x the origin
  EXPECT_FALSE(big.EncodedOriginLength().has_value());
  big.SetEncodedOriginLength(2000000000ull);
  EXPECT_NEAR(static_cast<double>(*big.EncodedOriginLength()), 2.0e9, 1.0e5);
}

TEST(AlternateMetadataEncodedLengthTest, TheTwoMeaningsDoNotMix) {
  AlternateMetadata coded = CodedMeta(ContentType::kCss, kGzipMask, 5000);
  coded.SetEncodedOriginLength(1200);
  EXPECT_FALSE(coded.QualityScoreX100().has_value());
  AlternateMetadata image = CodedMeta(ContentType::kImage, 0, 5000);
  image.SetQualityScoreX100(9500);
  EXPECT_EQ(image.QualityScoreX100(), 9500);
  image.SetQualityScoreX100(std::nullopt);
  EXPECT_FALSE(image.QualityScoreX100().has_value());
  image.SetQualityScoreX100(10001);
  EXPECT_FALSE(image.QualityScoreX100().has_value());
  // An identity entry whose field holds a code (hand-built wire) is read as
  // "score not available" and the code is dropped on parse.
  AlternateMetadata identity = CodedMeta(ContentType::kCss, 0, 5000);
  identity.ssimulacra2_score_x100 = 12000;
  const auto parsed = AlternateMetadata::Deserialize(identity.Serialize());
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->ssimulacra2_score_x100, AlternateMetadata::kScoreNA);
}

// What an entry carrying the value looks like to a reader built before it
// existed: same version, same length, and a score field that reader's parse
// clamps to "not available".  Only the two score bytes differ from an entry
// without the value.
TEST(AlternateMetadataEncodedLengthTest, ReleasedReadersSeeScoreNotAvailable) {
  AlternateMetadata without = CodedMeta(ContentType::kCss, kBrotliMask, 7777);
  without.origin_content_type = "text/css";
  without.origin_etag = "\"abc\"";
  without.origin_cache_control = "max-age=60";
  const auto plain = without.Serialize();
  for (const uint64_t encoded : {0ull, 1ull, 2500ull, 7777ull, 8600ull}) {
    AlternateMetadata with = without;
    with.SetEncodedOriginLength(encoded);
    const auto wire = with.Serialize();
    ASSERT_EQ(wire.size(), plain.size());
    ASSERT_EQ(wire[0], plain[0]);
    EXPECT_EQ(static_cast<uint8_t>(wire[0]),
              AlternateMetadata::kCurrentVersion);
    // Fixed prefix 9 + content type + the cache-control block (14) -> score.
    const size_t score_at = AlternateMetadata::kFixedPrefixSize +
                            without.origin_content_type.size() + 14;
    size_t differing = 0;
    for (size_t i = 0; i < wire.size(); ++i) {
      if (wire[i] != plain[i]) {
        ++differing;
        EXPECT_TRUE(i == score_at || i == score_at + 1) << i;
      }
    }
    EXPECT_LE(differing, 2u);
    uint16_t raw = 0;
    std::memcpy(&raw, &wire[score_at], 2);
    EXPECT_GT(raw, 10000);
    EXPECT_NE(raw, AlternateMetadata::kScoreNA);
    // The released reader's rule, verbatim: above 10000 and not 0xFFFF -> NA.
    const uint16_t released = (raw > 10000 && raw != 0xFFFF) ? 0xFFFF : raw;
    EXPECT_EQ(released, AlternateMetadata::kScoreNA);
  }
}

}  // namespace
}  // namespace pagespeed
