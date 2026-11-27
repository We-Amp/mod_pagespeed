// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for the cache-HIT weak-ETag construction (src/nginx/etag_util.h).
//
// The load-bearing property: two DIFFERENT content revisions must never
// share an ETag just because they happen to have the same byte length —
// a false If-None-Match 304 would pin the stale copy at the client.

#include "src/nginx/etag_util.h"

#include <array>
#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "lib/classify/alternate_metadata.h"

using pagespeed::AlternateMetadata;
using pagespeed::ContentIdentity64;
using pagespeed::FormatHitETag;
using pagespeed::kHitETagBufSize;

namespace {

constexpr size_t kBodyLen = 13;  // 0x0d

AlternateMetadata BaseMeta() {
  AlternateMetadata meta;
  meta.full_mask = 0x08;
  meta.flags = 0;
  return meta;
}

std::array<std::byte, AlternateMetadata::kHashSize> FillHash(uint8_t v) {
  std::array<std::byte, AlternateMetadata::kHashSize> h;
  h.fill(std::byte{v});
  return h;
}

std::string Format(const AlternateMetadata& meta, size_t body_len) {
  char buf[kHitETagBufSize];
  size_t n = FormatHitETag(meta, body_len, buf, sizeof(buf));
  EXPECT_GT(n, 0u);
  return std::string(buf, n);
}

// (i) Same mask, flags, and byte length but different content hashes must
// produce different ETags — the original false-304 bug.
TEST(EtagUtilTest, SameLengthDifferentContentDifferentTags) {
  AlternateMetadata a = BaseMeta();
  a.origin_html_hash = FillHash(0xAA);
  AlternateMetadata b = BaseMeta();
  b.origin_html_hash = FillHash(0xBB);

  EXPECT_NE(Format(a, kBodyLen), Format(b, kBodyLen));
}

// (ii) Unchanged content → byte-identical ETag on every call.
TEST(EtagUtilTest, StableAcrossCalls) {
  AlternateMetadata meta = BaseMeta();
  meta.origin_html_hash = FillHash(0xAA);

  const std::string first = Format(meta, kBodyLen);
  EXPECT_EQ(first, Format(meta, kBodyLen));
  EXPECT_EQ(first, "W/\"ps-0000000800-aaaaaaaaaaaaaaaa-000000000000000d\"");
}

// (iii) No content hash and no origin validators (pre-v7 entry) → NO tag:
// FormatHitETag returns 0 and the caller emits no ETag header. A
// mask+flags+length-only tag would validate any revision of the same
// optimized length, answering 304 for changed bytes; with no tag there is
// no 304 at all, so a stale copy can never be confirmed.
TEST(EtagUtilTest, MissingIdentityEmitsNoTag) {
  AlternateMetadata meta = BaseMeta();

  EXPECT_FALSE(ContentIdentity64(meta).has_value());
  char buf[kHitETagBufSize];
  EXPECT_EQ(FormatHitETag(meta, kBodyLen, buf, sizeof(buf)), 0u);
}

// (iv) Upgrade path: an entry without identity emits no tag, so its client
// holds no validator; once the entry carries identity it emits one. There is
// no shape the two could share, and the formerly-untagged client gets a full
// 200 exactly once (correct direction).
TEST(EtagUtilTest, GainingIdentityGivesTheEntryAValidator) {
  AlternateMetadata untagged = BaseMeta();
  AlternateMetadata hashed = BaseMeta();
  hashed.origin_html_hash = FillHash(0xAA);

  char buf[kHitETagBufSize];
  EXPECT_EQ(FormatHitETag(untagged, kBodyLen, buf, sizeof(buf)), 0u);
  EXPECT_GT(FormatHitETag(hashed, kBodyLen, buf, sizeof(buf)), 0u);
}

// Tier-1 identity is the first 8 bytes of origin_html_hash, big-endian.
TEST(EtagUtilTest, Tier1UsesHashPrefixBigEndian) {
  AlternateMetadata meta = BaseMeta();
  meta.origin_html_hash = FillHash(0x00);
  meta.origin_html_hash[0] = std::byte{0x01};
  meta.origin_html_hash[7] = std::byte{0xEF};
  // Trailing bytes beyond the first 8 make the hash non-zero but must not
  // affect the identity value.
  meta.origin_html_hash[31] = std::byte{0x99};

  auto id = ContentIdentity64(meta);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 0x01000000000000EFULL);
}

// Tier-2: origin validators discriminate revisions for entries without a
// stored content hash (CSS/images/HTML without agent_optimize).
TEST(EtagUtilTest, Tier2OriginValidatorsDiscriminate) {
  AlternateMetadata v1 = BaseMeta();
  v1.origin_etag = "\"v1\"";
  AlternateMetadata v2 = BaseMeta();
  v2.origin_etag = "\"v2\"";

  EXPECT_NE(Format(v1, kBodyLen), Format(v2, kBodyLen));

  // Pinned reference values (independently computed FNV-1a 64); these are
  // baked into t/301-etag.t and the format is client-visible state — a
  // change here churns every cached validator in the field.
  EXPECT_EQ(Format(v1, kBodyLen),
            "W/\"ps-0000000800-a3eff4fa2b510343-000000000000000d\"");
  EXPECT_EQ(Format(v2, kBodyLen),
            "W/\"ps-0000000800-985553fd825831f2-000000000000000d\"");
}

TEST(EtagUtilTest, Tier2LastModifiedOnlyWorks) {
  AlternateMetadata meta = BaseMeta();
  meta.origin_last_modified = 1700000000;

  ASSERT_TRUE(ContentIdentity64(meta).has_value());
  EXPECT_EQ(Format(meta, kBodyLen),
            "W/\"ps-0000000800-c144e057968ff669-000000000000000d\"");

  AlternateMetadata other = BaseMeta();
  other.origin_last_modified = 1700000001;
  EXPECT_NE(Format(meta, kBodyLen), Format(other, kBodyLen));
}

// Tier-1 wins over tier-2 when both are present.
TEST(EtagUtilTest, HashTakesPrecedenceOverOriginValidators) {
  AlternateMetadata meta = BaseMeta();
  meta.origin_html_hash = FillHash(0xAA);
  meta.origin_etag = "\"v1\"";

  EXPECT_EQ(Format(meta, kBodyLen),
            "W/\"ps-0000000800-aaaaaaaaaaaaaaaa-000000000000000d\"");
}

// Mask and flags still differentiate representations (RFC 9110 §8.8.3:
// per-representation validators) even for identical content identity.
TEST(EtagUtilTest, MaskAndFlagsDifferentiateRepresentations) {
  AlternateMetadata identity_enc = BaseMeta();
  identity_enc.origin_html_hash = FillHash(0xAA);
  AlternateMetadata gzip_enc = identity_enc;
  gzip_enc.full_mask = 0x09;  // different encoding dimension

  EXPECT_NE(Format(identity_enc, kBodyLen), Format(gzip_enc, 7));
  EXPECT_NE(Format(identity_enc, kBodyLen), Format(gzip_enc, kBodyLen));

  AlternateMetadata revalidating = identity_enc;
  revalidating.flags = AlternateMetadata::kFlagNeedsRevalidation;
  EXPECT_NE(Format(identity_enc, kBodyLen), Format(revalidating, kBodyLen));
}

// An entry stored from an origin that negotiates on Accept itself must not
// validate against one stored from an origin that does not, and vice versa.
//
// The two are not the same representation even when the bytes and the origin
// validators match: one is served with `Accept` named in `Vary` and is never
// re-optimized, the other is served without it and may be. A shared ETag
// would let a client holding one get a 304 for the other and keep serving a
// response whose downstream cache-keying is wrong.
//
// The separation is structural, not a special case: the flags byte is already
// part of the tag, so marking the entry moves it off the unmarked entry's tag
// automatically. This pins that it stays that way. Bounded extra revalidation
// when an origin gains or loses `Vary: Accept` is the accepted cost — one
// full 200 per client, once, in the correct direction.
TEST(EtagUtilTest, OriginVariesAcceptSeparatesValidators) {
  AlternateMetadata unmarked = BaseMeta();
  unmarked.origin_html_hash = FillHash(0xAA);

  AlternateMetadata marked = unmarked;
  marked.flags |= AlternateMetadata::kFlagOriginVariesAccept;

  EXPECT_NE(Format(unmarked, kBodyLen), Format(marked, kBodyLen));

  // Also on the no-identity arm, which is the shape a marked entry takes
  // when its origin sends neither ETag nor Last-Modified and the worker has
  // not stamped it: the population most likely to hold marked entries.
  // Neither entry emits a tag at all, so neither can ever validate the
  // other's response: the separation holds structurally (no shared
  // validator) rather than through the flags byte.
  AlternateMetadata untagged_unmarked = BaseMeta();
  AlternateMetadata untagged_marked = untagged_unmarked;
  untagged_marked.flags |= AlternateMetadata::kFlagOriginVariesAccept;

  char buf[kHitETagBufSize];
  EXPECT_FALSE(ContentIdentity64(untagged_unmarked).has_value());
  EXPECT_FALSE(ContentIdentity64(untagged_marked).has_value());
  EXPECT_EQ(FormatHitETag(untagged_unmarked, kBodyLen, buf, sizeof(buf)), 0u);
  EXPECT_EQ(FormatHitETag(untagged_marked, kBodyLen, buf, sizeof(buf)), 0u);

  // And the marker is independent of the other assigned flag bits: an entry
  // that is marked AND needs revalidation is a third distinct validator.
  AlternateMetadata marked_revalidating = marked;
  marked_revalidating.flags |= AlternateMetadata::kFlagNeedsRevalidation;
  EXPECT_NE(Format(marked, kBodyLen), Format(marked_revalidating, kBodyLen));
  EXPECT_NE(Format(unmarked, kBodyLen), Format(marked_revalidating, kBodyLen));
}

// Buffer discipline: too-small buffers yield 0 (header suppressed), the
// documented buffer size always suffices, and output length is exact.
TEST(EtagUtilTest, BufferBounds) {
  AlternateMetadata meta = BaseMeta();
  meta.origin_html_hash = FillHash(0xFF);

  char big[kHitETagBufSize];
  size_t n = FormatHitETag(meta, SIZE_MAX, big, sizeof(big));
  ASSERT_GT(n, 0u);
  EXPECT_LT(n, kHitETagBufSize);  // room for NUL

  char small[10];
  EXPECT_EQ(FormatHitETag(meta, SIZE_MAX, small, sizeof(small)), 0u);
  EXPECT_EQ(FormatHitETag(meta, SIZE_MAX, big, 0), 0u);
  EXPECT_EQ(FormatHitETag(meta, SIZE_MAX, nullptr, sizeof(big)), 0u);
}

}  // namespace
