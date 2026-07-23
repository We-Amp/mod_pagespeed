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

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "external/libwebp/src/webp/decode.h"
#include "pagespeed/kernel/base/countdown_timer.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/http/image_types.pb.h"

namespace pagespeed {

namespace {

static const char kInvalidImageFormat[] = "Invalid image format";
static const char kInvalidPixelFormat[] = "Invalid pixel format";

// Magic number of the images.
const char kPngHeader[] = "\x89PNG\r\n\x1a\n";
const size_t kPngHeaderLength = arraysize(kPngHeader) - 1;
const char kGifHeader[] = "GIF8";
const size_t kGifHeaderLength = arraysize(kGifHeader) - 1;

// char to int *without sign extension*.
inline int CharToInt(char c) {
  uint8 uc = static_cast<uint8>(c);
  return static_cast<int>(uc);
}

}  // namespace

namespace image_compression {

const char* ImageFormatToMimeTypeString(ImageFormat image_type) {
  switch (image_type) {
    case IMAGE_UNKNOWN:
      return "image/unknown";
    case IMAGE_JPEG:
      return "image/jpeg";
    case IMAGE_PNG:
      return "image/png";
    case IMAGE_GIF:
      return "image/gif";
    case IMAGE_WEBP:
      return "image/webp";
    case IMAGE_AVIF:
      return "image/avif";
      // No default so compiler will complain if any enum is not processed.
  }
  return kInvalidImageFormat;
}

const char* ImageFormatToString(ImageFormat image_type) {
  switch (image_type) {
    case IMAGE_UNKNOWN:
      return "IMAGE_UNKNOWN";
    case IMAGE_JPEG:
      return "IMAGE_JPEG";
    case IMAGE_PNG:
      return "IMAGE_PNG";
    case IMAGE_GIF:
      return "IMAGE_GIF";
    case IMAGE_WEBP:
      return "IMAGE_WEBP";
    case IMAGE_AVIF:
      return "IMAGE_AVIF";
      // No default so compiler will complain if any enum is not processed.
  }
  return kInvalidImageFormat;
}

const char* GetPixelFormatString(PixelFormat pixel_format) {
  switch (pixel_format) {
    case UNSUPPORTED:
      return "UNSUPPORTED";
    case RGB_888:
      return "RGB_888";
    case RGBA_8888:
      return "RGBA_8888";
    case GRAY_8:
      return "GRAY_8";
      // No default so compiler will complain if any enum is not processed.
  }
  return kInvalidPixelFormat;
}

size_t GetBytesPerPixel(PixelFormat pixel_format) {
  switch (pixel_format) {
    case UNSUPPORTED:
      return 0;
    case RGB_888:
      return 3;
    case RGBA_8888:
      return 4;
    case GRAY_8:
      return 1;
      // No default so compiler will complain if any enum is not processed.
  }
  return 0;
}

net_instaweb::ImageType ComputeImageType(const StringPiece& buf) {
  // Image classification based on buffer contents gakked from leptonica,
  // but based on well-documented headers (see Wikipedia etc.).
  // Note that we can be fooled if we're passed random binary data;
  // we make the call based on as few as two bytes (JPEG).
  net_instaweb::ImageType image_type = net_instaweb::IMAGE_UNKNOWN;

  // AVIF (and the HEIF family) are ISO-BMFF containers: a big-endian 4-byte box
  // size, then the box type "ftyp" at offset 4, a 4-byte major brand at offset
  // 8, a 4-byte minor version, and zero or more 4-byte compatible brands from
  // offset 16 to the end of the ftyp box. Because the file begins with 0x00 (the
  // high byte of the box size), it never matches the first-byte switch below, so
  // we probe for it explicitly first. The probe is exact -- an "ftyp" box whose
  // major or compatible brand is an AVIF brand -- so JPEG/PNG/GIF/WebP inputs
  // are never misclassified.
  if (buf.size() >= 12 && memcmp(buf.data() + 4, "ftyp", 4) == 0) {
    // Length of the ftyp box; clamp to the bytes we actually have so the
    // compatible-brand scan can never read past the buffer.
    size_t box_size = (static_cast<size_t>(CharToInt(buf[0])) << 24) |
                      (static_cast<size_t>(CharToInt(buf[1])) << 16) |
                      (static_cast<size_t>(CharToInt(buf[2])) << 8) |
                      static_cast<size_t>(CharToInt(buf[3]));
    if (box_size < 12 || box_size > buf.size()) {
      box_size = buf.size();
    }
    const char* major_brand = buf.data() + 8;
    bool is_avif = (memcmp(major_brand, "avif", 4) == 0);
    bool is_avis = (memcmp(major_brand, "avis", 4) == 0);
    for (size_t off = 16; off + 4 <= box_size; off += 4) {
      if (memcmp(buf.data() + off, "avis", 4) == 0) {
        is_avis = true;
      } else if (memcmp(buf.data() + off, "avif", 4) == 0) {
        is_avif = true;
      }
    }
    if (is_avis) {
      image_type = net_instaweb::IMAGE_AVIF_ANIMATED;
    } else if (is_avif) {
      // TODO(avif): deliberate M2 follow-up -- probe the decoded features here
      // (the libavif decode path exists) to promote alpha/lossless AVIF to
      // IMAGE_AVIF_LOSSLESS_OR_ALPHA, mirroring the WebPGetFeatures() check
      // below. In M1 all still AVIF classifies as the base IMAGE_AVIF; this
      // sniff stays byte-cheap (no decode) by design.
      image_type = net_instaweb::IMAGE_AVIF;
    }
    return image_type;
  }

  if (buf.size() >= 8) {
    // Note that gcc rightly complains about constant ranges with the
    // negative char constants unless we cast.
    switch (CharToInt(buf[0])) {
      case 0xff:
        // Either jpeg or jpeg2
        // (the latter we don't handle yet, and don't bother looking for).
        if (CharToInt(buf[1]) == 0xd8) {
          image_type = net_instaweb::IMAGE_JPEG;
        }
        break;
      case 0x89:
        // Possible png.
        if (StringPiece(
                buf.data(),
                kPngHeaderLength) ==  // NOLINT(bugprone-suspicious-stringview-data-usage)
            StringPiece(kPngHeader, kPngHeaderLength)) {
          image_type = net_instaweb::IMAGE_PNG;
        }
        break;
      case 'G':
        // Possible gif.
        if ((StringPiece(
                 buf.data(),
                 kGifHeaderLength) ==  // NOLINT(bugprone-suspicious-stringview-data-usage)
             StringPiece(kGifHeader, kGifHeaderLength)) &&
            (buf[kGifHeaderLength] == '7' || buf[kGifHeaderLength] == '9') &&
            buf[kGifHeaderLength + 1] == 'a') {
          image_type = net_instaweb::IMAGE_GIF;
        }
        break;
      case 'R':
        // Possible Webp
        // Detailed explanation on parsing webp format is available at
        // http://code.google.com/speed/webp/docs/riff_container.html
        WebPBitstreamFeatures features;
        if (WebPGetFeatures(reinterpret_cast<const uint8*>(buf.data()),
                            buf.length(), &features) == VP8_STATUS_OK) {
          if (features.has_animation) {
            image_type = net_instaweb::IMAGE_WEBP_ANIMATED;
          } else if (features.format == 2 || features.has_alpha) {
            image_type = net_instaweb::IMAGE_WEBP_LOSSLESS_OR_ALPHA;
          } else if (features.format == 1) {
            image_type = net_instaweb::IMAGE_WEBP;
          }
        }
        break;
      default:
        break;
    }
  }
  return image_type;
}

bool ConversionTimeoutHandler::Continue(int percent, void* user_data) {
  ConversionTimeoutHandler* timeout_handler =
      static_cast<ConversionTimeoutHandler*>(user_data);
  if (timeout_handler != nullptr &&
      !timeout_handler->countdown_timer_.HaveTimeLeft()) {
    // We include the output_->empty() check after HaveTimeLeft()
    // for testing, in case there's a callback that writes to
    // output_ invoked at a time that triggers a timeout.
    if (!timeout_handler->output_->empty()) {
      return true;
    }
    PS_LOG_WARN(timeout_handler->handler_, "Image conversion timed out.");
    timeout_handler->was_timed_out_ = true;
    return false;
  }
  return true;
}

ScanlineWriterConfig::~ScanlineWriterConfig() {}

// the design record C2PA / Content-Credentials provenance detector. Conservative, signature-only
// substring scan over the raw original bytes. It intentionally does NOT decode the
// container, validate a signature, or re-serialize anything. Two carrier classes are distinguished because
// the JPEG codec can carry only one of them through a recompress:
//   * JUMBF/box form -- FourCCs "jumb"/"jumd"/"c2pa" (JPEG APP11 + generic containers)
//     and the PNG C2PA chunk type "caBX". For JPEG this lives in APP11, which the codec
//     carries verbatim under preserve_c2pa.
//   * XMP form -- the "cr:" Content-Credentials namespace inside an XMP packet. For JPEG
//     this lives in APP1 (shared with EXIF), so the codec carries it only when EXIF/APP1
//     is retained; otherwise the rewrite gate must skip-not-strip (see image.cc).
namespace {
bool ContainsToken(StringPiece haystack, StringPiece needle) {
  if (needle.empty() || haystack.size() < needle.size()) {
    return false;
  }
  const size_t limit = haystack.size() - needle.size();
  for (size_t i = 0; i <= limit; ++i) {
    if (memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) {
      return true;
    }
  }
  return false;
}

// JUMBF/box-form C2PA: FourCCs that live in the JPEG APP11 segment or the PNG caBX
// chunk -- the form the codec carry (jpeg_optimizer.cc) preserves verbatim.
bool HasJumbfC2pa(StringPiece bytes) {
  static const char* const kC2paMarkers[] = {"jumb", "jumd", "c2pa", "caBX"};
  for (const char* marker : kC2paMarkers) {
    if (ContainsToken(bytes, marker)) {
      return true;
    }
  }
  return false;
}

// the design record Stream H: explicit ISO-BMFF / AVIF C2PA carrier detection. A C2PA
// manifest in an ISO-BMFF container (AVIF, HEIF, MP4) is stored either as a
// JUMBF superbox ("jumb", already caught by HasJumbfC2pa above) or inside a
// top-level "uuid" box tagged with the C2PA manifest UUID
// d8fec3d6-1b0e-483c-9297-5828877ec481. HasJumbfC2pa catches the JUMBF form
// regardless of container; this adds the raw-UUID form and is gated on the file
// actually being ISO-BMFF (an "ftyp" box at offset 4) so the 16-byte signature
// scan cannot false-positive on unrelated binary data. Signature-only, no box
// parse or re-emit, mirroring the other detectors here.
bool HasIsoBmffC2pa(StringPiece bytes) {
  // ISO-BMFF gate: 4-byte big-endian box size, then "ftyp" at offset 4.
  if (bytes.size() < 12 || memcmp(bytes.data() + 4, "ftyp", 4) != 0) {
    return false;
  }
  static const unsigned char kC2paUuid[16] = {
      0xd8, 0xfe, 0xc3, 0xd6, 0x1b, 0x0e, 0x48, 0x3c,
      0x92, 0x97, 0x58, 0x28, 0x87, 0x7e, 0xc4, 0x81};
  return ContainsToken(
      bytes, StringPiece(reinterpret_cast<const char*>(kC2paUuid), 16));
}
}  // namespace

bool ImageHasXmpC2pa(StringPiece bytes) {
  // Require enough bytes to plausibly carry a manifest token.
  if (bytes.size() < 12) {
    return false;
  }
  // The "cr:" namespace prefix only counts when it appears alongside an XMP packet
  // marker, to avoid false positives on unrelated binary data.
  return ContainsToken(bytes, "cr:") &&
         (ContainsToken(bytes, "xpacket") ||
          ContainsToken(bytes, "adobe.com/xap") ||
          ContainsToken(bytes, "contentauth"));
}

bool ImageHasC2paManifest(StringPiece bytes) {
  // Require enough bytes to plausibly carry a manifest token.
  if (bytes.size() < 12) {
    return false;
  }
  return HasJumbfC2pa(bytes) || HasIsoBmffC2pa(bytes) || ImageHasXmpC2pa(bytes);
}

namespace {

// Reads a big-endian unsigned 32-bit value from `p` (4 bytes must be available).
uint32_t ReadBE32(const char* p) {
  return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
         static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

}  // namespace

net_instaweb::StringPieceVector ExtractPngC2paChunks(StringPiece bytes) {
  // the design record Level A: capture the verbatim bytes of the C2PA carrier chunk
  // ("caBX") and the linked XMP chunk ("iTXt"). Whole chunks (length + type +
  // data + ORIGINAL CRC) are returned as views into `bytes` so the carry path
  // splices them unmodified; the original CRC is carried as-is, never recomputed
  // (the chunk data and its CRC travel together, so the CRC stays self-consistent
  // even though the chunk is relocated). The bytes are never decoded or
  // re-authored. Returns empty on any structural anomaly, on which
  // the caller MUST fall back to Level B (detect-and-skip) rather than emit a
  // stripped image.
  net_instaweb::StringPieceVector chunks;
  static const unsigned char kPngSig[8] = {0x89, 'P',  'N',  'G',
                                           0x0D, 0x0A, 0x1A, 0x0A};
  const size_t size = bytes.size();
  if (size < 8 + 12) {  // signature + at least one minimal chunk header + CRC.
    return chunks;
  }
  const char* data = bytes.data();
  if (memcmp(data, kPngSig, 8) != 0) {
    return chunks;  // Not a PNG; caller falls back to Level B.
  }
  size_t pos = 8;
  bool iend_seen = false;
  while (pos + 8 <= size) {  // need length(4) + type(4) at minimum.
    const uint32_t data_len = ReadBE32(data + pos);
    const size_t chunk_total = static_cast<size_t>(12) + data_len;
    // Guard against overflow and buffer overrun. On any structural anomaly,
    // discard ANY partial result and return empty so the caller falls back to
    // Level B (serve the original) rather than carrying a partial manifest.
    if (data_len > size || pos + chunk_total > size) {
      return net_instaweb::StringPieceVector();
    }
    const char* type = data + pos + 4;
    const StringPiece whole(data + pos, chunk_total);
    // The C2PA box chunk ("caBX") is always a carrier; an "iTXt" is carried only
    // when it actually holds the Content-Credentials XMP packet ("cr:" + an XMP
    // marker), never an arbitrary text iTXt chunk.
    const bool is_carrier =
        (memcmp(type, "caBX", 4) == 0) ||
        (memcmp(type, "iTXt", 4) == 0 && ImageHasXmpC2pa(whole));
    if (is_carrier) {
      chunks.push_back(whole);
    }
    if (memcmp(type, "IEND", 4) == 0) {
      iend_seen = true;
      break;  // IEND terminates the stream.
    }
    pos += chunk_total;
  }
  if (!iend_seen) {
    // Ran off the end without a terminating IEND: a structural anomaly. Fail
    // safe to empty so the caller serves the original (Level B).
    return net_instaweb::StringPieceVector();
  }
  return chunks;
}

}  // namespace image_compression

}  // namespace pagespeed
