// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for EXIF orientation handling (issue #1005): the tag parser, the
// upright-to-stored coordinate mapping, and the decode-boundary baking done
// by ExifOrientedScanlineReader via CreateScanlineReader().
//
// Fixtures: testdata/jpeg/exif_orientation_{1..8}.jpg share one upright
// 32x16 scene -- quadrants TL=red, TR=green, BL=blue, BR=yellow -- stored
// pre-transformed (rotated/mirrored) with the matching EXIF Orientation tag,
// so a correct decode yields the identical upright scene for every value.
// exif_orientation_6_photo.jpg is a larger (stored 128x256, upright 256x128)
// noisy variant of the same quadrant scene for size-gated pipelines.

#include "lib/image/exif_orientation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "lib/image/image_util.h"
#include "lib/image/read_image.h"
#include "lib/image/scanline_interface.h"

namespace {

using net_instaweb::ScanlineReaderInterface;
using pagespeed::NullMessageHandler;
using pagespeed::image_compression::BuildExifOrientationApp1;
using pagespeed::image_compression::CreateScanlineReader;
using pagespeed::image_compression::IMAGE_JPEG;
using pagespeed::image_compression::kExifOrientationApp1Size;
using pagespeed::image_compression::MapUprightToStored;
using pagespeed::image_compression::ReadImage;
using pagespeed::image_compression::ReadJpegExifOrientation;
using pagespeed::image_compression::RGB_888;

// Helper to read a test file into a string.
std::string ReadTestFile(const std::string& filename) {
  std::string path = "test/lib/image/testdata/jpeg/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return "";
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

std::string OrientationFixture(int orientation) {
  return ReadTestFile("exif_orientation_" + std::to_string(orientation) +
                      ".jpg");
}

// Decodes a JPEG through the scanline API (i.e., through the orientation-
// baking decode boundary) into an interleaved buffer.
bool DecodeJpeg(const std::string& jpeg, std::string* pixels, size_t* width,
                size_t* height, size_t* components,
                pagespeed::MessageHandler* handler) {
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_JPEG, jpeg.data(), jpeg.size(), handler));
  if (reader == nullptr) {
    return false;
  }
  *width = reader->GetImageWidth();
  *height = reader->GetImageHeight();
  if (*width == 0 || reader->GetBytesPerScanline() % *width != 0) {
    return false;
  }
  *components = reader->GetBytesPerScanline() / *width;
  pixels->clear();
  while (reader->HasMoreScanLines()) {
    void* scanline = nullptr;
    if (!reader->ReadNextScanline(&scanline)) {
      return false;
    }
    pixels->append(static_cast<const char*>(scanline),
                   reader->GetBytesPerScanline());
  }
  return true;
}

// Asserts that an RGB pixel is close to the expected color (JPEG is lossy).
void ExpectPixelNear(const std::string& pixels, size_t width, size_t components,
                     size_t x, size_t y, int r, int g, int b,
                     const std::string& what) {
  ASSERT_GE(components, 3u);
  const size_t offset = (y * width + x) * components;
  ASSERT_LE(offset + 3, pixels.size());
  const int tolerance = 48;
  EXPECT_NEAR(static_cast<uint8_t>(pixels[offset]), r, tolerance) << what;
  EXPECT_NEAR(static_cast<uint8_t>(pixels[offset + 1]), g, tolerance) << what;
  EXPECT_NEAR(static_cast<uint8_t>(pixels[offset + 2]), b, tolerance) << what;
}

// Asserts the upright 32x16 quadrant scene: TL=red, TR=green, BL=blue,
// BR=yellow, sampled at the quadrant centers.
void ExpectUprightQuadrants(const std::string& pixels, size_t width,
                            size_t height, size_t components,
                            const std::string& what) {
  ASSERT_EQ(width, 32u) << what;
  ASSERT_EQ(height, 16u) << what;
  ExpectPixelNear(pixels, width, components, 8, 4, 255, 0, 0, what + " TL");
  ExpectPixelNear(pixels, width, components, 24, 4, 0, 255, 0, what + " TR");
  ExpectPixelNear(pixels, width, components, 8, 12, 0, 0, 255, what + " BL");
  ExpectPixelNear(pixels, width, components, 24, 12, 255, 255, 0, what + " BR");
}

// ============================================================
// ReadJpegExifOrientation
// ============================================================

TEST(ReadJpegExifOrientationTest, AllOrientationFixtures) {
  for (int orientation = 1; orientation <= 8; ++orientation) {
    std::string jpeg = OrientationFixture(orientation);
    ASSERT_FALSE(jpeg.empty()) << "fixture " << orientation;
    EXPECT_EQ(ReadJpegExifOrientation(jpeg.data(), jpeg.size()), orientation);
  }
}

TEST(ReadJpegExifOrientationTest, NoExifReturnsOne) {
  std::string jpeg = ReadTestFile("sjpeg1.jpg");
  ASSERT_FALSE(jpeg.empty());
  EXPECT_EQ(ReadJpegExifOrientation(jpeg.data(), jpeg.size()), 1);
}

TEST(ReadJpegExifOrientationTest, MalformedInputReturnsOne) {
  EXPECT_EQ(ReadJpegExifOrientation(nullptr, 0), 1);
  const std::string not_a_jpeg = "not a jpeg at all";
  EXPECT_EQ(ReadJpegExifOrientation(not_a_jpeg.data(), not_a_jpeg.size()), 1);
  std::string corrupt = ReadTestFile("corrupt.jpg");
  ASSERT_FALSE(corrupt.empty());
  EXPECT_EQ(ReadJpegExifOrientation(corrupt.data(), corrupt.size()), 1);
  // Truncated fixtures must never read out of bounds nor misreport.
  std::string jpeg = OrientationFixture(6);
  for (size_t len = 0; len < jpeg.size(); len += 7) {
    ReadJpegExifOrientation(jpeg.data(), len);
  }
}

TEST(ReadJpegExifOrientationTest, MinimalApp1RoundTrip) {
  // BuildExifOrientationApp1 output embedded in a synthetic JPEG must parse
  // back to the same value.
  for (int orientation = 1; orientation <= 8; ++orientation) {
    uint8_t app1[kExifOrientationApp1Size];
    BuildExifOrientationApp1(orientation, app1);
    std::string jpeg;
    jpeg.push_back(static_cast<char>(0xFF));
    jpeg.push_back(static_cast<char>(0xD8));
    jpeg.push_back(static_cast<char>(0xFF));
    jpeg.push_back(static_cast<char>(0xE1));
    const size_t seg_len = kExifOrientationApp1Size + 2;
    jpeg.push_back(static_cast<char>((seg_len >> 8) & 0xFF));
    jpeg.push_back(static_cast<char>(seg_len & 0xFF));
    jpeg.append(reinterpret_cast<const char*>(app1), kExifOrientationApp1Size);
    jpeg.push_back(static_cast<char>(0xFF));
    jpeg.push_back(static_cast<char>(0xD9));
    EXPECT_EQ(ReadJpegExifOrientation(jpeg.data(), jpeg.size()), orientation);
  }
}

// ============================================================
// Structure bounds: every read stays inside the buffer
// ============================================================

namespace {

void Put16(std::vector<uint8_t>* v, uint16_t x, bool big_endian) {
  if (big_endian) {
    v->push_back(static_cast<uint8_t>(x >> 8));
    v->push_back(static_cast<uint8_t>(x & 0xFF));
  } else {
    v->push_back(static_cast<uint8_t>(x & 0xFF));
    v->push_back(static_cast<uint8_t>(x >> 8));
  }
}

void Put32(std::vector<uint8_t>* v, uint32_t x, bool big_endian) {
  if (big_endian) {
    Put16(v, static_cast<uint16_t>(x >> 16), true);
    Put16(v, static_cast<uint16_t>(x & 0xFFFF), true);
  } else {
    Put16(v, static_cast<uint16_t>(x & 0xFFFF), false);
    Put16(v, static_cast<uint16_t>(x >> 16), false);
  }
}

// Builds a TIFF block with one IFD holding |num_declared| as the entry count
// and |tags| real entries. Each entry is (tag, orientation-style SHORT).
std::vector<uint8_t> BuildTiff(bool big_endian, uint32_t ifd_offset,
                               uint16_t num_declared,
                               const std::vector<uint16_t>& tags,
                               uint16_t orientation) {
  std::vector<uint8_t> t;
  t.push_back(big_endian ? 'M' : 'I');
  t.push_back(big_endian ? 'M' : 'I');
  Put16(&t, 42, big_endian);
  Put32(&t, ifd_offset, big_endian);
  Put16(&t, num_declared, big_endian);
  for (uint16_t tag : tags) {
    Put16(&t, tag, big_endian);
    Put16(&t, 3, big_endian);  // SHORT.
    Put32(&t, 1, big_endian);
    Put16(&t, tag == 0x0112 ? orientation : 0, big_endian);
    Put16(&t, 0, big_endian);
  }
  return t;
}

// Wraps |tiff| in a minimal JPEG whose APP1 segment ends exactly at the end
// of an exactly-sized heap allocation (no EOI), so a read past the end of the
// structure is visible to the sanitizers. |segment_length| of 0 means "match
// the payload".
class ExactJpeg {
 public:
  ExactJpeg(const std::vector<uint8_t>& tiff, size_t segment_length = 0) {
    std::vector<uint8_t> v = {0xFF, 0xD8, 0xFF, 0xE1};
    const size_t seg = segment_length != 0 ? segment_length : tiff.size() + 8;
    v.push_back(static_cast<uint8_t>(seg >> 8));
    v.push_back(static_cast<uint8_t>(seg & 0xFF));
    const char kExif[] = {'E', 'x', 'i', 'f', 0, 0};
    v.insert(v.end(), kExif, kExif + 6);
    v.insert(v.end(), tiff.begin(), tiff.end());
    size_ = v.size();
    data_.reset(new uint8_t[size_]);
    std::copy(v.begin(), v.end(), data_.get());
  }
  int Orientation() const {
    return ReadJpegExifOrientation(data_.get(), size_);
  }

 private:
  std::unique_ptr<uint8_t[]> data_;
  size_t size_ = 0;
};

}  // namespace

TEST(ReadJpegExifOrientationBoundsTest, ValidFileYieldsOrientation) {
  for (bool big_endian : {false, true}) {
    for (uint16_t o = 1; o <= 8; ++o) {
      ExactJpeg jpeg(BuildTiff(big_endian, 8, 1, {0x0112}, o));
      EXPECT_EQ(jpeg.Orientation(), o) << big_endian << " " << o;
    }
    // Orientation is the last of several entries and ends the buffer exactly.
    ExactJpeg last(BuildTiff(big_endian, 8, 3, {0x010F, 0x0110, 0x0112}, 6));
    EXPECT_EQ(last.Orientation(), 6);
  }
}

TEST(ReadJpegExifOrientationBoundsTest, StopsAtTruncatedIfd) {
  // Truncate at every byte of the TIFF header, the entry count and the entry
  // (including the position just short of the last value byte).
  for (bool big_endian : {false, true}) {
    const std::vector<uint8_t> full = BuildTiff(big_endian, 8, 1, {0x0112}, 6);
    ASSERT_EQ(full.size(), 22u);
    for (size_t len = 0; len < full.size(); ++len) {
      std::vector<uint8_t> cut(full.begin(), full.begin() + len);
      ExactJpeg jpeg(cut);
      EXPECT_EQ(jpeg.Orientation(), 1)
          << "endian " << big_endian << " len " << len;
    }
    ExactJpeg whole(full);
    EXPECT_EQ(whole.Orientation(), 6);
  }
}

TEST(ReadJpegExifOrientationBoundsTest, StopsAtTruncatedEntryAfterOthers) {
  for (bool big_endian : {false, true}) {
    const std::vector<uint8_t> full =
        BuildTiff(big_endian, 8, 3, {0x010F, 0x0110, 0x0112}, 3);
    for (size_t len = 0; len < full.size(); ++len) {
      std::vector<uint8_t> cut(full.begin(), full.begin() + len);
      ExactJpeg jpeg(cut);
      EXPECT_EQ(jpeg.Orientation(), 1) << "len " << len;
    }
  }
}

TEST(ReadJpegExifOrientationBoundsTest, StopsAtEntryCountLargerThanBuffer) {
  for (bool big_endian : {false, true}) {
    // The orientation entry is present and fits: it is still found even
    // though the declared count runs past the end of the buffer.
    for (uint16_t declared : {2, 3, 100, 0xFFFF}) {
      ExactJpeg jpeg(BuildTiff(big_endian, 8, declared, {0x0112}, 6));
      EXPECT_EQ(jpeg.Orientation(), 6) << declared;
    }
    // The orientation entry would lie past the end of the buffer.
    for (uint16_t declared : {2, 3, 100, 0xFFFF}) {
      ExactJpeg jpeg(BuildTiff(big_endian, 8, declared, {0x010F}, 6));
      EXPECT_EQ(jpeg.Orientation(), 1) << declared;
    }
    // The orientation entry is cut short by one byte.
    std::vector<uint8_t> cut =
        BuildTiff(big_endian, 8, 100, {0x010F, 0x0112}, 6);
    cut.pop_back();
    EXPECT_EQ(ExactJpeg(cut).Orientation(), 1);
    // Count zero is a valid, empty IFD.
    ExactJpeg empty(BuildTiff(big_endian, 8, 0, {}, 0));
    EXPECT_EQ(empty.Orientation(), 1);
  }
}

TEST(ReadJpegExifOrientationBoundsTest, StopsAtOffsetPastEnd) {
  for (bool big_endian : {false, true}) {
    const std::vector<uint8_t> base = BuildTiff(big_endian, 8, 1, {0x0112}, 6);
    const uint32_t len = static_cast<uint32_t>(base.size());
    for (uint32_t offset :
         {len - 2, len - 1, len, len + 1, len + 12, 0x7FFFFFFFu, 0xFFFFFFF9u,
          0xFFFFFFFEu, 0xFFFFFFFFu}) {
      std::vector<uint8_t> t = base;
      std::vector<uint8_t> patched;
      Put32(&patched, offset, big_endian);
      std::copy(patched.begin(), patched.end(), t.begin() + 4);
      ExactJpeg jpeg(t);
      EXPECT_EQ(jpeg.Orientation(), 1) << "offset " << offset;
    }
    // An IFD offset that leaves room for the count but not for the entry.
    std::vector<uint8_t> t = BuildTiff(big_endian, 8, 1, {0x0112}, 6);
    t.resize(t.size() - 1);
    EXPECT_EQ(ExactJpeg(t).Orientation(), 1);
  }
}

TEST(ReadJpegExifOrientationBoundsTest, StopsAtSegmentLongerThanBuffer) {
  const std::vector<uint8_t> tiff = BuildTiff(false, 8, 1, {0x0112}, 6);
  // Declared APP1 length reaches past the end of the data.
  EXPECT_EQ(ExactJpeg(tiff, tiff.size() + 9).Orientation(), 1);
  EXPECT_EQ(ExactJpeg(tiff, 0xFFFF).Orientation(), 1);
  // Declared length too short to hold an EXIF header and TIFF header.
  EXPECT_EQ(ExactJpeg(tiff, 2).Orientation(), 1);
  EXPECT_EQ(ExactJpeg(tiff, 15).Orientation(), 1);
}

// ============================================================
// MapUprightToStored
// ============================================================

TEST(MapUprightToStoredTest, CornerMapping) {
  // Stored raster 4 wide x 2 high. Upright top-left corner must map to the
  // stored corner that the orientation semantics dictate.
  struct Case {
    uint8_t orientation;
    size_t expect_x;
    size_t expect_y;
  };
  const Case cases[] = {
      {1, 0, 0}, {2, 3, 0}, {3, 3, 1}, {4, 0, 1},
      {5, 0, 0}, {6, 0, 1}, {7, 3, 1}, {8, 3, 0},
  };
  for (const Case& c : cases) {
    size_t sx = 99;
    size_t sy = 99;
    MapUprightToStored(c.orientation, 0, 0, 4, 2, &sx, &sy);
    EXPECT_EQ(sx, c.expect_x) << "orientation " << int{c.orientation};
    EXPECT_EQ(sy, c.expect_y) << "orientation " << int{c.orientation};
  }
}

// ============================================================
// Decode-boundary baking (ExifOrientedScanlineReader)
// ============================================================

TEST(ExifOrientedScanlineReaderTest, AllOrientationsDecodeUpright) {
  NullMessageHandler handler;
  for (int orientation = 1; orientation <= 8; ++orientation) {
    std::string jpeg = OrientationFixture(orientation);
    ASSERT_FALSE(jpeg.empty()) << "fixture " << orientation;
    std::string pixels;
    size_t width = 0;
    size_t height = 0;
    size_t components = 0;
    ASSERT_TRUE(
        DecodeJpeg(jpeg, &pixels, &width, &height, &components, &handler))
        << "fixture " << orientation;
    ExpectUprightQuadrants(pixels, width, height, components,
                           "orientation " + std::to_string(orientation));
  }
}

TEST(ExifOrientedScanlineReaderTest, ReportsDisplayDimensionsBeforeDecode) {
  NullMessageHandler handler;
  // Orientation 6: stored 16x32, display 32x16. Dimensions must already be
  // swapped right after reader creation (header-only consumers rely on it).
  std::string jpeg = OrientationFixture(6);
  ASSERT_FALSE(jpeg.empty());
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_JPEG, jpeg.data(), jpeg.size(), &handler));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 16u);
  EXPECT_EQ(reader->GetBytesPerScanline(), 32u * 3);
  EXPECT_EQ(reader->GetPixelFormat(), RGB_888);
}

TEST(ExifOrientedScanlineReaderTest, ReadImageReturnsUprightDimensions) {
  NullMessageHandler handler;
  std::string jpeg = OrientationFixture(8);
  ASSERT_FALSE(jpeg.empty());
  void* pixels = nullptr;
  size_t width = 0;
  size_t height = 0;
  ASSERT_TRUE(ReadImage(IMAGE_JPEG, jpeg.data(), jpeg.size(), &pixels, nullptr,
                        &width, &height, nullptr, &handler));
  EXPECT_EQ(width, 32u);
  EXPECT_EQ(height, 16u);
  free(pixels);
}

// RAII override of the orientation-bake ceiling; restores the default even
// when an assertion aborts the test body early.
struct BakeLimitOverride {
  explicit BakeLimitOverride(size_t limit) {
    pagespeed::image_compression::SetOrientationBakeByteLimitForTesting(limit);
  }
  ~BakeLimitOverride() {
    pagespeed::image_compression::SetOrientationBakeByteLimitForTesting(0);
  }
};

TEST(ExifOrientedScanlineReaderTest, OversizedRasterFailsAtFirstReadNotInit) {
  NullMessageHandler handler;
  std::string jpeg = OrientationFixture(6);
  ASSERT_FALSE(jpeg.empty());
  // Force the fixture over the bake ceiling. Initialization and the
  // header-only queries must keep working (display dimensions reported);
  // only the first pixel read refuses, so pixel consumers fall back to
  // their serve-original paths.
  BakeLimitOverride limit(64);
  std::unique_ptr<ScanlineReaderInterface> reader(
      CreateScanlineReader(IMAGE_JPEG, jpeg.data(), jpeg.size(), &handler));
  ASSERT_NE(reader, nullptr);
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 16u);
  EXPECT_TRUE(reader->HasMoreScanLines());
  void* scanline = nullptr;
  EXPECT_FALSE(reader->ReadNextScanline(&scanline));
}

TEST(ExifOrientedScanlineReaderTest, ReinitializeResetsState) {
  NullMessageHandler handler;
  std::string oriented = OrientationFixture(6);
  std::string plain = OrientationFixture(1);
  ASSERT_FALSE(oriented.empty());
  ASSERT_FALSE(plain.empty());
  std::unique_ptr<ScanlineReaderInterface> reader(CreateScanlineReader(
      IMAGE_JPEG, oriented.data(), oriented.size(), &handler));
  ASSERT_NE(reader, nullptr);
  void* scanline = nullptr;
  ASSERT_TRUE(reader->ReadNextScanline(&scanline));
  // Re-initializing on a different image must fully reset transform state.
  ASSERT_TRUE(reader->Initialize(plain.data(), plain.size()));
  EXPECT_EQ(reader->GetImageWidth(), 32u);
  EXPECT_EQ(reader->GetImageHeight(), 16u);
  size_t rows = 0;
  while (reader->HasMoreScanLines()) {
    ASSERT_TRUE(reader->ReadNextScanline(&scanline));
    ++rows;
  }
  EXPECT_EQ(rows, 16u);
}

}  // namespace
