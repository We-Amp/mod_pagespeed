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

// The AVIF codec kernel: decode, encode, and animation.
//
// This is the sibling of webp_optimizer.{h,cc}. Every surface here parallels a
// WebP surface so a reviewer can diff "what WebP does" against "what AVIF does":
//   AvifConfiguration  <->  WebpConfiguration      (ScanlineWriterConfig)
//   AvifFrameWriter    <->  WebpFrameWriter        (MultipleFrameWriter)
//   AvifFrameReader    <->  (GifFrameReader)       (MultipleFrameReader)
//
// AVIF differs from WebP in one structural way on the read side: WebP exposes a
// native ScanlineReaderInterface (WebpScanlineReader) and a separate frame
// writer, whereas AVIF is animation-capable on BOTH sides, so the reader is a
// native MultipleFrameReader (like GIF) that read_image.cc wraps in a
// FrameToScanlineReaderAdapter for the still-image scanline path.

#ifndef PAGESPEED_KERNEL_IMAGE_AVIF_OPTIMIZER_H_
#define PAGESPEED_KERNEL_IMAGE_AVIF_OPTIMIZER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "avif/avif.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/image/image_frame_interface.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/scanline_interface.h"
#include "pagespeed/kernel/image/scanline_status.h"

namespace net_instaweb {
class MessageHandler;
}

namespace pagespeed {

namespace image_compression {

using net_instaweb::MessageHandler;

// Sentinel meaning "let libavif/aom pick the default" for the speed/effort knob.
const int kAvifSpeedDefault = -1;

// Encode-side configuration, mirroring WebpConfiguration. Holds a subset of
// avifEncoder settings and knows how to stamp them onto an avifEncoder.
struct AvifConfiguration : public ScanlineWriterConfig {
  // Progress hook signature mirrors WebpConfiguration::WebpProgressHook so the
  // same ConversionTimeoutHandler::Continue (image_util.cc) can drive the abort
  // of a slow AV1 encode. 'percent' is best-effort; libavif/aom do not expose a
  // fine-grained progress callback, so this is invoked at frame boundaries.
  //
  // IMPORTANT: for a STILL image the writer does not consult this hook at all.
  // libavif encodes a single-frame image entirely inside avifEncoderAddImage(),
  // so a frame-boundary check lands only after the encode has finished; acting
  // on it there would discard completed work rather than avoid it. The hook is
  // therefore consulted only for a multi-frame (animated) sequence, where a
  // frame boundary is a genuine mid-encode abort point. Stills are governed
  // instead by encode_budget_ms below, which declines BEFORE any work is done.
  using AvifProgressHook = bool (*)(int percent, void* user_data);

  AvifConfiguration();
  ~AvifConfiguration() override;

  // Stamps the settings below onto 'encoder'. NOTE: if you add a field that
  // feeds avifEncoder, update CopyTo() (mirrors WebpConfiguration's invariant).
  void CopyTo(avifEncoder* encoder) const;

  int lossless;       // 0 = lossy (default), 1 = lossless (RGB/identity path).
  int quality;        // 0 (smallest) .. 100 (best); 100 == lossless quantizer.
  int quality_alpha;  // 0 .. 100 alpha-plane quality. Default 100.
  int speed;          // avifEncoder->speed 0 (slow/best) .. 10 (fast); or
  // kAvifSpeedDefault. This is the aom cpu-used analogue and
  // the primary encode-time/CPU knob.
  int max_threads;  // encoder worker threads (avifEncoder->maxThreads).

  // Parameters related to animated AVIF (AVIS):
  int timescale;          // ticks per second. Default 1000 so a per-frame
                          // duration in ms maps 1:1 to ticks (drift-free for
                          // millisecond delays).
  int keyframe_interval;  // max frames between keyframes (0 = codec default).
  int repetition_count;   // AVIF loop count; AVIF_REPETITION_COUNT_INFINITE for
                          // an infinite loop (mirrors WebP loop_count == 0).

  AvifProgressHook progress_hook;  // If non-NULL, called at frame boundaries.
  void* user_data;  // Passed to progress_hook. Owned by the client; must remain
                    // valid until AvifFrameWriter::FinalizeWrite() completes.

  // Wall-clock budget, in milliseconds, that the caller is willing to spend on
  // a STILL-image encode. <= 0 means "no budget" (the guard is disabled).
  //
  // This is an ADMISSION test, not a deadline: PrepareImage() estimates the
  // encode cost from the image's pixel count and the configured speed, and
  // declines up front when the estimate exceeds the budget. Nothing checks the
  // clock once encoding starts -- it cannot, because AV1 exposes no mid-encode
  // abort (aom_codec_encode() has no deadline parameter and avifEncoder has no
  // cancel flag). Declining before the work is the only way to actually not do
  // the work; the alternative is to compute a result and then throw it away.
  int64 encode_budget_ms;

  // Metadata carry (mirrors JpegCompressionOptions'
  // retain_exif_data / retain_color_profile). When a retain_* flag is true AND
  // the matching blob below is non-empty, AvifFrameWriter attaches it to the
  // encoded primary image via libavif's exif/icc/xmp surfaces (avifImage->exif,
  // ->icc, ->xmp), so orientation/color/provenance survive the transcode. The
  // blobs are the caller-extracted SOURCE metadata; the codec never parses or
  // re-authors them -- a third party's XMP/C2PA provenance manifest is carried
  // verbatim or not at all, never re-signed or re-emitted. Empty blobs are a
  // no-op, so the still-image WebP-parity path (WebP carries no metadata) is
  // unaffected when the caller supplies nothing.
  bool retain_exif;
  bool retain_color_profile;
  bool retain_xmp;
  GoogleString exif_data;  // Raw EXIF payload (no "Exif\0\0" APP1 prefix).
  GoogleString icc_data;   // Raw ICC color-profile bytes.
  GoogleString xmp_data;   // Raw XMP packet bytes.
};

// Stream H helper: parse an AVIF byte buffer and copy out its embedded EXIF,
// ICC and XMP metadata (libavif exposes these on avifDecoder->image after
// parse). Used by the AVIF->AVIF recompress path so metadata carries through a
// re-encode. Returns true if the buffer parsed as AVIF (each out-blob may still
// be empty if the source had no such metadata); false on a parse failure, in
// which case the out-blobs are cleared and the caller proceeds without carry.
// Any of the out pointers may be NULL to skip that field.
bool AvifExtractMetadata(StringPiece avif_bytes, GoogleString* exif_out,
                         GoogleString* icc_out, GoogleString* xmp_out,
                         MessageHandler* handler);

// AvifFrameWriter encodes still and animated AVIF, mirroring WebpFrameWriter.
// Frames are composited onto a full-canvas RGB(A) buffer and handed to
// avifEncoderAddImage as an image sequence (so animation is a real AV1
// image-sequence, encoder-side inter-frame capable -- NOT an all-keyframe
// flatten). Full GIF-grade disposal/blend fidelity (dispose-to-restore,
// alpha blend-over) is a documented follow-up; this cut handles
// dispose-to-background and source-over-opaque, which covers the common cases.
// Correct dispose-to-background requires (and this writer therefore forces) an
// RGBA canvas for every multi-frame sequence, so a cleared region reads back as
// transparent rather than opaque black.
class AvifFrameWriter : public MultipleFrameWriter {
 public:
  explicit AvifFrameWriter(MessageHandler* handler);
  ~AvifFrameWriter() override;

  // 'config' must be a non-NULL AvifConfiguration*.
  ScanlineStatus Initialize(const void* config, GoogleString* out) override;

  // 'image_spec' must remain valid for the lifetime of the writer.
  ScanlineStatus PrepareImage(const ImageSpec* image_spec) override;

  // 'frame_spec' must remain valid while the frame is being written.
  ScanlineStatus PrepareNextFrame(const FrameSpec* frame_spec) override;

  ScanlineStatus WriteNextScanline(const void* scanline_bytes) override;

  ScanlineStatus FinalizeWrite() override;

  // Test-only observation of the encode speed actually in force after
  // PrepareImage() has derived it from the budget and the pixel count.
  // speed_for_testing() is the value the admission estimate was computed
  // against; encoder_speed_for_testing() is the value libavif will encode at.
  // The two agreeing is the invariant PrepareImage() exists to maintain, so
  // both are exposed rather than just one.
  int speed_for_testing() const { return speed_; }
  int encoder_speed_for_testing() const {
    return encoder_ == nullptr ? kAvifSpeedDefault : encoder_->speed;
  }

 private:
  // Encodes the frame currently composited in canvas_ into the encoder as the
  // next image in the sequence. No-op before the first frame or for an empty
  // frame.
  ScanlineStatus CommitFrame();

  // Releases all libavif structures.
  void FreeAvifStructs();

  // Not owned.
  const ImageSpec* image_spec_;
  GoogleString* output_image_;

  // Copied from the AvifConfiguration in Initialize().
  int lossless_;
  int quality_;
  int quality_alpha_;
  int speed_;
  int max_threads_;
  int timescale_;
  int keyframe_interval_;
  int repetition_count_;
  AvifConfiguration::AvifProgressHook progress_hook_;
  void* progress_hook_data_;
  int64 encode_budget_ms_;

  // Stream H metadata carry, copied from AvifConfiguration in Initialize() and
  // attached to the primary image in CommitFrame() (first committed frame only).
  bool retain_exif_;
  bool retain_color_profile_;
  bool retain_xmp_;
  GoogleString exif_data_;
  GoogleString icc_data_;
  GoogleString xmp_data_;

  avifEncoder* encoder_;

  // Full-image RGB(A) 8-bit composite canvas. Byte layout is packed
  // width*canvas_channels_ per row; alpha (if any) is straight (not
  // premultiplied).
  std::unique_ptr<uint8_t[]> canvas_;
  uint32_t canvas_channels_;  // 3 (RGB) or 4 (RGBA).
  bool has_alpha_;            // Whether the sequence carries an alpha channel.

  FrameSpec frame_spec_;
  FrameSpec previous_frame_spec_;
  size_px next_frame_;     // Number of frames prepared so far.
  size_px next_scanline_;  // Next scanline index within the current frame.
  bool empty_frame_;       // Current frame has a zero dimension.
  bool image_prepared_;    // PrepareImage() succeeded.
  bool should_expand_gray_to_rgb_;  // Current frame is GRAY_8 -> expand to RGB.
  bool committed_any_;              // At least one image handed to the encoder.

  AvifFrameWriter(const AvifFrameWriter&) = delete;
  AvifFrameWriter& operator=(const AvifFrameWriter&) = delete;
};

// AvifFrameReader decodes still and animated AVIF into RGB_888 / RGBA_8888
// scanlines, mirroring GifFrameReader's role (a native MultipleFrameReader).
// For a still image, read_image.cc wraps it in FrameToScanlineReaderAdapter to
// present a ScanlineReaderInterface. Each decoded frame is a fully-composited
// full-canvas image (AVIF image-sequence semantics), so frame dimensions equal
// image dimensions and disposal is always NONE.
class AvifFrameReader : public MultipleFrameReader {
 public:
  explicit AvifFrameReader(MessageHandler* handler);
  ~AvifFrameReader() override;

  ScanlineStatus Reset() override;
  ScanlineStatus Initialize() override;

  bool HasMoreFrames() const override;
  bool HasMoreScanlines() const override;
  ScanlineStatus PrepareNextFrame() override;
  ScanlineStatus ReadNextScanline(const void** out_scanline_bytes) override;
  ScanlineStatus GetFrameSpec(FrameSpec* frame_spec) const override;
  ScanlineStatus GetImageSpec(ImageSpec* image_spec) const override;

 private:
  // Decodes decoder_->image (the current frame) into rgb_pixels_.
  ScanlineStatus DecodeCurrentFrameToRgb();

  void FreeAvifStructs();

  avifDecoder* decoder_;

  ImageSpec image_spec_;
  FrameSpec frame_spec_;
  bool has_alpha_;
  PixelFormat pixel_format_;

  // Decoded RGB(A) 8-bit pixels of the CURRENT frame, packed
  // width*channels per row. Valid until the next PrepareNextFrame()/Reset().
  std::unique_ptr<uint8_t[]> rgb_pixels_;
  size_t frame_bytes_per_row_;

  size_px frames_prepared_;  // Number of frames advanced past.
  size_px row_;              // Next scanline index within the current frame.
  bool image_parsed_;        // avifDecoderParse() succeeded.

  AvifFrameReader(const AvifFrameReader&) = delete;
  AvifFrameReader& operator=(const AvifFrameReader&) = delete;
};

}  // namespace image_compression

}  // namespace pagespeed

#endif  // PAGESPEED_KERNEL_IMAGE_AVIF_OPTIMIZER_H_
