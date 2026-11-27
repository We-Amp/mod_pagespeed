// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// EXIF orientation handling for the image pipeline (issue #1005).
//
// Production strips metadata from optimized images, so a portrait photo's
// EXIF Orientation tag (values 2-8) would be dropped while its pixels stay
// stored rotated/mirrored -- the served image renders sideways. WebP and
// AVIF outputs cannot carry the tag at all. The fix is to bake the
// orientation into the pixels at the decode boundary: every consumer of the
// scanline API receives upright pixels and display (post-rotation)
// dimensions, so no downstream path needs to know the tag ever existed.
//
// This header provides:
//  * ReadJpegExifOrientation() -- a bounds-checked byte scan of a JPEG
//    stream for the EXIF Orientation tag (never decodes pixels).
//  * ExifOrientedScanlineReader -- a ScanlineReaderInterface wrapper that
//    serves orientation-corrected scanlines (identity passthrough for
//    orientation 1 / absent).
//  * ApplyExifOrientationToRaster() -- raster transform for callers that
//    already hold a full interleaved pixel buffer (the JPEG->JPEG lossy
//    recompress path).
//  * BuildExifOrientationApp1() -- a minimal, privacy-preserving APP1
//    segment carrying ONLY the Orientation tag, for JPEG->JPEG paths that
//    cannot bake pixels (lossless coefficient copy, oversized rasters) and
//    must keep the image rendering upright via an accurate tag instead.

#ifndef PAGESPEED_LIB_IMAGE_EXIF_ORIENTATION_H_
#define PAGESPEED_LIB_IMAGE_EXIF_ORIENTATION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "lib/base/basictypes.h"
#include "lib/image/image_util.h"
#include "lib/image/scanline_interface.h"
#include "lib/image/scanline_status.h"

namespace pagespeed {

class MessageHandler;

namespace image_compression {

using net_instaweb::ScanlineReaderInterface;
using net_instaweb::ScanlineStatus;

// Default ceiling for buffering a full stored raster in order to bake
// orientation. Matches the worker's kMaxDecodedPixels safety limit for
// decoded pixel buffers. Images above the limit are not orientation-baked;
// callers fall back to serving the original bytes or to an accurate minimal
// tag, both of which keep the image rendering upright.
inline constexpr size_t kMaxOrientationBakeBytes = 50 * 1024 * 1024;

// Returns the active bake ceiling: kMaxOrientationBakeBytes unless
// overridden via SetOrientationBakeByteLimitForTesting().
size_t OrientationBakeByteLimit();

// Test-only override of the bake ceiling, so the oversized-raster fallback
// paths are reachable with small fixtures. Pass 0 to restore the default.
// Not thread-safe; production code never calls this.
void SetOrientationBakeByteLimitForTesting(size_t limit);

// Byte size of the minimal EXIF APP1 payload produced by
// BuildExifOrientationApp1(): "Exif\0\0" + TIFF header + one-entry IFD0.
inline constexpr size_t kExifOrientationApp1Size = 32;

// Scans a JPEG stream for the EXIF Orientation tag (0x0112 in IFD0 of the
// APP1 "Exif" segment). Returns the tag value (1-8), or 1 when the tag is
// absent, out of range, or the stream is malformed / not a JPEG. Never
// decodes pixel data; every access is bounds-checked.
uint8_t ReadJpegExifOrientation(const void* data, size_t length);

// True for orientations that swap display width/height (90/270-degree
// rotations, plain or mirrored).
constexpr bool ExifOrientationSwapsDimensions(uint8_t orientation) {
  return orientation >= 5 && orientation <= 8;
}

// Scans a WebP (RIFF) stream for the EXIF chunk and returns its Orientation
// tag value (1-8), or 1 when there is none. The chunk is honoured only when
// the VP8X header's EXIF flag is set, as the container specification
// requires; a chunk without the flag is ignored. Same contract as
// ReadJpegExifOrientation: never decodes pixels, every access bounds-checked,
// anything malformed yields 1.
uint8_t ReadWebpExifOrientation(const void* data, size_t length);

// Scans a PNG stream for an "eXIf" chunk and returns its Orientation tag
// value (1-8), or 1 when there is none. Same contract as above. The legacy
// "Raw profile type exif" text-chunk form is not read.
uint8_t ReadPngExifOrientation(const void* data, size_t length);

// Detects the container from its signature (JPEG, WebP or PNG) and calls the
// matching reader above; returns 1 for any other input.
uint8_t ReadExifOrientation(const void* data, size_t length);

// Fills |out| (kExifOrientationApp1Size bytes) with a minimal APP1 marker
// payload -- "Exif\0\0" + little-endian TIFF header + a single-entry IFD0
// holding only the Orientation tag -- suitable for jpeg_write_marker().
// Deliberately carries no other metadata (no GPS, no timestamps). |out| is a
// plain array so callers inside libjpeg setjmp/longjmp regions stay free of
// non-trivially-destructible locals.
void BuildExifOrientationApp1(uint8_t orientation,
                              uint8_t out[kExifOrientationApp1Size]);

// Maps an upright (display) pixel coordinate to its stored-raster source
// coordinate for the given orientation. |stored_width| / |stored_height| are
// the raster dimensions as stored in the file. For orientations 5-8 the
// upright image has swapped dimensions, so 0 <= x < stored_height and
// 0 <= y < stored_width; for 1-4, 0 <= x < stored_width and
// 0 <= y < stored_height.
void MapUprightToStored(uint8_t orientation, size_t x, size_t y,
                        size_t stored_width, size_t stored_height,
                        size_t* stored_x, size_t* stored_y);

// Rewrites a full interleaved raster (|components| bytes per pixel) into its
// upright form. |dst| must hold width * height * components bytes and must
// not alias |src|. For orientations 5-8 the destination raster has swapped
// dimensions (height x width). Returns false for an invalid orientation or
// zero-sized input. Orientation 1 is a plain copy (callers should skip the
// call entirely in that case).
bool ApplyExifOrientationToRaster(uint8_t orientation, const uint8_t* src,
                                  size_t width, size_t height,
                                  size_t components, uint8_t* dst);

// ScanlineReaderInterface decorator that bakes the EXIF orientation of a
// JPEG, WebP or PNG stream into the pixels it serves. For orientation 1 (or
// no tag) it is a pure passthrough with no buffering. For orientations 2-8 it reports
// display dimensions from Initialize() onward, then buffers the full decoded
// raster at the first ReadNextScanline() call and serves transformed rows --
// one full-raster buffer plus one row buffer, the minimum a rotation needs.
// Rasters larger than kMaxOrientationBakeBytes fail at the first read (not
// at Initialize(), so header-only consumers such as dimension probes keep
// working); pixel consumers then fall back to their serve-original paths,
// which keep the tag and render upright.
class ExifOrientedScanlineReader : public ScanlineReaderInterface {
 public:
  // Takes ownership of |inner|.
  ExifOrientedScanlineReader(ScanlineReaderInterface* inner,
                             MessageHandler* handler);
  ~ExifOrientedScanlineReader() override;

  bool Reset() override;
  ScanlineStatus InitializeWithStatus(const void* image_buffer,
                                      size_t buffer_length) override;
  ScanlineStatus ReadNextScanlineWithStatus(void** out_scanline_bytes) override;

  size_t GetBytesPerScanline() override;
  bool HasMoreScanLines() override;
  size_t GetImageHeight() override;
  size_t GetImageWidth() override;
  PixelFormat GetPixelFormat() override { return inner_->GetPixelFormat(); }
  bool IsProgressive() override { return inner_->IsProgressive(); }

 private:
  // Decodes every scanline of the inner reader into stored_. Called at the
  // first ReadNextScanline() when orientation_ != 1.
  ScanlineStatus BufferStoredRaster();

  std::unique_ptr<ScanlineReaderInterface> inner_;
  MessageHandler* message_handler_;
  uint8_t orientation_ = 1;
  size_t stored_width_ = 0;  // Raster dimensions as stored in the file.
  size_t stored_height_ = 0;
  size_t out_width_ = 0;  // Display dimensions (swapped for 5-8).
  size_t out_height_ = 0;
  size_t components_ = 0;  // Bytes per pixel.
  size_t row_ = 0;         // Next output row to serve.
  bool buffered_ = false;
  std::string stored_;   // Full stored raster (orientation != 1 only).
  std::string out_row_;  // Reusable transformed-row buffer.

  DISALLOW_COPY_AND_ASSIGN(ExifOrientedScanlineReader);
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_IMAGE_EXIF_ORIENTATION_H_
