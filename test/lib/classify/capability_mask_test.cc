// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/capability_mask.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "lib/classify/alternate_id.h"

namespace pagespeed {
namespace {

TEST(CapabilityMaskTest, DefaultConstruction) {
  CapabilityMask mask;
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kDesktop);
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOff);
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);
  // Default mask is now 0x08 (Desktop/Identity)
  EXPECT_EQ(mask.Encode(), 0x08u);
}

TEST(CapabilityMaskTest, ExplicitConstruction) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kBrotli);

  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOn);
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);
}

TEST(CapabilityMaskTest, EncodeDecodeRoundtrip) {
  CapabilityMask original(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kTablet,
      CapabilityMask::PixelDensity::k2xPlus, CapabilityMask::SaveData::kOn,
      CapabilityMask::TransferEncoding::kGzip);

  uint32_t encoded = original.Encode();
  CapabilityMask decoded = CapabilityMask::Decode(encoded);

  EXPECT_EQ(decoded, original);
}

TEST(CapabilityMaskTest, EncodeBitLayout) {
  // Test that bits are laid out as documented
  CapabilityMask mask;

  // ImageFormat in bits 0-1
  mask.set_image_format(CapabilityMask::ImageFormat::kWebP);  // 01
  EXPECT_EQ(mask.Encode() & 0x03, 1u);

  mask.set_image_format(CapabilityMask::ImageFormat::kAvif);  // 10
  EXPECT_EQ(mask.Encode() & 0x03, 2u);

  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);  // 11
  EXPECT_EQ(mask.Encode() & 0x03, 3u);

  // Reset and test Viewport in bits 2-3
  mask = CapabilityMask();
  mask.set_viewport(CapabilityMask::Viewport::kTablet);  // 01
  EXPECT_EQ((mask.Encode() >> 2) & 0x03, 1u);

  mask.set_viewport(CapabilityMask::Viewport::kDesktop);  // 10
  EXPECT_EQ((mask.Encode() >> 2) & 0x03, 2u);

  // PixelDensity in bit 4
  mask = CapabilityMask();
  mask.set_pixel_density(CapabilityMask::PixelDensity::k2xPlus);
  EXPECT_EQ((mask.Encode() >> 4) & 0x01, 1u);

  // SaveData in bit 5
  mask = CapabilityMask();
  mask.set_save_data(CapabilityMask::SaveData::kOn);
  EXPECT_EQ((mask.Encode() >> 5) & 0x01, 1u);

  // TransferEncoding in bits 6-7
  mask = CapabilityMask();
  mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kBrotli);  // 10
  EXPECT_EQ((mask.Encode() >> 6) & 0x03, 2u);

  mask.set_transfer_encoding(CapabilityMask::TransferEncoding::kGzip);  // 01
  EXPECT_EQ((mask.Encode() >> 6) & 0x03, 1u);
}

TEST(CapabilityMaskTest, FromHeadersWebP) {
  auto mask =
      CapabilityMask::FromHeaders("image/webp,image/png,image/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

TEST(CapabilityMaskTest, FromHeadersAvif) {
  auto mask =
      CapabilityMask::FromHeaders("image/avif,image/webp,image/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);
}

TEST(CapabilityMaskTest, FromHeadersJxlMapsToOriginal) {
  // Firefox sends image/jxl in Accept, but JXL support was dropped.
  // image/jxl must NOT map to kSvg — it maps to kOriginal (or falls
  // through to AVIF/WebP if those are also present).
  auto mask = CapabilityMask::FromHeaders(
      "image/jxl,image/avif,image/webp,image/*", "", "", "");
  // AVIF is highest priority among supported formats.
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);

  // image/jxl with image/* => kWebP (image/* wildcard maps to WebP,
  // checked before the JXL fallback-to-Original branch).
  mask = CapabilityMask::FromHeaders("image/jxl,image/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);

  // image/jxl without image/* wildcard => kOriginal
  mask = CapabilityMask::FromHeaders("image/jxl", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);
}

TEST(CapabilityMaskTest, SvgNeverFromHeaders) {
  // kSvg is never produced by FromHeaders() regardless of Accept header.
  // It is only set programmatically by the worker.
  auto mask = CapabilityMask::FromHeaders("image/svg+xml", "", "", "");
  EXPECT_NE(mask.image_format(), CapabilityMask::ImageFormat::kSvg);
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);

  mask = CapabilityMask::FromHeaders("image/svg+xml,image/avif,image/webp", "",
                                     "", "");
  EXPECT_NE(mask.image_format(), CapabilityMask::ImageFormat::kSvg);
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);
}

TEST(CapabilityMaskTest, SvgEncodeDecodeRoundtrip) {
  CapabilityMask mask(
      CapabilityMask::ImageFormat::kSvg, CapabilityMask::Viewport::kDesktop,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);
  uint32_t encoded = mask.Encode();
  CapabilityMask decoded = CapabilityMask::Decode(encoded);
  EXPECT_EQ(decoded.image_format(), CapabilityMask::ImageFormat::kSvg);
  EXPECT_EQ(decoded, mask);
}

TEST(CapabilityMaskTest, SvgFormatBitsAreThree) {
  // kSvg occupies format bits = 3 (binary 11).
  CapabilityMask mask;
  mask.set_image_format(CapabilityMask::ImageFormat::kSvg);
  EXPECT_EQ(static_cast<uint8_t>(CapabilityMask::ImageFormat::kSvg), 3);
  EXPECT_EQ(mask.Encode() & 0x03, 3u);
}

TEST(CapabilityMaskTest, FromHeadersMobileUserAgent) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPhone; CPU iPhone OS 14_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Mobile/15E148",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
}

TEST(CapabilityMaskTest, FromHeadersTabletUserAgent) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPad; CPU OS 14_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/14.0",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kTablet);
}

TEST(CapabilityMaskTest, FromHeadersDesktopUserAgent) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
      "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kDesktop);
}

TEST(CapabilityMaskTest, FromHeadersSaveData) {
  auto mask = CapabilityMask::FromHeaders("", "", "on", "");
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOn);

  mask = CapabilityMask::FromHeaders("", "", "On", "");
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOn);

  mask = CapabilityMask::FromHeaders("", "", "1", "");
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOn);

  mask = CapabilityMask::FromHeaders("", "", "", "");
  EXPECT_EQ(mask.save_data(), CapabilityMask::SaveData::kOff);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncoding) {
  // Brotli preferred
  auto mask = CapabilityMask::FromHeaders("", "", "", "gzip, br");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Gzip only
  mask = CapabilityMask::FromHeaders("", "", "", "gzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);

  // Brotli only
  mask = CapabilityMask::FromHeaders("", "", "", "br");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Empty = identity
  mask = CapabilityMask::FromHeaders("", "", "", "");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);

  // Wildcard = brotli (best available)
  mask = CapabilityMask::FromHeaders("", "", "", "*");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncodingQualityZero) {
  // Brotli disabled via q=0
  auto mask = CapabilityMask::FromHeaders("", "", "", "br;q=0, gzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);

  // Both disabled
  mask = CapabilityMask::FromHeaders("", "", "", "br;q=0, gzip;q=0");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);

  // q=0.0 also means disabled
  mask = CapabilityMask::FromHeaders("", "", "", "br;q=0.0, gzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);

  // q=0.5 should NOT disable
  mask = CapabilityMask::FromHeaders("", "", "", "br;q=0.5, gzip");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Wildcard disabled via q=0 — falls back to identity
  mask = CapabilityMask::FromHeaders("", "", "", "*;q=0");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);

  // Wildcard disabled but gzip explicit
  mask = CapabilityMask::FromHeaders("", "", "", "*;q=0, gzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncodingMultiValue) {
  // Chrome-style header
  auto mask =
      CapabilityMask::FromHeaders("", "", "", "gzip, deflate, br, zstd");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Identity only
  mask = CapabilityMask::FromHeaders("", "", "", "identity");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);

  // Unknown encodings ignored
  mask = CapabilityMask::FromHeaders("", "", "", "deflate, zstd");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kIdentity);
}

TEST(CapabilityMaskTest, ViewportWidthRange) {
  auto mobile =
      CapabilityMask::ViewportWidthRange(CapabilityMask::Viewport::kMobile);
  EXPECT_EQ(mobile.min_px, 0u);
  EXPECT_EQ(mobile.max_px, 479u);

  auto tablet =
      CapabilityMask::ViewportWidthRange(CapabilityMask::Viewport::kTablet);
  EXPECT_EQ(tablet.min_px, 480u);
  EXPECT_EQ(tablet.max_px, 1279u);

  // iPad Pro 12.9" has a 1024px CSS viewport in portrait — must be tablet
  EXPECT_GE(1024u, tablet.min_px);
  EXPECT_LE(1024u, tablet.max_px);

  auto desktop =
      CapabilityMask::ViewportWidthRange(CapabilityMask::Viewport::kDesktop);
  EXPECT_EQ(desktop.min_px, 1280u);
  EXPECT_EQ(desktop.max_px, 65535u);

  // Ranges should be contiguous (no gaps)
  EXPECT_EQ(mobile.max_px + 1, tablet.min_px);
  EXPECT_EQ(tablet.max_px + 1, desktop.min_px);
}

TEST(CapabilityMaskTest, Equality) {
  CapabilityMask a(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  CapabilityMask b(
      CapabilityMask::ImageFormat::kWebP, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  CapabilityMask c(
      CapabilityMask::ImageFormat::kAvif, CapabilityMask::Viewport::kMobile,
      CapabilityMask::PixelDensity::k1x, CapabilityMask::SaveData::kOff,
      CapabilityMask::TransferEncoding::kIdentity);

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(CapabilityMaskTest, EncodeDecodeAllEncodings) {
  for (int enc = 0; enc < 4; ++enc) {
    auto te = static_cast<CapabilityMask::TransferEncoding>(enc);
    CapabilityMask mask(CapabilityMask::ImageFormat::kOriginal,
                        CapabilityMask::Viewport::kDesktop,
                        CapabilityMask::PixelDensity::k1x,
                        CapabilityMask::SaveData::kOff, te);
    auto decoded = CapabilityMask::Decode(mask.Encode());
    EXPECT_EQ(decoded.transfer_encoding(), te);
  }
}

TEST(CapabilityMaskTest, FromHeadersAndroidTabletUserAgent) {
  // Android device without "Mobile" in UA → tablet
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Linux; Android 14; SM-X810) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kTablet);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncodingWhitespaceTrimming) {
  // Trailing whitespace around encoding token names
  auto mask = CapabilityMask::FromHeaders("", "", "", "gzip , br ");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Only gzip with trailing space
  mask = CapabilityMask::FromHeaders("", "", "", "gzip ");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);
}

TEST(CapabilityMaskTest, FromHeadersHighDpi2x) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPhone; CPU iPhone OS 14_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) 2x Mobile/15E148",
      "", "");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

TEST(CapabilityMaskTest, FromHeadersHighDpiRetina) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) "
      "AppleWebKit/537.36 Retina",
      "", "");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

// =========================================================================
// Pixel density: Sec-CH-DPR hint + mobile 2x default (issue #657)
// =========================================================================

namespace {
// Modern iPhone Safari UA — no "2x"/"Retina" marker, no client hints.
constexpr std::string_view kModernIphoneUa =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 "
    "Mobile/15E148 Safari/604.1";
constexpr std::string_view kDesktopUa =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0";
}  // namespace

TEST(CapabilityMaskTest, MobileUaDefaultsToHighDensity) {
  // Modern phones are uniformly >= 2x but carry no density marker in
  // the UA — mobile must default to 2x when no hint is present.
  auto mask = CapabilityMask::FromHeaders("", kModernIphoneUa, "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

TEST(CapabilityMaskTest, SecChDprLowClearsDensityOnMobile) {
  // An explicit hint wins over the mobile default.
  auto mask = CapabilityMask::FromHeaders("", kModernIphoneUa, "", "", "1");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
}

TEST(CapabilityMaskTest, SecChDprHighSetsDensityOnDesktop) {
  auto mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "", "3");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kDesktop);
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

TEST(CapabilityMaskTest, SecChDprFractionalThreshold) {
  // 1.5 is the high-density threshold.
  auto mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "", "1.5");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
  mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "", "1.4");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
  mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "", "2.0");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

TEST(CapabilityMaskTest, SecChDprOverridesRetinaUa) {
  // Explicit low-DPR hint beats the legacy "Retina" UA marker.
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15) "
      "AppleWebKit/537.36 Retina",
      "", "", "1");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
}

TEST(CapabilityMaskTest, SecChDprUnparseableFallsBackToUa) {
  // Garbage hints fall back to UA heuristics: desktop stays 1x,
  // mobile defaults to 2x.
  auto mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "", "garbage");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
  mask = CapabilityMask::FromHeaders("", kModernIphoneUa, "", "", "1.x");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k2xPlus);
}

TEST(CapabilityMaskTest, TabletAndDesktopStayLowDensityByDefault) {
  // The mobile 2x default must not leak into tablet/desktop classes.
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
      "Mobile/15E148 Safari/604.1",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kTablet);
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
  mask = CapabilityMask::FromHeaders("", kDesktopUa, "", "");
  EXPECT_EQ(mask.pixel_density(), CapabilityMask::PixelDensity::k1x);
}

TEST(CapabilityMaskTest, ViewportWidthRangeUnknownEnum) {
  // Defensive fallback for unknown viewport value
  auto range = CapabilityMask::ViewportWidthRange(
      static_cast<CapabilityMask::Viewport>(99));
  EXPECT_EQ(range.min_px, 1280u);
  EXPECT_EQ(range.max_px, 65535u);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncodingMalformedQuality) {
  // Malformed q= with no number: "q=" followed by empty/non-numeric.
  // The q_val would be empty or non-'0', so it should NOT disable the encoding.
  auto mask = CapabilityMask::FromHeaders("", "", "", "br;q=, gzip");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // q= followed by garbage characters
  mask = CapabilityMask::FromHeaders("", "", "", "br;q=abc, gzip");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // q= at end of string with nothing after
  mask = CapabilityMask::FromHeaders("", "", "", "gzip;q=");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);

  // q= followed by space then comma (empty quality value)
  mask = CapabilityMask::FromHeaders("", "", "", "br;q= , gzip");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);
}

TEST(CapabilityMaskTest, AcceptMimeTypeBoundary) {
  // "image/webpng" should NOT match as "image/webp".
  auto mask = CapabilityMask::FromHeaders("image/webpng", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);

  // "image/webp" with quality value should still match.
  mask = CapabilityMask::FromHeaders("image/webp;q=0.8", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);

  // "image/avif" followed by comma should match.
  mask = CapabilityMask::FromHeaders("image/avif, image/webp", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);

  // Prefix overlap: "ximage/webp" should NOT match.
  mask = CapabilityMask::FromHeaders("ximage/webp", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);

  // Real-world Chrome Accept header.
  mask = CapabilityMask::FromHeaders(
      "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8", "",
      "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);
}

TEST(CapabilityMaskTest, FromHeadersAcceptEncodingTabAsOWS) {
  // RFC 9110: OWS includes both space and horizontal tab.
  auto mask = CapabilityMask::FromHeaders("", "", "", "gzip,\tbr");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Tab before first token
  mask = CapabilityMask::FromHeaders("", "", "", "\tgzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);

  // Tab after token (trailing)
  mask = CapabilityMask::FromHeaders("", "", "", "br\t");
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);

  // Tab around q-value
  mask = CapabilityMask::FromHeaders("", "", "", "br;q=\t0, gzip");
  EXPECT_EQ(mask.transfer_encoding(), CapabilityMask::TransferEncoding::kGzip);
}

// =========================================================================
// image/* wildcard → kWebP tests
// =========================================================================

TEST(CapabilityMaskTest, FromHeadersSafariAcceptImageWildcard) {
  // Safari (macOS/iOS 14+) sends this Accept for images.  It supports WebP
  // but does not list "image/webp" explicitly.  The image/* wildcard must
  // trigger kWebP per RFC 9110 §12.5.1.
  auto mask = CapabilityMask::FromHeaders(
      "image/png,image/svg+xml,image/*;q=0.8", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

TEST(CapabilityMaskTest, FromHeadersImageWildcardOnly) {
  auto mask = CapabilityMask::FromHeaders("image/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

TEST(CapabilityMaskTest, FromHeadersGenericWildcardMapsToWebP) {
  // Per RFC 9110, */* means any media type — subsumes image/*.
  // Chrome on iOS (CriOS/WKWebView) sends Accept: */* for image
  // subresource requests.  Format bits only affect image serving.
  auto mask = CapabilityMask::FromHeaders("text/html,*/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);

  mask = CapabilityMask::FromHeaders("*/*", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

TEST(CapabilityMaskTest,
     FromHeadersGenericWildcardLowerPriorityThanImageWildcard) {
  // image/* should match before */* (both → WebP, but tests priority chain).
  auto mask = CapabilityMask::FromHeaders("image/*,*/*;q=0.5", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

TEST(CapabilityMaskTest, FromHeadersCriOSAcceptHeader) {
  // Real-world Chrome on iOS sends just */* for image subresource loads.
  std::string_view crios_ua =
      "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/123.0.6312.52 "
      "Mobile/15E148 Safari/604.1";
  auto mask =
      CapabilityMask::FromHeaders("*/*", crios_ua, "", "gzip, deflate, br");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
  EXPECT_EQ(mask.transfer_encoding(),
            CapabilityMask::TransferEncoding::kBrotli);
}

TEST(CapabilityMaskTest, FromHeadersNoWildcardRasterOnly) {
  // Explicit raster types without wildcard → kOriginal.
  auto mask =
      CapabilityMask::FromHeaders("image/png,image/jpeg,image/gif", "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kOriginal);
}

TEST(CapabilityMaskTest, FromHeadersImageWildcardWithExplicitAvif) {
  auto mask = CapabilityMask::FromHeaders("image/avif,image/png,image/*;q=0.8",
                                          "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kAvif);
}

TEST(CapabilityMaskTest, FromHeadersImageWildcardWithExplicitWebP) {
  auto mask = CapabilityMask::FromHeaders("image/webp,image/png,image/*;q=0.8",
                                          "", "", "");
  EXPECT_EQ(mask.image_format(), CapabilityMask::ImageFormat::kWebP);
}

// =========================================================================
// iPad viewport classification fix
// =========================================================================

TEST(CapabilityMaskTest, FromHeadersIPadWithMobileInUA) {
  // Real-world iPad Safari UA includes "Mobile/15E148".  The iPad check
  // must run before the "Mobile" check → kTablet, not kMobile.
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
      "Mobile/15E148 Safari/604.1",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kTablet);
}

TEST(CapabilityMaskTest, FromHeadersIPadOSDesktopMode) {
  // iPadOS "Request Desktop Website" sends a macOS UA — no "iPad", no
  // "Mobile" → kDesktop (user opted for desktop mode).
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
      "Safari/605.1.15",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kDesktop);
}

TEST(CapabilityMaskTest, FromHeadersIPhoneNotBrokenByIPadFix) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
      "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
      "Mobile/15E148 Safari/604.1",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
}

TEST(CapabilityMaskTest, FromHeadersAndroidPhoneNotBrokenByIPadFix) {
  auto mask = CapabilityMask::FromHeaders(
      "",
      "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36",
      "", "");
  EXPECT_EQ(mask.viewport(), CapabilityMask::Viewport::kMobile);
}

// agent_optimize Accept negotiation.
TEST(CapabilityMaskTest, WantsAgentMarkdown) {
  EXPECT_TRUE(WantsAgentMarkdown("text/markdown"));
  EXPECT_TRUE(WantsAgentMarkdown("text/markdown;q=0.9"));
  EXPECT_TRUE(WantsAgentMarkdown("text/markdown, text/html;q=0.1"));
  // Equal q selects Markdown, q=0 selects HTML; the full matrix is
  // accept_markdown_preference_test.cc.
  EXPECT_TRUE(WantsAgentMarkdown("text/html, text/markdown"));
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown;q=0"));
  // Must NOT fire on the wildcard or on plain HTML/other.
  EXPECT_FALSE(WantsAgentMarkdown("*/*"));
  EXPECT_FALSE(WantsAgentMarkdown("text/html"));
  EXPECT_FALSE(WantsAgentMarkdown("text/html, */*"));
  EXPECT_FALSE(WantsAgentMarkdown(""));
  // Must NOT fire on a substring that isn't a whole token.
  EXPECT_FALSE(WantsAgentMarkdown("text/markdownish"));
  EXPECT_FALSE(WantsAgentMarkdown("application/text/markdown-x"));
}

// ---------------------------------------------------------------------------
// Absent Accept header (#1377)
// ---------------------------------------------------------------------------

TEST(CapabilityMaskTest, FromHeadersAbsentAcceptClassifiesAsWildcard) {
  // RFC 9110 §12.5.1: a request with no Accept header field accepts any media
  // type, i.e. it is semantically */*.  Before #1377 the absent header was the
  // ONLY request shape in the whole space that landed on the original-only
  // mask, which made every family whose derived slots all sit at converted
  // formats unselectable for it.
  auto absent = CapabilityMask::FromHeaders("", "", "", "");
  EXPECT_EQ(absent.image_format(), CapabilityMask::ImageFormat::kWebP);
  EXPECT_NE(absent.image_format(), CapabilityMask::ImageFormat::kOriginal);
}

TEST(CapabilityMaskTest, FromHeadersImageFormatClassificationTable) {
  // The whole image-format classification table in one place, absent header
  // included.  A drive of every branch of the format chain in FromHeaders.
  struct Row {
    std::string_view accept;
    CapabilityMask::ImageFormat expected;
  };
  const Row rows[] = {
      // Absent header -> */* -> WebP (#1377).
      {"", CapabilityMask::ImageFormat::kWebP},
      // Wildcards.
      {"*/*", CapabilityMask::ImageFormat::kWebP},
      {"image/*", CapabilityMask::ImageFormat::kWebP},
      {"text/html,*/*", CapabilityMask::ImageFormat::kWebP},
      // Explicit modern formats.
      {"image/webp", CapabilityMask::ImageFormat::kWebP},
      {"image/avif", CapabilityMask::ImageFormat::kAvif},
      {"image/avif,image/webp,*/*", CapabilityMask::ImageFormat::kAvif},
      // Present, but narrower than any wildcard: an explicit statement of what
      // the client takes, and NOT a wildcard -- stays on original-only.
      {"image/png,image/jpeg,image/gif",
       CapabilityMask::ImageFormat::kOriginal},
      {"image/jxl", CapabilityMask::ImageFormat::kOriginal},
      {"text/html", CapabilityMask::ImageFormat::kOriginal},
      // Near-miss tokens must not be read as wildcards.
      {"image/webpng", CapabilityMask::ImageFormat::kOriginal},
      {"ximage/webp", CapabilityMask::ImageFormat::kOriginal},
  };
  for (const auto& row : rows) {
    auto mask = CapabilityMask::FromHeaders(row.accept, "", "", "");
    EXPECT_EQ(mask.image_format(), row.expected)
        << "Accept: \"" << row.accept << "\"";
  }
}

TEST(CapabilityMaskTest, AbsentAcceptAgreesWithWildcardEverywhere) {
  // #1377 says an absent Accept *is* */*.  That has to hold for the whole
  // mask, not just the format bits: same accessors, same encoded byte, and --
  // the point that decides reachability -- the same AlternateId, so the two
  // shapes address the same stored slot.  Census over the rest of the header
  // space so no other dimension can smuggle in a divergence.
  const std::vector<std::string_view> uas = {
      "",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/120.0",
      "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) Mobile/15E148",
      "Mozilla/5.0 (iPad; CPU OS 17_4 like Mac OS X) Mobile/15E148",
      "Mozilla/5.0 (Linux; Android 13; SM-X700) Tablet Chrome/120.0",
      "Mozilla/5.0 (Macintosh) Retina",
  };
  const std::vector<std::string_view> save_datas = {"", "on", "On", "1", "off"};
  const std::vector<std::string_view> encodings = {
      "", "gzip", "br", "gzip, br", "identity", "*", "br;q=0, gzip"};
  const std::vector<std::string_view> dprs = {"",  "1",   "2",
                                              "3", "1.4", "garbage"};

  size_t compared = 0;
  for (auto ua : uas) {
    for (auto sd : save_datas) {
      for (auto ae : encodings) {
        for (auto dpr : dprs) {
          auto absent = CapabilityMask::FromHeaders("", ua, sd, ae, dpr);
          auto wildcard = CapabilityMask::FromHeaders("*/*", ua, sd, ae, dpr);
          ASSERT_EQ(absent, wildcard)
              << "ua=\"" << ua << "\" sd=\"" << sd << "\" ae=\"" << ae
              << "\" dpr=\"" << dpr << "\"";
          ASSERT_EQ(absent.Encode(), wildcard.Encode());
          ASSERT_EQ(
              MaskToAlternateId(static_cast<uint8_t>(absent.Encode() & 0xFF)),
              MaskToAlternateId(
                  static_cast<uint8_t>(wildcard.Encode() & 0xFF)));
          ++compared;
        }
      }
    }
  }
  EXPECT_EQ(compared,
            uas.size() * save_datas.size() * encodings.size() * dprs.size());
}

TEST(CapabilityMaskTest, AbsentAcceptDoesNotOptIntoAgentMarkdown) {
  // The #1377 widening is scoped to the image-format chain.  */* does not opt
  // a client into the markdown variant, so neither may an absent Accept.
  EXPECT_FALSE(WantsAgentMarkdown(""));
  EXPECT_FALSE(WantsAgentMarkdown("*/*"));
}

}  // namespace
}  // namespace pagespeed
