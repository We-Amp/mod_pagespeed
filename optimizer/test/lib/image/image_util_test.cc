// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for image utility functions.

#include "lib/image/image_util.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

// =============================================================================
// ImageType enum tests (net_instaweb namespace)
// =============================================================================

TEST(ImageTypeTest, ImageTypeToMimeTypeString) {
  EXPECT_STREQ("image/unknown", ImageTypeToMimeTypeString(IMAGE_UNKNOWN));
  EXPECT_STREQ("image/jpeg", ImageTypeToMimeTypeString(IMAGE_JPEG));
  EXPECT_STREQ("image/png", ImageTypeToMimeTypeString(IMAGE_PNG));
  EXPECT_STREQ("image/gif", ImageTypeToMimeTypeString(IMAGE_GIF));
  EXPECT_STREQ("image/webp", ImageTypeToMimeTypeString(IMAGE_WEBP));
  EXPECT_STREQ("image/webp",
               ImageTypeToMimeTypeString(IMAGE_WEBP_LOSSLESS_OR_ALPHA));
  EXPECT_STREQ("image/webp", ImageTypeToMimeTypeString(IMAGE_WEBP_ANIMATED));
  EXPECT_STREQ("image/avif", ImageTypeToMimeTypeString(IMAGE_AVIF));
}

// NOTE: Removed ImageTypeToMimeTypeStringInvalid test — passing a value outside
// the enum range (static_cast<ImageType>(99)) is undefined behavior and is
// correctly flagged by UBSan.  The fallthrough return in
// ImageTypeToMimeTypeString is defense-in-depth that doesn't need a UB test.

TEST(ImageTypeTest, ImageTypeToString) {
  EXPECT_STREQ("IMAGE_UNKNOWN", ImageTypeToString(IMAGE_UNKNOWN));
  EXPECT_STREQ("IMAGE_JPEG", ImageTypeToString(IMAGE_JPEG));
  EXPECT_STREQ("IMAGE_PNG", ImageTypeToString(IMAGE_PNG));
  EXPECT_STREQ("IMAGE_GIF", ImageTypeToString(IMAGE_GIF));
  EXPECT_STREQ("IMAGE_WEBP", ImageTypeToString(IMAGE_WEBP));
  EXPECT_STREQ("IMAGE_WEBP_LOSSLESS_OR_ALPHA",
               ImageTypeToString(IMAGE_WEBP_LOSSLESS_OR_ALPHA));
  EXPECT_STREQ("IMAGE_WEBP_ANIMATED", ImageTypeToString(IMAGE_WEBP_ANIMATED));
  EXPECT_STREQ("IMAGE_AVIF", ImageTypeToString(IMAGE_AVIF));
}

// NOTE: Removed ImageTypeToStringInvalid test — same UB issue as above.

// =============================================================================
// ComputeImageType tests - magic number detection
// =============================================================================

TEST(ComputeImageTypeTest, UnknownForEmptyBuffer) {
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType(""));
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("short"));
}

TEST(ComputeImageTypeTest, UnknownForRandomData) {
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("This is not an image at all!!"));
}

TEST(ComputeImageTypeTest, DetectsJpeg) {
  // JPEG magic: 0xFF 0xD8
  std::string jpeg_data;
  jpeg_data.push_back(static_cast<char>(0xFF));
  jpeg_data.push_back(static_cast<char>(0xD8));
  jpeg_data.append("padding_to_make_it_8_bytes_long");
  EXPECT_EQ(IMAGE_JPEG, ComputeImageType(jpeg_data));
}

TEST(ComputeImageTypeTest, DoesNotDetectInvalidJpeg) {
  // First byte is 0xFF but second is not 0xD8
  std::string not_jpeg;
  not_jpeg.push_back(static_cast<char>(0xFF));
  not_jpeg.push_back(static_cast<char>(0x00));
  not_jpeg.append("padding_to_make_it_8_bytes");
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType(not_jpeg));
}

TEST(ComputeImageTypeTest, DetectsPng) {
  // PNG magic: 0x89 P N G 0x0D 0x0A 0x1A 0x0A
  std::string png_data = "\x89PNG\r\n\x1a\n";
  EXPECT_EQ(IMAGE_PNG, ComputeImageType(png_data));
}

TEST(ComputeImageTypeTest, DoesNotDetectInvalidPng) {
  // Starts with 0x89 but rest doesn't match
  std::string not_png = "\x89NOT_PNG_";
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType(not_png));
}

TEST(ComputeImageTypeTest, DetectsGif87a) {
  // GIF87a magic
  EXPECT_EQ(IMAGE_GIF, ComputeImageType("GIF87a__"));
}

TEST(ComputeImageTypeTest, DetectsGif89a) {
  // GIF89a magic
  EXPECT_EQ(IMAGE_GIF, ComputeImageType("GIF89a__"));
}

TEST(ComputeImageTypeTest, DoesNotDetectInvalidGif) {
  // Starts with GIF8 but wrong version character
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("GIF80a__"));
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("GIF8xa__"));
  // GIF with wrong terminator
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("GIF89b__"));
}

// WebP detection requires valid RIFF container which is complex to construct,
// so we test that invalid WebP-like data is rejected.
TEST(ComputeImageTypeTest, DoesNotDetectInvalidWebp) {
  // Starts with R but not valid WebP RIFF
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType("RIFF____INVALID_"));
}

// Real WebP header test - minimal valid lossy WebP
TEST(ComputeImageTypeTest, DetectsLossyWebp) {
  // Minimal valid lossy WebP: RIFF + size + WEBP + VP8 chunk
  // This is a minimal 26-byte lossy WebP header that libwebp will accept
  const uint8_t kMinimalLossyWebp[] = {
      'R',
      'I',
      'F',
      'F',  // RIFF
      0x1a,
      0x00,
      0x00,
      0x00,  // File size (little-endian) = 26
      'W',
      'E',
      'B',
      'P',  // WEBP
      'V',
      'P',
      '8',
      ' ',  // VP8 chunk (lossy)
      0x0e,
      0x00,
      0x00,
      0x00,  // Chunk size = 14
      // VP8 bitstream header
      0x30,
      0x01,
      0x00,
      0x9d,
      0x01,
      0x2a,
      0x01,
      0x00,
      0x01,
      0x00,
      0x00,
      0x34,
      0x25,
      0xa4,
  };
  std::string_view webp(reinterpret_cast<const char*>(kMinimalLossyWebp),
                        sizeof(kMinimalLossyWebp));
  EXPECT_EQ(IMAGE_WEBP, ComputeImageType(webp));
}

// Test WebP lossless detection
TEST(ComputeImageTypeTest, DetectsLosslessWebp) {
  // Minimal valid lossless WebP: RIFF + size + WEBP + VP8L chunk
  const uint8_t kMinimalLosslessWebp[] = {
      'R',
      'I',
      'F',
      'F',  // RIFF
      0x18,
      0x00,
      0x00,
      0x00,  // File size (little-endian) = 24
      'W',
      'E',
      'B',
      'P',  // WEBP
      'V',
      'P',
      '8',
      'L',  // VP8L chunk (lossless)
      0x0c,
      0x00,
      0x00,
      0x00,  // Chunk size = 12
      // VP8L bitstream header
      0x2f,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
  };
  std::string_view webp(reinterpret_cast<const char*>(kMinimalLosslessWebp),
                        sizeof(kMinimalLosslessWebp));
  // Lossless WebP is detected as IMAGE_WEBP_LOSSLESS_OR_ALPHA
  EXPECT_EQ(IMAGE_WEBP_LOSSLESS_OR_ALPHA, ComputeImageType(webp));
}

// Test AVIF detection with "avif" brand
TEST(ComputeImageTypeTest, DetectsAvifBrand) {
  // AVIF: bytes 4-7 = "ftyp", bytes 8-11 = "avif"
  std::string avif_data(12, '\0');
  avif_data[0] = '\x00';
  avif_data[1] = '\x00';
  avif_data[2] = '\x00';
  avif_data[3] = '\x1c';
  avif_data[4] = 'f';
  avif_data[5] = 't';
  avif_data[6] = 'y';
  avif_data[7] = 'p';
  avif_data[8] = 'a';
  avif_data[9] = 'v';
  avif_data[10] = 'i';
  avif_data[11] = 'f';
  EXPECT_EQ(IMAGE_AVIF, ComputeImageType(avif_data));
}

// Test AVIF detection with "avis" brand (AVIF sequence)
TEST(ComputeImageTypeTest, DetectsAvisBrand) {
  std::string avis_data(12, '\0');
  avis_data[4] = 'f';
  avis_data[5] = 't';
  avis_data[6] = 'y';
  avis_data[7] = 'p';
  avis_data[8] = 'a';
  avis_data[9] = 'v';
  avis_data[10] = 'i';
  avis_data[11] = 's';
  EXPECT_EQ(IMAGE_AVIF, ComputeImageType(avis_data));
}

// Test AVIF detection with "mif1" brand
TEST(ComputeImageTypeTest, DetectsMif1Brand) {
  std::string mif1_data(12, '\0');
  mif1_data[4] = 'f';
  mif1_data[5] = 't';
  mif1_data[6] = 'y';
  mif1_data[7] = 'p';
  mif1_data[8] = 'm';
  mif1_data[9] = 'i';
  mif1_data[10] = 'f';
  mif1_data[11] = '1';
  EXPECT_EQ(IMAGE_AVIF, ComputeImageType(mif1_data));
}

// Test AVIF detection fails for unknown brand
TEST(ComputeImageTypeTest, RejectsUnknownFtypBrand) {
  std::string unknown_ftyp(12, '\0');
  unknown_ftyp[4] = 'f';
  unknown_ftyp[5] = 't';
  unknown_ftyp[6] = 'y';
  unknown_ftyp[7] = 'p';
  unknown_ftyp[8] = 'h';
  unknown_ftyp[9] = 'e';
  unknown_ftyp[10] = 'i';
  unknown_ftyp[11] = 'c';
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType(unknown_ftyp));
}

// Test AVIF detection fails when buffer is too short (< 12 bytes)
TEST(ComputeImageTypeTest, RejectsShortAvifBuffer) {
  // 8 bytes: starts with 0x00 and has "ftyp" but no brand
  std::string short_data(8, '\0');
  short_data[4] = 'f';
  short_data[5] = 't';
  short_data[6] = 'y';
  short_data[7] = 'p';
  EXPECT_EQ(IMAGE_UNKNOWN, ComputeImageType(short_data));
}

// Test animated WebP detection
TEST(ComputeImageTypeTest, DetectsAnimatedWebp) {
  // Construct a minimal extended WebP with animation flag.
  // RIFF header + WEBP + VP8X chunk with animation flag set.
  const uint8_t kAnimatedWebp[] = {
      'R',
      'I',
      'F',
      'F',  // RIFF
      0x24,
      0x00,
      0x00,
      0x00,  // File size = 36
      'W',
      'E',
      'B',
      'P',  // WEBP
      'V',
      'P',
      '8',
      'X',  // VP8X chunk (extended)
      0x0a,
      0x00,
      0x00,
      0x00,  // Chunk size = 10
      0x02,
      0x00,
      0x00,
      0x00,  // Flags: animation bit set (bit 1)
      0x00,
      0x00,
      0x00,  // Canvas width - 1 (24 bits)
      0x00,
      0x00,
      0x00,  // Canvas height - 1 (24 bits)
      // ANIM chunk header (minimal, to make WebPGetFeatures parse)
      'A',
      'N',
      'I',
      'M',
      0x06,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
      0x00,
  };
  std::string_view webp(reinterpret_cast<const char*>(kAnimatedWebp),
                        sizeof(kAnimatedWebp));
  EXPECT_EQ(IMAGE_WEBP_ANIMATED, ComputeImageType(webp));
}

// =============================================================================
// ExtractPngC2paChunks tests (net_instaweb namespace)
//
// Direct, hermetic exercise of the PNG carrier-chunk extractor. The extractor only
// walks the chunk stream (it never validates CRCs), so these build minimal PNGs by
// hand with placeholder CRCs. It returns the WHOLE, verbatim bytes of every caBX
// chunk and every C2PA-bearing iTXt chunk, in file order, and fails SAFE (empty) on
// any structural anomaly so the caller serves the original rather than a stripped
// image.
// =============================================================================

namespace {

void AppendBE32(std::string* out, uint32_t v) {
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

// Builds a PNG chunk (length + type + data + placeholder CRC). The extractor never
// validates the CRC, so a zero CRC is fine for these structural tests.
std::string Chunk(std::string_view type, std::string_view data) {
  std::string c;
  AppendBE32(&c, static_cast<uint32_t>(data.size()));
  c.append(type.data(), type.size());
  c.append(data.data(), data.size());
  AppendBE32(&c, 0);  // CRC placeholder.
  return c;
}

std::string PngSig() {
  static const unsigned char sig[8] = {0x89, 'P',  'N',  'G',
                                       0x0D, 0x0A, 0x1A, 0x0A};
  return std::string(reinterpret_cast<const char*>(sig), 8);
}

// Builds the DATA block of an iTXt chunk: keyword \0 compression_flag
// compression_method language_tag \0 translated_keyword \0 text. Built with
// explicit null separators so the embedded NULs survive (a string literal would
// truncate at the first \0).
std::string ItxtData(std::string_view keyword, std::string_view text) {
  std::string d(keyword);
  d.push_back('\0');  // keyword null terminator.
  d.push_back('\0');  // compression flag (0 = uncompressed).
  d.push_back('\0');  // compression method.
  d.push_back('\0');  // language tag (empty) + null.
  d.push_back('\0');  // translated keyword (empty) + null.
  d.append(text.data(), text.size());
  return d;
}

}  // namespace

TEST(ExtractPngC2paChunksTest, FindsSingleCaBx) {
  const std::string cabx = Chunk("caBX", "jumbjumdc2pa-manifest");
  const std::string png = PngSig() + Chunk("IHDR", std::string(13, '\0')) +
                          cabx + Chunk("IEND", "");
  const std::vector<std::string_view> chunks = ExtractPngC2paChunks(png);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(cabx, std::string(chunks[0]));
}

TEST(ExtractPngC2paChunksTest, FindsMultipleCaBxInOrder) {
  const std::string cabx1 = Chunk("caBX", "first-box");
  const std::string cabx2 = Chunk("caBX", "second-box-longer");
  const std::string png = PngSig() + Chunk("IHDR", std::string(13, '\0')) +
                          cabx1 + Chunk("IDAT", "pixels") + cabx2 +
                          Chunk("IEND", "");
  const std::vector<std::string_view> chunks = ExtractPngC2paChunks(png);
  ASSERT_EQ(2u, chunks.size());
  EXPECT_EQ(cabx1, std::string(chunks[0]));
  EXPECT_EQ(cabx2, std::string(chunks[1]));
}

TEST(ExtractPngC2paChunksTest, ItxtScopedToC2paOnly) {
  // A plain iTXt (no cr:/XMP tokens) is NOT a carrier; an iTXt carrying the
  // Content-Credentials XMP packet IS.
  const std::string plain_itxt =
      Chunk("iTXt", ItxtData("Comment", "just text"));
  const std::string c2pa_itxt =
      Chunk("iTXt", ItxtData("XML:com.adobe.xmp", "<?xpacket?> cr:provenance"));
  const std::string png = PngSig() + Chunk("IHDR", std::string(13, '\0')) +
                          plain_itxt + c2pa_itxt + Chunk("IEND", "");
  const std::vector<std::string_view> chunks = ExtractPngC2paChunks(png);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(c2pa_itxt, std::string(chunks[0]));
}

TEST(ExtractPngC2paChunksTest, CaBxAndItxtBothCarriedInOrder) {
  const std::string cabx = Chunk("caBX", "box");
  const std::string c2pa_itxt =
      Chunk("iTXt", ItxtData("XML:com.adobe.xmp", "<?xpacket?> cr:prov"));
  const std::string png = PngSig() + Chunk("IHDR", std::string(13, '\0')) +
                          cabx + c2pa_itxt + Chunk("IEND", "");
  const std::vector<std::string_view> chunks = ExtractPngC2paChunks(png);
  ASSERT_EQ(2u, chunks.size());
  EXPECT_EQ(cabx, std::string(chunks[0]));
  EXPECT_EQ(c2pa_itxt, std::string(chunks[1]));
}

TEST(ExtractPngC2paChunksTest, IgnoresNonCarrierChunks) {
  const std::string png = PngSig() + Chunk("IHDR", std::string(13, '\0')) +
                          Chunk("tEXt", "Author=test") + Chunk("IDAT", "px") +
                          Chunk("IEND", "");
  EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
}

TEST(ExtractPngC2paChunksTest, NonPngReturnsEmpty) {
  const std::string not_png = "\xFF\xD8\xFF\xE0 this is a jpeg caBX jumb c2pa";
  EXPECT_TRUE(ExtractPngC2paChunks(not_png).empty());
}

TEST(ExtractPngC2paChunksTest, NoIendFailsSafeEmpty) {
  // A caBX is present but there is no terminating IEND -> structural anomaly ->
  // empty (the caller must serve the original, not a partial carry).
  const std::string png =
      PngSig() + Chunk("IHDR", std::string(13, '\0')) + Chunk("caBX", "box");
  EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
}

TEST(ExtractPngC2paChunksTest, OverlongChunkLengthFailsSafeEmpty) {
  // A chunk whose declared length runs past the buffer -> empty (no overrun, no
  // partial result), even though a caBX appeared before it.
  std::string png = PngSig() + Chunk("caBX", "real-box");
  AppendBE32(&png, 0xFFFFFF00u);  // absurd length.
  png.append("caBX");             // type.
  png.append("short");            // far fewer than the declared length.
  EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
}

TEST(ExtractPngC2paChunksTest, TooShortReturnsEmpty) {
  EXPECT_TRUE(ExtractPngC2paChunks(PngSig()).empty());   // signature only.
  EXPECT_TRUE(ExtractPngC2paChunks("").empty());         // empty.
  EXPECT_TRUE(ExtractPngC2paChunks("\x89PNG").empty());  // truncated sig.
}

}  // namespace
}  // namespace net_instaweb

namespace pagespeed::image_compression {
namespace {

// =============================================================================
// ImageFormat enum tests (pagespeed::image_compression namespace)
// =============================================================================

TEST(ImageFormatTest, ImageFormatToMimeTypeString) {
  EXPECT_STREQ("image/unknown", ImageFormatToMimeTypeString(IMAGE_UNKNOWN));
  EXPECT_STREQ("image/jpeg", ImageFormatToMimeTypeString(IMAGE_JPEG));
  EXPECT_STREQ("image/png", ImageFormatToMimeTypeString(IMAGE_PNG));
  EXPECT_STREQ("image/gif", ImageFormatToMimeTypeString(IMAGE_GIF));
  EXPECT_STREQ("image/webp", ImageFormatToMimeTypeString(IMAGE_WEBP));
}

// NOTE: Removed ImageFormatToMimeTypeStringInvalid test — casting an
// out-of-range value to an enum type is undefined behavior (UBSan).

TEST(ImageFormatTest, ImageFormatToString) {
  EXPECT_STREQ("IMAGE_UNKNOWN", ImageFormatToString(IMAGE_UNKNOWN));
  EXPECT_STREQ("IMAGE_JPEG", ImageFormatToString(IMAGE_JPEG));
  EXPECT_STREQ("IMAGE_PNG", ImageFormatToString(IMAGE_PNG));
  EXPECT_STREQ("IMAGE_GIF", ImageFormatToString(IMAGE_GIF));
  EXPECT_STREQ("IMAGE_WEBP", ImageFormatToString(IMAGE_WEBP));
}

// NOTE: Removed ImageFormatToStringInvalid test — same UB issue.

// =============================================================================
// PixelFormat tests
// =============================================================================

TEST(PixelFormatTest, GetPixelFormatString) {
  EXPECT_STREQ("UNSUPPORTED", GetPixelFormatString(UNSUPPORTED));
  EXPECT_STREQ("RGB_888", GetPixelFormatString(RGB_888));
  EXPECT_STREQ("RGBA_8888", GetPixelFormatString(RGBA_8888));
  EXPECT_STREQ("GRAY_8", GetPixelFormatString(GRAY_8));
}

// NOTE: Removed GetPixelFormatStringInvalid test — same UB issue.

TEST(PixelFormatTest, GetBytesPerPixel) {
  EXPECT_EQ(0u, GetBytesPerPixel(UNSUPPORTED));
  EXPECT_EQ(3u, GetBytesPerPixel(RGB_888));
  EXPECT_EQ(4u, GetBytesPerPixel(RGBA_8888));
  EXPECT_EQ(1u, GetBytesPerPixel(GRAY_8));
}

// NOTE: Removed GetBytesPerPixelInvalid test — same UB issue.

// =============================================================================
// Pixel packing tests
// =============================================================================

TEST(PixelPackingTest, PackHiToLo) {
  // Pack 0xAB, 0xCD, 0xEF, 0x12 -> 0xABCDEF12
  EXPECT_EQ(0xABCDEF12u, PackHiToLo(0xAB, 0xCD, 0xEF, 0x12));
  EXPECT_EQ(0x00000000u, PackHiToLo(0x00, 0x00, 0x00, 0x00));
  EXPECT_EQ(0xFFFFFFFFu, PackHiToLo(0xFF, 0xFF, 0xFF, 0xFF));
}

TEST(PixelPackingTest, PackAsArgb) {
  // Alpha=0xFF, Red=0x11, Green=0x22, Blue=0x33 -> 0xFF112233
  EXPECT_EQ(0xFF112233u, PackAsArgb(0xFF, 0x11, 0x22, 0x33));
  // Fully transparent black
  EXPECT_EQ(0x00000000u, PackAsArgb(0x00, 0x00, 0x00, 0x00));
  // Opaque white
  EXPECT_EQ(0xFFFFFFFFu, PackAsArgb(0xFF, 0xFF, 0xFF, 0xFF));
}

TEST(PixelPackingTest, RgbaToPackedArgb) {
  PixelRgbaChannels rgba = {0x11, 0x22, 0x33, 0x44};  // R, G, B, A
  // Expected ARGB: A=0x44, R=0x11, G=0x22, B=0x33 -> 0x44112233
  EXPECT_EQ(0x44112233u, RgbaToPackedArgb(rgba));
}

TEST(PixelPackingTest, RgbToPackedArgb) {
  PixelRgbaChannels rgb = {0x11, 0x22, 0x33, 0x00};  // R, G, B, (A ignored)
  // Expected ARGB: A=0xFF (opaque), R=0x11, G=0x22, B=0x33 -> 0xFF112233
  EXPECT_EQ(0xFF112233u, RgbToPackedArgb(rgb));
}

TEST(PixelPackingTest, GrayscaleToPackedArgb) {
  // Luminance 0x80 -> ARGB with R=G=B=0x80 and A=0xFF
  EXPECT_EQ(0xFF808080u, GrayscaleToPackedArgb(0x80));
  // Black
  EXPECT_EQ(0xFF000000u, GrayscaleToPackedArgb(0x00));
  // White
  EXPECT_EQ(0xFFFFFFFFu, GrayscaleToPackedArgb(0xFF));
}

// =============================================================================
// Constants tests
// =============================================================================

TEST(ConstantsTest, AlphaConstants) {
  EXPECT_EQ(255u, kAlphaOpaque);
  EXPECT_EQ(0u, kAlphaTransparent);
}

TEST(ConstantsTest, RgbaChannelIndices) {
  EXPECT_EQ(0, RGBA_RED);
  EXPECT_EQ(1, RGBA_GREEN);
  EXPECT_EQ(2, RGBA_BLUE);
  EXPECT_EQ(3, RGBA_ALPHA);
  EXPECT_EQ(4, RGBA_NUM_CHANNELS);
}

TEST(ConstantsTest, QuirksMode) {
  EXPECT_EQ(0, QUIRKS_NONE);
  EXPECT_EQ(1, QUIRKS_CHROME);
  EXPECT_EQ(2, QUIRKS_FIREFOX);
}

TEST(ConstantsTest, PreferredLibwebpLevel) {
  EXPECT_EQ(0, WEBP_NONE);
  EXPECT_EQ(1, WEBP_LOSSY);
  EXPECT_EQ(2, WEBP_LOSSLESS);
  EXPECT_EQ(3, WEBP_ANIMATED);
}

// =============================================================================
// ScanlineWriterConfig tests
// =============================================================================

TEST(ScanlineWriterConfigTest, VirtualDestructor) {
  // Test that ScanlineWriterConfig can be destroyed polymorphically
  struct DerivedConfig : public ScanlineWriterConfig {
    int value = 42;
  };

  ScanlineWriterConfig* config = new DerivedConfig();
  delete config;  // Should not leak or crash
}

}  // namespace
}  // namespace pagespeed::image_compression
