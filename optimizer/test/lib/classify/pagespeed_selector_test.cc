// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for PageSpeedSelector — the Cyclone StorageAlternateSelector
// that picks the best cache alternate for a client capability mask.

#include "lib/classify/pagespeed_selector.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "cyclone/alternate.hpp"
#include "gtest/gtest.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

// Helper: build a cyclone::AlternateInfo from a full 32-bit mask.
// The selector derives the stored mask from AlternateId, not from
// header metadata (see Cyclone set_header multi-process bug).
cyclone::AlternateInfo MakeAlternateInfo(uint32_t full_mask) {
  cyclone::AlternateInfo info;
  info.id = static_cast<cyclone::AlternateId>(full_mask & 0xFF);
  return info;
}

// Helper: build selection context with a client mask.
cyclone::AlternateSelectionContext MakeContext(uint32_t client_mask) {
  cyclone::AlternateSelectionContext ctx;
  // Store 4-byte LE client mask in request_metadata.
  static thread_local std::vector<std::byte> buf(4);
  std::memcpy(buf.data(), &client_mask, 4);
  ctx.request_metadata = buf;
  return ctx;
}

// --- ScoreAlternate tests ---

TEST(ScoreAlternateTest, ExactMatch) {
  uint32_t mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                 CapabilityMask::Viewport::kMobile,
                                 CapabilityMask::PixelDensity::k2xPlus,
                                 CapabilityMask::SaveData::kOn,
                                 CapabilityMask::TransferEncoding::kIdentity)
                      .Encode();
  // Exact match: 1000 + 2000 + 40 + 20 + 60 = 3120
  EXPECT_EQ(ScoreAlternate(mask, mask), 3120);
}

TEST(ScoreAlternateTest, FormatMismatch) {
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  // Format mismatch (neither side Original/SVG) hard-disqualifies (#1331):
  // the kAvif client almost certainly decodes WebP, but the mask cannot
  // prove it, and an unprovable format must not be served.
  EXPECT_EQ(ScoreAlternate(client, stored), 0);
}

TEST(ScoreAlternateTest, EncodingExactMatch) {
  // Client wants brotli, stored is brotli — exact encoding match.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  // 1000 + 2000 + 40 + 20 + 60 = 3120
  EXPECT_EQ(ScoreAlternate(client, stored), 3120);
}

TEST(ScoreAlternateTest, EncodingIdentityFallback) {
  // Client wants brotli, stored is identity — fallback.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  // 1000 + 2000 + 40 + 20 + 5 = 3065
  EXPECT_EQ(ScoreAlternate(client, stored), 3065);
}

TEST(ScoreAlternateTest, EncodingMismatchDisqualifies) {
  // Client wants gzip, stored is brotli — hard disqualify.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kGzip)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored), 0);
}

TEST(ScoreAlternateTest, EncodingMismatchBothDirections) {
  // Client wants brotli, stored is gzip — also disqualifies.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kGzip)
                    .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored), 0);
}

TEST(ScoreAlternateTest, IdentityClientDisqualifiesCompressed) {
  // Client wants identity, stored is gzip — disqualifies.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kGzip)
                    .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored), 0);
}

TEST(ScoreAlternateTest, SentinelReturnsZero) {
  // A sentinel mask has viewport=3 in the low byte.
  auto sentinel = static_cast<uint32_t>(SentinelId::kOriginalContent);
  uint32_t client = CapabilityMask().Encode();
  EXPECT_EQ(ScoreAlternate(client, sentinel), 0);
}

TEST(ScoreAlternateTest, OriginalFormatFallbackBonus) {
  // Client requests AVIF but only Original is stored.  The Original
  // alternate should receive +100 fallback bonus instead of +1000.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  // Original bonus + viewport + density + save-data + encoding:
  // 100 + 2000 + 40 + 20 + 60 = 2220
  EXPECT_EQ(ScoreAlternate(client, stored), 2220);
}

TEST(ScoreAlternateTest, ExactFormatMatchOverridesOriginalBonus) {
  // When the client also requests Original, the exact-match +1000
  // applies — not the +100 fallback bonus.
  auto mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                             CapabilityMask::Viewport::kDesktop,
                             CapabilityMask::PixelDensity::k1x,
                             CapabilityMask::SaveData::kOff,
                             CapabilityMask::TransferEncoding::kIdentity)
                  .Encode();
  // Exact match: 1000 + 2000 + 40 + 20 + 60 = 3120
  EXPECT_EQ(ScoreAlternate(mask, mask), 3120);
}

TEST(ScoreAlternateTest, NonOriginalFormatMismatchDisqualifies) {
  // Client requests AVIF, stored is WebP.  Neither is Original, so there is
  // no fallback — the mismatch hard-disqualifies (#1331).
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored), 0);
}

// --- SVG alternate scoring tests ---

TEST(ScoreAlternateTest, SvgUniversalFormatBonus) {
  // SVG alternate gets +1200 format bonus regardless of client format.
  auto client_webp = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // SVG: 1200 + 2000 (viewport) + 40 (density) + 20 (save-data match) + 60
  // (encoding) = 3320
  EXPECT_EQ(ScoreAlternate(client_webp, stored_svg), 3320);
}

TEST(ScoreAlternateTest, SvgFormatBonusWithAvifClient) {
  // Client requests AVIF, SVG alternate still gets +1200.
  auto client_avif = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                    CapabilityMask::Viewport::kMobile,
                                    CapabilityMask::PixelDensity::k2xPlus,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k2xPlus,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // 1200 + 2000 + 40 + 20 + 60 = 3320
  EXPECT_EQ(ScoreAlternate(client_avif, stored_svg), 3320);
}

TEST(ScoreAlternateTest, SvgViewportIndependent) {
  // SVG alternate gets viewport+density bonuses even when viewport
  // and density differ from client.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k2xPlus,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // SVG: 1200 + 2000 (always) + 40 (always) + 20 (save-data match) + 60 =
  // 3320
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 3320);
}

TEST(ScoreAlternateTest, SvgSaveDataBonus) {
  // SVG alternate gets +50 Save-Data bonus when client has save-data=on.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOn,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // SVG: 1200 + 2000 + 40 + 50 (save-data SVG bonus) + 60 = 3350
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 3350);
}

TEST(ScoreAlternateTest, SvgSaveDataOffNoBonus) {
  // When client has save-data=off and stored SVG also has save-data=off,
  // normal exact match +20 applies (not the +50 SVG bonus).
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // SVG: 1200 + 2000 + 40 + 20 (save-data match) + 60 = 3320
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 3320);
}

TEST(ScoreAlternateTest, SvgBeatsWebPAndAvif) {
  // SVG alternate beats both WebP and AVIF alternatives.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_avif = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto stored_webp = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();

  int score_avif = ScoreAlternate(client, stored_avif);
  int score_webp = ScoreAlternate(client, stored_webp);
  int score_svg = ScoreAlternate(client, stored_svg);

  // AVIF exact match: 1000 + 2000 + 40 + 20 + 60 = 3120
  EXPECT_EQ(score_avif, 3120);
  // WebP mismatch: hard-disqualified (#1331)
  EXPECT_EQ(score_webp, 0);
  // SVG: 1200 + 2000 + 40 + 20 + 60 = 3320
  EXPECT_EQ(score_svg, 3320);
  EXPECT_GT(score_svg, score_avif);
  EXPECT_GT(score_svg, score_webp);
}

TEST(ScoreAlternateTest, OriginalFallbackStillWorksWithSvg) {
  // Original content still provides +100 fallback when client requests
  // a non-SVG format and SVG is not stored.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_original =
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();
  // Original fallback: 100 + 2000 + 40 + 20 + 60 = 2220
  EXPECT_EQ(ScoreAlternate(client, stored_original), 2220);
}

TEST(ScoreAlternateTest, SvgEncodingExactMatch) {
  // SVG with brotli encoding, client also wants brotli.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kBrotli)
                        .Encode();
  // 1200 + 2000 + 40 + 20 + 60 = 3320
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 3320);
}

TEST(ScoreAlternateTest, SvgEncodingIdentityFallback) {
  // SVG with identity encoding, client wants brotli — identity fallback.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // 1200 + 2000 + 40 + 20 + 5 (identity fallback) = 3265
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 3265);
}

TEST(ScoreAlternateTest, SvgEncodingMismatchDisqualifies) {
  // SVG with brotli encoding, client wants gzip — disqualified.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kGzip)
                    .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kMobile,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kBrotli)
                        .Encode();
  // Encoding mismatch (non-identity) — hard disqualify.
  EXPECT_EQ(ScoreAlternate(client, stored_svg), 0);
}

TEST(ScoreAlternateTest, NonSvgAlternatesUnaffected) {
  // Verify non-SVG alternates score exactly as before.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                               CapabilityMask::Viewport::kMobile,
                               CapabilityMask::PixelDensity::k2xPlus,
                               CapabilityMask::SaveData::kOn,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  // Exact match: 1000 + 2000 + 40 + 20 + 60 = 3120
  EXPECT_EQ(ScoreAlternate(client, client), 3120);

  // Format mismatch (AVIF stored, WebP wanted): hard-disqualified (#1331).
  auto stored_avif = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                    CapabilityMask::Viewport::kMobile,
                                    CapabilityMask::PixelDensity::k2xPlus,
                                    CapabilityMask::SaveData::kOn,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored_avif), 0);

  // Original fallback: 100 + 2000 + 40 + 20 + 60 = 2220
  auto stored_original =
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kMobile,
                     CapabilityMask::PixelDensity::k2xPlus,
                     CapabilityMask::SaveData::kOn,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();
  EXPECT_EQ(ScoreAlternate(client, stored_original), 2220);
}

TEST(ScoreAlternateTest, SvgExactFormatMatchWithSvgClient) {
  // When client mask also has SVG format, the SVG universal bonus
  // still applies (it's based on stored format, not format match).
  auto client_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  auto stored_svg = CapabilityMask(CapabilityMask::ImageFormat::kSvg,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();
  // SVG: 1200 + 2000 + 40 + 20 + 60 = 3320
  EXPECT_EQ(ScoreAlternate(client_svg, stored_svg), 3320);
}

// --- Format hard-disqualify and tie determinism (issue #1331) ---

TEST(ScoreAlternateTest, OriginalClientDisqualifiesWebpAndAvif) {
  // The exact bytes from issue #1331: a client whose Accept named neither
  // image/avif, image/webp, image/* nor */* negotiates mask 0x08
  // (kOriginal/Desktop/1x/off/identity).  Stored WebP (0x09) and AVIF (0x0A)
  // variants of the same dimensions must score 0, not 200.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto stored_webp = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto stored_avif = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  ASSERT_EQ(client & 0xFF, 0x08u);
  ASSERT_EQ(stored_webp & 0xFF, 0x09u);
  ASSERT_EQ(stored_avif & 0xFF, 0x0Au);
  EXPECT_EQ(ScoreAlternate(client, stored_webp), 0);
  EXPECT_EQ(ScoreAlternate(client, stored_avif), 0);
}

TEST(PageSpeedSelectorTest, FormatIncompatibleFamilySelectsNothing) {
  // The issue #1331 pathology end to end at the selector seam: client mask
  // 0x08 against a family whose only content variants are WebP (0x09) and
  // AVIF (0x0A), plus the durable original (0x0C — never selectable, by
  // design).  Before the fix, both variants scored 200 and whichever came
  // first in the alternates listing won.  Now both are hard-disqualified,
  // the durable original stays skipped, and select() returns nothing —
  // the caller falls through exactly as in the no-alternates case.  Driven
  // with both listing orders to pin the order-independent aggravator.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto webp = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                             CapabilityMask::Viewport::kDesktop,
                             CapabilityMask::PixelDensity::k1x,
                             CapabilityMask::SaveData::kOff,
                             CapabilityMask::TransferEncoding::kIdentity)
                  .Encode();
  auto avif = CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                             CapabilityMask::Viewport::kDesktop,
                             CapabilityMask::PixelDensity::k1x,
                             CapabilityMask::SaveData::kOff,
                             CapabilityMask::TransferEncoding::kIdentity)
                  .Encode();

  cyclone::AlternateInfo original;
  original.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);

  for (bool avif_first : {false, true}) {
    std::vector<cyclone::AlternateInfo> alts;
    alts.push_back(original);
    alts.push_back(MakeAlternateInfo(avif_first ? avif : webp));
    alts.push_back(MakeAlternateInfo(avif_first ? webp : avif));
    auto ctx = MakeContext(client_mask);
    EXPECT_FALSE(selector.select(alts, ctx).has_value())
        << "avif_first=" << avif_first
        << " — a format-incompatible variant must never be served";
  }
}

TEST(PageSpeedSelectorTest, OriginalClientStillSelectsOriginalFormatVariant) {
  // Positive control for the family above: when a kOriginal-FORMAT content
  // variant exists alongside the disqualified WebP/AVIF variants, it still
  // wins (masks that advertise the stored format keep current behavior),
  // and the durable original still does not.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto original_variant =
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kMobile,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  cyclone::AlternateInfo original;
  original.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);
  alts.push_back(original);  // index 0: durable original — never selectable
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));                                  // index 1: disqualified
  alts.push_back(MakeAlternateInfo(original_variant));  // index 2: 1120

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 2u);
}

TEST(PageSpeedSelectorTest, WebpClientFallsBackToOriginalVariantNotAvif) {
  // A kWebP client against a family with only an AVIF variant and an
  // Original-format variant: AVIF is disqualified (the client's Accept may
  // have carried image/* or */*, which per RFC 9110 covers AVIF — but the
  // mask cannot prove it, and the safe direction is disqualify), so the
  // Original fallback wins rather than the undecodable-by-proof AVIF.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));  // index 0: disqualified
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));  // index 1: 100 + 2000 + 40 + 20 + 60 = 2220

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(ScoreAlternateTest, DistinctMasksCanTieAtPositiveScores) {
  // Pins the premise the tie-break rule in select() exists for: per-dimension
  // bonuses are binary, so two variants that match the client on the same
  // dimensions but differ in a mismatching one score identically.  Client
  // 0x00 = Original/Mobile/1x/off/identity; the Tablet (0x04) and Desktop
  // (0x08) variants both miss only the viewport and tie at 1120.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kMobile,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  ASSERT_EQ(client & 0xFF, 0x00u);
  auto tablet = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kTablet,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto desktop = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                CapabilityMask::Viewport::kDesktop,
                                CapabilityMask::PixelDensity::k1x,
                                CapabilityMask::SaveData::kOff,
                                CapabilityMask::TransferEncoding::kIdentity)
                     .Encode();
  ASSERT_EQ(tablet & 0xFF, 0x04u);
  ASSERT_EQ(desktop & 0xFF, 0x08u);
  EXPECT_EQ(ScoreAlternate(client, tablet), 1120);
  EXPECT_EQ(ScoreAlternate(client, tablet), ScoreAlternate(client, desktop));
}

TEST(PageSpeedSelectorTest, TieBreakSelectsDocumentedWinnerRegardlessOfOrder) {
  // The documented rule: equal positive score -> higher format value, then
  // numerically larger mask byte.  For the 0x04/0x08 tie above (same
  // format), the larger byte 0x08 (Desktop) must win from BOTH listing
  // orders — before the fix the winner was whichever came first.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                    CapabilityMask::Viewport::kMobile,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  auto tablet = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kTablet,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kIdentity)
                    .Encode();
  auto desktop = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                CapabilityMask::Viewport::kDesktop,
                                CapabilityMask::PixelDensity::k1x,
                                CapabilityMask::SaveData::kOff,
                                CapabilityMask::TransferEncoding::kIdentity)
                     .Encode();

  for (bool desktop_first : {false, true}) {
    std::vector<cyclone::AlternateInfo> alts;
    alts.push_back(MakeAlternateInfo(desktop_first ? desktop : tablet));
    alts.push_back(MakeAlternateInfo(desktop_first ? tablet : desktop));
    auto ctx = MakeContext(client_mask);
    auto result = selector.select(alts, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(alts[*result].id, static_cast<cyclone::AlternateId>(0x08))
        << "desktop_first=" << desktop_first;
  }
}

TEST(PageSpeedSelectorTest, SelectionIsOrderIndependent) {
  // Determinism through the public API: for every client mask FromHeaders
  // can produce, selecting from the full 192-variant matrix in forward,
  // reversed, and rotated listing orders must yield the same stored mask
  // byte every time.
  PageSpeedSelector selector;

  std::vector<uint32_t> stored_masks;
  for (int fmt = 0; fmt < 4; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      for (int den = 0; den < 2; ++den) {
        for (int sd = 0; sd < 2; ++sd) {
          for (int enc = 0; enc < 4; ++enc) {
            stored_masks.push_back(
                CapabilityMask(
                    static_cast<CapabilityMask::ImageFormat>(fmt),
                    static_cast<CapabilityMask::Viewport>(vp),
                    static_cast<CapabilityMask::PixelDensity>(den),
                    static_cast<CapabilityMask::SaveData>(sd),
                    static_cast<CapabilityMask::TransferEncoding>(enc))
                    .Encode());
          }
        }
      }
    }
  }
  ASSERT_EQ(stored_masks.size(), 192u);

  for (int cfmt = 0; cfmt < 3; ++cfmt) {  // formats FromHeaders can produce
    for (int cvp = 0; cvp < 3; ++cvp) {
      for (int cden = 0; cden < 2; ++cden) {
        for (int csd = 0; csd < 2; ++csd) {
          for (int cenc = 0; cenc < 4; ++cenc) {
            auto client =
                CapabilityMask(
                    static_cast<CapabilityMask::ImageFormat>(cfmt),
                    static_cast<CapabilityMask::Viewport>(cvp),
                    static_cast<CapabilityMask::PixelDensity>(cden),
                    static_cast<CapabilityMask::SaveData>(csd),
                    static_cast<CapabilityMask::TransferEncoding>(cenc))
                    .Encode();
            int selected_byte = -2;  // -2: unset, -1: nullopt
            for (int order = 0; order < 3; ++order) {
              std::vector<cyclone::AlternateInfo> alts;
              alts.reserve(stored_masks.size());
              for (size_t k = 0; k < stored_masks.size(); ++k) {
                size_t idx = k;
                if (order == 1) {
                  idx = stored_masks.size() - 1 - k;  // reversed
                } else if (order == 2) {
                  idx = (k + 97) % stored_masks.size();  // rotated
                }
                alts.push_back(MakeAlternateInfo(stored_masks[idx]));
              }
              auto ctx = MakeContext(client);
              auto result = selector.select(alts, ctx);
              int byte =
                  result.has_value() ? static_cast<int>(alts[*result].id) : -1;
              if (order == 0) {
                selected_byte = byte;
              } else {
                EXPECT_EQ(byte, selected_byte)
                    << "order " << order << " selected 0x" << std::hex << byte
                    << " but order 0 selected 0x" << selected_byte
                    << " for client 0x" << client;
              }
            }
          }
        }
      }
    }
  }
}

TEST(PageSpeedSelectorTest, DuplicateMaskSelectionIsOrderIndependent) {
  // Two alternates carrying the SAME stored mask byte are interchangeable as
  // far as selection can tell, and the documented rule keeps the first of
  // identical bytes — assert both listing orders select an entry with that
  // same byte (the index may differ; the outcome must not).
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();
  // A weaker but compatible alternate both duplicates must beat.
  auto fallback = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                 CapabilityMask::Viewport::kMobile,
                                 CapabilityMask::PixelDensity::k1x,
                                 CapabilityMask::SaveData::kOff,
                                 CapabilityMask::TransferEncoding::kIdentity)
                      .Encode();

  for (bool reversed : {false, true}) {
    std::vector<cyclone::AlternateInfo> alts;
    alts.push_back(MakeAlternateInfo(fallback));
    alts.push_back(MakeAlternateInfo(client_mask));
    alts.push_back(MakeAlternateInfo(client_mask));  // duplicate, same byte
    if (reversed) {
      std::swap(alts[1], alts[2]);
    }
    auto ctx = MakeContext(client_mask);
    auto result = selector.select(alts, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(alts[*result].id,
              static_cast<cyclone::AlternateId>(client_mask & 0xFF))
        << "reversed=" << reversed;
  }
}

// --- Encoding selection scenario tests ---

TEST(ScoreAlternateTest, ViewportCorrectnessOverEncodingPreference) {
  // Desktop/brotli client, cache has Mobile/brotli + Desktop/identity.
  // Desktop/identity should win (viewport correctness > encoding).
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto mobile_br = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                  CapabilityMask::Viewport::kMobile,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kBrotli)
                       .Encode();
  auto desktop_id = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                   CapabilityMask::Viewport::kDesktop,
                                   CapabilityMask::PixelDensity::k1x,
                                   CapabilityMask::SaveData::kOff,
                                   CapabilityMask::TransferEncoding::kIdentity)
                        .Encode();

  int score_mobile_br = ScoreAlternate(client, mobile_br);
  int score_desktop_id = ScoreAlternate(client, desktop_id);

  // Desktop/identity: 1000 + 2000 + 40 + 20 + 5 = 3065
  // Mobile/brotli: 1000 + 0 + 40 + 20 + 60 = 1120
  EXPECT_EQ(score_desktop_id, 3065);
  EXPECT_EQ(score_mobile_br, 1120);
  EXPECT_GT(score_desktop_id, score_mobile_br);
}

TEST(ScoreAlternateTest, GzipClientBrotliOnlyGetsIdentity) {
  // Client wants gzip, only brotli and identity exist.
  // Brotli is disqualified. Identity should win.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kGzip)
                    .Encode();
  auto stored_br = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kBrotli)
                       .Encode();
  auto stored_id = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kIdentity)
                       .Encode();

  EXPECT_EQ(ScoreAlternate(client, stored_br), 0);
  EXPECT_GT(ScoreAlternate(client, stored_id), 0);
}

TEST(ScoreAlternateTest, BrotliClientBrotliExistsWinsOverIdentity) {
  // Client wants brotli, brotli exists — should win over identity.
  auto client = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                               CapabilityMask::Viewport::kDesktop,
                               CapabilityMask::PixelDensity::k1x,
                               CapabilityMask::SaveData::kOff,
                               CapabilityMask::TransferEncoding::kBrotli)
                    .Encode();
  auto stored_br = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kBrotli)
                       .Encode();
  auto stored_id = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                  CapabilityMask::Viewport::kDesktop,
                                  CapabilityMask::PixelDensity::k1x,
                                  CapabilityMask::SaveData::kOff,
                                  CapabilityMask::TransferEncoding::kIdentity)
                       .Encode();

  int score_br = ScoreAlternate(client, stored_br);
  int score_id = ScoreAlternate(client, stored_id);
  // Brotli exact match (60) > identity fallback (5)
  EXPECT_GT(score_br, score_id);
}

// --- PageSpeedSelector tests ---

TEST(PageSpeedSelectorTest, SelectsExactMatch) {
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kMobile,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Add some non-matching alternates first
  alts.push_back(
      MakeAlternateInfo(CapabilityMask(CapabilityMask::ImageFormat::kAvif,
                                       CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::PixelDensity::k2xPlus,
                                       CapabilityMask::SaveData::kOn,
                                       CapabilityMask::TransferEncoding::kGzip)
                            .Encode()));
  // Exact match at index 1
  alts.push_back(MakeAlternateInfo(client_mask));
  // Another non-match
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kTablet,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kBrotli)
          .Encode()));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, PrefersFormatOverViewport) {
  PageSpeedSelector selector;

  // Client wants WebP + Mobile
  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kMobile,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Alt 0: wrong format, right viewport
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(
          CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
          CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
          CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));
  // Alt 1: right format, wrong viewport
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  // AVIF is disqualified for a WebP client, so the WebP at another viewport
  // is the only candidate.
  EXPECT_EQ(*result, 1u);
}

// A phone whose Accept names WebP but not AVIF (mask 0x11: WebP, Mobile,
// 2x), against the variant set a stock worker leaves behind: the desktop
// pass wrote JPEG/WebP/AVIF at full size, the phone pass wrote JPEG and AVIF
// at phone size and refused the phone WebP (not smaller than the phone
// JPEG).  The phone must get the phone-size JPEG, not the full-size desktop
// WebP that is many times larger.
TEST(PageSpeedSelectorTest, PhoneWebpClientGetsPhoneSizeJpegNotDesktopWebp) {
  PageSpeedSelector selector;
  const uint32_t client = 0x11;
  std::vector<cyclone::AlternateInfo> alts;
  for (uint32_t id : {0x08u, 0x09u, 0x0Au, 0x10u, 0x12u}) {
    alts.push_back(MakeAlternateInfo(id));
  }
  auto result = selector.select(alts, MakeContext(client));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(alts[*result].id, static_cast<cyclone::AlternateId>(0x10));
}

// The mirror image: a desktop WebP client must not be handed a phone-size
// WebP when a desktop-size image exists.
TEST(PageSpeedSelectorTest, DesktopClientNeverGetsPhoneSizeVariant) {
  PageSpeedSelector selector;
  const uint32_t client = 0x09;  // WebP, Desktop, 1x
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAlternateInfo(0x11));  // WebP, Mobile, 2x
  alts.push_back(MakeAlternateInfo(0x08));  // Original, Desktop, 1x
  auto result = selector.select(alts, MakeContext(client));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(alts[*result].id, static_cast<cyclone::AlternateId>(0x08));
}

// Invariant sweep: for every WebP/AVIF client mask, when the original-format
// variant at the client's viewport exists but the client's own format at
// that viewport does not (refused or not built yet), the selection stays at
// the client's viewport whatever else the family holds.
TEST(PageSpeedSelectorTest, SizeClassMatchAlwaysBeatsFormatAtOtherSize) {
  PageSpeedSelector selector;
  // WebP and AVIF clients: an Original client's own format IS the fallback.
  for (int fmt = 1; fmt < 3; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      for (int den = 0; den < 2; ++den) {
        for (int sd = 0; sd < 2; ++sd) {
          const auto client =
              CapabilityMask(static_cast<CapabilityMask::ImageFormat>(fmt),
                             static_cast<CapabilityMask::Viewport>(vp),
                             static_cast<CapabilityMask::PixelDensity>(den),
                             static_cast<CapabilityMask::SaveData>(sd),
                             CapabilityMask::TransferEncoding::kIdentity)
                  .Encode();
          std::vector<cyclone::AlternateInfo> alts;
          for (int sf = 0; sf < 3; ++sf) {
            for (int sv = 0; sv < 3; ++sv) {
              for (int sden = 0; sden < 2; ++sden) {
                for (int ssd = 0; ssd < 2; ++ssd) {
                  if (sf == fmt && sv == vp) continue;
                  // Keep only the original-format variant at the client's
                  // viewport with the "wrong" density and save-data, the
                  // weakest same-size candidate there is.
                  if (sv == vp && (sden == den || ssd == sd)) continue;
                  alts.push_back(MakeAlternateInfo(
                      CapabilityMask(
                          static_cast<CapabilityMask::ImageFormat>(sf),
                          static_cast<CapabilityMask::Viewport>(sv),
                          static_cast<CapabilityMask::PixelDensity>(sden),
                          static_cast<CapabilityMask::SaveData>(ssd),
                          CapabilityMask::TransferEncoding::kIdentity)
                          .Encode()));
                }
              }
            }
          }
          auto result = selector.select(alts, MakeContext(client));
          ASSERT_TRUE(result.has_value());
          const auto chosen =
              CapabilityMask::Decode(static_cast<uint8_t>(alts[*result].id));
          EXPECT_EQ(static_cast<int>(chosen.viewport()), vp)
              << "client 0x" << std::hex << client << " got 0x"
              << static_cast<int>(alts[*result].id);
        }
      }
    }
  }
}

TEST(PageSpeedSelectorTest, SkipsSentinels) {
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask().Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Sentinel alternate
  cyclone::AlternateInfo sentinel;
  sentinel.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);
  alts.push_back(sentinel);

  // Real alternate at index 1
  alts.push_back(MakeAlternateInfo(client_mask));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, SelectsBetterMatchOverLesserMatch) {
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask().Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Alternate with a different format (worse match).
  cyclone::AlternateInfo lesser;
  lesser.id =
      static_cast<cyclone::AlternateId>(MaskToAlternateId(static_cast<uint8_t>(
          CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                         CapabilityMask::Viewport::kDesktop,
                         CapabilityMask::PixelDensity::k1x,
                         CapabilityMask::SaveData::kOff,
                         CapabilityMask::TransferEncoding::kIdentity)
              .Encode())));
  alts.push_back(lesser);

  // Exact match at index 1.
  alts.push_back(MakeAlternateInfo(client_mask));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, EmptyAlternatesReturnsNullopt) {
  PageSpeedSelector selector;
  std::vector<cyclone::AlternateInfo> alts;
  auto ctx = MakeContext(CapabilityMask().Encode());
  EXPECT_FALSE(selector.select(alts, ctx).has_value());
}

TEST(PageSpeedSelectorTest, AllSentinelsReturnsNullopt) {
  PageSpeedSelector selector;

  std::vector<cyclone::AlternateInfo> alts;
  for (auto sid : {SentinelId::kOriginalContent, SentinelId::kEarlyHints,
                   SentinelId::kWarmupRequest}) {
    cyclone::AlternateInfo info;
    info.id = static_cast<cyclone::AlternateId>(sid);
    alts.push_back(info);
  }

  auto ctx = MakeContext(CapabilityMask().Encode());
  EXPECT_FALSE(selector.select(alts, ctx).has_value());
}

TEST(PageSpeedSelectorTest, EncodingMismatchDisqualifiesInSelection) {
  // Client wants gzip. Cache has brotli alternate and identity alternate.
  // Brotli should be disqualified; identity should be selected.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kGzip)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Index 0: brotli (should be disqualified)
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kBrotli)
          .Encode()));
  // Index 1: identity (should be selected as fallback)
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, AllDisqualifiedReturnsNullopt) {
  // Client wants identity only.  All stored alternates are non-identity
  // compressed — every one is disqualified (score 0).  Selector must
  // return nullopt, not the first disqualified alternate.
  PageSpeedSelector selector;

  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Only brotli and gzip alternates — no identity fallback.
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                     CapabilityMask::Viewport::kDesktop,
                     CapabilityMask::PixelDensity::k1x,
                     CapabilityMask::SaveData::kOff,
                     CapabilityMask::TransferEncoding::kBrotli)
          .Encode()));
  alts.push_back(
      MakeAlternateInfo(CapabilityMask(CapabilityMask::ImageFormat::kOriginal,
                                       CapabilityMask::Viewport::kDesktop,
                                       CapabilityMask::PixelDensity::k1x,
                                       CapabilityMask::SaveData::kOff,
                                       CapabilityMask::TransferEncoding::kGzip)
                            .Encode()));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  EXPECT_FALSE(result.has_value())
      << "All-disqualified alternates should return nullopt";
}

TEST(PageSpeedSelectorTest, Combinatorial144NonSvgMasks) {
  // For each of 144 non-SVG client masks, populate alternates with
  // all 144 non-SVG content masks and verify the selector always picks
  // the exact match.  SVG is excluded because its universal +1200
  // format bonus outscores exact matches for non-SVG formats.
  PageSpeedSelector selector;

  // Build all 144 non-SVG content masks (formats 0-2 only).
  std::vector<uint32_t> all_masks;
  for (int fmt = 0; fmt < 3; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      for (int den = 0; den < 2; ++den) {
        for (int sd = 0; sd < 2; ++sd) {
          for (int enc = 0; enc < 4; ++enc) {
            auto mask = CapabilityMask(
                            static_cast<CapabilityMask::ImageFormat>(fmt),
                            static_cast<CapabilityMask::Viewport>(vp),
                            static_cast<CapabilityMask::PixelDensity>(den),
                            static_cast<CapabilityMask::SaveData>(sd),
                            static_cast<CapabilityMask::TransferEncoding>(enc))
                            .Encode();
            all_masks.push_back(mask);
          }
        }
      }
    }
  }
  ASSERT_EQ(all_masks.size(), 144u);

  // Build alternates from all masks.
  std::vector<cyclone::AlternateInfo> alts;
  alts.reserve(all_masks.size());
  for (auto m : all_masks) {
    alts.push_back(MakeAlternateInfo(m));
  }

  // For each client mask, verify exact match is selected.
  for (size_t i = 0; i < all_masks.size(); ++i) {
    auto ctx = MakeContext(all_masks[i]);
    auto result = selector.select(alts, ctx);
    ASSERT_TRUE(result.has_value())
        << "No match for mask 0x" << std::hex << all_masks[i];
    // Exact match should always win (score 3120).
    EXPECT_EQ(*result, i) << "Wrong match for mask 0x" << std::hex
                          << all_masks[i] << " — got index " << *result
                          << " (mask 0x" << all_masks[*result] << ")";
  }
}

TEST(PageSpeedSelectorTest, CombinatorialSvgAlwaysWins) {
  // When SVG alternates are present alongside non-SVG alternates,
  // SVG should always be selected (highest score) as long as the
  // encoding is compatible.
  PageSpeedSelector selector;

  // Client wants WebP/Desktop/1x/off/identity.
  auto client_mask = CapabilityMask(CapabilityMask::ImageFormat::kWebP,
                                    CapabilityMask::Viewport::kDesktop,
                                    CapabilityMask::PixelDensity::k1x,
                                    CapabilityMask::SaveData::kOff,
                                    CapabilityMask::TransferEncoding::kIdentity)
                         .Encode();

  std::vector<cyclone::AlternateInfo> alts;
  // Index 0: exact WebP match.
  alts.push_back(MakeAlternateInfo(client_mask));
  // Index 1: SVG with matching encoding.
  alts.push_back(MakeAlternateInfo(
      CapabilityMask(
          CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
          CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
          CapabilityMask::TransferEncoding::kIdentity)
          .Encode()));

  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  // SVG (1200+2000+40+20+60=3320) beats WebP exact (3120).
  EXPECT_EQ(*result, 1u);
}

// --- agent_optimize kAgentMarkdown entitlement gating ---

// 5-byte context: 4-byte LE client mask + 1 agent-entitled flag byte.
cyclone::AlternateSelectionContext MakeAgentContext(uint32_t client_mask,
                                                    bool entitled) {
  cyclone::AlternateSelectionContext ctx;
  static thread_local std::vector<std::byte> buf;
  buf.assign(5, std::byte{0});
  std::memcpy(buf.data(), &client_mask, 4);
  buf[4] = std::byte{static_cast<unsigned char>(entitled ? 1 : 0)};
  ctx.request_metadata = buf;
  return ctx;
}

cyclone::AlternateInfo MakeAgentMarkdownAlt() {
  cyclone::AlternateInfo info;
  info.id = static_cast<cyclone::AlternateId>(SentinelId::kAgentMarkdown);
  return info;
}

TEST(PageSpeedSelectorTest, NonAgentRequestNeverSelectsAgentMarkdown) {
  PageSpeedSelector selector;
  auto client_mask = CapabilityMask().Encode();
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAgentMarkdownAlt());          // index 0: the 0x7C sentinel
  alts.push_back(MakeAlternateInfo(client_mask));  // index 1: a content variant
  // Legacy 4-byte context => not an agent request: the sentinel stays skipped.
  auto ctx = MakeContext(client_mask);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, EntitledAgentRequestSelectsAgentMarkdown) {
  PageSpeedSelector selector;
  auto client_mask = CapabilityMask().Encode();
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(
      MakeAlternateInfo(client_mask));     // index 0: PERFECT content match
  alts.push_back(MakeAgentMarkdownAlt());  // index 1: the 0x7C sentinel
  auto ctx = MakeAgentContext(client_mask, /*entitled=*/true);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);  // beats even a perfect content score
}

TEST(PageSpeedSelectorTest, UnentitledAgentRequestNeverSelectsAgentMarkdown) {
  PageSpeedSelector selector;
  auto client_mask = CapabilityMask().Encode();
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAgentMarkdownAlt());
  alts.push_back(MakeAlternateInfo(client_mask));
  // 5-byte context, flag=0 => not entitled.
  auto ctx = MakeAgentContext(client_mask, /*entitled=*/false);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(PageSpeedSelectorTest, EntitledAgentRequestFallsBackWhenNoMarkdown) {
  // No kAgentMarkdown present: the new branch never fires; normal scoring picks
  // the best content variant.
  PageSpeedSelector selector;
  auto client_mask = CapabilityMask().Encode();
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAlternateInfo(client_mask));
  auto ctx = MakeAgentContext(client_mask, /*entitled=*/true);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0u);
}

TEST(PageSpeedSelectorTest, OnlyAgentMarkdownSentinelPresent) {
  PageSpeedSelector selector;
  std::vector<cyclone::AlternateInfo> alts;
  alts.push_back(MakeAgentMarkdownAlt());
  // entitled => selectable.
  auto ctx = MakeAgentContext(CapabilityMask().Encode(), /*entitled=*/true);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0u);
  // non-agent => nothing selectable (a lone sentinel stays skipped).
  auto ctx2 = MakeContext(CapabilityMask().Encode());
  EXPECT_FALSE(selector.select(alts, ctx2).has_value());
}

// --- an entry class this build has never heard of is never selectable ---

// The read-side half of namespace skew.  A peer built against a newer registry
// writes an entry class into a shared volume under a sentinel id this build
// does not know.  Selection must treat it as furniture: never scored, never
// selected, never handed to a client — because this build cannot know what
// those bytes are, and serving them would be the one outcome skew must never
// produce.  Falling back to a variant it does understand (or to nothing) costs
// only cache warmth.
//
// Driven with and without an entitled agent request on purpose: the
// agent-markdown exception is the only sentinel that is ever selectable, and
// the guard against it generalising to an UNKNOWN id has to be asserted, not
// inferred from the fact that the branch names a specific constant today.
TEST(PageSpeedSelectorTest, UnregisteredSentinelIsNeverSelected) {
  // 0xFC is sentinel-shaped (viewport bits = 3) and belongs to no class in
  // this build's registry — exactly what a newer peer's class looks like here.
  constexpr uint8_t kUnknownSentinel = 0xFC;
  ASSERT_TRUE(IsSentinel(kUnknownSentinel));
  ASSERT_FALSE(IsRegisteredSentinel(kUnknownSentinel));

  cyclone::AlternateInfo unknown;
  unknown.id = static_cast<cyclone::AlternateId>(kUnknownSentinel);

  PageSpeedSelector selector;
  auto client_mask = CapabilityMask().Encode();

  // With a content variant present, selection lands on the variant.
  {
    std::vector<cyclone::AlternateInfo> alts;
    alts.push_back(unknown);                         // index 0
    alts.push_back(MakeAlternateInfo(client_mask));  // index 1
    auto ctx = MakeContext(client_mask);
    auto result = selector.select(alts, ctx);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 1u);

    // An entitled agent request must not reach it either: the markdown
    // exception is for one named id, not for "sentinels an agent may have".
    auto agent_ctx = MakeAgentContext(client_mask, /*entitled=*/true);
    auto agent_result = selector.select(alts, agent_ctx);
    ASSERT_TRUE(agent_result.has_value());
    EXPECT_EQ(*agent_result, 1u);
  }

  // Alone, it is not a fallback: a MISS is the correct answer, and a cold
  // cache is the correct cost.
  {
    std::vector<cyclone::AlternateInfo> alts;
    alts.push_back(unknown);
    EXPECT_FALSE(selector.select(alts, MakeContext(client_mask)).has_value());
    EXPECT_FALSE(
        selector.select(alts, MakeAgentContext(client_mask, /*entitled=*/true))
            .has_value());
  }

  // And it scores zero, so it cannot win by accident if the skip above were
  // ever restructured.
  EXPECT_EQ(ScoreAlternate(client_mask, kUnknownSentinel), 0);
}

// --- the durable original is never selectable ---

TEST(PageSpeedSelectorTest, DurableOriginalIsNeverSelectedByScoring) {
  // A realistic chain: the durable original beside the variants that were
  // derived from it.  Whatever the client asks for, selection must land on a
  // variant — the original is read by exact id or not at all.
  PageSpeedSelector selector;

  cyclone::AlternateInfo original;
  original.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);

  for (int fmt = 0; fmt < 4; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      for (int enc = 0; enc < 3; ++enc) {
        auto client_mask =
            CapabilityMask(static_cast<CapabilityMask::ImageFormat>(fmt),
                           static_cast<CapabilityMask::Viewport>(vp),
                           CapabilityMask::PixelDensity::k1x,
                           CapabilityMask::SaveData::kOff,
                           static_cast<CapabilityMask::TransferEncoding>(enc))
                .Encode();
        SCOPED_TRACE(client_mask);

        std::vector<cyclone::AlternateInfo> alts;
        alts.push_back(original);  // index 0 — must never win
        alts.push_back(MakeAlternateInfo(CapabilityMask().Encode()));
        alts.push_back(MakeAlternateInfo(client_mask));

        auto ctx = MakeContext(client_mask);
        auto result = selector.select(alts, ctx);
        ASSERT_TRUE(result.has_value());
        EXPECT_NE(*result, 0u);
      }
    }
  }
}

TEST(PageSpeedSelectorTest, LoneDurableOriginalSelectsNothing) {
  // The population the class exists for — a resource with no optimized
  // original stored — must come back as "nothing to serve" from selection,
  // not as the original.  Including for an entitled agent request: the
  // markdown exception is the ONLY sentinel exception, and it does not
  // generalize to other sentinels.
  PageSpeedSelector selector;
  std::vector<cyclone::AlternateInfo> alts;
  cyclone::AlternateInfo original;
  original.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);
  alts.push_back(original);

  auto ctx = MakeContext(CapabilityMask().Encode());
  EXPECT_FALSE(selector.select(alts, ctx).has_value());

  auto agent_ctx = MakeAgentContext(CapabilityMask().Encode(),
                                    /*entitled=*/true);
  EXPECT_FALSE(selector.select(alts, agent_ctx).has_value());
}

TEST(PageSpeedSelectorTest, EntitledAgentRequestStillSkipsTheDurableOriginal) {
  // Both sentinel-with-a-prefix classes on one key.  The entitled request
  // takes the markdown; nothing takes the original.
  PageSpeedSelector selector;
  std::vector<cyclone::AlternateInfo> alts;
  cyclone::AlternateInfo original;
  original.id = static_cast<cyclone::AlternateId>(SentinelId::kOriginalContent);
  alts.push_back(original);                // index 0
  alts.push_back(MakeAgentMarkdownAlt());  // index 1

  auto ctx = MakeAgentContext(CapabilityMask().Encode(), /*entitled=*/true);
  auto result = selector.select(alts, ctx);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1u);
}

TEST(ScoreAlternateTest, DurableOriginalScoresZero) {
  // The scoring half of the same guarantee: even asked directly, the class
  // scores nothing, for every client shape.
  auto stored = static_cast<uint32_t>(SentinelId::kOriginalContent);
  for (int fmt = 0; fmt < 4; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {
      auto client =
          CapabilityMask(static_cast<CapabilityMask::ImageFormat>(fmt),
                         static_cast<CapabilityMask::Viewport>(vp),
                         CapabilityMask::PixelDensity::k1x,
                         CapabilityMask::SaveData::kOff,
                         CapabilityMask::TransferEncoding::kIdentity)
              .Encode();
      EXPECT_EQ(ScoreAlternate(client, stored), 0);
    }
  }
}

// --- Save-Data mismatch ---

uint32_t SdMask(CapabilityMask::ImageFormat fmt, CapabilityMask::Viewport vp,
                CapabilityMask::SaveData sd) {
  return CapabilityMask(fmt, vp, CapabilityMask::PixelDensity::k1x, sd,
                        CapabilityMask::TransferEncoding::kIdentity)
      .Encode();
}

TEST(ScoreAlternateTest, SaveDataVariantPenalizedForNormalClient) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  uint32_t client = SdMask(F::kWebP, V::kDesktop, S::kOff);
  uint32_t sd_webp = SdMask(F::kWebP, V::kDesktop, S::kOn);
  // 1000 + 2000 + 40 + 0 + 60 - 980
  EXPECT_EQ(ScoreAlternate(client, sd_webp), 2120);
  // The same slot for a Save-Data client keeps its full score.
  EXPECT_EQ(ScoreAlternate(sd_webp, sd_webp), 3120);
}

TEST(ScoreAlternateTest, SaveDataPenaltyNeverScoresZero) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  // Original-format Save-Data variant at another viewport: the raw sum is
  // negative, but the variant stays selectable as a last resort.
  uint32_t client = SdMask(F::kOriginal, V::kDesktop, S::kOff);
  uint32_t stored = SdMask(F::kOriginal, V::kMobile, S::kOn);
  EXPECT_GT(ScoreAlternate(client, stored), 0);
}

TEST(ScoreAlternateTest, SaveDataPenaltyKeepsOrderingAtTheBandEdges) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  using E = CapabilityMask::TransferEncoding;
  auto m = [](F f, V v, CapabilityMask::PixelDensity d, S sd, E e) {
    return CapabilityMask(f, v, d, sd, e).Encode();
  };
  constexpr auto k1 = CapabilityMask::PixelDensity::k1x;
  constexpr auto k2 = CapabilityMask::PixelDensity::k2xPlus;
  // Client: WebP, desktop, 1x, no Save-Data, gzip.
  uint32_t client = m(F::kWebP, V::kDesktop, k1, S::kOff, E::kGzip);
  // Penalized exact-format variant at its best (density and encoding match).
  uint32_t sd_best = m(F::kWebP, V::kDesktop, k1, S::kOn, E::kGzip);
  // Normal original-format variant at the client's viewport at its worst
  // (density and encoding fall back).
  uint32_t normal_orig_worst =
      m(F::kOriginal, V::kDesktop, k2, S::kOff, E::kIdentity);
  EXPECT_EQ(ScoreAlternate(client, sd_best), 2120);
  EXPECT_EQ(ScoreAlternate(client, normal_orig_worst), 2125);
  EXPECT_GT(ScoreAlternate(client, normal_orig_worst),
            ScoreAlternate(client, sd_best));
  // Penalized original-format variant at the client's viewport at its worst
  // against the best normal exact-format variant at another viewport.
  uint32_t sd_orig_worst =
      m(F::kOriginal, V::kDesktop, k2, S::kOn, E::kIdentity);
  uint32_t normal_other_vp = m(F::kWebP, V::kMobile, k1, S::kOff, E::kGzip);
  EXPECT_EQ(ScoreAlternate(client, sd_orig_worst), 1125);
  EXPECT_EQ(ScoreAlternate(client, normal_other_vp), 1120);
  EXPECT_GT(ScoreAlternate(client, sd_orig_worst),
            ScoreAlternate(client, normal_other_vp));
}

TEST(ScoreAlternateTest, SaveDataPenaltyDoesNotApplyToSvg) {
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  auto mk = [](CapabilityMask::ImageFormat f, S sd) {
    return CapabilityMask(f, V::kDesktop, CapabilityMask::PixelDensity::k1x, sd,
                          CapabilityMask::TransferEncoding::kIdentity)
        .Encode();
  };
  uint32_t client = mk(CapabilityMask::ImageFormat::kWebP, S::kOff);
  // 1200 + 2000 + 40 + 0 (no Save-Data match) + 60: no penalty.
  EXPECT_EQ(
      ScoreAlternate(client, mk(CapabilityMask::ImageFormat::kSvg, S::kOn)),
      3300);
}

TEST(PageSpeedSelectorTest, NormalClientPrefersNormalJpegOverSaveDataWebp) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  PageSpeedSelector sel;
  uint32_t client = SdMask(F::kWebP, V::kDesktop, S::kOff);
  // The normal WebP slot is empty: the Save-Data WebP sibling and the normal
  // JPEG are what the cache holds at the client's viewport.
  std::vector<cyclone::AlternateInfo> alts = {
      MakeAlternateInfo(SdMask(F::kWebP, V::kDesktop, S::kOn)),
      MakeAlternateInfo(SdMask(F::kOriginal, V::kDesktop, S::kOff))};
  auto ctx = MakeContext(client);
  auto pick = sel.select(alts, ctx);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(*pick, 1u);
}

TEST(PageSpeedSelectorTest, SaveDataClientKeepsItsSibling) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  PageSpeedSelector sel;
  uint32_t client = SdMask(F::kWebP, V::kDesktop, S::kOn);
  std::vector<cyclone::AlternateInfo> alts = {
      MakeAlternateInfo(SdMask(F::kWebP, V::kDesktop, S::kOff)),
      MakeAlternateInfo(SdMask(F::kWebP, V::kDesktop, S::kOn)),
      MakeAlternateInfo(SdMask(F::kOriginal, V::kDesktop, S::kOff))};
  auto ctx = MakeContext(client);
  auto pick = sel.select(alts, ctx);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(*pick, 1u);
  // With its own slot absent it takes the normal bytes of the same format.
  alts.erase(alts.begin() + 1);
  pick = sel.select(alts, ctx);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(*pick, 0u);
}

TEST(PageSpeedSelectorTest, SaveDataVariantStillBeatsAnotherViewport) {
  using F = CapabilityMask::ImageFormat;
  using V = CapabilityMask::Viewport;
  using S = CapabilityMask::SaveData;
  PageSpeedSelector sel;
  uint32_t client = SdMask(F::kWebP, V::kDesktop, S::kOff);
  std::vector<cyclone::AlternateInfo> alts = {
      MakeAlternateInfo(SdMask(F::kWebP, V::kMobile, S::kOff)),
      MakeAlternateInfo(SdMask(F::kWebP, V::kDesktop, S::kOn))};
  auto ctx = MakeContext(client);
  auto pick = sel.select(alts, ctx);
  ASSERT_TRUE(pick.has_value());
  EXPECT_EQ(*pick, 1u);
}

}  // namespace
}  // namespace pagespeed
