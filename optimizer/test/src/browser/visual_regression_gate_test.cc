// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Visual Regression Gate Tests
//
// Tests the CompareScreenshots static method using synthetic PNGs,
// plus CDP-based tests for CaptureScreenshot and Compare against a scripted
// CDP peer (test/test_util/cdp_scripted_peer.h).

#include "src/browser/visual_regression_gate.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "test/test_util/cdp_scripted_peer.h"
#include "test/test_util/cdp_setup_order.h"
#include "test/test_util/png_image.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;
using test::EncodePng;
using test::MakeSolidImage;

TEST(VisualRegressionGateTest, IdenticalScreenshots) {
  uint32_t w = 100, h = 100;
  auto pixels = MakeSolidImage(w, h, 255, 0, 0);
  auto png = EncodePng(pixels, w, h);
  ASSERT_FALSE(png.empty());

  auto result = VisualRegressionGate::CompareScreenshots(png, png, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 0u);
  EXPECT_FLOAT_EQ(result.diff_ratio, 0.0f);
  EXPECT_EQ(result.total_pixels, w * h);
  EXPECT_EQ(result.above_fold_height, h);
}

TEST(VisualRegressionGateTest, CompletelyDifferent) {
  uint32_t w = 100, h = 100;
  auto red = MakeSolidImage(w, h, 255, 0, 0);
  auto blue = MakeSolidImage(w, h, 0, 0, 255);
  auto red_png = EncodePng(red, w, h);
  auto blue_png = EncodePng(blue, w, h);
  ASSERT_FALSE(red_png.empty());
  ASSERT_FALSE(blue_png.empty());

  auto result = VisualRegressionGate::CompareScreenshots(red_png, blue_png, h);
  EXPECT_FALSE(result.passed);
  EXPECT_EQ(result.diff_pixels, w * h);
  EXPECT_FLOAT_EQ(result.diff_ratio, 1.0f);
}

TEST(VisualRegressionGateTest, WithinChannelTolerance) {
  uint32_t w = 50, h = 50;
  auto pixels_a = MakeSolidImage(w, h, 100, 100, 100);
  // Differ by exactly kChannelTolerance (2) in each channel.
  auto pixels_b = MakeSolidImage(w, h, 102, 102, 102);
  auto png_a = EncodePng(pixels_a, w, h);
  auto png_b = EncodePng(pixels_b, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 0u);
}

TEST(VisualRegressionGateTest, ExceedChannelTolerance) {
  uint32_t w = 50, h = 50;
  auto pixels_a = MakeSolidImage(w, h, 100, 100, 100);
  // Differ by kChannelTolerance + 1 (3) in one channel.
  auto pixels_b = MakeSolidImage(w, h, 103, 100, 100);
  auto png_a = EncodePng(pixels_a, w, h);
  auto png_b = EncodePng(pixels_b, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  // All pixels differ.
  EXPECT_EQ(result.diff_pixels, w * h);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, SmallDiffBelowThreshold) {
  uint32_t w = 100, h = 100;
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);

  // Modify a few pixels (less than 0.5% = 50 pixels).
  auto modified = pixels;
  for (int i = 0; i < 10 * 4; i += 4) {
    modified[i] = 255;  // Change 10 pixels to red.
  }

  auto png_a = EncodePng(pixels, w, h);
  auto png_b = EncodePng(modified, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 10u);
  EXPECT_LT(result.diff_ratio, 0.005f);
}

TEST(VisualRegressionGateTest, SmallDiffAboveThreshold) {
  uint32_t w = 10, h = 10;  // 100 total pixels
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);

  // Modify 1 pixel (1% > 0.5% threshold).
  auto modified = pixels;
  modified[0] = 255;

  auto png_a = EncodePng(pixels, w, h);
  auto png_b = EncodePng(modified, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_FALSE(result.passed);
  EXPECT_EQ(result.diff_pixels, 1u);
}

TEST(VisualRegressionGateTest, CustomThreshold) {
  uint32_t w = 10, h = 10;  // 100 total pixels
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);

  // Modify 5 pixels (5%).
  auto modified = pixels;
  for (int i = 0; i < 5; ++i) {
    modified[static_cast<size_t>(i) * 4] = 255;
  }

  auto png_a = EncodePng(pixels, w, h);
  auto png_b = EncodePng(modified, w, h);

  // With 10% threshold, should pass.
  auto result =
      VisualRegressionGate::CompareScreenshots(png_a, png_b, h, 0.10f);
  EXPECT_TRUE(result.passed);

  // With 1% threshold, should fail.
  result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h, 0.01f);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, AboveFoldOnly) {
  uint32_t w = 100, h = 200;
  auto pixels_a = MakeSolidImage(w, h, 128, 128, 128);

  // Make the bottom half completely different.
  auto pixels_b = pixels_a;
  size_t fold_offset =
      static_cast<size_t>(w) * 100 * 4;  // Row 100 starts here.
  for (size_t i = fold_offset; i < pixels_b.size(); i += 4) {
    pixels_b[i] = 255;
    pixels_b[i + 1] = 0;
    pixels_b[i + 2] = 0;
  }

  auto png_a = EncodePng(pixels_a, w, h);
  auto png_b = EncodePng(pixels_b, w, h);

  // Compare only above fold (100 pixels high) -- should pass.
  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, 100);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 0u);
  EXPECT_EQ(result.total_pixels, w * 100u);
  EXPECT_EQ(result.above_fold_height, 100u);
}

TEST(VisualRegressionGateTest, DifferentSizes) {
  // Different image sizes -- compares the overlapping region.
  uint32_t w1 = 100, h1 = 100;
  uint32_t w2 = 80, h2 = 120;
  auto pixels_a = MakeSolidImage(w1, h1, 128, 128, 128);
  auto pixels_b = MakeSolidImage(w2, h2, 128, 128, 128);

  auto png_a = EncodePng(pixels_a, w1, h1);
  auto png_b = EncodePng(pixels_b, w2, h2);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, 150);
  EXPECT_TRUE(result.passed);
  // Compare region: min(100,80)=80 x min(100,120,150)=100.
  EXPECT_EQ(result.total_pixels, 80u * 100u);
}

TEST(VisualRegressionGateTest, EmptyPngData) {
  std::vector<uint8_t> empty;
  uint32_t w = 10, h = 10;
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);
  auto valid_png = EncodePng(pixels, w, h);

  // Empty original -- should fail (passed = false).
  auto result = VisualRegressionGate::CompareScreenshots(empty, valid_png, h);
  EXPECT_FALSE(result.passed);

  // Empty optimized -- should fail.
  result = VisualRegressionGate::CompareScreenshots(valid_png, empty, h);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, InvalidPngData) {
  std::vector<uint8_t> garbage = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB};
  uint32_t w = 10, h = 10;
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);
  auto valid_png = EncodePng(pixels, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(garbage, valid_png, h);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, ZeroViewportHeight) {
  uint32_t w = 10, h = 10;
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);
  auto png = EncodePng(pixels, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png, png, 0);
  // Zero height means zero pixels to compare -- passes.
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.total_pixels, 0u);
}

TEST(VisualRegressionGateTest, OneByOnePixel) {
  uint32_t w = 1, h = 1;
  auto red = MakeSolidImage(w, h, 255, 0, 0);
  auto green = MakeSolidImage(w, h, 0, 255, 0);
  auto red_png = EncodePng(red, w, h);
  auto green_png = EncodePng(green, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(red_png, green_png, h);
  EXPECT_FALSE(result.passed);
  EXPECT_EQ(result.diff_pixels, 1u);
  EXPECT_EQ(result.total_pixels, 1u);
  EXPECT_FLOAT_EQ(result.diff_ratio, 1.0f);
}

TEST(VisualRegressionGateTest, AlphaChannelDifference) {
  uint32_t w = 10, h = 10;
  auto opaque = MakeSolidImage(w, h, 128, 128, 128, 255);
  auto transparent = MakeSolidImage(w, h, 128, 128, 128, 0);
  auto png_a = EncodePng(opaque, w, h);
  auto png_b = EncodePng(transparent, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  // Alpha differs by 255 -- all pixels different.
  EXPECT_FALSE(result.passed);
  EXPECT_EQ(result.diff_pixels, w * h);
}

TEST(VisualRegressionGateTest, ExactThresholdBoundary) {
  uint32_t w = 100, h = 100;  // 10000 pixels
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);

  // Modify exactly 50 pixels (0.5% = exact default threshold).
  auto modified = pixels;
  for (int i = 0; i < 50; ++i) {
    modified[static_cast<size_t>(i) * 4] = 255;
  }

  auto png_a = EncodePng(pixels, w, h);
  auto png_b = EncodePng(modified, w, h);

  // 50/10000 = 0.005 = exact threshold. Passes (uses <= comparison).
  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 50u);
}

TEST(VisualRegressionGateTest, JustBelowThreshold) {
  uint32_t w = 100, h = 100;  // 10000 pixels
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);

  // Modify 49 pixels (0.49% < 0.5% threshold).
  auto modified = pixels;
  for (int i = 0; i < 49; ++i) {
    modified[static_cast<size_t>(i) * 4] = 255;
  }

  auto png_a = EncodePng(pixels, w, h);
  auto png_b = EncodePng(modified, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 49u);
}

TEST(VisualRegressionGateTest, SemiTransparentAlpha) {
  uint32_t w = 10, h = 10;
  // Differ only by alpha channel, within tolerance.
  auto pixels_a = MakeSolidImage(w, h, 128, 128, 128, 200);
  auto pixels_b = MakeSolidImage(w, h, 128, 128, 128, 202);
  auto png_a = EncodePng(pixels_a, w, h);
  auto png_b = EncodePng(pixels_b, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png_a, png_b, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.diff_pixels, 0u);
}

TEST(VisualRegressionGateTest, BothEmptyPng) {
  std::vector<uint8_t> empty;
  auto result = VisualRegressionGate::CompareScreenshots(empty, empty, 100);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, BothInvalid) {
  std::vector<uint8_t> garbage = {0x00, 0x01, 0x02};
  auto result = VisualRegressionGate::CompareScreenshots(garbage, garbage, 100);
  EXPECT_FALSE(result.passed);
}

TEST(VisualRegressionGateTest, LargerImage) {
  uint32_t w = 400, h = 300;
  auto pixels = MakeSolidImage(w, h, 50, 100, 150);
  auto png = EncodePng(pixels, w, h);

  auto result = VisualRegressionGate::CompareScreenshots(png, png, h);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.total_pixels, w * h);
  EXPECT_EQ(result.diff_pixels, 0u);
}

TEST(VisualRegressionGateTest, ViewportTallerThanImage) {
  uint32_t w = 10, h = 10;
  auto pixels = MakeSolidImage(w, h, 128, 128, 128);
  auto png = EncodePng(pixels, w, h);

  // viewport_height > image height: should use image height.
  auto result = VisualRegressionGate::CompareScreenshots(png, png, 1000);
  EXPECT_TRUE(result.passed);
  EXPECT_EQ(result.total_pixels, w * h);
}

TEST(VisualRegressionGateTest, DefaultConstants) {
  EXPECT_FLOAT_EQ(VisualRegressionGate::kDefaultThreshold, 0.005f);
  EXPECT_EQ(VisualRegressionGate::kChannelTolerance, 2);
}

// ---- CDP-scripted tests ----
//
// The scripted peer lives in test/test_util/cdp_scripted_peer.h; everything
// below drives the gate's real transport against it.

class VisualRegressionGateCdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(peer_.Start());
    auto pixels = MakeSolidImage(10, 10, 128, 128, 128);
    grey_png_ = EncodePng(pixels, 10, 10);
    peer_.set_screenshot_b64(test::Base64Encode(grey_png_));
  }

  void TearDown() override { peer_.Stop(); }

  CdpClient* client() { return peer_.client(); }

  // Pump until the document has not just been SENT but answered: the capture
  // deliberately ignores lifecycle events until then, because before that they
  // describe the about:blank tab it started in.
  void SettleDocument() {
    peer_.PumpUntil(
        [this] { return peer_.SawCommand("Page.setDocumentContent"); }, 40);
    peer_.PumpUntil([] { return false; }, 10);
  }

  test::ScriptedCdpPeer peer_;
  std::vector<uint8_t> grey_png_;
};

TEST_F(VisualRegressionGateCdpTest, CaptureScreenshotSuccess) {
  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<ScreenshotResult> result;

  gate.CaptureScreenshot("<html><body>Test</body></html>", 1440, 900,
                         [&](absl::StatusOr<ScreenshotResult> r) {
                           result = std::move(r);
                           done = true;
                         });

  SettleDocument();
  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });

  ASSERT_TRUE(done);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(result->png_data.empty());
  EXPECT_EQ(result->width, 1440u);
  EXPECT_EQ(result->height, 900u);
}

TEST_F(VisualRegressionGateCdpTest, CaptureScreenshotCreateTargetFails) {
  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<ScreenshotResult> result;

  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Target.createTarget") return false;
    peer_.RespondError(id, -32000, "failed");
    return true;
  });

  gate.CaptureScreenshot("<html></html>", 1440, 900,
                         [&](absl::StatusOr<ScreenshotResult> r) {
                           result = std::move(r);
                           done = true;
                         });

  peer_.PumpUntil([&done] { return done; });

  ASSERT_TRUE(done);
  EXPECT_FALSE(result.ok());
}

TEST_F(VisualRegressionGateCdpTest, CaptureScreenshotNetworkFailure) {
  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<ScreenshotResult> result;

  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Network.emulateNetworkConditions") return false;
    peer_.RespondError(id, -32000, "not supported");
    return true;
  });

  gate.CaptureScreenshot("<html></html>", 1440, 900,
                         [&](absl::StatusOr<ScreenshotResult> r) {
                           result = std::move(r);
                           done = true;
                         });

  peer_.PumpUntil([&done] { return done; });

  ASSERT_TRUE(done);
  EXPECT_FALSE(result.ok());
}

TEST_F(VisualRegressionGateCdpTest, CaptureScreenshotEmptyData) {
  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<ScreenshotResult> result;

  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Page.captureScreenshot") return false;
    peer_.RespondToCommand(id, {{"data", ""}});
    return true;
  });

  gate.CaptureScreenshot("<html></html>", 1440, 900,
                         [&](absl::StatusOr<ScreenshotResult> r) {
                           result = std::move(r);
                           done = true;
                         });

  SettleDocument();
  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });

  ASSERT_TRUE(done);
  EXPECT_FALSE(result.ok());
}

TEST_F(VisualRegressionGateCdpTest, FetchRequestBlocked) {
  VisualRegressionGate gate(client());
  bool done = false;

  gate.CaptureScreenshot(
      "<html></html>", 1440, 900,
      [&](absl::StatusOr<ScreenshotResult>) { done = true; });

  SettleDocument();

  // Every subresource is failed, unconditionally: this is the SSRF defense,
  // not a policy knob.
  peer_.SendEvent("Fetch.requestPaused",
                  {{"requestId", "req-1"},
                   {"request", {{"url", "http://evil.com/img.png"}}}},
                  peer_.session_id(1));
  peer_.PumpUntil([&] { return peer_.SawCommand("Fetch.failRequest"); }, 10);
  EXPECT_TRUE(peer_.SawCommand("Fetch.failRequest"));
  // With Chromium's parameter name, `errorReason`: a `reason` is rejected
  // ("Invalid parameters") and the request stays paused, so the capture never
  // reaches networkIdle and times out.
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") != "Fetch.failRequest") continue;
    const json params = cmd.value("params", json::object());
    EXPECT_EQ(params.value("errorReason", ""), "BlockedByClient");
    EXPECT_FALSE(params.contains("reason"));
  }

  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });
}

TEST_F(VisualRegressionGateCdpTest, SessionIdPropagated) {
  VisualRegressionGate gate(client());
  bool done = false;

  gate.CaptureScreenshot(
      "<html></html>", 375, 667,
      [&](absl::StatusOr<ScreenshotResult>) { done = true; });

  SettleDocument();

  bool found_session = false;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.contains("sessionId") && cmd["sessionId"] == peer_.session_id(1)) {
      found_session = true;
      break;
    }
  }
  EXPECT_TRUE(found_session);

  bool found_viewport = false;
  bool found_touch = false;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") == "Emulation.setDeviceMetricsOverride") {
      auto params = cmd.value("params", json::object());
      EXPECT_EQ(params.value("width", 0), 375);
      EXPECT_EQ(params.value("height", 0), 667);
      EXPECT_TRUE(params.value("mobile", false));
      found_viewport = true;
    }
    // The validation render is a touch device at 375 px, as the
    // coverage render that produced the block is.
    if (cmd.value("method", "") == "Emulation.setTouchEmulationEnabled") {
      EXPECT_TRUE(found_viewport) << "touch follows the device metrics";
      auto params = cmd.value("params", json::object());
      EXPECT_TRUE(params.value("enabled", false));
      EXPECT_EQ(params.value("maxTouchPoints", 0), 5);
      found_touch = true;
      break;
    }
  }
  EXPECT_TRUE(found_viewport);
  EXPECT_TRUE(found_touch);
  // Then the phone's user agent, and a fresh window.
  test::ExpectFreshWindowRenderSetup(peer_.received_commands(),
                                     peer_.session_id(1), 375, 667);

  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });
}

// The desktop validation render sends no user agent override,
// so its commands up to the blank document are those before User-Agent emulation.
TEST_F(VisualRegressionGateCdpTest, DesktopSetupOrder) {
  VisualRegressionGate gate(client());
  bool done = false;
  gate.CaptureScreenshot(
      "<html></html>", 1440, 900,
      [&](absl::StatusOr<ScreenshotResult>) { done = true; });
  SettleDocument();
  test::ExpectFreshWindowRenderSetup(peer_.received_commands(),
                                     peer_.session_id(1), 1440, 900);
  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });
  EXPECT_TRUE(done);
}

TEST_F(VisualRegressionGateCdpTest, CaptureIgnoresLifecycleFromTheBlankTab) {
  // A capture starts by creating a tab at about:blank and only then writes the
  // document it wants a picture of. about:blank reaches networkIdle on its own,
  // and enabling lifecycle events can surface that. Screenshotting on it
  // photographs a blank page and reports it as the page — which, for the
  // critical-CSS validation built on this, is a blank reference diffed against
  // a blank candidate: zero difference, "confirmed", on a page nobody rendered.
  int content_command_id = -1;
  peer_.SetResponder(
      [&](int id, const std::string& method, const json&) -> bool {
        if (method != "Page.setDocumentContent") return false;
        content_command_id = id;  // held: the document is NOT in the frame yet
        return true;
      });

  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<ScreenshotResult> result;
  gate.CaptureScreenshot("<html><body>real content</body></html>", 1440, 900,
                         [&](absl::StatusOr<ScreenshotResult> r) {
                           result = std::move(r);
                           done = true;
                         });

  peer_.PumpUntil([&] { return content_command_id >= 0; }, 40);
  ASSERT_GE(content_command_id, 0) << "setDocumentContent was never sent";

  // The blank tab settles.
  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([] { return false; }, 15);

  EXPECT_EQ(peer_.CountCommands("Page.captureScreenshot"), 0u)
      << "screenshotted the blank tab before the document was in it";
  EXPECT_FALSE(done);

  // Now the document lands, and the document's own networkIdle follows.
  peer_.RespondToCommand(content_command_id, json::object());
  peer_.PumpUntil([] { return false; }, 10);
  peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                  peer_.session_id(1));
  peer_.PumpUntil([&done] { return done; });

  ASSERT_TRUE(done) << "the capture never completed once the document landed";
  EXPECT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(peer_.CountCommands("Page.captureScreenshot"), 1u);
}

// ---- Compare(): the sequential two-capture handoff ----
//
// Compare drives two full captures back to back and hands the first's PNG to
// the second's completion, because CdpClient supports exactly one event
// subscriber at a time (visual_regression_gate.cc, "temporary gate" comment).
// Nothing exercised that handoff until these tests; every Compare defect below
// would have shipped silently.

namespace {

// Answers each session's Page.setDocumentContent and then pushes that session's
// networkIdle, so a Compare run advances without the test hand-scripting the
// second capture's session id.  `png_for_capture` supplies a distinct payload
// per capture (1-based) so the diff is a real measurement, not a tautology.
test::ScriptedCdpPeer::Responder DriveSequentialCaptures(
    test::ScriptedCdpPeer* peer,
    std::function<std::string(int)> png_for_capture, int* screenshots_taken) {
  return
      [peer, png_for_capture = std::move(png_for_capture), screenshots_taken](
          int id, const std::string& method, const json& cmd) -> bool {
        if (method == "Page.setDocumentContent") {
          peer->RespondToCommand(id, json::object());
          peer->SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                          cmd.value("sessionId", ""));
          return true;
        }
        if (method == "Page.captureScreenshot") {
          ++(*screenshots_taken);
          peer->RespondToCommand(
              id, {{"data", png_for_capture(*screenshots_taken)}});
          return true;
        }
        return false;
      };
}

}  // namespace

TEST_F(VisualRegressionGateCdpTest, CompareRunsBothCapturesAndReportsDiff) {
  // Reference: solid grey.  Candidate: same image with a 4-pixel row of black,
  // i.e. 4/100 = 0.04 of the compared region.
  auto reference = EncodePng(MakeSolidImage(10, 10, 128, 128, 128), 10, 10);
  auto candidate_pixels = MakeSolidImage(10, 10, 128, 128, 128);
  for (int i = 0; i < 4; ++i) {
    candidate_pixels[i * 4 + 0] = 0;
    candidate_pixels[i * 4 + 1] = 0;
    candidate_pixels[i * 4 + 2] = 0;
  }
  auto candidate = EncodePng(candidate_pixels, 10, 10);

  int shots = 0;
  peer_.SetResponder(DriveSequentialCaptures(
      &peer_,
      [&](int n) { return test::Base64Encode(n == 1 ? reference : candidate); },
      &shots));

  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<RegressionResult> result;
  gate.Compare(
      "<html>ref</html>", "<html>cand</html>", 10, 10,
      [&](absl::StatusOr<RegressionResult> r) {
        result = std::move(r);
        done = true;
      },
      /*threshold=*/0.10f);

  peer_.PumpUntil([&done] { return done; }, 120);

  ASSERT_TRUE(done) << "Compare never completed";
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(shots, 2) << "Compare must capture both documents";
  EXPECT_EQ(peer_.CountCommands("Target.createTarget"), 2u);
  EXPECT_EQ(result->total_pixels, 100u);
  EXPECT_EQ(result->diff_pixels, 4u);
  EXPECT_FLOAT_EQ(result->diff_ratio, 0.04f);
  EXPECT_TRUE(result->passed) << "0.04 is below the 0.10 threshold passed in";

  // Both documents really did reach the browser, and in the right order.
  std::vector<std::string> contents;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") == "Page.setDocumentContent") {
      contents.push_back(cmd.value("params", json::object()).value("html", ""));
    }
  }
  ASSERT_EQ(contents.size(), 2u);
  EXPECT_EQ(contents[0], "<html>ref</html>");
  EXPECT_EQ(contents[1], "<html>cand</html>");

  // Both at the SAME viewport, or the two renders are of different pages and
  // the ratio means nothing.
  int viewports_set = 0;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") != "Emulation.setDeviceMetricsOverride") {
      continue;
    }
    auto params = cmd.value("params", json::object());
    EXPECT_EQ(params.value("width", 0), 10);
    EXPECT_EQ(params.value("height", 0), 10);
    ++viewports_set;
  }
  EXPECT_EQ(viewports_set, 2);
}

TEST_F(VisualRegressionGateCdpTest, CompareVerdictFollowsTheThreshold) {
  auto reference = EncodePng(MakeSolidImage(10, 10, 128, 128, 128), 10, 10);
  auto candidate_pixels = MakeSolidImage(10, 10, 128, 128, 128);
  for (int i = 0; i < 4; ++i) {
    candidate_pixels[i * 4 + 0] = 0;
    candidate_pixels[i * 4 + 1] = 0;
    candidate_pixels[i * 4 + 2] = 0;
  }
  auto candidate = EncodePng(candidate_pixels, 10, 10);

  int shots = 0;
  peer_.SetResponder(DriveSequentialCaptures(
      &peer_,
      [&](int n) { return test::Base64Encode(n == 1 ? reference : candidate); },
      &shots));

  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<RegressionResult> result;
  gate.Compare(
      "<html>ref</html>", "<html>cand</html>", 10, 10,
      [&](absl::StatusOr<RegressionResult> r) {
        result = std::move(r);
        done = true;
      },
      /*threshold=*/0.01f);

  peer_.PumpUntil([&done] { return done; }, 120);

  ASSERT_TRUE(done);
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FLOAT_EQ(result->diff_ratio, 0.04f);
  EXPECT_FALSE(result->passed) << "0.04 is above the 0.01 threshold passed in";
}

TEST_F(VisualRegressionGateCdpTest, CompareFailsClosedWhenFirstCaptureErrors) {
  peer_.SetResponder([this](int id, const std::string& method, const json&) {
    if (method != "Target.createTarget") return false;
    peer_.RespondError(id, -32000, "no targets");
    return true;
  });

  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<RegressionResult> result;
  gate.Compare("<html>ref</html>", "<html>cand</html>", 10, 10,
               [&](absl::StatusOr<RegressionResult> r) {
                 result = std::move(r);
                 done = true;
               });

  peer_.PumpUntil([&done] { return done; }, 120);

  ASSERT_TRUE(done);
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("original screenshot failed"),
            std::string::npos)
      << result.status().message();
  // The second document must never have been rendered.
  EXPECT_EQ(peer_.CountCommands("Page.setDocumentContent"), 0u);
}

TEST_F(VisualRegressionGateCdpTest, CompareFailsClosedWhenSecondCaptureErrors) {
  auto grey = EncodePng(MakeSolidImage(10, 10, 128, 128, 128), 10, 10);
  int targets = 0;
  int shots = 0;
  peer_.SetResponder(
      [&](int id, const std::string& method, const json& cmd) -> bool {
        if (method == "Target.createTarget") {
          // The second capture cannot get a tab.  A gate that reported the first
          // capture's screenshot as a comparison would validate a page it never
          // rendered the candidate for.
          if (++targets == 2) {
            peer_.RespondError(id, -32000, "out of targets");
            return true;
          }
          return false;
        }
        if (method == "Page.setDocumentContent") {
          peer_.RespondToCommand(id, json::object());
          peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                          cmd.value("sessionId", ""));
          return true;
        }
        if (method == "Page.captureScreenshot") {
          ++shots;
          peer_.RespondToCommand(id, {{"data", test::Base64Encode(grey)}});
          return true;
        }
        return false;
      });

  VisualRegressionGate gate(client());
  bool done = false;
  absl::StatusOr<RegressionResult> result;
  gate.Compare("<html>ref</html>", "<html>cand</html>", 10, 10,
               [&](absl::StatusOr<RegressionResult> r) {
                 result = std::move(r);
                 done = true;
               });

  peer_.PumpUntil([&done] { return done; }, 120);

  ASSERT_TRUE(done);
  ASSERT_FALSE(result.ok()) << "a missing candidate render must not compare";
  EXPECT_NE(result.status().message().find("optimized screenshot failed"),
            std::string::npos)
      << result.status().message();
  EXPECT_EQ(shots, 1);
}

TEST_F(VisualRegressionGateCdpTest, DestroyingTheGateCancelsItsCapturesAtOnce) {
  // A capture holds a raw CdpClient* and a 60 s libuv timer. Previously the
  // second one ran on a TEMPORARY gate, which put it beyond every owner's
  // reach: when the client died the timer was the only thing left that could
  // finish it, and it fired into freed memory. Ownership replaces that — a
  // destroyed gate resolves its captures immediately, and fails them, because
  // a browser that went away measured nothing.
  // Hold the attach so the capture is stopped at a KNOWN point in its setup
  // chain: createTarget answered, attachToTarget outstanding. Cancelling here
  // and then releasing the attach is what makes "did the chain keep marching?"
  // an observable question rather than a race with the pump budget.
  int held_attach_id = -1;
  peer_.SetResponder(
      [&](int id, const std::string& method, const json&) -> bool {
        if (method != "Target.attachToTarget") return false;
        held_attach_id = id;
        return true;
      });

  bool done = false;
  absl::StatusOr<RegressionResult> result;
  auto gate = std::make_unique<VisualRegressionGate>(client());
  gate->Compare("<html>ref</html>", "<html>cand</html>", 10, 10,
                [&](absl::StatusOr<RegressionResult> r) {
                  result = std::move(r);
                  done = true;
                });
  peer_.PumpUntil([&] { return held_attach_id >= 0; }, 40);
  ASSERT_GE(held_attach_id, 0) << "precondition: attachToTarget never sent";
  ASSERT_FALSE(done) << "precondition: a capture must be in flight";

  gate.reset();

  ASSERT_TRUE(done) << "an in-flight capture was orphaned by its gate's death";
  EXPECT_FALSE(result.ok()) << "cancellation must not look like a comparison";
  // Cancellation cleans up after itself: the tab it opened is closed.
  peer_.PumpUntil([] { return false; }, 10);
  EXPECT_TRUE(peer_.SawCommand("Target.closeTarget"))
      << "cancellation abandoned the tab it opened";

  // Release the held attach. A cancelled session must NOT resume its setup
  // chain on the answer — the next step would be the viewport override.
  peer_.RespondToCommand(held_attach_id, {{"sessionId", "sess-late"}});
  peer_.PumpUntil([] { return false; }, 20);
  EXPECT_FALSE(peer_.SawCommand("Emulation.setDeviceMetricsOverride"))
      << "a cancelled capture kept marching through its setup sequence";
}

TEST_F(VisualRegressionGateCdpTest, CaptureStartedOnADyingGateIsRefused) {
  // Belt and braces for the reverse order: a callback fired during
  // cancellation must not be able to start a fresh capture on a gate that is
  // halfway through being destroyed.
  auto gate = std::make_unique<VisualRegressionGate>(client());
  // The raw pointer, not the unique_ptr: reset() nulls its handle before it
  // runs the destructor, and the callback below fires from inside it.
  VisualRegressionGate* raw = gate.get();
  bool first_done = false;
  bool second_done = false;
  absl::StatusOr<ScreenshotResult> second;
  gate->CaptureScreenshot(
      "<html>a</html>", 10, 10, [&](absl::StatusOr<ScreenshotResult>) {
        first_done = true;
        raw->CaptureScreenshot("<html>b</html>", 10, 10,
                               [&](absl::StatusOr<ScreenshotResult> r) {
                                 second = std::move(r);
                                 second_done = true;
                               });
      });
  peer_.PumpUntil([&] { return peer_.SawCommand("Target.createTarget"); }, 20);

  gate.reset();

  EXPECT_TRUE(first_done);
  ASSERT_TRUE(second_done);
  EXPECT_FALSE(second.ok())
      << "a capture was started on a gate being destroyed";
}

TEST_F(VisualRegressionGateCdpTest, CompareLeavesNoEventSubscriberBehind) {
  // Each capture registers the client's single event callback and releases it
  // on completion.  If the second capture's registration were clobbered by the
  // first's release, the run would hang rather than fail — so assert the run
  // completes AND that a post-run event reaches nobody (no further commands).
  auto grey = EncodePng(MakeSolidImage(10, 10, 128, 128, 128), 10, 10);
  int shots = 0;
  peer_.SetResponder(DriveSequentialCaptures(
      &peer_, [&](int) { return test::Base64Encode(grey); }, &shots));

  VisualRegressionGate gate(client());
  bool done = false;
  gate.Compare("<html>ref</html>", "<html>cand</html>", 10, 10,
               [&](absl::StatusOr<RegressionResult>) { done = true; });
  peer_.PumpUntil([&done] { return done; }, 120);
  ASSERT_TRUE(done);

  size_t commands_at_completion = peer_.received_commands().size();
  peer_.SendEvent("Fetch.requestPaused",
                  {{"requestId", "late"},
                   {"request", {{"url", "http://evil.com/late.png"}}}},
                  peer_.session_id(2));
  peer_.PumpUntil([] { return false; }, 10);
  EXPECT_EQ(peer_.received_commands().size(), commands_at_completion)
      << "a stale event subscriber was still attached after Compare finished";
}

// The critical-CSS validator refuses a comparison whose reference is one
// uniform colour; the gate reports it and leaves `passed` alone.
TEST(VisualRegressionGateTest, ReportsAUniformReference) {
  const uint32_t w = 10, h = 10;
  auto solid = MakeSolidImage(w, h, 255, 255, 255);
  auto solid_png = EncodePng(solid, w, h);
  auto result =
      VisualRegressionGate::CompareScreenshots(solid_png, solid_png, h);
  EXPECT_TRUE(result.passed);
  EXPECT_TRUE(result.reference_uniform);

  auto two_tone = solid;
  for (size_t i = 0; i < 4; ++i) two_tone[i] = 0;  // one pixel's channels
  auto two_tone_png = EncodePng(two_tone, w, h);
  result =
      VisualRegressionGate::CompareScreenshots(two_tone_png, two_tone_png, h);
  EXPECT_TRUE(result.passed);
  EXPECT_FALSE(result.reference_uniform);
  // Only the reference counts: a blank candidate is an ordinary diff.
  result = VisualRegressionGate::CompareScreenshots(two_tone_png, solid_png, h);
  EXPECT_FALSE(result.reference_uniform);
}

}  // namespace
}  // namespace pagespeed
