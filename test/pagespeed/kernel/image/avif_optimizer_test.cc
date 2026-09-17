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

// Kernel-level POSITIVE coverage for the
// AVIF codec (avif_optimizer.cc), the sibling of webp_optimizer_test.cc. A
// genuine encode -> decode round trip through AvifFrameWriter/AvifFrameReader
// (via the read_image.cc adapters): dimensions must survive and the decoded
// pixels must match the source approximately (lossy, PSNR-bounded) or exactly
// (lossless). A codec-less libavif build -- one without the AOM
// encoder/decoder threaded through -- fails here instead of silently passing
// the rest of the suite.

#include "pagespeed/kernel/image/avif_optimizer.h"

#include <cstdint>
#include <cstdlib>
#include <memory>

#include "base/logging.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/image/image_util.h"
#include "pagespeed/kernel/image/png_optimizer.h"
#include "pagespeed/kernel/image/read_image.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/image/test_utils.h"

namespace {

using net_instaweb::MockMessageHandler;
using net_instaweb::NullMutex;
using pagespeed::image_compression::AvifConfiguration;
using pagespeed::image_compression::AvifFrameWriter;
using pagespeed::image_compression::ComputeImageType;
using pagespeed::image_compression::FrameSpec;
using pagespeed::image_compression::ImageSpec;
using pagespeed::image_compression::RGB_888;
using pagespeed::image_compression::SCANLINE_STATUS_TIMEOUT_ERROR;
using pagespeed::image_compression::IMAGE_AVIF;
using pagespeed::image_compression::IMAGE_PNG;
using pagespeed::image_compression::kMessagePatternPixelFormat;
using pagespeed::image_compression::kMessagePatternStats;
using pagespeed::image_compression::kWebpTestDir;
using pagespeed::image_compression::PixelFormat;
using pagespeed::image_compression::PngScanlineReaderRaw;
using pagespeed::image_compression::ReadImage;
using pagespeed::image_compression::ReadTestFile;
using pagespeed::image_compression::ScanlineStatus;
using pagespeed::image_compression::ScanlineWriterInterface;

struct ImageInfo {
  const char* original_file;
  const char* gold_file;
};

// The same source images webp_optimizer_test.cc round-trips (all stored as
// PNG in the webp test-data directory): with alpha, opaque, and grayscale.
// gray_saved_as_gray.png is stored GRAY_8; the AVIF writer expands gray to
// RGB, so its decode is compared against the RGB-stored gold twin.
const ImageInfo kValidImages[] = {
    {"alpha_32x32", "alpha_32x32"},    // size 32-by-32 with alpha.
    {"opaque_32x20", "opaque_32x20"},  // size 32-by-20 without alpha.
    {"gray_saved_as_gray", "gray_saved_as_rgb"},  // images of same contents.
};
const size_t kValidImageCount = arraysize(kValidImages);

const double kMinPSNR = 33.0;

class AvifOptimizerTest : public testing::Test {
 public:
  AvifOptimizerTest() : message_handler_(new NullMutex) {}

  // Encodes a PNG into AVIF through the scanline/frame writer stack, exactly
  // as image_converter drives it in production.
  void ConvertPngToAvif(const GoogleString& png_image,
                        const AvifConfiguration& avif_config,
                        GoogleString* avif_image) {
    PngScanlineReaderRaw png_reader(&message_handler_);
    // Initialize a PNG reader for reading the original image.
    ASSERT_TRUE(png_reader.Initialize(png_image.data(), png_image.length()));

    // Get the sizes and pixel format of the original image.
    const size_t width = png_reader.GetImageWidth();
    const size_t height = png_reader.GetImageHeight();
    const PixelFormat pixel_format = png_reader.GetPixelFormat();

    // Create an AVIF writer.
    std::unique_ptr<ScanlineWriterInterface> avif_writer(CreateScanlineWriter(
        pagespeed::image_compression::IMAGE_AVIF, pixel_format, width, height,
        &avif_config, avif_image, &message_handler_));
    ASSERT_NE(static_cast<ScanlineWriterInterface*>(nullptr),
              avif_writer.get());

    // Read the scanlines from the original image and write them to the new
    // one.
    while (png_reader.HasMoreScanLines()) {
      uint8_t* scanline = nullptr;
      ASSERT_TRUE(
          png_reader.ReadNextScanline(reinterpret_cast<void**>(&scanline)));
      ASSERT_TRUE(
          avif_writer->WriteNextScanline(reinterpret_cast<void*>(scanline)));
    }
    ScanlineStatus status = avif_writer->FinalizeWriteWithStatus();
    ASSERT_TRUE(status.Success()) << "Status=" << status.ToString();
  }

 protected:
  void SetUp() override {
    message_handler_.AddPatternToSkipPrinting(kMessagePatternPixelFormat);
    message_handler_.AddPatternToSkipPrinting(kMessagePatternStats);
  }

 protected:
  MockMessageHandler message_handler_;

 private:
  AvifOptimizerTest(const AvifOptimizerTest&) = delete;
  AvifOptimizerTest& operator=(const AvifOptimizerTest&) = delete;
};

// Encode each source image to lossy AVIF, then decode it back and compare
// with the gold image: same dimensions, pixels equal within the PSNR bound.
TEST_F(AvifOptimizerTest, ConvertToAndReadLossyAvif) {
  AvifConfiguration avif_config;
  avif_config.lossless = 0;
  avif_config.quality = 90;
  avif_config.speed = 8;  // Keep the AV1 encode fast in tests.

  for (size_t i = 0; i < kValidImageCount; ++i) {
    GoogleString original_image, gold_image, avif_image;
    ReadTestFile(kWebpTestDir, kValidImages[i].original_file, "png",
                 &original_image);
    ConvertPngToAvif(original_image, avif_config, &avif_image);
    ASSERT_FALSE(avif_image.empty());
    // The encoded bytes sniff as a genuine ISO-BMFF AVIF.
    EXPECT_EQ(net_instaweb::IMAGE_AVIF, ComputeImageType(avif_image));
    ReadTestFile(kWebpTestDir, kValidImages[i].gold_file, "png", &gold_image);
    DecodeAndCompareImagesByPSNR(
        IMAGE_PNG, gold_image.c_str(), gold_image.length(), IMAGE_AVIF,
        avif_image.c_str(), avif_image.length(), kMinPSNR,
        true,  // ignore_transparent_rgb
        true,  // expand colors (GRAY_8 source vs RGB_888 AVIF decode)
        &message_handler_);
  }
}

// Encode to lossless AVIF (identity-matrix RGB path), decode, and require an
// exact pixel match against the gold image.
TEST_F(AvifOptimizerTest, ConvertToAndReadLosslessAvif) {
  AvifConfiguration avif_config;
  avif_config.lossless = 1;
  avif_config.speed = 8;  // Keep the AV1 encode fast in tests.

  for (size_t i = 0; i < kValidImageCount; ++i) {
    GoogleString original_image, gold_image, avif_image;
    ReadTestFile(kWebpTestDir, kValidImages[i].original_file, "png",
                 &original_image);
    ConvertPngToAvif(original_image, avif_config, &avif_image);
    ASSERT_FALSE(avif_image.empty());
    EXPECT_EQ(net_instaweb::IMAGE_AVIF, ComputeImageType(avif_image));
    ReadTestFile(kWebpTestDir, kValidImages[i].gold_file, "png", &gold_image);
    DecodeAndCompareImages(IMAGE_PNG, gold_image.c_str(), gold_image.length(),
                           IMAGE_AVIF, avif_image.c_str(), avif_image.length(),
                           true,  // ignore_transparent_rgb
                           &message_handler_);
  }
}

// Corrupt or truncated AVIF input must fail cleanly (no crash, no partial
// success), mirroring the webp reader's invalid-input tests.
TEST_F(AvifOptimizerTest, InvalidAvifFailsGracefully) {
  // A structurally plausible prefix (real ftyp box) followed by garbage.
  GoogleString corrupt;
  corrupt.append("\x00\x00\x00\x1C", 4);
  corrupt.append("ftypavif");
  corrupt.append(GoogleString(64, '\x42'));

  uint8_t* pixels = nullptr;
  PixelFormat pixel_format;
  size_t width, height, stride;
  EXPECT_FALSE(ReadImage(IMAGE_AVIF, corrupt.data(), corrupt.length(),
                         reinterpret_cast<void**>(&pixels), &pixel_format,
                         &width, &height, &stride, &message_handler_));

  // A valid AVIF truncated mid-file must also fail cleanly.
  AvifConfiguration avif_config;
  avif_config.speed = 8;
  GoogleString original_image, avif_image;
  ReadTestFile(kWebpTestDir, kValidImages[0].original_file, "png",
               &original_image);
  ConvertPngToAvif(original_image, avif_config, &avif_image);
  ASSERT_GT(avif_image.length(), static_cast<size_t>(32));
  EXPECT_FALSE(ReadImage(IMAGE_AVIF, avif_image.data(), avif_image.length() / 2,
                         reinterpret_cast<void**>(&pixels), &pixel_format,
                         &width, &height, &stride, &message_handler_));
}


// ---------------------------------------------------------------------------
// Progress-hook / timeout coverage (the ANIMATED deadline).
//
// avif_optimizer.cc consults the progress hook at a frame boundary only when
// the image is animated:
//
//     if (progress_hook_ != nullptr && animated) { ... }
//
// Both halves of that condition need a test, and they need DIFFERENT tests:
//
//   * AnimatedEncodeAbortsOnProgressHook covers the `progress_hook_ != nullptr`
//     half -- for a multi-frame sequence the hook is a genuine mid-encode abort
//     point, because the later frames have not been encoded yet.
//
//   * StillEncodeIgnoresProgressHook covers the `&& animated` half, and it is
//     the real negative control here. Deleting `&& animated` does NOT change
//     animated behaviour at all -- it only makes the hook apply to stills
//     again -- so an animated-only test stays green either way and would prove
//     nothing. This one goes red, because a still with a hostile hook must
//     still SUCCEED: libavif encodes a single-frame image entirely inside
//     avifEncoderAddImage(), so by the frame boundary the work is already paid
//     for and aborting there would throw away a finished result. That was the
//     defect.
// ---------------------------------------------------------------------------

// A progress hook that refuses to continue after `allow_calls` invocations,
// and counts how many times it was consulted.
struct ProgressHookState {
  int calls = 0;
  int allow_calls = 0;
};

bool CountingProgressHook(int percent, void* user_data) {
  ProgressHookState* state = static_cast<ProgressHookState*>(user_data);
  ++state->calls;
  return state->calls <= state->allow_calls;
}

// Drives AvifFrameWriter over `num_frames` synthetic frames, exactly as
// image.cc's RewriteToAvif loop does, and returns the resulting status.
// `hook_state` is installed as the progress hook's user data.
ScanlineStatus WriteFrames(int num_frames, ProgressHookState* hook_state,
                           net_instaweb::MessageHandler* handler,
                           GoogleString* out) {
  const size_t kWidth = 32;
  const size_t kHeight = 32;

  AvifConfiguration avif_config;
  avif_config.lossless = 0;
  avif_config.quality = 60;
  avif_config.speed = 9;  // Keep the AV1 encode fast in tests.
  avif_config.progress_hook = CountingProgressHook;
  avif_config.user_data = hook_state;

  AvifFrameWriter writer(handler);
  ScanlineStatus status = writer.Initialize(&avif_config, out);
  if (!status.Success()) {
    return status;
  }

  ImageSpec image_spec;
  image_spec.width = kWidth;
  image_spec.height = kHeight;
  image_spec.num_frames = num_frames;
  status = writer.PrepareImage(&image_spec);
  if (!status.Success()) {
    return status;
  }

  // A mid-grey frame; the content does not matter, only the frame count does.
  GoogleString scanline(kWidth * 3, '\x80');
  for (int frame = 0; frame < num_frames; ++frame) {
    FrameSpec frame_spec;
    frame_spec.width = kWidth;
    frame_spec.height = kHeight;
    frame_spec.top = 0;
    frame_spec.left = 0;
    frame_spec.pixel_format = RGB_888;
    frame_spec.duration_ms = 100;
    status = writer.PrepareNextFrame(&frame_spec);
    if (!status.Success()) {
      return status;
    }
    for (size_t row = 0; row < kHeight; ++row) {
      status = writer.WriteNextScanline(scanline.data());
      if (!status.Success()) {
        return status;
      }
    }
  }
  return writer.FinalizeWrite();
}

// ANIMATION: the frame-boundary hook is a real deadline. The hook allows the
// first frame and then refuses, so the encode is abandoned partway through a
// 4-frame sequence instead of running to the end.
TEST_F(AvifOptimizerTest, AnimatedEncodeAbortsOnProgressHook) {
  ProgressHookState hook_state;
  hook_state.allow_calls = 1;  // First frame proceeds; the next one aborts.

  GoogleString out;
  ScanlineStatus status =
      WriteFrames(4, &hook_state, &message_handler_, &out);

  // The encode failed, and specifically as a TIMEOUT rather than a codec
  // error -- image.cc keys the timeout counter off exactly this status type.
  EXPECT_FALSE(status.Success());
  EXPECT_EQ(SCANLINE_STATUS_TIMEOUT_ERROR, status.type())
      << "Status=" << status.ToString();

  // It really did stop early: the hook was consulted, and the run ended before
  // all 4 frames were committed.
  EXPECT_EQ(2, hook_state.calls);
}

// ANIMATION, control: the identical 4-frame encode with a hook that always
// says "continue" must succeed. Without this, the test above could pass for
// the wrong reason (a multi-frame encode that was broken anyway).
TEST_F(AvifOptimizerTest, AnimatedEncodeCompletesWhenHookAllows) {
  ProgressHookState hook_state;
  hook_state.allow_calls = 1000;  // Never refuses.

  GoogleString out;
  ScanlineStatus status =
      WriteFrames(4, &hook_state, &message_handler_, &out);

  EXPECT_TRUE(status.Success()) << "Status=" << status.ToString();
  EXPECT_FALSE(out.empty());
  // A multi-frame sequence is written with the "avis" brand, so it sniffs as
  // ANIMATED AVIF -- which also confirms the writer really took the animation
  // path rather than collapsing the frames into a still.
  EXPECT_EQ(net_instaweb::IMAGE_AVIF_ANIMATED, ComputeImageType(out));
  // One hook call per committed frame.
  EXPECT_EQ(4, hook_state.calls);
}

// STILL: the hook must NOT be consulted at all.
//
// This is the negative control for the `&& animated` condition. The hook here
// refuses on its very first call, so if the still path consulted it the encode
// would fail -- which is precisely the old behaviour: work already finished
// inside avifEncoderAddImage() thrown away at the frame boundary. The encode
// must succeed and the hook must never be called.
TEST_F(AvifOptimizerTest, StillEncodeIgnoresProgressHook) {
  ProgressHookState hook_state;
  hook_state.allow_calls = 0;  // Refuses immediately, if ever asked.

  GoogleString out;
  ScanlineStatus status =
      WriteFrames(1, &hook_state, &message_handler_, &out);

  EXPECT_TRUE(status.Success()) << "Status=" << status.ToString();
  EXPECT_FALSE(out.empty());
  EXPECT_EQ(net_instaweb::IMAGE_AVIF, ComputeImageType(out));
  EXPECT_EQ(0, hook_state.calls)
      << "the still path must never consult the progress hook: by the frame "
         "boundary the encode is already complete, so acting on it would "
         "discard finished work";
}

// ---------------------------------------------------------------------------
// The still-image encode budget must be MONOTONE.
//
// The encode speed used to be chosen in image.cc by stepping on a fixed
// 2000 ms threshold (below it: speed 8; at or above it: speed 6) while the
// writer's admission test estimated cost from that same speed. Because speed 6
// costs roughly 5x more per megapixel than speed 8, raising AvifTimeoutMs
// across 2000 ms made the rewriter admit FEWER megapixels -- a bigger budget
// buying less work. Nothing tested the coupling, so it shipped.
//
// The speed is now derived here, per image, from the budget and the pixel
// count: the slowest (best quality-per-byte) speed at or above the configured
// floor whose estimate fits, declining only when not even the fastest known
// speed does. These tests pin the three properties that follow from that and
// did not hold before: admission is monotone in the budget, admission is
// anti-monotone in image size, and the derived speed improves with budget.
//
// They deliberately stop after PrepareImage() -- the admission decision is
// made entirely there, so nothing is encoded. That keeps them deterministic
// and independent of how fast the machine running them is.
// ---------------------------------------------------------------------------

// image.cc configures no speed at all, so the floor is AvifConfiguration's
// default. Encoding a speed literal here instead would let the two drift.
const int kProductionFloorSpeed = AvifConfiguration().speed;

struct AdmissionResult {
  bool admitted;
  int speed;          // Speed the admission estimate was computed against.
  int encoder_speed;  // Speed actually stamped on the libavif encoder.
};

// Runs Initialize() + PrepareImage() only, and reports the admission decision
// together with both views of the resulting speed.
AdmissionResult TryAdmit(int width, int height, int64 budget_ms,
                         int configured_speed,
                         net_instaweb::MessageHandler* handler) {
  AvifConfiguration avif_config;
  avif_config.lossless = 0;
  avif_config.quality = 60;
  avif_config.speed = configured_speed;
  avif_config.encode_budget_ms = budget_ms;

  GoogleString out;
  AvifFrameWriter writer(handler);
  ScanlineStatus status = writer.Initialize(&avif_config, &out);
  EXPECT_TRUE(status.Success()) << "Initialize: " << status.ToString();

  ImageSpec image_spec;
  image_spec.width = width;
  image_spec.height = height;
  image_spec.num_frames = 1;
  status = writer.PrepareImage(&image_spec);

  AdmissionResult result;
  result.admitted = status.Success();
  result.speed = writer.speed_for_testing();
  result.encoder_speed = writer.encoder_speed_for_testing();

  // The invariant the whole guard rests on: whatever speed the estimate
  // assumed is the speed libavif will actually encode at. Checked on EVERY
  // admission decision rather than in one dedicated test, because a re-stamp
  // that is missed on one path is exactly the failure mode that matters.
  EXPECT_EQ(result.speed, result.encoder_speed)
      << "the derived speed did not reach the libavif encoder: estimated "
         "against speed "
      << result.speed << " but avifEncoder->speed is " << result.encoder_speed;
  return result;
}

// 4.0 Mpx: large enough that the production floor speed does NOT fit a
// sub-second budget on either architecture, small enough that the fastest
// speeds do. That is what makes the step-up observable at all.
const int kSweepWidth = 2000;
const int kSweepHeight = 2000;

// 1. MONOTONE IN BUDGET. Sweeping the budget upward -- straight across the old
// 2000 ms cliff -- admission may only ever get MORE permissive, and the
// selected speed may only ever get slower (better quality). Under the old
// design the 1999 -> 2000 step moved both the wrong way.
TEST_F(AvifOptimizerTest, StillBudgetAdmissionIsMonotoneInBudget) {
  const int64 kBudgets[] = {1, 50, 200, 500, 1000, 1999, 2000, 2001, 5000};

  bool seen_admitted = false;
  int64 admitted_at = 0;
  int previous_speed = 0;
  for (int64 budget : kBudgets) {
    AdmissionResult result = TryAdmit(kSweepWidth, kSweepHeight, budget,
                                      kProductionFloorSpeed, &message_handler_);
    if (result.admitted) {
      if (seen_admitted) {
        // Quality must not degrade as the operator gives the encoder MORE
        // time. Lower speed number == slower == better quality-per-byte.
        EXPECT_LE(result.speed, previous_speed)
            << "raising the budget from " << admitted_at << " ms to " << budget
            << " ms selected a FASTER (lower-quality) speed: " << previous_speed
            << " -> " << result.speed;
      }
      seen_admitted = true;
      admitted_at = budget;
      previous_speed = result.speed;
    } else {
      EXPECT_FALSE(seen_admitted)
          << "a budget of " << budget
          << " ms declined an image that a SMALLER budget of " << admitted_at
          << " ms admitted";
    }
  }
  // The sweep has to actually straddle the boundary, or it proves nothing.
  EXPECT_TRUE(seen_admitted) << "sweep never admitted; pick a smaller image";
  AdmissionResult tightest = TryAdmit(kSweepWidth, kSweepHeight, kBudgets[0],
                                      kProductionFloorSpeed, &message_handler_);
  EXPECT_FALSE(tightest.admitted)
      << "sweep never declined; pick a larger image";
}

// 2. ANTI-MONOTONE IN SIZE. For one fixed budget, once an image is too big to
// fit, every larger image must also be declined.
TEST_F(AvifOptimizerTest, StillBudgetAdmissionIsMonotoneInImageSize) {
  const int64 kBudgetMs = 500;
  bool seen_declined = false;
  int declined_at = 0;
  for (int side = 500; side <= 9000; side += 500) {
    AdmissionResult result = TryAdmit(side, side, kBudgetMs,
                                      kProductionFloorSpeed, &message_handler_);
    if (result.admitted) {
      EXPECT_FALSE(seen_declined)
          << side << "x" << side << " was admitted on a " << kBudgetMs
          << " ms budget, but the SMALLER " << declined_at << "x" << declined_at
          << " was declined";
    } else {
      seen_declined = true;
      declined_at = side;
    }
  }
  EXPECT_TRUE(seen_declined) << "size sweep never declined; widen the range";
}

// 3. THE SPEED IS DERIVED, not fixed. Same image, two budgets: the generous
// one must buy a slower, better-quality speed than the tight one. This is the
// test the old design cannot pass -- there the speed was decided before the
// image was known, so both budgets would encode at whatever image.cc had
// already picked.
TEST_F(AvifOptimizerTest, StillBudgetSelectsSlowestSpeedThatFits) {
  AdmissionResult tight = TryAdmit(kSweepWidth, kSweepHeight, 500,
                                   kProductionFloorSpeed, &message_handler_);
  AdmissionResult generous = TryAdmit(kSweepWidth, kSweepHeight, 60000,
                                      kProductionFloorSpeed, &message_handler_);

  ASSERT_TRUE(tight.admitted) << "the tight budget should still fit at speed";
  ASSERT_TRUE(generous.admitted);

  EXPECT_LT(generous.speed, tight.speed)
      << "a generous budget must buy a slower, better-quality speed than a "
         "tight one on the same image";
  // A generous budget stays AT the configured floor -- it must never make the
  // encoder slower than the caller asked for, or a big AvifTimeoutMs on a
  // small image would silently select speed 0 and cost seconds per image.
  EXPECT_EQ(kProductionFloorSpeed, generous.speed);
  EXPECT_GT(tight.speed, kProductionFloorSpeed)
      << "a tight budget must step UP from the floor rather than decline";
}

// 4. Still declines when nothing fits. A budget too small even at the fastest
// known speed is a refusal, and specifically a TIMEOUT status -- image.cc keys
// its AVIF timeout counter off exactly that type.
TEST_F(AvifOptimizerTest, StillBudgetDeclinesWhenEvenFastestSpeedDoesNotFit) {
  AdmissionResult result = TryAdmit(kSweepWidth, kSweepHeight, 1,
                                    kProductionFloorSpeed, &message_handler_);
  EXPECT_FALSE(result.admitted);

  // And the same image is admitted once the budget is plainly sufficient, so
  // the refusal above is the budget talking and not a broken writer.
  AdmissionResult roomy = TryAdmit(kSweepWidth, kSweepHeight, 600000,
                                   kProductionFloorSpeed, &message_handler_);
  EXPECT_TRUE(roomy.admitted);
}

// The guard stands down for kAvifSpeedDefault ("let libavif decide"), because
// an unknown speed is not estimable and guessing one would break the
// estimate-equals-encode invariant. Documented in PrepareImage(); pinned here.
TEST_F(AvifOptimizerTest, StillBudgetStandsDownForDefaultSpeed) {
  // A budget of 1 ms would decline this image at any known speed.
  AdmissionResult result = TryAdmit(
      kSweepWidth, kSweepHeight, 1,
      pagespeed::image_compression::kAvifSpeedDefault, &message_handler_);
  EXPECT_TRUE(result.admitted)
      << "an un-estimable speed must restore always-admit, not guess";
  EXPECT_EQ(pagespeed::image_compression::kAvifSpeedDefault, result.speed)
      << "the guard must not rewrite a speed it cannot estimate";
}

}  // namespace
