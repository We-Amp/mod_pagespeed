// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Image type detection and utility functions for PageSpeed 2.0.
// Ported from mod_pagespeed's pagespeed/kernel/image/image_util.h

#ifndef PAGESPEED_LIB_IMAGE_IMAGE_UTIL_H_
#define PAGESPEED_LIB_IMAGE_IMAGE_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

// Image type enumeration - matches the proto enum from mod_pagespeed.
// This is used for identifying image formats based on magic bytes.
enum ImageType : std::uint8_t {
  IMAGE_UNKNOWN = 0,
  IMAGE_JPEG = 1,
  IMAGE_PNG = 2,
  IMAGE_GIF = 3,
  IMAGE_WEBP = 4,
  IMAGE_WEBP_LOSSLESS_OR_ALPHA = 5,  // webp that is lossless or transparent
  IMAGE_WEBP_ANIMATED = 6,
  IMAGE_AVIF = 7,
};

// Returns the MIME-type string corresponding to the given ImageType.
const char* ImageTypeToMimeTypeString(ImageType image_type);

// Returns a string representation of the given ImageType enum value.
const char* ImageTypeToString(ImageType image_type);

// Returns format of the image by inspecting magic numbers (certain values at
// certain bytes) in the file content. This method is fast, but if a random
// binary file happens to have the magic numbers, it will incorrectly report
// a format for the file. The problem will be corrected when the binary file
// is decoded.
ImageType ComputeImageType(std::string_view buf);

// Conservative, signature-only detection of a C2PA / Content-Credentials
// provenance manifest in raw image bytes. These NEVER decode, validate, or re-emit
// the manifest -- they only scan for well-known marker/box signatures so the worker
// can serve a manifest-bearing image unchanged (skip-not-strip) instead of
// transcoding/resizing it (which would drop the manifest). A false positive only
// costs a skipped optimization (fail-safe). Intended to run once per image.
//
// ImageHasJumbfC2pa: the JUMBF/box form (JPEG APP11 + PNG caBX) -- the only form the
//   JPEG->JPEG codec carry can preserve.
// ImageHasXmpC2pa: the XMP form ("cr:" in an XMP packet); for JPEG it lives in APP1
//   (shared with EXIF) and is not reliably carried, so the worker skip-not-strips it.
// ImageHasC2paManifest: either form present.
bool ImageHasJumbfC2pa(std::string_view bytes);
bool ImageHasXmpC2pa(std::string_view bytes);
bool ImageHasC2paManifest(std::string_view bytes);

// Carry-through, PNG only. Walks a PNG chunk stream (8-byte
// signature -> length-prefixed chunks) and returns the VERBATIM byte ranges
// (views into `bytes`) of every C2PA carrier chunk ("caBX") and linked XMP chunk
// ("iTXt"), in original file order. Each range is the WHOLE chunk (4-byte length,
// 4-byte type, data, 4-byte original CRC carried as-is). The returned views alias
// `bytes`, so the original buffer must outlive them; the carry path splices these
// unmodified bytes into the recompressed PNG, never decoding or re-authoring the
// manifest. Returns empty on any structural anomaly, an unrecognized
// format, or when no carrier is found -- on which the caller MUST fall back to the
// detect-and-skip behavior (serve the original) rather than emit a stripped image.
//
// (JPEG needs no equivalent: jpeg_optimizer.cc already carries APP11/JUMBF through a
// recompress via libjpeg's marker API whenever preserve_c2pa is set.)
std::vector<std::string_view> ExtractPngC2paChunks(std::string_view bytes);

}  // namespace net_instaweb

namespace pagespeed {
namespace image_compression {

// Sometimes image readers or writers may need to tweak their behavior
// away from what is in the spec to emulate or adapt to the
// idiosyncratic behavior of real renderers in the wild. This enum
// allows those classes to parametrize that quirky behavior.
enum QuirksMode : std::uint8_t {
  QUIRKS_NONE = 0,
  QUIRKS_CHROME,
  QUIRKS_FIREFOX
};

enum ImageFormat : std::uint8_t {
  IMAGE_UNKNOWN,
  IMAGE_JPEG,
  IMAGE_PNG,
  IMAGE_GIF,
  IMAGE_WEBP
};

enum PixelFormat : std::uint8_t {
  UNSUPPORTED,  // Not supported.
  RGB_888,      // RGB triplets, 24 bits per pixel.
  RGBA_8888,    // RGB triplet plus alpha channel, 32 bits per pixel.
  GRAY_8        // Grayscale, 8 bits per pixel.
};

enum RgbaChannels : std::uint8_t {
  RGBA_RED = 0,
  RGBA_GREEN,
  RGBA_BLUE,
  RGBA_ALPHA,

  RGBA_NUM_CHANNELS
};

enum PreferredLibwebpLevel : std::uint8_t {
  WEBP_NONE = 0,
  WEBP_LOSSY,
  WEBP_LOSSLESS,
  WEBP_ANIMATED
};

constexpr uint8_t kAlphaOpaque = 255;
constexpr uint8_t kAlphaTransparent = 0;
using PixelRgbaChannels = uint8_t[RGBA_NUM_CHANNELS];

// Packs four uint8_ts into a single uint32_t in the high-to-low order given.
inline uint32_t PackHiToLo(uint8_t i3, uint8_t i2, uint8_t i1, uint8_t i0) {
  return (static_cast<uint32_t>(i3) << 24) | (i2 << 16) | (i1 << 8) | (i0);
}

// Packs the given A, R, G, B values into a single ARGB uint32.
inline uint32_t PackAsArgb(uint8_t alpha, uint8_t red, uint8_t green,
                           uint8_t blue) {
  return PackHiToLo(alpha, red, green, blue);
}

// Packs a pixel's color channel data in RGBA format to a single
// uint32_t in ARGB format.
inline uint32_t RgbaToPackedArgb(const PixelRgbaChannels rgba) {
  return PackAsArgb(rgba[RGBA_ALPHA], rgba[RGBA_RED], rgba[RGBA_GREEN],
                    rgba[RGBA_BLUE]);
}

// Packs a pixel's color channel data in RGB format to a single
// uint32_t in ARGB format.
inline uint32_t RgbToPackedArgb(const PixelRgbaChannels rgba) {
  return PackAsArgb(kAlphaOpaque, rgba[RGBA_RED], rgba[RGBA_GREEN],
                    rgba[RGBA_BLUE]);
}

// Converts a pixel's grayscale data into a single uint32_t in ARGB format.
inline uint32_t GrayscaleToPackedArgb(const uint8_t luminance) {
  return PackAsArgb(kAlphaOpaque, luminance, luminance, luminance);
}

// Sizes that can be measured in units of pixels: width, height,
// number of frames (a third dimension of the image), and indices into
// the same.
using size_px = uint32_t;

// Returns the MIME-type string corresponding to the given ImageFormat.
const char* ImageFormatToMimeTypeString(ImageFormat image_type);

// Returns a string representation of the given ImageFormat.
const char* ImageFormatToString(ImageFormat image_type);

// Returns a string representation of the given PixelFormat.
const char* GetPixelFormatString(PixelFormat pixel_format);

// Returns the number of bytes needed to encode each pixel in the
// given format.
size_t GetBytesPerPixel(PixelFormat pixel_format);

// Base class for scanline writer configuration.
struct ScanlineWriterConfig {
  virtual ~ScanlineWriterConfig();
};

}  // namespace image_compression
}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_IMAGE_UTIL_H_
