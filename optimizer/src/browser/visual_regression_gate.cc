// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// 5. Fetch.enable + Fetch.failRequest for all requests  (SSRF defense)
// 6. Emulation.setScriptExecutionDisabled({value: true}) (SSRF defense)
// 7. Page.enable, Page.navigate(about:blank) for a fresh window
//    (LoadFreshBlankDocument), Page.setLifecycleEventsEnabled
// 8. Page.setDocumentContent({frameId, html})
// 9. Wait for Page.lifecycleEvent("networkIdle")
// 10. Page.captureScreenshot({format: "png", clip: viewport})
// 11. Target.closeTarget({targetId})
//
// CompareScreenshots decodes both PNGs and compares pixel-by-pixel
// in the above-fold region with per-channel tolerance for anti-aliasing.

#include "src/browser/visual_regression_gate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "nlohmann/json.hpp"
#include "png.h"  // NOLINT
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/device_emulation.h"
#include "src/browser/render_support.h"
#include "uv.h"  // NOLINT

namespace pagespeed {

using json = nlohmann::json;

namespace {

// Base64 decode a string. Returns empty vector on failure.
std::vector<uint8_t> Base64Decode(std::string_view input) {
  // Standard base64 alphabet lookup table.
  static constexpr int8_t kTable[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57,
      58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
      7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
      25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
      37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1,
  };

  std::vector<uint8_t> output;
  output.reserve((input.size() * 3) / 4);

  uint32_t buffer = 0;
  int bits = 0;

  for (char c : input) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    int8_t val = kTable[static_cast<uint8_t>(c)];
    if (val < 0) return {};  // Invalid character.
    buffer = (buffer << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
    }
  }

  return output;
}

// RAII wrapper for PNG read state.
struct PngReadState {
  const uint8_t* data;
  size_t size;
  size_t offset;
};

void PngReadCallback(png_structp png, png_bytep out, png_size_t count) {
  auto* state = static_cast<PngReadState*>(png_get_io_ptr(png));
  if (state->offset + count > state->size) {
    png_error(png, "read past end of data");
    return;
  }
  std::memcpy(out, state->data + state->offset, count);
  state->offset += count;
}

// Decode a PNG from memory into RGBA pixels.
// Returns empty vector on failure. Sets width/height.
std::vector<uint8_t> DecodePng(const std::vector<uint8_t>& png_data,
                               uint32_t* out_width, uint32_t* out_height) {
  if (png_data.size() < 8) return {};

  png_structp png =
      png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (!png) return {};

  // Decompression bomb protection: limit to 16384x16384.
  png_set_user_limits(png, 16384, 16384);

  png_infop info = png_create_info_struct(png);
  if (!info) {
    png_destroy_read_struct(&png, nullptr, nullptr);
    return {};
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    return {};
  }

  PngReadState state{png_data.data(), png_data.size(), 0};
  png_set_read_fn(png, &state, PngReadCallback);

  png_read_info(png, info);

  uint32_t width = png_get_image_width(png, info);
  uint32_t height = png_get_image_height(png, info);
  png_byte color_type = png_get_color_type(png, info);
  png_byte bit_depth = png_get_bit_depth(png, info);

  // Convert to RGBA 8-bit.
  if (bit_depth == 16) png_set_strip_16(png);
  if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
    png_set_expand_gray_1_2_4_to_8(png);
  }
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_PALETTE) {
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  }
  if (color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
    png_set_gray_to_rgb(png);
  }

  png_read_update_info(png, info);

  // Belt-and-suspenders overflow check (png_set_user_limits is primary).
  if (width > 16384 || height > 16384) {
    png_destroy_read_struct(&png, &info, nullptr);
    return {};
  }

  size_t row_bytes = png_get_rowbytes(png, info);
  // After RGBA transforms, row_bytes must equal width*4 (no padding).
  // CompareScreenshots assumes contiguous RGBA, so reject padded rows.
  if (row_bytes != static_cast<size_t>(width) * 4) {
    png_destroy_read_struct(&png, &info, nullptr);
    return {};  // Unexpected row padding.
  }

  std::vector<uint8_t> pixels(row_bytes * height);
  std::vector<png_bytep> row_ptrs(height);
  for (uint32_t y = 0; y < height; ++y) {
    row_ptrs[y] = pixels.data() + y * row_bytes;
  }

  png_read_image(png, row_ptrs.data());
  png_destroy_read_struct(&png, &info, nullptr);

  *out_width = width;
  *out_height = height;
  return pixels;
}

}  // namespace

// Internal session state for screenshot capture.
struct VisualRegressionGate::Session
    : public std::enable_shared_from_this<Session> {
  CdpClient* client;
  std::string html_content;
  uint32_t viewport_width;
  uint32_t viewport_height;
  uint32_t timeout_ms;
  bool completed = false;
  // A refused Fetch.failRequest was reported already (FailPausedRequest).
  bool fetch_error_reported = false;

  std::string target_id;
  std::string session_id;
  std::string frame_id;
  // The requested document is in the frame.  A capture starts life in an
  // about:blank tab, which settles on its own; a lifecycle event arriving
  // before this is set describes THAT page, and screenshotting on it
  // photographs a blank tab and reports it as the page.  Fail-safe direction:
  // waiting too long ends at the session timeout (an error), while not waiting
  // long enough silently returns a blank picture that compares equal to any
  // other blank picture.
  bool content_set = false;
  bool got_network_idle = false;

  // Overall session timeout timer.
  uv_timer_t* timeout_timer = nullptr;

  std::function<void(absl::StatusOr<ScreenshotResult>)> callback;

  void StartTimeout() {
    if (timeout_ms == 0) return;
    timeout_timer = new uv_timer_t;
    uv_timer_init(client->loop(), timeout_timer);
    auto weak = std::weak_ptr<Session>(shared_from_this());
    timeout_timer->data = new std::weak_ptr<Session>(weak);
    uv_timer_start(
        timeout_timer,
        [](uv_timer_t* t) {
          auto* wp = static_cast<std::weak_ptr<Session>*>(t->data);
          auto s = wp->lock();
          delete wp;
          t->data = nullptr;
          if (s) {
            s->FinishError("session timeout");
          }
          uv_close(reinterpret_cast<uv_handle_t*>(t),
                   [](uv_handle_t* h) { delete (uv_timer_t*)h; });
        },
        timeout_ms, 0);
  }

  bool Send(const CdpCommand& cmd, CdpResponseCallback cb) {
    // A finished session — including a cancelled one — must stop driving the
    // chain. Without this the setup sequence marches on after cancellation,
    // creating targets for a capture whose answer has already been given.
    if (completed) return false;
    auto status = client->SendCommand(cmd, std::move(cb));
    if (!status.ok()) {
      FinishError(
          absl::StrCat("SendCommand failed: ", status.status().message()));
      return false;
    }
    return true;
  }

  void Finish(absl::StatusOr<ScreenshotResult> result) {
    if (completed) return;
    completed = true;

    // Cancel the overall session timeout.
    if (timeout_timer != nullptr && timeout_timer->data != nullptr) {
      auto* wp = static_cast<std::weak_ptr<Session>*>(timeout_timer->data);
      delete wp;
      timeout_timer->data = nullptr;
      uv_timer_stop(timeout_timer);
      uv_close(reinterpret_cast<uv_handle_t*>(timeout_timer),
               [](uv_handle_t* h) { delete (uv_timer_t*)h; });
      timeout_timer = nullptr;
    }

    // Close the target (best effort).
    if (!target_id.empty() && client->connected()) {
      CdpCommand close_cmd;
      close_cmd.method = "Target.closeTarget";
      close_cmd.params = {{"targetId", target_id}};
      close_cmd.timeout_ms = 5000;
      auto status = client->SendCommand(close_cmd, [](auto) {});
      (void)status;
    }

    // Release the event callback.
    client->SetEventCallback(nullptr);

    if (callback) {
      callback(std::move(result));
    }
  }

  void FinishError(std::string_view msg) { Finish(absl::InternalError(msg)); }

  // Take screenshot once the page is idle.
  void TakeScreenshot() {
    CdpCommand screenshot_cmd;
    screenshot_cmd.method = "Page.captureScreenshot";
    screenshot_cmd.params = {
        {"format", "png"},
        {"clip",
         {{"x", 0},
          {"y", 0},
          {"width", viewport_width},
          {"height", viewport_height},
          {"scale", 1}}},
    };
    screenshot_cmd.session_id = session_id;
    screenshot_cmd.timeout_ms = 15000;

    auto self = shared_from_this();
    Send(screenshot_cmd, [self](absl::StatusOr<CdpResponse> result) {
      if (!result.ok() || result->is_error()) {
        self->FinishError("captureScreenshot failed");
        return;
      }

      std::string b64_data = result->result.value("data", "");
      if (b64_data.empty()) {
        self->FinishError("captureScreenshot: empty data");
        return;
      }

      auto png_data = Base64Decode(b64_data);
      if (png_data.empty()) {
        self->FinishError("captureScreenshot: base64 decode failed");
        return;
      }

      ScreenshotResult sr;
      sr.png_data = std::move(png_data);
      sr.width = self->viewport_width;
      sr.height = self->viewport_height;
      self->Finish(sr);
    });
  }
};

VisualRegressionGate::VisualRegressionGate(CdpClient* client)
    : client_(client) {}

VisualRegressionGate::~VisualRegressionGate() {
  destroying_ = true;
  // Cancel everything still in flight, while the client is still alive to be
  // told about it. Finish() stops the timeout timer, closes the target and
  // releases the event subscriber; skipping that is what leaves a timer to
  // fire into a freed client later.
  for (const auto& weak : sessions_) {
    if (auto session = weak.lock()) {
      session->FinishError("visual comparison cancelled");
    }
  }
  sessions_.clear();
}

void VisualRegressionGate::PruneSessions() {
  std::erase_if(sessions_, [](const std::weak_ptr<Session>& w) {
    auto s = w.lock();
    return s == nullptr || s->completed;
  });
}

void VisualRegressionGate::CaptureScreenshot(
    std::string_view html_content, uint32_t viewport_width,
    uint32_t viewport_height,
    std::function<void(absl::StatusOr<ScreenshotResult>)> callback) {
  if (destroying_) {
    // Refuse rather than start a capture nothing will be able to cancel.
    callback(absl::InternalError("visual comparison cancelled"));
    return;
  }
  auto session = std::make_shared<Session>();
  session->client = client_;
  session->callback = std::move(callback);
  session->html_content = std::string(html_content);
  session->viewport_width = viewport_width;
  session->viewport_height = viewport_height;
  session->timeout_ms = kDefaultTimeoutMs;

  PruneSessions();
  sessions_.push_back(session);

  session->StartTimeout();

  // Step 1: Create target.
  CdpCommand create_cmd;
  create_cmd.method = "Target.createTarget";
  create_cmd.params = {{"url", "about:blank"}};
  create_cmd.timeout_ms = 10000;

  auto send_result = client_->SendCommand(
      create_cmd, [session](absl::StatusOr<CdpResponse> result) {
        if (!result.ok() || result->is_error()) {
          session->FinishError("createTarget failed");
          return;
        }
        session->target_id = result->result.value("targetId", "");
        if (session->target_id.empty()) {
          session->FinishError("createTarget: no targetId");
          return;
        }

        // Step 2: Attach to target.
        CdpCommand attach_cmd;
        attach_cmd.method = "Target.attachToTarget";
        attach_cmd.params = {
            {"targetId", session->target_id},
            {"flatten", true},
        };
        attach_cmd.timeout_ms = 10000;

        session->Send(attach_cmd, [session](
                                      absl::StatusOr<CdpResponse> result) {
          if (!result.ok() || result->is_error()) {
            session->FinishError("attachToTarget failed");
            return;
          }
          session->session_id = result->result.value("sessionId", "");
          if (session->session_id.empty()) {
            session->FinishError("no sessionId");
            return;
          }

          auto s =  // NOLINT(performance-unnecessary-copy-initialization)
              session;

          // Step 3: Emulate the device (metrics, touch, UA).
          EmulateDevice(s, s->viewport_width, s->viewport_height, [s]() {
            // Step 4: Network offline (SSRF).
            CdpCommand offline_cmd;
            offline_cmd.method = "Network.emulateNetworkConditions";
            offline_cmd.params = {
                {"offline", true},
                {"latency", 0},
                {"downloadThroughput", -1},
                {"uploadThroughput", -1},
            };
            offline_cmd.session_id = s->session_id;

            s->Send(offline_cmd, [s](auto result) {
              if (!result.ok() || result->is_error()) {
                s->FinishError("Network offline setup failed");
                return;
              }
              // Step 5: Fetch interception (SSRF).
              CdpCommand fetch_cmd;
              fetch_cmd.method = "Fetch.enable";
              fetch_cmd.params = {
                  {"patterns", json::array({{{"urlPattern", "*"}}})}};
              fetch_cmd.session_id = s->session_id;

              s->Send(fetch_cmd, [s](auto result) {
                if (!result.ok() || result->is_error()) {
                  s->FinishError("Fetch.enable setup failed");
                  return;
                }
                // Step 6: Disable JS (SSRF).
                CdpCommand js_cmd;
                js_cmd.method =
                    "Emulation."
                    "setScriptExecutionDisabled";
                js_cmd.params = {{"value", true}};
                js_cmd.session_id = s->session_id;

                s->Send(js_cmd, [s](auto result) {
                  if (!result.ok() || result->is_error()) {
                    s->FinishError("JS disable setup failed");
                    return;
                  }
                  // Step 7: Enable Page + lifecycle.
                  CdpCommand page_cmd;
                  page_cmd.method = "Page.enable";
                  page_cmd.session_id = s->session_id;

                  s->Send(page_cmd, [s](auto result) {
                    if (!result.ok() || result->is_error()) {
                      s->FinishError("Page.enable failed");
                      return;
                    }
                    // A fresh window for the document (LoadFreshBlankDocument).
                    LoadFreshBlankDocument(s, [s]() {
                      CdpCommand lc_cmd;
                      lc_cmd.method =
                          "Page."
                          "setLifecycleEventsEnabled";
                      lc_cmd.params = {{"enabled", true}};
                      lc_cmd.session_id = s->session_id;

                      s->Send(lc_cmd, [s](auto result) {
                        if (!result.ok() || result->is_error()) {
                          s->FinishError(
                              "lifecycle enable "
                              "failed");
                          return;
                        }
                        // Step 8: Get frame + set
                        // content.
                        CdpCommand tree_cmd;
                        tree_cmd.method = "Page.getFrameTree";
                        tree_cmd.session_id = s->session_id;

                        s->Send(tree_cmd,
                                [s](absl::StatusOr<CdpResponse> result) {
                                  if (!result.ok() || result->is_error()) {
                                    s->FinishError(
                                        "getFrameTree "
                                        "failed");
                                    return;
                                  }
                                  s->frame_id =
                                      result->result
                                          .value("frameTree", json::object())
                                          .value("frame", json::object())
                                          .value("id", "");

                                  CdpCommand content_cmd;
                                  content_cmd.method =
                                      "Page."
                                      "setDocumentContent";
                                  content_cmd.params = {
                                      {"frameId", s->frame_id},
                                      {"html", s->html_content},
                                  };
                                  content_cmd.session_id = s->session_id;
                                  content_cmd.timeout_ms = 15000;

                                  s->Send(content_cmd, [s](auto r) {
                                    s->html_content.clear();
                                    s->html_content.shrink_to_fit();
                                    if (!r.ok() || r->is_error()) {
                                      s->FinishError(
                                          "setDocument"
                                          "Content "
                                          "failed");
                                      return;
                                    }
                                    // From here a networkIdle describes the
                                    // document we asked for.
                                    s->content_set = true;
                                  });
                                });
                      });
                    });
                  });
                });
              });
            });
          });
        });
      });

  if (!send_result.ok()) {
    session->FinishError(
        absl::StrCat("SendCommand failed: ", send_result.status().message()));
    return;
  }

  // Set event callback for lifecycle + Fetch interception.
  client_->SetEventCallback([session](const CdpEvent& event) {
    if (session->completed) return;
    if (event.session_id != session->session_id) return;

    // Fail all network requests (SSRF defense).
    if (event.method == "Fetch.requestPaused") {
      FailPausedRequest(session, event.params.value("requestId", ""),
                        "validation render");
      return;
    }

    if (event.method == "Page.lifecycleEvent") {
      std::string name = event.params.value("name", "");
      // `content_set` is load-bearing, not defensive: the tab is created at
      // about:blank and lifecycle events are enabled before the document is
      // written, so an unguarded networkIdle here screenshots a blank tab.
      if (name == "networkIdle" && session->content_set &&
          !session->got_network_idle) {
        session->got_network_idle = true;
        session->TakeScreenshot();
      }
    }
  });
}

void VisualRegressionGate::Compare(std::string_view original_html,
                                   std::string_view optimized_html,
                                   uint32_t viewport_width,
                                   uint32_t viewport_height, Callback callback,
                                   float threshold) {
  // Capture original screenshot first, then optimized.
  auto original_html_str = std::string(original_html);
  auto optimized_html_str = std::string(optimized_html);

  // Sequential, because CdpClient supports one event subscriber at a time.
  //
  // The second capture runs on THIS gate, not on a temporary one. The
  // temporary existed so the caller's gate need not outlive the first capture
  // — but that also put a capture beyond the reach of every owner, holding a
  // raw CdpClient* and a 60 s timer nobody could cancel. Ownership is the
  // answer instead: the gate outlives its run because its owner keeps it, and
  // destroying it cancels both captures (see the header's lifetime note).
  CaptureScreenshot(
      original_html_str, viewport_width, viewport_height,
      [this, optimized_html_str, viewport_width, viewport_height, threshold,
       callback = std::move(callback)](
          absl::StatusOr<ScreenshotResult> original_result) mutable {
        if (!original_result.ok()) {
          callback(absl::InternalError(
              absl::StrCat("original screenshot failed: ",
                           original_result.status().message())));
          return;
        }

        auto original_png = std::make_shared<std::vector<uint8_t>>(
            std::move(original_result->png_data));

        CaptureScreenshot(
            optimized_html_str, viewport_width, viewport_height,
            [original_png, viewport_height, threshold,
             callback = std::move(callback)](
                absl::StatusOr<ScreenshotResult> optimized_result) mutable {
              if (!optimized_result.ok()) {
                callback(absl::InternalError(
                    absl::StrCat("optimized screenshot failed: ",
                                 optimized_result.status().message())));
                return;
              }

              auto result =
                  CompareScreenshots(*original_png, optimized_result->png_data,
                                     viewport_height, threshold);
              callback(result);
            });
      });
}

RegressionResult VisualRegressionGate::CompareScreenshots(
    const std::vector<uint8_t>& original_png,
    const std::vector<uint8_t>& optimized_png, uint32_t viewport_height,
    float threshold) {
  RegressionResult result;
  result.above_fold_height = viewport_height;

  // Decode both PNGs.
  uint32_t orig_w = 0, orig_h = 0;
  auto orig_pixels = DecodePng(original_png, &orig_w, &orig_h);
  if (orig_pixels.empty()) {
    // Cannot decode original -- fail the gate.
    return result;
  }

  uint32_t opt_w = 0, opt_h = 0;
  auto opt_pixels = DecodePng(optimized_png, &opt_w, &opt_h);
  if (opt_pixels.empty()) {
    return result;
  }

  // Compare only the above-fold region.
  uint32_t compare_w = std::min(orig_w, opt_w);
  uint32_t compare_h = std::min({orig_h, opt_h, viewport_height});

  if (compare_w == 0 || compare_h == 0) {
    result.passed = true;
    return result;
  }

  result.total_pixels = compare_w * compare_h;
  uint32_t diff_count = 0;
  bool reference_uniform = true;

  for (uint32_t y = 0; y < compare_h; ++y) {
    for (uint32_t x = 0; x < compare_w; ++x) {
      // RGBA, 4 bytes per pixel.
      size_t orig_idx = (static_cast<size_t>(y) * orig_w + x) * 4;
      size_t opt_idx = (static_cast<size_t>(y) * opt_w + x) * 4;

      for (int c = 0; c < 4 && reference_uniform; ++c) {
        if (std::abs(static_cast<int>(orig_pixels[orig_idx + c]) -
                     static_cast<int>(orig_pixels[c])) > kChannelTolerance) {
          reference_uniform = false;
        }
      }

      bool pixel_differs = false;
      for (int c = 0; c < 4; ++c) {
        int diff = std::abs(static_cast<int>(orig_pixels[orig_idx + c]) -
                            static_cast<int>(opt_pixels[opt_idx + c]));
        if (diff > kChannelTolerance) {
          pixel_differs = true;
          break;
        }
      }
      if (pixel_differs) {
        ++diff_count;
      }
    }
  }

  result.reference_uniform = reference_uniform;
  result.diff_pixels = diff_count;
  result.diff_ratio =
      static_cast<float>(diff_count) / static_cast<float>(result.total_pixels);
  result.passed = result.diff_ratio <= threshold;

  return result;
}

}  // namespace pagespeed
