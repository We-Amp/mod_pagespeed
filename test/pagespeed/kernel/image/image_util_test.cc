/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "pagespeed/kernel/image/image_util.h"

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/image/test_utils.h"

namespace {

static const char kInvalidImageFormat[] = "Invalid image format";

const char kGifImage[] = "transparent.gif";
const char kPngImage[] = "this_is_a_test.png";
const char kJpegImage[] = "sjpeg1.jpg";
const char kWebpOpaqueImage[] = "opaque_32x20.webp";
const char kWebpLosslessImage[] = "img3.webpla";
const char kWebpAnimatedImage[] = "animated.webp";
// icc_xmp_ex.webp contains only one lossily compressed image, but has "VP8X"
// chunk because of XMP and ICC.
const char kWebpIccXmpImage[] = "icc_xmp_ex.webp";

// Enums
using pagespeed::image_compression::ImageFormat;
using pagespeed::image_compression::PixelFormat;

// Image formats.
using pagespeed::image_compression::IMAGE_AVIF;
using pagespeed::image_compression::IMAGE_GIF;
using pagespeed::image_compression::IMAGE_JPEG;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::IMAGE_UNKNOWN;
using pagespeed::image_compression::IMAGE_WEBP;

// Pixel formats.
using pagespeed::image_compression::GRAY_8;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::RGBA_8888;
using pagespeed::image_compression::UNSUPPORTED;

// WebP formats.
using pagespeed::image_compression::WEBP_ANIMATED;
using pagespeed::image_compression::WEBP_LOSSLESS;
using pagespeed::image_compression::WEBP_LOSSY;
using pagespeed::image_compression::WEBP_NONE;

// Folders for testing images.
using pagespeed::image_compression::kGifTestDir;
using pagespeed::image_compression::kJpegTestDir;
using pagespeed::image_compression::kPngTestDir;
using pagespeed::image_compression::kWebpTestDir;

using pagespeed::image_compression::ComputeImageType;
using pagespeed::image_compression::PreferredLibwebpLevel;
using pagespeed::image_compression::ReadTestFileWithExt;

TEST(ImageUtilTest, ImageFormatToMimeTypeString) {
  EXPECT_STREQ("image/unknown", ImageFormatToMimeTypeString(IMAGE_UNKNOWN));
  EXPECT_STREQ("image/jpeg", ImageFormatToMimeTypeString(IMAGE_JPEG));
  EXPECT_STREQ("image/png", ImageFormatToMimeTypeString(IMAGE_PNG));
  EXPECT_STREQ("image/gif", ImageFormatToMimeTypeString(IMAGE_GIF));
  EXPECT_STREQ("image/webp", ImageFormatToMimeTypeString(IMAGE_WEBP));
  EXPECT_STREQ("image/avif", ImageFormatToMimeTypeString(IMAGE_AVIF));
  EXPECT_STREQ(kInvalidImageFormat,
               ImageFormatToMimeTypeString(static_cast<ImageFormat>(6)));
}

TEST(ImageUtilTest, ImageFormatToString) {
  EXPECT_STREQ("IMAGE_UNKNOWN", ImageFormatToString(IMAGE_UNKNOWN));
  EXPECT_STREQ("IMAGE_JPEG", ImageFormatToString(IMAGE_JPEG));
  EXPECT_STREQ("IMAGE_PNG", ImageFormatToString(IMAGE_PNG));
  EXPECT_STREQ("IMAGE_GIF", ImageFormatToString(IMAGE_GIF));
  EXPECT_STREQ("IMAGE_WEBP", ImageFormatToString(IMAGE_WEBP));
  EXPECT_STREQ("IMAGE_AVIF", ImageFormatToString(IMAGE_AVIF));
  EXPECT_STREQ(kInvalidImageFormat,
               ImageFormatToString(static_cast<ImageFormat>(6)));
}

TEST(ImageUtilTest, GetPixelFormatString) {
  EXPECT_STREQ("UNSUPPORTED", GetPixelFormatString(UNSUPPORTED));
  EXPECT_STREQ("RGB_888", GetPixelFormatString(RGB_888));
  EXPECT_STREQ("RGBA_8888", GetPixelFormatString(RGBA_8888));
  EXPECT_STREQ("GRAY_8", GetPixelFormatString(GRAY_8));
}

TEST(ImageUtilTest, GetBytesPerPixel) {
  EXPECT_EQ(0, GetBytesPerPixel(UNSUPPORTED));
  EXPECT_EQ(3, GetBytesPerPixel(RGB_888));
  EXPECT_EQ(4, GetBytesPerPixel(RGBA_8888));
  EXPECT_EQ(1, GetBytesPerPixel(GRAY_8));
}

TEST(ImageUtilTest, ImageFormat) {
  GoogleString buffer;

  ASSERT_TRUE(ReadTestFileWithExt(kGifTestDir, kGifImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_GIF, ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kPngTestDir, kPngImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_PNG, ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kJpegTestDir, kJpegImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_JPEG, ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kWebpTestDir, kWebpOpaqueImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_WEBP, ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kWebpTestDir, kWebpLosslessImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA,
            ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kWebpTestDir, kWebpAnimatedImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_WEBP_ANIMATED, ComputeImageType(buffer));

  ASSERT_TRUE(ReadTestFileWithExt(kWebpTestDir, kWebpIccXmpImage, &buffer));
  EXPECT_EQ(net_instaweb::IMAGE_WEBP, ComputeImageType(buffer));
}

namespace {

void AppendBE32ToString(uint32_t v, GoogleString* out) {
  out->push_back(static_cast<char>((v >> 24) & 0xFF));
  out->push_back(static_cast<char>((v >> 16) & 0xFF));
  out->push_back(static_cast<char>((v >> 8) & 0xFF));
  out->push_back(static_cast<char>(v & 0xFF));
}

// Builds an ISO-BMFF "ftyp" box: size(BE32) + "ftyp" + major brand +
// minor version + compatible brands. When declared_size is non-zero it
// overrides the computed box size (to exercise the clamp paths); the actual
// bytes emitted are always the real fields.
GoogleString MakeFtypBox(const GoogleString& major_brand,
                         const std::vector<GoogleString>& compatible_brands,
                         uint32_t declared_size = 0) {
  EXPECT_EQ(static_cast<size_t>(4), major_brand.size());
  GoogleString box;
  const uint32_t real_size =
      static_cast<uint32_t>(16 + 4 * compatible_brands.size());
  AppendBE32ToString(declared_size != 0 ? declared_size : real_size, &box);
  box.append("ftyp");
  box.append(major_brand);
  box.append(4, '\0');  // minor version.
  for (const GoogleString& brand : compatible_brands) {
    EXPECT_EQ(static_cast<size_t>(4), brand.size());
    box.append(brand);
  }
  return box;
}

}  // namespace

// the design record Stream D: ISO-BMFF / AVIF sniffing in ComputeImageType. The probe is
// keyed on an "ftyp" box whose major or compatible brand is an AVIF brand, so
// non-AVIF ISO-BMFF (mp4/HEIC) and truncated/hostile inputs never
// misclassify -- and never crash.
TEST(ImageUtilTest, ComputeImageTypeAvifSniffing) {
  // (a) Major brand "avif" -> still AVIF.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(MakeFtypBox("avif", {"mif1", "miaf"})));
  // No compatible brands at all -- the minimal 16-byte ftyp.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(MakeFtypBox("avif", {})));

  // (b) Major brand "avis" -> animated AVIF (image sequence). "avis" wins
  // even when "avif" is also present as a compatible brand.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF_ANIMATED,
            ComputeImageType(MakeFtypBox("avis", {"avif", "msf1", "miaf"})));

  // (c) AVIF brand only in the compatible-brands list, different major.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(MakeFtypBox("mif1", {"miaf", "avif"})));
  EXPECT_EQ(net_instaweb::IMAGE_AVIF_ANIMATED,
            ComputeImageType(MakeFtypBox("msf1", {"miaf", "avis"})));

  // (e) Non-AVIF ftyp (a plain mp4/isom container) -> NOT AVIF.
  EXPECT_EQ(net_instaweb::IMAGE_UNKNOWN,
            ComputeImageType(MakeFtypBox("isom", {"iso2", "mp41"})));
  // HEIC: same container family, not AVIF.
  EXPECT_EQ(net_instaweb::IMAGE_UNKNOWN,
            ComputeImageType(MakeFtypBox("heic", {"mif1", "heic"})));
}

TEST(ImageUtilTest, ComputeImageTypeAvifSniffingHostileBuffers) {
  // (d) Truncated buffers: shorter than the 12-byte probe minimum -> no crash,
  // IMAGE_UNKNOWN (the leading 0x00 size byte matches no other sniffer).
  const GoogleString full = MakeFtypBox("avif", {"mif1"});
  for (size_t len = 0; len < 12; ++len) {
    EXPECT_EQ(net_instaweb::IMAGE_UNKNOWN,
              ComputeImageType(StringPiece(full.data(), len)))
        << "at truncation length " << len;
  }
  // Exactly 12 bytes: the major brand is readable, the declared box size
  // (larger than the buffer) is clamped, and the type is still detected.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(StringPiece(full.data(), 12)));

  // Absurd declared box sizes must not crash or read out of bounds.
  // Size 0xFFFFFFFF with an AVIF compatible brand inside the actual buffer:
  // clamped to the buffer, brand found, best-effort detection.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(MakeFtypBox("mif1", {"avif"}, 0xFFFFFFFF)));
  // Absurd size on a non-AVIF ftyp with trailing non-brand garbage: no crash,
  // and no misclassification.
  GoogleString hostile = MakeFtypBox("isom", {"mp41"}, 0xFFFFFFFF);
  hostile.append("mdat-not-a-brand-avXfavYs");
  EXPECT_EQ(net_instaweb::IMAGE_UNKNOWN, ComputeImageType(hostile));
  // A declared size smaller than the 16-byte minimum is also clamped, not
  // trusted (a zero/8-byte size would otherwise wrap the brand scan).
  EXPECT_EQ(net_instaweb::IMAGE_AVIF,
            ComputeImageType(MakeFtypBox("avif", {}, 8)));

  // Trailing bytes after the ftyp box (the normal case: meta/mdat boxes
  // follow) do not confuse the probe.
  GoogleString with_tail = MakeFtypBox("avif", {"mif1"});
  with_tail.append(GoogleString(64, '\x5A'));
  EXPECT_EQ(net_instaweb::IMAGE_AVIF, ComputeImageType(with_tail));
}

// the design record Stream H: the ISO-BMFF C2PA carrier -- a top-level "uuid" box tagged
// with the C2PA manifest UUID (d8fec3d6-1b0e-483c-9297-5828877ec481) -- must
// trip ImageHasC2paManifest, and only when the buffer is actually ISO-BMFF
// (an "ftyp" box gate prevents false positives on random binary data).
TEST(ImageUtilTest, IsoBmffUuidC2paDetection) {
  using pagespeed::image_compression::ImageHasC2paManifest;

  static const unsigned char kC2paUuid[16] = {
      0xd8, 0xfe, 0xc3, 0xd6, 0x1b, 0x0e, 0x48, 0x3c,
      0x92, 0x97, 0x58, 0x28, 0x87, 0x7e, 0xc4, 0x81};
  const GoogleString payload = "opaque-manifest-bytes";
  GoogleString uuid_box;
  AppendBE32ToString(static_cast<uint32_t>(8 + 16 + payload.size()), &uuid_box);
  uuid_box.append("uuid");
  uuid_box.append(reinterpret_cast<const char*>(kC2paUuid), 16);
  uuid_box.append(payload);

  // Note: no "jumb"/"jumd"/"c2pa"/"caBX" tokens and no XMP markers anywhere in
  // these fixtures, so ONLY the ISO-BMFF uuid path can fire.
  const GoogleString ftyp = MakeFtypBox("avif", {"mif1", "miaf"});

  // Positive: ftyp-gated buffer carrying the C2PA uuid box.
  EXPECT_TRUE(ImageHasC2paManifest(ftyp + uuid_box));

  // The gate: the very same uuid box WITHOUT an ISO-BMFF ftyp prefix is not
  // detected (the raw 16-byte scan must not fire on non-BMFF data).
  EXPECT_FALSE(ImageHasC2paManifest(uuid_box));
  EXPECT_FALSE(ImageHasC2paManifest(GoogleString(16, 'x') + uuid_box));

  // A uuid box with a DIFFERENT UUID is not a C2PA carrier.
  GoogleString other_uuid_box;
  AppendBE32ToString(static_cast<uint32_t>(8 + 16 + payload.size()),
                     &other_uuid_box);
  other_uuid_box.append("uuid");
  other_uuid_box.append(16, '\x42');
  other_uuid_box.append(payload);
  EXPECT_FALSE(ImageHasC2paManifest(ftyp + other_uuid_box));

  // A clean AVIF (ftyp, no manifest boxes) is manifest-free.
  EXPECT_FALSE(ImageHasC2paManifest(ftyp));
}

// the design record: the C2PA / Content-Credentials provenance detector.
TEST(ImageUtilTest, C2paManifestDetection) {
  using pagespeed::image_compression::ImageHasC2paManifest;
  using pagespeed::image_compression::ImageHasXmpC2pa;

  // JUMBF/box FourCCs (JPEG APP11 + generic containers) and the PNG caBX chunk type.
  EXPECT_TRUE(ImageHasC2paManifest("xxxxxxxxjumbxxxx"));
  EXPECT_TRUE(ImageHasC2paManifest("padding..jumd..padding"));
  EXPECT_TRUE(ImageHasC2paManifest("padding-c2pa-padding"));
  EXPECT_TRUE(ImageHasC2paManifest("\x89PNG\r\n....caBX....IEND"));
  EXPECT_FALSE(ImageHasXmpC2pa("padding-c2pa-padding"));  // not the XMP form

  // XMP Content-Credentials form: "cr:" only counts alongside an XMP packet marker.
  EXPECT_TRUE(ImageHasXmpC2pa("<?xpacket?> cr:provenance bytes"));
  EXPECT_TRUE(ImageHasXmpC2pa("ns http://ns.adobe.com/xap/ ... cr:foo"));
  EXPECT_TRUE(ImageHasXmpC2pa("contentauth ... cr:thing"));
  EXPECT_TRUE(ImageHasC2paManifest("<?xpacket?> cr:provenance bytes"));

  // Load-bearing false-positive guard: a bare "cr:" with no XMP marker is NOT a match.
  EXPECT_FALSE(ImageHasXmpC2pa("style: color: red; cr: not provenance at all"));
  EXPECT_FALSE(
      ImageHasC2paManifest("style: color: red; cr: not provenance at all"));

  // Too-short buffers and clean content return false.
  EXPECT_FALSE(ImageHasC2paManifest("c2pa"));  // < 12 bytes
  EXPECT_FALSE(
      ImageHasC2paManifest("a perfectly ordinary caption, no markers"));
}

// the design record Level A: PNG carrier-chunk extractor (the JPEG path needs no extractor;
// jpeg_optimizer carries APP11/JUMBF via libjpeg's marker API).
TEST(ImageUtilTest, ExtractPngC2paChunks) {
  using pagespeed::image_compression::ExtractPngC2paChunks;

  auto be32 = [](uint32_t v, GoogleString* o) {
    o->push_back(static_cast<char>((v >> 24) & 0xFF));
    o->push_back(static_cast<char>((v >> 16) & 0xFF));
    o->push_back(static_cast<char>((v >> 8) & 0xFF));
    o->push_back(static_cast<char>(v & 0xFF));
  };
  // Frames a whole PNG chunk: length(BE32) + type + data + CRC(BE32). The
  // extractor does not verify the CRC, so a dummy value is fine here.
  auto chunk = [&](const GoogleString& type, const GoogleString& data) {
    GoogleString c;
    be32(static_cast<uint32_t>(data.size()), &c);
    c.append(type);
    c.append(data);
    be32(0, &c);  // dummy CRC.
    return c;
  };
  const GoogleString sig("\x89PNG\r\n\x1a\n", 8);
  const GoogleString ihdr = chunk("IHDR", GoogleString(13, '\0'));
  const GoogleString cabx = chunk("caBX", "JPjumbjumdc2pa-manifest-bytes");
  // A Content-Credentials XMP packet (cr: + an xpacket marker) -> carried.
  const GoogleString itxt =
      chunk("iTXt", "XML:com.adobe.xmp <?xpacket?> cr:provenance");
  // A plain, non-C2PA iTXt (no cr:/XMP marker) -> NOT carried.
  const GoogleString plain_itxt =
      chunk("iTXt", "Comment plain caption, no provenance here");
  const GoogleString iend = chunk("IEND", "");

  // Valid PNG with caBX + C2PA iTXt -> both captured, in order, as whole chunks.
  {
    const GoogleString png = sig + ihdr + cabx + itxt + iend;
    const net_instaweb::StringPieceVector got = ExtractPngC2paChunks(png);
    ASSERT_EQ(static_cast<size_t>(2), got.size());
    EXPECT_EQ(cabx, GoogleString(got[0].data(), got[0].size()));
    EXPECT_EQ(itxt, GoogleString(got[1].data(), got[1].size()));
  }
  // A non-C2PA iTXt alongside caBX is NOT carried (scoped to the manifest).
  {
    const GoogleString png = sig + ihdr + cabx + plain_itxt + iend;
    const net_instaweb::StringPieceVector got = ExtractPngC2paChunks(png);
    ASSERT_EQ(static_cast<size_t>(1), got.size());
    EXPECT_EQ(cabx, GoogleString(got[0].data(), got[0].size()));
  }
  // Two caBX chunks -> both captured in order.
  {
    const GoogleString cabx2 = chunk("caBX", "JPjumbc2pa-second-manifest-box");
    const GoogleString png = sig + ihdr + cabx + cabx2 + iend;
    const net_instaweb::StringPieceVector got = ExtractPngC2paChunks(png);
    ASSERT_EQ(static_cast<size_t>(2), got.size());
    EXPECT_EQ(cabx, GoogleString(got[0].data(), got[0].size()));
    EXPECT_EQ(cabx2, GoogleString(got[1].data(), got[1].size()));
  }
  // No carrier chunks present -> empty.
  {
    const GoogleString png = sig + ihdr + iend;
    EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
  }
  // Not a PNG (corrupt signature) -> empty (fail-safe).
  {
    GoogleString bad = sig + ihdr + cabx + iend;
    bad[1] = 'X';
    EXPECT_TRUE(ExtractPngC2paChunks(bad).empty());
  }
  // No terminating IEND -> empty, NOT a partial result (structural anomaly).
  {
    const GoogleString png = sig + ihdr + cabx;  // walks off the end, no IEND.
    EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
  }
  // Truncated chunk (a length that overruns the buffer) -> empty, NOT a partial
  // result: a structural anomaly must fail safe to Level B.
  {
    GoogleString png = sig + ihdr;
    be32(0x00FFFFFF, &png);  // caBX claims ~16MB of data...
    png.append("caBX");
    png.append("only-a-few-bytes");  // ...but the buffer ends here.
    EXPECT_TRUE(ExtractPngC2paChunks(png).empty());
  }
}

}  // namespace
