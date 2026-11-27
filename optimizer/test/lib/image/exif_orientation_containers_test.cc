// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// EXIF orientation for WebP and PNG sources: the Orientation tag is read from
// the container's EXIF chunk and baked into the pixels at the decode
// boundary, exactly as for JPEG.
//
// Fixtures (test/lib/image/testdata/exif/exif_orientation_{3,6,8}.{png,webp}):
// the same 32x16 upright scene as the JPEG fixtures (top-left red, top-right
// green, bottom-left blue, bottom-right yellow), stored the way a camera
// would store it for that orientation (16x32 for 6 and 8) with a one-entry
// little-endian EXIF block carrying the Orientation tag. Regenerate them with
// test/lib/image/testdata/exif/make_fixtures.py (Pillow only); the script
// documents how each file is built.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/exif_orientation.h"
#include "lib/image/image_util.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"

namespace {

using net_instaweb::ScanlineReaderInterface;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::CreateScanlineReader;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::IMAGE_WEBP;
using pagespeed::image_compression::ImageFormat;
using pagespeed::image_compression::ReadExifOrientation;
using pagespeed::image_compression::ReadPngExifOrientation;
using pagespeed::image_compression::ReadWebpExifOrientation;

std::string ReadFixture(const std::string& name) {
  std::ifstream file("test/lib/image/testdata/exif/" + name, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

std::string Fixture(const char* ext, int orientation) {
  return ReadFixture("exif_orientation_" + std::to_string(orientation) + "." +
                     ext);
}

struct Decoded {
  bool ok = false;
  size_t width = 0;
  size_t height = 0;
  size_t components = 0;
  std::string pixels;
};

Decoded Decode(ImageFormat format, const std::string& bytes) {
  Decoded d;
  NullMessageHandler handler;
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(format, bytes.data(), bytes.size(), &handler));
  if (reader == nullptr) return d;
  d.width = reader->GetImageWidth();
  d.height = reader->GetImageHeight();
  if (d.width == 0 || reader->GetBytesPerScanline() % d.width != 0) return d;
  d.components = reader->GetBytesPerScanline() / d.width;
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!reader->ReadNextScanline(&scanline)) return d;
    d.pixels.append(static_cast<const char*>(scanline),
                    reader->GetBytesPerScanline());
  }
  d.ok = true;
  return d;
}

void ExpectPixel(const Decoded& d, size_t x, size_t y, int r, int g, int b,
                 const std::string& what) {
  ASSERT_GE(d.components, 3u) << what;
  const size_t off = (y * d.width + x) * d.components;
  ASSERT_LE(off + 3, d.pixels.size()) << what;
  EXPECT_NEAR(static_cast<uint8_t>(d.pixels[off]), r, 8) << what;
  EXPECT_NEAR(static_cast<uint8_t>(d.pixels[off + 1]), g, 8) << what;
  EXPECT_NEAR(static_cast<uint8_t>(d.pixels[off + 2]), b, 8) << what;
}

void ExpectUpright(const Decoded& d, const std::string& what) {
  ASSERT_TRUE(d.ok) << what;
  ASSERT_EQ(d.width, 32u) << what;
  ASSERT_EQ(d.height, 16u) << what;
  ExpectPixel(d, 8, 4, 255, 0, 0, what + " TL");
  ExpectPixel(d, 24, 4, 0, 255, 0, what + " TR");
  ExpectPixel(d, 8, 12, 0, 0, 255, what + " BL");
  ExpectPixel(d, 24, 12, 255, 255, 0, what + " BR");
}

// A malformed-EXIF image must decode (the pixels are fine) but stay exactly
// as stored: orientation reads as 1, so no rotation happens (16x32 for the
// orientation-6 fixture, where a rotation would give 32x16).
void ExpectUnrotated(ImageFormat format, const std::string& bytes,
                     const std::string& what) {
  EXPECT_EQ(ReadExifOrientation(bytes.data(), bytes.size()), 1) << what;
  Decoded d = Decode(format, bytes);
  ASSERT_TRUE(d.ok) << what;
  EXPECT_EQ(d.width, 16u) << what;
  EXPECT_EQ(d.height, 32u) << what;
}

// ---- Byte-level helpers -------------------------------------------------

uint32_t Crc32(const uint8_t* p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void PutBe32(std::string* s, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    (*s)[off + i] = static_cast<char>(v >> (24 - 8 * i));
}
void PutLe32(std::string* s, size_t off, uint32_t v) {
  for (int i = 0; i < 4; ++i) (*s)[off + i] = static_cast<char>(v >> (8 * i));
}

// Offset of the PNG chunk with |type|, or npos. Chunk starts at its length.
size_t FindPngChunk(const std::string& png, const char* type) {
  size_t pos = 8;
  while (pos + 12 <= png.size()) {
    uint32_t len = 0;
    for (int i = 0; i < 4; ++i)
      len = (len << 8) | static_cast<uint8_t>(png[pos + i]);
    if (png.compare(pos + 4, 4, type) == 0) return pos;
    pos += 12 + len;
  }
  return std::string::npos;
}

// Total size (length, type, payload and CRC) of the PNG chunk at |chunk|.
size_t PngChunkTotalSize(const std::string& png, size_t chunk) {
  uint32_t len = 0;
  for (int i = 0; i < 4; ++i) {
    len = (len << 8) | static_cast<uint8_t>(png[chunk + i]);
  }
  return 12 + len;
}

// Rewrites the CRC of the PNG chunk starting at |chunk| so that a deliberately
// damaged payload is still a well-formed chunk.
void FixPngCrc(std::string* png, size_t chunk) {
  uint32_t len = 0;
  for (int i = 0; i < 4; ++i)
    len = (len << 8) | static_cast<uint8_t>((*png)[chunk + i]);
  PutBe32(png, chunk + 8 + len,
          Crc32(reinterpret_cast<const uint8_t*>(png->data()) + chunk + 4,
                4 + len));
}

// A PNG eXIf chunk is at offset |chunk|+8; the TIFF block layout is
// "II*\0" ifd_offset(4) count(2) tag(2) type(2) count(4) value(4) next(4)
constexpr size_t kTiffIfdOffsetField = 4;
constexpr size_t kTiffEntryCountField = 8;

std::string WithPngTiffMutation(const std::string& png, size_t tiff_off,
                                const std::string& bytes) {
  std::string m = png;
  const size_t chunk = FindPngChunk(m, "eXIf");
  m.replace(chunk + 8 + tiff_off, bytes.size(), bytes);
  FixPngCrc(&m, chunk);
  return m;
}

size_t FindWebpExif(const std::string& webp) {
  size_t pos = 12;
  while (pos + 8 <= webp.size()) {
    uint32_t len = 0;
    for (int i = 3; i >= 0; --i)
      len = (len << 8) | static_cast<uint8_t>(webp[pos + 4 + i]);
    if (webp.compare(pos, 4, "EXIF") == 0) return pos;
    pos += 8 + len + (len & 1);
  }
  return std::string::npos;
}

std::string WithWebpTiffMutation(const std::string& webp, size_t tiff_off,
                                 const std::string& bytes) {
  std::string m = webp;
  m.replace(FindWebpExif(m) + 8 + tiff_off, bytes.size(), bytes);
  return m;
}

// Exactly-sized heap copy so sanitizers see any read past the end.
class Exact {
 public:
  Exact(const std::string& s, size_t len)
      : size_(len), data_(new uint8_t[len]) {
    memcpy(data_.get(), s.data(), len);
  }
  const uint8_t* data() const { return data_.get(); }
  size_t size() const { return size_; }

 private:
  size_t size_;
  std::unique_ptr<uint8_t[]> data_;
};

// ---- Tests ----------------------------------------------------------------

TEST(ExifContainersTest, FixturesCarryTheirOrientation) {
  for (int o : {3, 6, 8}) {
    const std::string png = Fixture("png", o);
    const std::string webp = Fixture("webp", o);
    ASSERT_FALSE(png.empty());
    ASSERT_FALSE(webp.empty());
    EXPECT_LT(png.size(), 2048u);
    EXPECT_LT(webp.size(), 2048u);
    EXPECT_EQ(ReadPngExifOrientation(png.data(), png.size()), o);
    EXPECT_EQ(ReadWebpExifOrientation(webp.data(), webp.size()), o);
    EXPECT_EQ(ReadExifOrientation(png.data(), png.size()), o);
    EXPECT_EQ(ReadExifOrientation(webp.data(), webp.size()), o);
  }
}

TEST(ExifContainersTest, PngDecodesUpright) {
  for (int o : {3, 6, 8}) {
    ExpectUpright(Decode(IMAGE_PNG, Fixture("png", o)),
                  "png orientation " + std::to_string(o));
  }
}

TEST(ExifContainersTest, WebpDecodesUpright) {
  for (int o : {3, 6, 8}) {
    ExpectUpright(Decode(IMAGE_WEBP, Fixture("webp", o)),
                  "webp orientation " + std::to_string(o));
  }
}

TEST(ExifContainersTest, DisplayDimensionsReportedBeforeDecode) {
  NullMessageHandler handler;
  for (ImageFormat format : {IMAGE_PNG, IMAGE_WEBP}) {
    const std::string bytes = Fixture(format == IMAGE_PNG ? "png" : "webp", 6);
    std::unique_ptr<ScanlineReaderInterface> reader(
        CreateScanlineReader(format, bytes.data(), bytes.size(), &handler));
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->GetImageWidth(), 32u);
    EXPECT_EQ(reader->GetImageHeight(), 16u);
  }
}

TEST(ExifContainersTest, NoExifIsPassthrough) {
  // A plain (orientation-free) PNG and WebP from the shared fixtures decode
  // with their stored dimensions.
  for (const char* name : {"alpha_32x32.png", "alpha_32x32.webp"}) {
    std::ifstream f(std::string("test/lib/image/testdata/") + name,
                    std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty()) << name;
    EXPECT_EQ(ReadExifOrientation(bytes.data(), bytes.size()), 1) << name;
    Decoded d = Decode(bytes[0] == 'R' ? IMAGE_WEBP : IMAGE_PNG, bytes);
    ASSERT_TRUE(d.ok) << name;
    EXPECT_EQ(d.width, 32u) << name;
    EXPECT_EQ(d.height, 32u) << name;
  }
}

TEST(ExifContainersTest, PngChunkAfterImageDataIsHonoured) {
  // The PNG specification allows eXIf after IDAT. Move the chunk to just
  // before IEND.
  std::string png = Fixture("png", 6);
  const size_t exif = FindPngChunk(png, "eXIf");
  ASSERT_NE(exif, std::string::npos);
  const std::string chunk = png.substr(exif, PngChunkTotalSize(png, exif));
  png.erase(exif, chunk.size());
  png.insert(FindPngChunk(png, "IEND"), chunk);
  EXPECT_EQ(ReadPngExifOrientation(png.data(), png.size()), 6);
  ExpectUpright(Decode(IMAGE_PNG, png), "png eXIf after IDAT");
}

TEST(ExifContainersTest, WebpPayloadWithExifPrefixIsHonoured) {
  // Some writers put the JPEG-style "Exif\0\0" prefix in front of the TIFF
  // block inside the WebP EXIF chunk.
  std::string webp = Fixture("webp", 8);
  const size_t exif = FindWebpExif(webp);
  ASSERT_NE(exif, std::string::npos);
  const uint32_t tiff_size = static_cast<uint8_t>(webp[exif + 4]);
  ASSERT_EQ(webp.size(), exif + 8 + tiff_size);  // EXIF is the last chunk.
  ASSERT_EQ(tiff_size % 2, 0u);  // The 6-byte prefix keeps the size even.
  webp.insert(exif + 8, std::string("Exif\0\0", 6));
  PutLe32(&webp, exif + 4, tiff_size + 6);
  PutLe32(&webp, 4, static_cast<uint32_t>(webp.size() - 8));
  EXPECT_EQ(ReadWebpExifOrientation(webp.data(), webp.size()), 8);
  ExpectUpright(Decode(IMAGE_WEBP, webp), "webp prefixed exif");
}

// Damaged TIFF blocks, as (offset in the TIFF block, replacement bytes).
struct TiffDamage {
  const char* what;
  size_t offset;
  std::string bytes;
};

std::vector<TiffDamage> AllTiffDamage() {
  return {
      {"IFD offset far past the end", kTiffIfdOffsetField,
       std::string("\xF0\xFF\xFF\xFF", 4)},
      {"IFD offset inside the header", kTiffIfdOffsetField,
       std::string("\x02\x00\x00\x00", 4)},
      {"entry count larger than the data (and no orientation tag)",
       kTiffEntryCountField,
       std::string("\xFF\xFF\x11\x01", 4)},  // count 0xFFFF, tag 0x0111.
      {"wrong tag type", kTiffEntryCountField + 4, std::string("\x04\x00", 2)},
      {"value out of range", kTiffEntryCountField + 10,
       std::string("\x09\x00", 2)},
      {"bad TIFF magic", 2, std::string("\x2B\x00", 2)},
      {"bad byte-order mark", 0, std::string("XX", 2)},
  };
}

// Inserts a chunk with |payload_size| payload bytes (plus a pad byte when
// |pad| is set) just before the EXIF chunk and fixes the RIFF size.
std::string WithWebpChunkBeforeExif(const std::string& webp,
                                    size_t payload_size, bool pad) {
  std::string m = webp;
  std::string chunk = "XMP ";
  chunk.append(4, '\0');
  PutLe32(&chunk, 4, static_cast<uint32_t>(payload_size));
  chunk.append(payload_size, 'x');
  if (pad) chunk.push_back('\0');
  m.insert(FindWebpExif(m), chunk);
  PutLe32(&m, 4, static_cast<uint32_t>(m.size() - 8));
  return m;
}

TEST(ExifContainersTest, WebpOddSizedChunkBeforeExifIsPadded) {
  const std::string webp =
      WithWebpChunkBeforeExif(Fixture("webp", 6), 3, /*pad=*/true);
  EXPECT_EQ(ReadWebpExifOrientation(webp.data(), webp.size()), 6);
  // Without the pad byte the chunk walk lands off the chunk boundary and the
  // EXIF chunk is not found; it must never be read from the wrong place.
  const std::string unpadded =
      WithWebpChunkBeforeExif(Fixture("webp", 6), 3, /*pad=*/false);
  EXPECT_EQ(ReadWebpExifOrientation(unpadded.data(), unpadded.size()), 1);
}

TEST(ExifContainersTest, WebpOddSizedLastChunkWithoutPadReadsAsOne) {
  // VP8X + VP8L + an odd-sized chunk whose pad byte is missing at the end of
  // the buffer: the walk must stop, not step past the end.
  std::string webp = Fixture("webp", 6);
  webp.resize(FindWebpExif(webp));
  std::string chunk = "XMP ";
  chunk.append(4, '\0');
  PutLe32(&chunk, 4, 3);
  chunk.append(3, 'x');
  webp += chunk;
  PutLe32(&webp, 4, static_cast<uint32_t>(webp.size() - 8));
  Exact e(webp, webp.size());
  EXPECT_EQ(ReadWebpExifOrientation(e.data(), e.size()), 1);
}

TEST(ExifContainersTest, WebpShortVp8xHeaderReadsAsOne) {
  std::string webp = Fixture("webp", 6);
  PutLe32(&webp, 16, 4);  // VP8X payload size field: 4 instead of 10.
  EXPECT_EQ(ReadWebpExifOrientation(webp.data(), webp.size()), 1);
}

TEST(ExifContainersTest, PngMalformedExifDoesNotRotate) {
  const std::string base = Fixture("png", 6);
  ASSERT_NE(FindPngChunk(base, "eXIf"), std::string::npos);
  for (const TiffDamage& d : AllTiffDamage()) {
    ExpectUnrotated(IMAGE_PNG, WithPngTiffMutation(base, d.offset, d.bytes),
                    std::string("png: ") + d.what);
  }
}

TEST(ExifContainersTest, WebpMalformedExifDoesNotRotate) {
  const std::string base = Fixture("webp", 6);
  ASSERT_NE(FindWebpExif(base), std::string::npos);
  for (const TiffDamage& d : AllTiffDamage()) {
    ExpectUnrotated(IMAGE_WEBP, WithWebpTiffMutation(base, d.offset, d.bytes),
                    std::string("webp: ") + d.what);
  }
}

TEST(ExifContainersTest, PngChunkLengthPastEndDoesNotRotate) {
  std::string png = Fixture("png", 6);
  const size_t exif = FindPngChunk(png, "eXIf");
  PutBe32(&png, exif, 0xFFFFFFF0u);
  EXPECT_EQ(ReadPngExifOrientation(png.data(), png.size()), 1);
  // A zero-length eXIf chunk carries nothing to read.
  png = Fixture("png", 6);
  PutBe32(&png, exif, 0);
  EXPECT_EQ(ReadPngExifOrientation(png.data(), png.size()), 1);
}

TEST(ExifContainersTest, WebpContainerDamageDoesNotRotate) {
  const std::string base = Fixture("webp", 6);
  // EXIF flag cleared in the VP8X header: the chunk must be ignored.
  std::string no_flag = base;
  no_flag[20] = static_cast<char>(no_flag[20] & ~0x08);
  ExpectUnrotated(IMAGE_WEBP, no_flag, "webp: EXIF flag clear");
  // Declared RIFF size too small to reach the chunk.
  std::string small_riff = base;
  PutLe32(&small_riff, 4, 4);
  EXPECT_EQ(ReadWebpExifOrientation(small_riff.data(), small_riff.size()), 1);
  // EXIF chunk size far larger than the file.
  std::string big_chunk = base;
  PutLe32(&big_chunk, FindWebpExif(big_chunk) + 4, 0xFFFFFFFFu);
  EXPECT_EQ(ReadWebpExifOrientation(big_chunk.data(), big_chunk.size()), 1);
  // First chunk is not VP8X (simple-format file with a stray EXIF chunk).
  std::string simple = base;
  simple.replace(12, 4, "VP8L");
  EXPECT_EQ(ReadWebpExifOrientation(simple.data(), simple.size()), 1);
}

TEST(ExifContainersTest, TruncationNeverReadsPastTheEnd) {
  // Every prefix of each fixture, in an exactly-sized heap block so the
  // sanitizer build catches any over-read. A prefix that ends before the
  // EXIF block is complete must read as 1; the full block reads as 6.
  const std::string png = Fixture("png", 6);
  const size_t png_exif = FindPngChunk(png, "eXIf");
  const size_t png_exif_end = png_exif + PngChunkTotalSize(png, png_exif);
  for (size_t len = 0; len <= png.size(); ++len) {
    Exact e(png, len);
    const int got = ReadPngExifOrientation(e.data(), e.size());
    EXPECT_EQ(got, len >= png_exif_end ? 6 : 1) << "png len " << len;
    EXPECT_EQ(ReadExifOrientation(e.data(), e.size()), got) << "png " << len;
  }
  const std::string webp = Fixture("webp", 6);
  for (size_t len = 0; len <= webp.size(); ++len) {
    Exact e(webp, len);
    const int got = ReadWebpExifOrientation(e.data(), e.size());
    EXPECT_EQ(got, len == webp.size() ? 6 : 1) << "webp len " << len;
    EXPECT_EQ(ReadExifOrientation(e.data(), e.size()), got) << "webp " << len;
  }
}

TEST(ExifContainersTest, NonContainerInputReadsAsOne) {
  EXPECT_EQ(ReadPngExifOrientation(nullptr, 0), 1);
  EXPECT_EQ(ReadWebpExifOrientation(nullptr, 0), 1);
  EXPECT_EQ(ReadExifOrientation(nullptr, 0), 1);
  const std::string junk(64, 'x');
  EXPECT_EQ(ReadPngExifOrientation(junk.data(), junk.size()), 1);
  EXPECT_EQ(ReadWebpExifOrientation(junk.data(), junk.size()), 1);
  EXPECT_EQ(ReadExifOrientation(junk.data(), junk.size()), 1);
}

}  // namespace
