// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Image type detection and utility functions for PageSpeed 2.0.
// Ported from mod_pagespeed's pagespeed/kernel/image/image_util.cc

#include "lib/image/image_util.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "src/webp/decode.h"

namespace {

const char kInvalidImageFormat[] = "Invalid image format";
const char kInvalidPixelFormat[] = "Invalid pixel format";

// Magic number of the images.
const char kPngHeader[] = "\x89PNG\r\n\x1a\n";
constexpr size_t kPngHeaderLength = 8;
const char kGifHeader[] = "GIF8";
constexpr size_t kGifHeaderLength = 4;

// char to int *without sign extension*.
inline int CharToInt(char c) {
  auto uc = static_cast<uint8_t>(c);
  return static_cast<int>(uc);
}

}  // namespace

namespace net_instaweb {

const char* ImageTypeToMimeTypeString(ImageType image_type) {
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
    case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
    case IMAGE_WEBP_ANIMATED:
      return "image/webp";
    case IMAGE_AVIF:
      return "image/avif";
      // No default so compiler will complain if any enum is not processed.
  }
  return kInvalidImageFormat;
}

const char* ImageTypeToString(ImageType image_type) {
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
    case IMAGE_WEBP_LOSSLESS_OR_ALPHA:
      return "IMAGE_WEBP_LOSSLESS_OR_ALPHA";
    case IMAGE_WEBP_ANIMATED:
      return "IMAGE_WEBP_ANIMATED";
    case IMAGE_AVIF:
      return "IMAGE_AVIF";
      // No default so compiler will complain if any enum is not processed.
  }
  return kInvalidImageFormat;
}

ImageType ComputeImageType(std::string_view buf) {
  // Image classification based on buffer contents gakked from leptonica,
  // but based on well-documented headers (see Wikipedia etc.).
  // Note that we can be fooled if we're passed random binary data;
  // we make the call based on as few as two bytes (JPEG).
  ImageType image_type = IMAGE_UNKNOWN;
  if (buf.size() >= 8) {
    // Note that gcc rightly complains about constant ranges with the
    // negative char constants unless we cast.
    switch (CharToInt(buf[0])) {
      case 0xff:
        // Either jpeg or jpeg2
        // (the latter we don't handle yet, and don't bother looking for).
        if (CharToInt(buf[1]) == 0xd8) {
          image_type = IMAGE_JPEG;
        }
        break;
      case 0x89:
        // Possible png.
        if (std::string_view(
                buf.data(),  // NOLINT(bugprone-suspicious-stringview-data-usage)
                kPngHeaderLength) ==
            std::string_view(kPngHeader, kPngHeaderLength)) {
          image_type = IMAGE_PNG;
        }
        break;
      case 'G':
        // Possible gif.
        if ((std::string_view(
                 buf.data(),  // NOLINT(bugprone-suspicious-stringview-data-usage)
                 kGifHeaderLength) ==
             std::string_view(kGifHeader, kGifHeaderLength)) &&
            (buf[kGifHeaderLength] == '7' || buf[kGifHeaderLength] == '9') &&
            buf[kGifHeaderLength + 1] == 'a') {
          image_type = IMAGE_GIF;
        }
        break;
      case 0x00:
        // Possible AVIF (ISO BMFF container).
        // bytes 4-7 = "ftyp", bytes 8-11 in {"avif","avis","mif1"}
        if (buf.size() >= 12 && std::string_view(buf.data() + 4, 4) == "ftyp") {
          std::string_view brand(buf.data() + 8, 4);
          if (brand == "avif" || brand == "avis" || brand == "mif1") {
            image_type = IMAGE_AVIF;
          }
        }
        break;
      case 'R':
        // Possible Webp
        // Detailed explanation on parsing webp format is available at
        // http://code.google.com/speed/webp/docs/riff_container.html
        WebPBitstreamFeatures features;
        if (WebPGetFeatures(  // NOLINT(bugprone-suspicious-stringview-data-usage)
                reinterpret_cast<const uint8_t*>(buf.data()), buf.length(),
                &features) == VP8_STATUS_OK) {
          if (features.has_animation) {
            image_type = IMAGE_WEBP_ANIMATED;
          } else if (features.format == 2 || features.has_alpha) {
            image_type = IMAGE_WEBP_LOSSLESS_OR_ALPHA;
          } else if (features.format == 1) {
            image_type = IMAGE_WEBP;
          }
        }
        break;
      default:
        break;
    }
  }
  return image_type;
}

// C2PA / Content-Credentials provenance detector. Conservative,
// signature-only byte scan over the raw original bytes. Two carrier classes are
// distinguished because the worker can carry only one through optimization:
//   * JUMBF/box form -- FourCCs "jumb"/"jumd"/"c2pa" (JPEG APP11 + generic
//     containers) and the PNG C2PA chunk type "caBX". For JPEG->JPEG this lives in
//     APP11, which the codec carries verbatim under preserve_c2pa.
//   * XMP form -- the "cr:" Content-Credentials namespace inside an XMP packet. For
//     JPEG this lives in APP1 (shared with EXIF), not reliably carried, so the
//     worker skip-not-strips it. AVIF/WebP/resize/PNG carry neither form and also
//     skip-not-strip when a manifest is present.
// This NEVER decodes, validates, or re-emits a manifest.
namespace {
bool ContainsToken(std::string_view haystack, std::string_view needle) {
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

bool HasJumbfC2paTokens(std::string_view bytes) {
  static const char* const kC2paMarkers[] = {"jumb", "jumd", "c2pa", "caBX"};
  for (const char* marker : kC2paMarkers) {
    if (ContainsToken(bytes, marker)) {
      return true;
    }
  }
  return false;
}
}  // namespace

bool ImageHasJumbfC2pa(std::string_view bytes) {
  if (bytes.size() < 12) {
    return false;
  }
  return HasJumbfC2paTokens(bytes);
}

bool ImageHasXmpC2pa(std::string_view bytes) {
  if (bytes.size() < 12) {
    return false;
  }
  return ContainsToken(bytes, "cr:") &&
         (ContainsToken(bytes, "xpacket") ||
          ContainsToken(bytes, "adobe.com/xap") ||
          ContainsToken(bytes, "contentauth"));
}

bool ImageHasC2paManifest(std::string_view bytes) {
  if (bytes.size() < 12) {
    return false;
  }
  return ImageHasJumbfC2pa(bytes) || ImageHasXmpC2pa(bytes);
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

std::vector<std::string_view> ExtractPngC2paChunks(std::string_view bytes) {
  // Capture the verbatim bytes of the C2PA carrier chunk
  // ("caBX") and the linked XMP chunk ("iTXt"). Whole chunks (length + type +
  // data + ORIGINAL CRC) are returned as views into `bytes` so the carry path
  // splices them unmodified; the original CRC is carried as-is, never recomputed
  // (the chunk data and its CRC travel together, so the CRC stays self-consistent
  // even though the chunk is relocated). The bytes are never decoded or
  // re-authored. Returns empty on any structural anomaly, on which
  // the caller MUST fall back to the detect-and-skip behavior (serve the
  // original) rather than emit a stripped image.
  std::vector<std::string_view> chunks;
  static const unsigned char kPngSig[8] = {0x89, 'P',  'N',  'G',
                                           0x0D, 0x0A, 0x1A, 0x0A};
  const size_t size = bytes.size();
  if (size < 8 + 12) {  // signature + at least one minimal chunk header + CRC.
    return chunks;
  }
  const char* data = bytes.data();
  if (memcmp(data, kPngSig, 8) != 0) {
    return chunks;  // Not a PNG; caller falls back to detect-and-skip.
  }
  size_t pos = 8;
  bool iend_seen = false;
  while (pos + 8 <= size) {  // need length(4) + type(4) at minimum.
    const uint32_t data_len = ReadBE32(data + pos);
    const size_t chunk_total = static_cast<size_t>(12) + data_len;
    // Guard against overflow and buffer overrun. On any structural anomaly,
    // discard ANY partial result and return empty so the caller falls back to
    // detect-and-skip (serve the original) rather than carrying a partial
    // manifest.
    if (data_len > size || pos + chunk_total > size) {
      return std::vector<std::string_view>();
    }
    const char* type = data + pos + 4;
    const std::string_view whole(data + pos, chunk_total);
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
    // safe to empty so the caller serves the original (detect-and-skip).
    return std::vector<std::string_view>();
  }
  return chunks;
}

}  // namespace net_instaweb

namespace pagespeed::image_compression {

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

ScanlineWriterConfig::~ScanlineWriterConfig() = default;

}  // namespace pagespeed::image_compression
