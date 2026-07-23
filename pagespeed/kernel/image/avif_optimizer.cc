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

#include "pagespeed/kernel/image/avif_optimizer.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "base/logging.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/scanline_utils.h"

namespace pagespeed {

namespace image_compression {

using net_instaweb::MessageHandler;

namespace {

// Decode-side resource limits (Stream B untrusted-input hardening). AVIF is
// decoded from attacker-influenceable origin bytes, so these caps are enforced
// by avifDecoderParse() BEFORE any plane allocation -- a decode bomb (huge
// declared canvas / extreme frame count) is refused up front, never after a
// giant malloc. These mirror the spirit of the WebP/GIF dimension guards.
const uint32_t kAvifMaxDimension = 16384;         // per-side pixel cap.
const uint32_t kAvifMaxImagePixels = 100u << 20;  // 100 Mpx total canvas cap.
const uint32_t kAvifMaxFrameCount = 1024;         // AVIS frame-count cap.

// Estimated STILL-image AVIF encode cost in milliseconds per megapixel,
// indexed by avifEncoder->speed (the aom cpu-used analogue), 0..10.
//
// ---------------------------------------------------------------------------
// BIAS: these are the LOW end of what was measured, and that is deliberate.
// ---------------------------------------------------------------------------
// The two ways of being wrong are not symmetric:
//
//   * Estimating too HIGH is harmful and silent. The image is declined before
//     any work happens, so AVIF simply disappears for a whole size class that
//     used to convert successfully -- with nothing but an INFO log to say so.
//     A single over-estimated entry can drop, say, every 2560x1600 hero image
//     back to WebP/JPEG on a default configuration.
//   * Estimating too LOW is benign. The image is admitted and the encode runs
//     to completion, which is exactly the behaviour that existed before this
//     guard was added. The budget may be exceeded, but the caller still gets
//     its optimized image, and the overrun is counted
//     (image_avif_conversion_*_overruns) rather than hidden.
//
// So when in doubt, UNDER-estimate. Every entry below is therefore the
// smallest ms/Mpx observed across the size sweep, not a mean and not a padded
// worst case. That also matches where the guard actually bites: cost per
// megapixel FALLS as images get larger (fixed per-encode overhead amortizes
// over more pixels), and the cutoff only ever matters for large images -- so
// the low end of the range is the right end to calibrate from.
//
// THREADING ASSUMPTION, load-bearing: every number is a SINGLE-THREADED
// measurement, because AvifConfiguration::max_threads defaults to 1 and no
// caller on the serving path ever raises it. A multi-threaded measurement
// would understate real serving cost by the parallel speedup -- re-calibrate
// ONLY from a max_threads=1 run of avif_roundtrip_main (which defaults to 1
// thread and prints the thread count with every result).
//
// METHOD (identical for both tables): avif_roundtrip_main, built -c opt, on a
// synthetic photographic source (smooth gradient + mid-frequency texture +
// fine noise), quality 60, max_threads=1, Linux. Speeds 6-9 swept at
// 0.48/1.09/2.07/3.69 Mpx; speeds 0-5 and 10 at 2.07/3.69 Mpx (the low speeds
// cost minutes per encode and the per-Mpx figure is flat by then anyway).
// Measured 2026-07-19.
//
// PER-ARCHITECTURE, because the difference is far too large to paper over: the
// same sweep runs roughly 2.2x to 2.8x FASTER per megapixel on 64-bit ARM than
// on x86-64. Using one table for both would over-estimate by ~2.5x on ARM --
// the harmful direction above -- and would refuse ordinary images there that
// convert comfortably. (Note this is the opposite of the intuition that ARM is
// the slower target: on the class of ARM core measured, it is not.)
#if defined(__aarch64__) || defined(_M_ARM64)

// 64-bit ARM. Basis: Linux/aarch64, single-threaded, on an Apple Silicon
// performance core (contemporary desktop/laptop class), quality 60, 2026-07-19.
const int kAvifEncodeMsPerMpxBySpeed[] = {
    15055,  // speed 0   (measured 15055-15206)
    6764,   // speed 1   (measured 6764-6895)
    5164,   // speed 2   (measured 5164-5364)
    3143,   // speed 3   (measured 3143-3146)
    812,    // speed 4   (measured 812-832)
    707,    // speed 5   (measured 707-731)
    200,    // speed 6   (measured 200-245; the production default)
    105,    // speed 7   (measured 105-131)
    31,     // speed 8   (measured 31-35; used under a tight budget)
    26,     // speed 9   (measured 26-28)
    26,     // speed 10  (measured 26-38)
};

#else

// x86-64, and the fallback for any architecture not named above.
//
// Basis: Linux/x86-64, single-threaded, on a contemporary many-core desktop /
// workstation-class CPU, quality 60, 2026-07-19.
//
// An UNKNOWN architecture lands here too. That is the conservative choice only
// in one direction, so it is worth being explicit: these are the HIGHER of the
// two tables, so on an unknown-but-fast core the guard may refuse images that
// would have fit. If a third architecture ever matters, measure it and give it
// its own arm rather than letting it inherit this one.
const int kAvifEncodeMsPerMpxBySpeed[] = {
    29665,  // speed 0   (measured 29665-30969)
    14688,  // speed 1   (measured 14688-14723)
    11486,  // speed 2   (measured 11486-11653)
    7212,   // speed 3   (measured 7212-7384)
    1839,   // speed 4   (measured 1839-1855)
    1626,   // speed 5   (measured 1626-1688)
    445,    // speed 6   (measured 445-599; the production default)
    242,    // speed 7   (measured 242-311)
    86,     // speed 8   (measured 86-99; used under a tight budget)
    72,     // speed 9   (measured 72-81)
    73,     // speed 10  (measured 73-74)
};

#endif

const int kAvifMaxKnownSpeed = (sizeof(kAvifEncodeMsPerMpxBySpeed) /
                                sizeof(kAvifEncodeMsPerMpxBySpeed[0])) -
                               1;

// Estimated single-threaded cost, in ms, of encoding 'pixels' pixels at
// 'speed'. 'speed' must be a known in-table speed.
int64 EstimatedEncodeMs(int speed, int64 pixels) {
  return (pixels * kAvifEncodeMsPerMpxBySpeed[speed]) / 1000000;
}

// Returns the SLOWEST -- i.e. lowest-numbered, best quality-per-byte -- speed
// at or above 'floor_speed' whose estimated cost for 'pixels' fits
// 'budget_ms', or kAvifSpeedNoneFits if not even the fastest known speed fits.
//
// Scanning upward from 'floor_speed' and returning the first fit yields the
// LOWEST fitting index by construction, which is what "slowest that fits"
// means. That stays correct even though the calibration table is not perfectly
// monotone (on x86-64 speed 10 measures marginally slower than speed 9).
//
// 'floor_speed' is the speed the caller configured, and it is a floor rather
// than a starting guess on purpose: a large budget must never make the encoder
// slower/higher-quality than the caller asked for. Otherwise a generous
// AvifTimeoutMs on a small image would silently select speed 0 and spend
// literal seconds per image. The budget can only ever make the encode FASTER
// than configured, never slower.
const int kAvifSpeedNoneFits = -1;

// The cheapest per-megapixel speed at or above 'floor_speed'. Used only to
// name the right speed in the decline log; the table is not perfectly
// monotone, so this is not simply kAvifMaxKnownSpeed.
int CheapestSpeedAtOrAbove(int floor_speed) {
  int cheapest = floor_speed;
  for (int speed = floor_speed; speed <= kAvifMaxKnownSpeed; ++speed) {
    if (kAvifEncodeMsPerMpxBySpeed[speed] <
        kAvifEncodeMsPerMpxBySpeed[cheapest]) {
      cheapest = speed;
    }
  }
  return cheapest;
}

int SlowestSpeedWithinBudget(int floor_speed, int64 pixels, int64 budget_ms) {
  for (int speed = floor_speed; speed <= kAvifMaxKnownSpeed; ++speed) {
    if (EstimatedEncodeMs(speed, pixels) <= budget_ms) {
      return speed;
    }
  }
  return kAvifSpeedNoneFits;
}

// libavif clamps quality to [0,100]; keep our clamp identical so a stray config
// value can never trip an avifEncoder assert.
int ClampQuality(int q) {
  if (q < 0) return 0;
  if (q > 100) return 100;
  return q;
}

}  // namespace

////////// AvifConfiguration

AvifConfiguration::AvifConfiguration()
    : lossless(0),
      quality(60),
      quality_alpha(100),
      speed(6),
      max_threads(1),
      timescale(1000),
      keyframe_interval(0),
      repetition_count(AVIF_REPETITION_COUNT_INFINITE),
      progress_hook(nullptr),
      user_data(nullptr),
      encode_budget_ms(0),
      retain_exif(false),
      retain_color_profile(false),
      retain_xmp(false) {}

AvifConfiguration::~AvifConfiguration() {}

void AvifConfiguration::CopyTo(avifEncoder* encoder) const {
  if (lossless) {
    // Lossless AVIF requires the identity matrix (RGB coded directly) plus the
    // top quality. The matching yuvFormat/matrixCoefficients are set on the
    // avifImage in AvifFrameWriter::CommitFrame().
    encoder->quality = AVIF_QUALITY_LOSSLESS;
    encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
  } else {
    encoder->quality = ClampQuality(quality);
    encoder->qualityAlpha = ClampQuality(quality_alpha);
  }
  encoder->speed = speed;
  encoder->maxThreads = max_threads < 1 ? 1 : max_threads;
  encoder->timescale = timescale > 0 ? timescale : 1000;
  encoder->keyframeInterval = keyframe_interval;
  encoder->repetitionCount = repetition_count;
}

////////// AvifFrameWriter

AvifFrameWriter::AvifFrameWriter(MessageHandler* handler)
    : MultipleFrameWriter(handler),
      image_spec_(nullptr),
      output_image_(nullptr),
      lossless_(0),
      quality_(60),
      quality_alpha_(100),
      speed_(6),
      max_threads_(1),
      timescale_(1000),
      keyframe_interval_(0),
      repetition_count_(AVIF_REPETITION_COUNT_INFINITE),
      progress_hook_(nullptr),
      progress_hook_data_(nullptr),
      encode_budget_ms_(0),
      retain_exif_(false),
      retain_color_profile_(false),
      retain_xmp_(false),
      encoder_(nullptr),
      canvas_channels_(0),
      has_alpha_(false),
      next_frame_(0),
      next_scanline_(0),
      empty_frame_(false),
      image_prepared_(false),
      should_expand_gray_to_rgb_(false),
      committed_any_(false) {}

AvifFrameWriter::~AvifFrameWriter() { FreeAvifStructs(); }

void AvifFrameWriter::FreeAvifStructs() {
  if (encoder_ != nullptr) {
    avifEncoderDestroy(encoder_);
    encoder_ = nullptr;
  }
  canvas_.reset();
}

ScanlineStatus AvifFrameWriter::Initialize(const void* config,
                                           GoogleString* out) {
  FreeAvifStructs();
  if (config == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "missing AvifConfiguration*");
  }

  const AvifConfiguration* avif_config =
      static_cast<const AvifConfiguration*>(config);

  encoder_ = avifEncoderCreate();
  if (encoder_ == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_MEMORY_ERROR, FRAME_AVIFWRITER,
                            "avifEncoderCreate()");
  }

  lossless_ = avif_config->lossless;
  quality_ = avif_config->quality;
  quality_alpha_ = avif_config->quality_alpha;
  speed_ = avif_config->speed;
  max_threads_ = avif_config->max_threads;
  timescale_ = avif_config->timescale;
  keyframe_interval_ = avif_config->keyframe_interval;
  repetition_count_ = avif_config->repetition_count;
  progress_hook_ = avif_config->progress_hook;
  progress_hook_data_ = avif_config->user_data;
  encode_budget_ms_ = avif_config->encode_budget_ms;

  // Stream H: copy metadata-carry intent + blobs. Blobs are only attached later
  // when the matching retain flag is set AND the blob is non-empty.
  retain_exif_ = avif_config->retain_exif;
  retain_color_profile_ = avif_config->retain_color_profile;
  retain_xmp_ = avif_config->retain_xmp;
  exif_data_ = avif_config->exif_data;
  icc_data_ = avif_config->icc_data;
  xmp_data_ = avif_config->xmp_data;

  avif_config->CopyTo(encoder_);

  output_image_ = out;
  image_prepared_ = false;
  next_frame_ = 0;
  committed_any_ = false;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameWriter::PrepareImage(const ImageSpec* image_spec) {
  DVLOG(1) << image_spec->ToString();
  if (image_prepared_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "image already prepared");
  }
  if ((image_spec->height < 1) || (image_spec->width < 1)) {
    return PS_LOGGED_STATUS(
        PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_UNSUPPORTED_FEATURE,
        FRAME_AVIFWRITER, "each image dimension must be at least 1");
  }
  if ((image_spec->height > kAvifMaxDimension) ||
      (image_spec->width > kAvifMaxDimension)) {
    return PS_LOGGED_STATUS(
        PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_UNSUPPORTED_FEATURE,
        FRAME_AVIFWRITER, "each image dimension must be at most %u",
        kAvifMaxDimension);
  }
  // Mirror the decoder's imageSizeLimit total-pixel cap on the encode side:
  // libavif/aom expose no mid-encode progress callback, so a huge still image
  // would pin a worker uninterruptibly. Compute the product in size_t so it
  // cannot overflow the 32-bit dimensions.
  if (static_cast<size_t>(image_spec->width) *
          static_cast<size_t>(image_spec->height) >
      kAvifMaxImagePixels) {
    return PS_LOGGED_STATUS(
        PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_UNSUPPORTED_FEATURE,
        FRAME_AVIFWRITER, "total image pixels must be at most %u",
        kAvifMaxImagePixels);
  }

  // Budget-derived admission test for STILL images, alongside the absolute
  // pixel cap above. The cap answers "is this image insane?"; this answers
  // "will this image fit the deadline the caller actually configured?".
  //
  // It has to happen HERE, before a single scanline is encoded, because for a
  // single-frame image libavif does the whole encode inside one
  // avifEncoderAddImage() call. There is no mid-encode abort to fall back on:
  // aom_codec_encode() takes no deadline and avifEncoder carries no cancel
  // flag. So the only two options are "decline before starting" and "finish,
  // then throw the result away" -- and the second one spends the CPU anyway.
  //
  // Restricted to stills on purpose: a multi-frame sequence hands libavif one
  // frame at a time, so its frame-boundary progress hook IS a real abort point
  // and CommitFrame() still uses it.
  //
  // The speed is DERIVED here rather than fixed by the caller, because it is
  // the only place both halves of the estimate are known: the caller picks a
  // budget without knowing the image, and the pixel count only arrives with
  // the ImageSpec. We pick the slowest (best quality-per-byte) speed at or
  // above the configured one whose estimated cost fits the budget, and decline
  // only when not even the fastest known speed fits.
  //
  // That derivation is what makes the budget MONOTONE. The previous design
  // chose the speed in image.cc by stepping on a fixed timeout threshold, so
  // raising AvifTimeoutMs across that threshold dropped the encoder from
  // speed 8 to speed 6 and admitted roughly 5x FEWER megapixels than a
  // marginally tighter budget did -- the opposite of what raising a timeout
  // should do. With the speed derived from the budget, a larger budget can
  // only ever admit more (the set of fitting speeds grows) and can only ever
  // select an equal or better-quality speed (the minimum of a growing set).
  //
  // CRITICAL INVARIANT: the estimate and the encode must never disagree about
  // which speed is in play. Initialize() copied speed_ from the same
  // AvifConfiguration that CopyTo() stamped onto encoder_->speed, so on entry
  // they agree; if we change speed_ we MUST re-stamp encoder_->speed too. This
  // is safe to do here: nothing has been handed to libavif yet (the first
  // avifEncoderAddImage() happens in CommitFrame(), which cannot run before
  // PrepareImage() sets image_prepared_), and the encoder is only recreated by
  // Initialize().
  //
  // An out-of-table configured speed -- notably kAvifSpeedDefault, "let libavif
  // decide" -- is not estimable, so the guard stands down rather than guess.
  // We deliberately do NOT map it onto the production default for estimation:
  // kAvifSpeedDefault is an explicit instruction to delegate the choice to
  // libavif, whose pick we do not know, so estimating against a speed libavif
  // may not use would break exactly the invariant above. Standing down just
  // restores the previous always-admit behaviour. No production caller sets it.
  if (encode_budget_ms_ > 0 && image_spec->num_frames <= 1 && speed_ >= 0 &&
      speed_ <= kAvifMaxKnownSpeed) {
    const int64 pixels = static_cast<int64>(image_spec->width) *
                         static_cast<int64>(image_spec->height);
    const int derived_speed =
        SlowestSpeedWithinBudget(speed_, pixels, encode_budget_ms_);
    if (derived_speed == kAvifSpeedNoneFits) {
      // Name the CHEAPEST reachable speed, which is not necessarily the
      // highest-numbered one: on x86-64 speed 10 measures marginally slower
      // than speed 9. Naming the wrong one would send whoever reads this log
      // line to the wrong table entry.
      const int cheapest_speed = CheapestSpeedAtOrAbove(speed_);
      return PS_LOGGED_STATUS(
          PS_LOG_INFO, message_handler(), SCANLINE_STATUS_TIMEOUT_ERROR,
          FRAME_AVIFWRITER,
          "still-image encode declined up front: %ld pixels do not fit the "
          "%ld ms budget even at the cheapest available speed %d "
          "(estimated %ld ms, single-threaded)",
          static_cast<long>(pixels),             // NOLINT(runtime/int)
          static_cast<long>(encode_budget_ms_),  // NOLINT(runtime/int)
          cheapest_speed,
          static_cast<long>(  // NOLINT(runtime/int)
              EstimatedEncodeMs(cheapest_speed, pixels)));
    }
    if (derived_speed != speed_) {
      speed_ = derived_speed;
      // Re-stamp, so the encoder really runs at the speed we just estimated.
      // Without this the guard would admit on a speed-8 estimate and then
      // encode at speed 6 -- a worse defect than the one it replaces.
      if (encoder_ != nullptr) {
        encoder_->speed = speed_;
      }
    }
  }

  image_spec_ = image_spec;
  next_frame_ = 0;
  next_scanline_ = 0;
  image_prepared_ = true;
  canvas_.reset();
  canvas_channels_ = 0;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameWriter::PrepareNextFrame(const FrameSpec* frame_spec) {
  if (!image_prepared_) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "PrepareNextFrame: image not prepared");
  }
  if (next_frame_ >= image_spec_->num_frames) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "PrepareNextFrame: no next frame");
  }

  // Flush the frame we just finished compositing into the encoder.
  ScanlineStatus status = CommitFrame();
  if (!status.Success()) {
    return status;
  }

  if (!image_spec_->CanContainFrame(*frame_spec)) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "PrepareNextFrame: frame does not fit in image:\n"
                            "%s\n%s",
                            image_spec_->ToString().c_str(),
                            frame_spec->ToString().c_str());
  }

  if (next_frame_ == 0) {
    previous_frame_spec_.width = image_spec_->width;
    previous_frame_spec_.height = image_spec_->height;
    previous_frame_spec_.top = 0;
    previous_frame_spec_.left = 0;
    previous_frame_spec_.disposal = FrameSpec::DISPOSAL_NONE;
  } else {
    previous_frame_spec_ = frame_spec_;
  }
  ++next_frame_;
  frame_spec_ = *frame_spec;

  // Resolve the pixel format of the incoming scanlines, expanding GRAY_8 to RGB
  // exactly as WebpFrameWriter does (AVIF, like WebP, has no native grayscale
  // surface on this path).
  should_expand_gray_to_rgb_ = false;
  bool frame_has_alpha = false;
  switch (frame_spec_.pixel_format) {
    case RGB_888:
      frame_has_alpha = false;
      break;
    case RGBA_8888:
      frame_has_alpha = true;
      break;
    case GRAY_8:
      frame_has_alpha = false;
      should_expand_gray_to_rgb_ = true;
      break;
    default:
      return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                              SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFWRITER,
                              "unknown pixel format: %d",
                              frame_spec_.pixel_format);
  }

  // The canvas alpha channel is fixed by the first frame and used for the whole
  // sequence (AVIF sequences carry a single alpha-presence for the track).
  // Multi-frame sequences ALWAYS get an RGBA canvas, even if frame 1 is opaque:
  // a later frame may carry alpha (which an RGB canvas would silently drop),
  // and dispose-to-background's memset(0) must read back as transparent, not
  // opaque black -- both require the alpha plane. RGB_888 frames composited
  // into the RGBA canvas get an opaque alpha fill in WriteNextScanline.
  if (next_frame_ == 1) {
    has_alpha_ = frame_has_alpha || (image_spec_->num_frames > 1);
    canvas_channels_ = has_alpha_ ? 4 : 3;
    const size_t canvas_bytes = static_cast<size_t>(image_spec_->width) *
                                image_spec_->height * canvas_channels_;
    canvas_.reset(new uint8_t[canvas_bytes]);
    // Clear to transparent-black (or opaque-black for RGB) so an offset first
    // frame or a background-disposed region reads back cleanly.
    memset(canvas_.get(), 0, canvas_bytes);
    if (!has_alpha_) {
      // No alpha plane: nothing more to clear; RGB already zeroed.
    }
  } else if (next_frame_ > 1) {
    // Apply the previous frame's disposal before compositing this frame.
    // dispose-to-background clears the previous frame's rect; dispose-none and
    // (approximated) dispose-restore keep the canvas. Blend-over is treated as
    // source-copy in this cut (documented simplification, Stream C follow-up).
    if (previous_frame_spec_.disposal == FrameSpec::DISPOSAL_BACKGROUND ||
        previous_frame_spec_.disposal == FrameSpec::DISPOSAL_UNKNOWN) {
      for (size_px y = 0; y < previous_frame_spec_.height; ++y) {
        uint8_t* row =
            canvas_.get() + (static_cast<size_t>(previous_frame_spec_.top + y) *
                                 image_spec_->width +
                             previous_frame_spec_.left) *
                                canvas_channels_;
        memset(
            row, 0,
            static_cast<size_t>(previous_frame_spec_.width) * canvas_channels_);
      }
    }
  }

  empty_frame_ = (frame_spec_.width < 1) || (frame_spec_.height < 1);
  next_scanline_ = 0;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameWriter::WriteNextScanline(const void* scanline_bytes) {
  if (next_scanline_ >= frame_spec_.height) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "WriteNextScanline: too many scanlines");
  }
  if (!empty_frame_) {
    const uint8_t* in = reinterpret_cast<const uint8_t*>(scanline_bytes);
    const size_px dst_row = frame_spec_.top + next_scanline_;
    uint8_t* out =
        canvas_.get() +
        (static_cast<size_t>(dst_row) * image_spec_->width + frame_spec_.left) *
            canvas_channels_;

    if (should_expand_gray_to_rgb_) {
      for (size_px x = 0; x < frame_spec_.width; ++x) {
        const uint8_t luminance = in[x];
        out[0] = luminance;
        out[1] = luminance;
        out[2] = luminance;
        if (canvas_channels_ == 4) {
          out[3] = kAlphaOpaque;
        }
        out += canvas_channels_;
      }
    } else if (frame_spec_.pixel_format == RGBA_8888) {
      if (canvas_channels_ == 4) {
        memcpy(out, in, static_cast<size_t>(frame_spec_.width) * 4);
      } else {
        // RGBA input into an RGB canvas: drop alpha.
        for (size_px x = 0; x < frame_spec_.width; ++x) {
          out[0] = in[0];
          out[1] = in[1];
          out[2] = in[2];
          out += 3;
          in += 4;
        }
      }
    } else {  // RGB_888 input.
      if (canvas_channels_ == 3) {
        memcpy(out, in, static_cast<size_t>(frame_spec_.width) * 3);
      } else {
        for (size_px x = 0; x < frame_spec_.width; ++x) {
          out[0] = in[0];
          out[1] = in[1];
          out[2] = in[2];
          out[3] = kAlphaOpaque;
          out += 4;
          in += 3;
        }
      }
    }
  }

  ++next_scanline_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameWriter::CommitFrame() {
  if (next_frame_ < 1 || empty_frame_ || canvas_ == nullptr) {
    return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
  }
  if (next_scanline_ < frame_spec_.height) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFWRITER,
                            "CommitFrame: not all scanlines written");
  }

  const bool animated = image_spec_->num_frames > 1;
  const avifPixelFormat yuv_format =
      lossless_ ? AVIF_PIXEL_FORMAT_YUV444 : AVIF_PIXEL_FORMAT_YUV420;

  avifImage* image =
      avifImageCreate(image_spec_->width, image_spec_->height, 8, yuv_format);
  if (image == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_MEMORY_ERROR, FRAME_AVIFWRITER,
                            "avifImageCreate()");
  }
  if (lossless_) {
    // Code RGB directly (no color loss) with full-range identity coefficients.
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
    image->yuvRange = AVIF_RANGE_FULL;
  }

  // Stream H: attach carried metadata to the PRIMARY image only (the first
  // committed frame). libavif serializes these into the file-level meta box, so
  // they must ride on the first image handed to the encoder. Attachment is
  // best-effort: a metadata allocation failure logs but does NOT fail the encode
  // (the pixels are still valid; parity floor is "don't strip", not "must
  // carry"). Empty blobs / cleared retain flags are a no-op.
  if (!committed_any_) {
    if (retain_exif_ && !exif_data_.empty()) {
      const avifResult r = avifImageSetMetadataExif(
          image, reinterpret_cast<const uint8_t*>(exif_data_.data()),
          exif_data_.size());
      if (r != AVIF_RESULT_OK) {
        PS_LOG_INFO(message_handler(), "avifImageSetMetadataExif(): %s",
                    avifResultToString(r));
      }
    }
    if (retain_xmp_ && !xmp_data_.empty()) {
      const avifResult r = avifImageSetMetadataXMP(
          image, reinterpret_cast<const uint8_t*>(xmp_data_.data()),
          xmp_data_.size());
      if (r != AVIF_RESULT_OK) {
        PS_LOG_INFO(message_handler(), "avifImageSetMetadataXMP(): %s",
                    avifResultToString(r));
      }
    }
    if (retain_color_profile_ && !icc_data_.empty()) {
      const avifResult r = avifImageSetProfileICC(
          image, reinterpret_cast<const uint8_t*>(icc_data_.data()),
          icc_data_.size());
      if (r != AVIF_RESULT_OK) {
        PS_LOG_INFO(message_handler(), "avifImageSetProfileICC(): %s",
                    avifResultToString(r));
      }
    }
  }

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = has_alpha_ ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  rgb.depth = 8;
  rgb.pixels = canvas_.get();
  rgb.rowBytes = image_spec_->width * canvas_channels_;

  avifResult res = avifImageRGBToYUV(image, &rgb);
  if (res != AVIF_RESULT_OK) {
    avifImageDestroy(image);
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFWRITER,
                            "avifImageRGBToYUV(): %s", avifResultToString(res));
  }

  uint64_t duration_ticks = 1;
  if (animated) {
    // timescale_ ticks per second; frame duration is in ms -> ticks scale by
    // timescale/1000. With the default timescale 1000 this is drift-free.
    duration_ticks = static_cast<uint64_t>(frame_spec_.duration_ms) *
                     static_cast<uint64_t>(timescale_) / 1000;
    if (duration_ticks < 1) {
      duration_ticks = 1;
    }
  }
  const avifAddImageFlags flags =
      animated ? AVIF_ADD_IMAGE_FLAG_NONE : AVIF_ADD_IMAGE_FLAG_SINGLE;

  res = avifEncoderAddImage(encoder_, image, duration_ticks, flags);
  avifImageDestroy(image);
  if (res != AVIF_RESULT_OK) {
    return PS_LOGGED_STATUS(
        PS_LOG_ERROR, message_handler(), SCANLINE_STATUS_INTERNAL_ERROR,
        FRAME_AVIFWRITER, "avifEncoderAddImage(): %s", avifResultToString(res));
  }
  committed_any_ = true;

  // Best-effort progress/timeout hook at the frame boundary -- ANIMATION ONLY.
  //
  // For a multi-frame sequence this is a real abort point: the frames after
  // this one have not been encoded yet, so refusing here genuinely saves the
  // remaining work.
  //
  // For a STILL image it is not. libavif encodes a single-frame image in its
  // entirety inside the avifEncoderAddImage() call just above, so by the time
  // control reaches this line the encode is already paid for. Consulting the
  // hook here would only let a slow-but-SUCCESSFUL encode be discarded after
  // the fact -- burning the CPU and getting nothing for it. libavif/aom expose
  // no finer callback and no cancel flag, so there is no way to make the still
  // case abort mid-encode; the still-image deadline is enforced instead as an
  // up-front admission test in PrepareImage() (see encode_budget_ms).
  if (progress_hook_ != nullptr && animated) {
    if (!progress_hook_(100, progress_hook_data_)) {
      return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                              SCANLINE_STATUS_TIMEOUT_ERROR, FRAME_AVIFWRITER,
                              "AVIF encode aborted by progress hook (timeout)");
    }
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameWriter::FinalizeWrite() {
  ScanlineStatus status = CommitFrame();
  if (!status.Success()) {
    return status;
  }
  if (!committed_any_) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFWRITER,
                            "FinalizeWrite: no frames written");
  }

  avifRWData output = AVIF_DATA_EMPTY;
  avifResult res = avifEncoderFinish(encoder_, &output);
  if (res != AVIF_RESULT_OK) {
    avifRWDataFree(&output);
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFWRITER,
                            "avifEncoderFinish(): %s", avifResultToString(res));
  }
  output_image_->append(reinterpret_cast<const char*>(output.data),
                        output.size);
  avifRWDataFree(&output);
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

////////// AvifFrameReader

AvifFrameReader::AvifFrameReader(MessageHandler* handler)
    : MultipleFrameReader(handler),
      decoder_(nullptr),
      has_alpha_(false),
      pixel_format_(UNSUPPORTED),
      frame_bytes_per_row_(0),
      frames_prepared_(0),
      row_(0),
      image_parsed_(false) {
  image_spec_.Reset();
  frame_spec_.Reset();
}

AvifFrameReader::~AvifFrameReader() { FreeAvifStructs(); }

void AvifFrameReader::FreeAvifStructs() {
  if (decoder_ != nullptr) {
    avifDecoderDestroy(decoder_);
    decoder_ = nullptr;
  }
  rgb_pixels_.reset();
}

ScanlineStatus AvifFrameReader::Reset() {
  FreeAvifStructs();
  image_spec_.Reset();
  frame_spec_.Reset();
  has_alpha_ = false;
  pixel_format_ = UNSUPPORTED;
  frame_bytes_per_row_ = 0;
  frames_prepared_ = 0;
  row_ = 0;
  image_parsed_ = false;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameReader::Initialize() {
  Reset();

  decoder_ = avifDecoderCreate();
  if (decoder_ == nullptr) {
    return PS_LOGGED_STATUS(PS_LOG_ERROR, message_handler(),
                            SCANLINE_STATUS_MEMORY_ERROR, FRAME_AVIFREADER,
                            "avifDecoderCreate()");
  }

  // Untrusted-input hardening: cap what avifDecoderParse() will allocate. These
  // are enforced during Parse, before any plane allocation.
  decoder_->imageSizeLimit = kAvifMaxImagePixels;
  decoder_->imageDimensionLimit = kAvifMaxDimension;
  decoder_->imageCountLimit = kAvifMaxFrameCount;
  decoder_->maxThreads = 1;
  decoder_->strictFlags = AVIF_STRICT_DISABLED;

  avifResult res = avifDecoderSetIOMemory(
      decoder_, reinterpret_cast<const uint8_t*>(image_buffer_),
      buffer_length_);
  if (res != AVIF_RESULT_OK) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFREADER,
                            "avifDecoderSetIOMemory(): %s",
                            avifResultToString(res));
  }

  res = avifDecoderParse(decoder_);
  if (res != AVIF_RESULT_OK) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_PARSE_ERROR, FRAME_AVIFREADER,
                            "avifDecoderParse(): %s", avifResultToString(res));
  }

  has_alpha_ = decoder_->alphaPresent != 0;
  pixel_format_ = has_alpha_ ? RGBA_8888 : RGB_888;

  image_spec_.Reset();
  image_spec_.width = decoder_->image->width;
  image_spec_.height = decoder_->image->height;
  image_spec_.num_frames = decoder_->imageCount > 0 ? decoder_->imageCount : 1;
  // repetitionCount < 0 means infinite; the ImageSpec loop_count is unsigned, so
  // map infinite to 0 (WebP's "loop forever" convention).
  image_spec_.loop_count =
      decoder_->repetitionCount < 0
          ? 0
          : static_cast<unsigned int>(decoder_->repetitionCount);

  frames_prepared_ = 0;
  row_ = 0;
  image_parsed_ = true;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

bool AvifFrameReader::HasMoreFrames() const {
  return image_parsed_ && (frames_prepared_ < image_spec_.num_frames);
}

bool AvifFrameReader::HasMoreScanlines() const {
  return (frames_prepared_ > 0) && (row_ < frame_spec_.height);
}

ScanlineStatus AvifFrameReader::DecodeCurrentFrameToRgb() {
  const avifImage* image = decoder_->image;

  frame_bytes_per_row_ =
      static_cast<size_t>(image->width) * (has_alpha_ ? 4 : 3);
  rgb_pixels_.reset(new uint8_t[frame_bytes_per_row_ * image->height]);

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = has_alpha_ ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
  // Always down-convert to 8-bit here. libavif performs a range-correct
  // bit-depth reduction (NOT a raw truncation) for 10/12-bit input, so
  // highlights are preserved. HDR (PQ/HLG) tone-mapping is a documented
  // follow-up (Stream H); the result is a valid, non-clipped 8-bit image.
  rgb.depth = 8;
  rgb.pixels = rgb_pixels_.get();
  rgb.rowBytes = frame_bytes_per_row_;

  avifResult res = avifImageYUVToRGB(image, &rgb);
  if (res != AVIF_RESULT_OK) {
    rgb_pixels_.reset();
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFREADER,
                            "avifImageYUVToRGB(): %s", avifResultToString(res));
  }
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameReader::PrepareNextFrame() {
  if (!HasMoreFrames()) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFREADER,
                            "PrepareNextFrame: no next frame");
  }

  avifResult res = avifDecoderNextImage(decoder_);
  if (res != AVIF_RESULT_OK) {
    return PS_LOGGED_STATUS(PS_LOG_INFO, message_handler(),
                            SCANLINE_STATUS_INTERNAL_ERROR, FRAME_AVIFREADER,
                            "avifDecoderNextImage(): %s",
                            avifResultToString(res));
  }

  ScanlineStatus status = DecodeCurrentFrameToRgb();
  if (!status.Success()) {
    return status;
  }

  // Each decoded AVIF frame is a fully-composited full-canvas image, so the
  // frame dimensions equal the image dimensions and disposal is NONE.
  frame_spec_.Reset();
  frame_spec_.width = decoder_->image->width;
  frame_spec_.height = decoder_->image->height;
  frame_spec_.top = 0;
  frame_spec_.left = 0;
  frame_spec_.pixel_format = pixel_format_;
  frame_spec_.disposal = FrameSpec::DISPOSAL_NONE;
  frame_spec_.hint_progressive = false;
  // imageTiming.duration is in seconds; convert to ms.
  frame_spec_.duration_ms = static_cast<size_t>(
      std::llround(decoder_->imageTiming.duration * 1000.0));

  ++frames_prepared_;
  row_ = 0;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameReader::ReadNextScanline(
    const void** out_scanline_bytes) {
  if (!image_parsed_ || !HasMoreScanlines()) {
    return PS_LOGGED_STATUS(PS_LOG_DFATAL, message_handler(),
                            SCANLINE_STATUS_INVOCATION_ERROR, FRAME_AVIFREADER,
                            "ReadNextScanline: not initialized or no more "
                            "scanlines");
  }
  *out_scanline_bytes = static_cast<const void*>(
      rgb_pixels_.get() + static_cast<size_t>(row_) * frame_bytes_per_row_);
  ++row_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameReader::GetFrameSpec(FrameSpec* frame_spec) const {
  *frame_spec = frame_spec_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

ScanlineStatus AvifFrameReader::GetImageSpec(ImageSpec* image_spec) const {
  *image_spec = image_spec_;
  return ScanlineStatus(SCANLINE_STATUS_SUCCESS);
}

////////// AvifExtractMetadata (Stream H)

bool AvifExtractMetadata(StringPiece avif_bytes, GoogleString* exif_out,
                         GoogleString* icc_out, GoogleString* xmp_out,
                         MessageHandler* handler) {
  if (exif_out != nullptr) {
    exif_out->clear();
  }
  if (icc_out != nullptr) {
    icc_out->clear();
  }
  if (xmp_out != nullptr) {
    xmp_out->clear();
  }

  avifDecoder* decoder = avifDecoderCreate();
  if (decoder == nullptr) {
    PS_LOG_INFO(handler, "AvifExtractMetadata: avifDecoderCreate() failed");
    return false;
  }
  // Same untrusted-input caps as AvifFrameReader; this parses attacker-supplied
  // bytes too.
  decoder->imageSizeLimit = kAvifMaxImagePixels;
  decoder->imageDimensionLimit = kAvifMaxDimension;
  decoder->imageCountLimit = kAvifMaxFrameCount;
  decoder->maxThreads = 1;
  decoder->strictFlags = AVIF_STRICT_DISABLED;

  bool ok = false;
  avifResult res = avifDecoderSetIOMemory(
      decoder, reinterpret_cast<const uint8_t*>(avif_bytes.data()),
      avif_bytes.size());
  if (res == AVIF_RESULT_OK) {
    res = avifDecoderParse(decoder);
  }
  if (res == AVIF_RESULT_OK) {
    const avifImage* image = decoder->image;
    if (exif_out != nullptr && image->exif.data != nullptr &&
        image->exif.size > 0) {
      exif_out->assign(reinterpret_cast<const char*>(image->exif.data),
                       image->exif.size);
    }
    if (icc_out != nullptr && image->icc.data != nullptr &&
        image->icc.size > 0) {
      icc_out->assign(reinterpret_cast<const char*>(image->icc.data),
                      image->icc.size);
    }
    if (xmp_out != nullptr && image->xmp.data != nullptr &&
        image->xmp.size > 0) {
      xmp_out->assign(reinterpret_cast<const char*>(image->xmp.data),
                      image->xmp.size);
    }
    ok = true;
  } else {
    PS_LOG_INFO(handler, "AvifExtractMetadata: parse failed: %s",
                avifResultToString(res));
  }
  avifDecoderDestroy(decoder);
  return ok;
}

}  // namespace image_compression

}  // namespace pagespeed
