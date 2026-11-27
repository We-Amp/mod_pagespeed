// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Font Glyph Scanner Tests
//
// Unit tests for CodePointsToUnicodeRange and mock pipe tests for
// the full scanner CDP workflow.

#include "src/browser/font_glyph_scanner.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/cdp_types.h"
#include "src/browser/render_support.h"
#include "test/test_util/cdp_pipe.h"
#include "test/test_util/cdp_setup_order.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// The settle-after-load bound in the tests (kSettleAfterLoadMs is 2 s).
constexpr uint64_t kTestSettleMs = 50;

// Test fixture for CDP-based Scan() tests.
class FontGlyphScannerCdpTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    // Chrome -> Client pipe pair.
    ASSERT_TRUE(chrome_to_client_.Create());
    // Client -> Chrome pipe pair.
    ASSERT_TRUE(client_to_chrome_.Create());

    // Raw FD for test to write simulated Chrome responses.
    chrome_write_fd_ = chrome_to_client_.write_fd;

    // Client-side pipe handles (wrapped in libuv).
    uv_pipe_init(loop_, &read_pipe_, 0);
    uv_pipe_open(&read_pipe_, chrome_to_client_.TakeReadFd());

    uv_pipe_init(loop_, &client_write_, 0);
    uv_pipe_open(&client_write_, client_to_chrome_.TakeWriteFd());

    // Raw FD for test to read client writes (track commands).
    test_read_fd_ = client_to_chrome_.read_fd;
    ASSERT_TRUE(test::SetNonBlocking(test_read_fd_));

    client_ = std::make_unique<CdpClient>(loop_);
    ASSERT_TRUE(client_->AttachPipes(&read_pipe_, &client_write_).ok());
  }

  void TearDown() override {
    client_.reset();

    // Close all libuv handles.
    uv_walk(
        loop_,
        [](uv_handle_t* h, void*) {
          if (!uv_is_closing(h)) uv_close(h, nullptr);
        },
        nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);

    // Close test-owned raw FDs (not owned by libuv).
    chrome_to_client_.CloseWrite();
    client_to_chrome_.CloseRead();

    uv_loop_close(loop_);
    delete loop_;
  }

  // Send a CDP message from "Chrome" to the client via raw write().
  void SendFromChrome(const std::string& json_str) {
    std::string msg = json_str;
    msg.push_back('\0');
    ssize_t n = test::PipeWrite(chrome_write_fd_, msg.data(), msg.size());
    (void)n;
  }

  // Poll for commands sent by the client via raw non-blocking read().
  void PollCommands() {
    char buf[4096];
    while (true) {
      ssize_t n = test::PipeRead(test_read_fd_, buf, sizeof(buf));
      if (n <= 0) break;
      received_data_.append(buf, static_cast<size_t>(n));
    }
    size_t pos;
    while ((pos = received_data_.find('\0')) != std::string::npos) {
      std::string msg = received_data_.substr(0, pos);
      received_data_.erase(0, pos + 1);
      try {
        received_commands_.push_back(json::parse(msg));
      } catch (...) {  // NOLINT(bugprone-empty-catch)
      }
    }
  }

  // Run the event loop briefly to process I/O.
  void RunLoop() {
    for (int i = 0; i < 5; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
    PollCommands();
  }

  // Run the event loop until a condition is met or timeout.
  // Uses UV_RUN_NOWAIT + usleep to avoid UV_RUN_ONCE blocking
  // issues on Linux in Docker (epoll + pipe fd interaction).
  void RunLoopUntil(std::function<bool()> condition,
                    int max_iterations = 5000) {
    for (int i = 0; i < max_iterations; i++) {
      if (condition()) return;
      uv_run(loop_, UV_RUN_NOWAIT);
      PollCommands();
      test::SleepUs(1000);  // 1ms
    }
  }

  void RespondToCommand(int id, const json& result) {
    json response = {{"id", id}, {"result", result}};
    SendFromChrome(response.dump());
  }

  void RespondError(int id, int code, const std::string& msg) {
    json response = {{"id", id}, {"error", {{"code", code}, {"message", msg}}}};
    SendFromChrome(response.dump());
  }

  void SendEvent(const std::string& method, const json& params,
                 const std::string& session_id = "") {
    json event = {{"method", method}, {"params", params}};
    if (!session_id.empty()) {
      event["sessionId"] = session_id;
    }
    SendFromChrome(event.dump());
  }

  void AutoRespondAll(const std::string& eval_result_json = "") {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Page.setLifecycleEventsEnabled" &&
          replay_blank_lifecycle_) {
        // What Chromium does: enabling lifecycle events
        // replays the current document's lifecycle, the blank tab's
        // networkIdle included, ahead of the reply.
        for (const char* name :
             {"commit", "DOMContentLoaded", "load", "networkIdle"}) {
          SendEvent("Page.lifecycleEvent",
                    {{"name", name}, {"loaderId", "blank-loader"}}, "sess-1");
        }
        RespondToCommand(id, json::object());
        continue;
      }
      if (method == "Page.setDocumentContent" &&
          (load_before_content_reply_ || hold_content_reply_)) {
        // On a page with nothing left to fetch, Chromium fires the page's own
        // load before it answers setDocumentContent.
        SendEvent("Page.lifecycleEvent",
                  {{"name", "load"}, {"loaderId", "blank-loader"}}, "sess-1");
        if (hold_content_reply_) {
          held_content_reply_id_ = id;
        } else {
          RespondToCommand(id, json::object());
        }
        continue;
      }
      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        if (!eval_result_json.empty()) {
          RespondToCommand(
              id,
              {{"result", {{"type", "string"}, {"value", eval_result_json}}}});
        } else {
          // Default: simple result with some codepoints.
          json result;
          result["text_nodes"] = 3;
          result["codepoints"] = json::array({0x48, 0x65, 0x6C, 0x6C, 0x6F});
          result["fonts"] = json::array({{{"family", "Open Sans"},
                                          {"src_url", "/fonts/open-sans.woff2"},
                                          {"format", "woff2"},
                                          {"weight", "400"},
                                          {"style", "normal"}}});
          RespondToCommand(
              id, {{"result", {{"type", "string"}, {"value", result.dump()}}}});
        }
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  // Fire the page's load ahead of the setDocumentContent reply, as Chromium
  // does on a page with nothing left to fetch; with hold_content_reply_ the
  // reply is held (held_content_reply_id_) until the test sends it.
  bool load_before_content_reply_ = false;
  bool hold_content_reply_ = false;
  int held_content_reply_id_ = -1;
  // Index of the first received command with `method`, or -1.
  int IndexOf(const std::string& method) const {
    for (size_t i = 0; i < received_commands_.size(); ++i) {
      if (received_commands_[i].value("method", "") == method) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  // Answer and pump until `condition` holds (test::PumpUntil).
  void PumpUntil(const std::function<bool()>& condition) {
    test::PumpUntil(
        [this] {
          AutoRespondAll();
          RunLoop();
        },
        condition);
  }
  // Let the settle timer, armed by an event already sent, run out
  // (test::RunPastTimer).
  void RunOutTheSettleBound() {
    test::RunPastTimer([this] { RunLoop(); },
                       std::chrono::milliseconds(kTestSettleMs));
  }
  // Replay the blank tab's lifecycle on Page.setLifecycleEventsEnabled, as
  // Chromium does.
  bool replay_blank_lifecycle_ = false;
  uv_loop_t* loop_ = nullptr;
  test::PipePair chrome_to_client_;
  test::PipePair client_to_chrome_;
  uv_pipe_t read_pipe_{};
  uv_pipe_t client_write_{};
  int test_read_fd_ = -1;
  int chrome_write_fd_ = -1;
  std::unique_ptr<CdpClient> client_;
  bool scan_done_ = false;
  absl::StatusOr<FontGlyphResult> scan_result_;
  std::vector<json> received_commands_;
  std::string received_data_;
  std::set<int> responded_ids_;
};

// ---- CodePointsToUnicodeRange unit tests ----

TEST(CodePointsToUnicodeRangeTest, EmptyInput) {
  std::vector<uint32_t> codepoints;
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints), "");
}

TEST(CodePointsToUnicodeRangeTest, SingleCodePoint) {
  std::vector<uint32_t> codepoints = {0x41};  // 'A'
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints), "U+0041");
}

TEST(CodePointsToUnicodeRangeTest, TwoConsecutiveCodePoints) {
  std::vector<uint32_t> codepoints = {0x41, 0x42};  // 'A', 'B'
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0041-0042");
}

TEST(CodePointsToUnicodeRangeTest, ConsecutiveRange) {
  // 'A' through 'Z' = 0x41..0x5A
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 0x41; cp <= 0x5A; ++cp) {
    codepoints.push_back(cp);
  }
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0041-005A");
}

TEST(CodePointsToUnicodeRangeTest, DisjointSingles) {
  std::vector<uint32_t> codepoints = {0x20, 0x41, 0x61};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0020,U+0041,U+0061");
}

TEST(CodePointsToUnicodeRangeTest, MixedSinglesAndRanges) {
  // 0x20 (space), 0x41-0x43 (A-C), 0x61 (a)
  std::vector<uint32_t> codepoints = {0x20, 0x41, 0x42, 0x43, 0x61};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0020,U+0041-0043,U+0061");
}

TEST(CodePointsToUnicodeRangeTest, TypicalAsciiRange) {
  // Printable ASCII: 0x20 through 0x7E
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 0x20; cp <= 0x7E; ++cp) {
    codepoints.push_back(cp);
  }
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0020-007E");
}

TEST(CodePointsToUnicodeRangeTest, UnsortedInput) {
  // Should sort before grouping.
  std::vector<uint32_t> codepoints = {0x43, 0x41, 0x42};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0041-0043");
}

TEST(CodePointsToUnicodeRangeTest, DuplicatesRemoved) {
  std::vector<uint32_t> codepoints = {0x41, 0x41, 0x42, 0x42, 0x43};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0041-0043");
}

TEST(CodePointsToUnicodeRangeTest, LatinExtendedRange) {
  // Latin Extended-A: U+0100-017F (some characters)
  std::vector<uint32_t> codepoints = {0x0100, 0x0101, 0x0102, 0x0110};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0100-0102,U+0110");
}

TEST(CodePointsToUnicodeRangeTest, AstralPlane) {
  // Emoji: U+1F600 (grinning face)
  std::vector<uint32_t> codepoints = {0x1F600, 0x1F601, 0x1F602};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+1F600-1F602");
}

TEST(CodePointsToUnicodeRangeTest, MultipleDisjointRanges) {
  // Basic Latin + Latin Extended + CJK
  std::vector<uint32_t> codepoints = {
      0x20,   0x21,   0x22,  // U+0020-0022
      0x100,  0x101,         // U+0100-0101
      0x4E00, 0x4E01,        // CJK range
  };
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+0020-0022,U+0100-0101,U+4E00-4E01");
}

// ---- FontGlyphResult struct tests ----

TEST(FontGlyphResultTest, DefaultValues) {
  FontGlyphResult result;
  EXPECT_TRUE(result.fonts.empty());
  EXPECT_TRUE(result.all_used_codepoints.empty());
  EXPECT_EQ(result.total_text_nodes, 0u);
}

TEST(FontUsageTest, DefaultValues) {
  FontUsage usage;
  EXPECT_TRUE(usage.family.empty());
  EXPECT_TRUE(usage.src_url.empty());
  EXPECT_TRUE(usage.format.empty());
  EXPECT_EQ(usage.weight, "");
  EXPECT_EQ(usage.style, "");
  EXPECT_TRUE(usage.unicode_range.empty());
  EXPECT_EQ(usage.total_glyphs, 0u);
  EXPECT_EQ(usage.used_glyphs, 0u);
}

// ---- Additional coverage: emoji, surrogate boundaries, large input ----

TEST(CodePointsToUnicodeRangeTest, EmojiRange) {
  // U+1F600-1F64F = Emoticons range (subset)
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 0x1F600; cp <= 0x1F64F; ++cp) {
    codepoints.push_back(cp);
  }
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+1F600-1F64F");
}

TEST(CodePointsToUnicodeRangeTest, SurrogateBoundary) {
  // U+FFFF and U+10000 are consecutive but span the BMP boundary.
  std::vector<uint32_t> codepoints = {0xFFFF, 0x10000};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+FFFF-10000");
}

TEST(CodePointsToUnicodeRangeTest, SingleHighCodePoint) {
  std::vector<uint32_t> codepoints = {0x10FFFF};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints), "U+10FFFF");
}

TEST(CodePointsToUnicodeRangeTest, LargeInput) {
  // 1000 codepoints: printable ASCII extended to test performance.
  std::vector<uint32_t> codepoints;
  for (uint32_t cp = 0x20; cp < 0x20 + 1000; ++cp) {
    codepoints.push_back(cp);
  }
  auto result = FontGlyphScanner::CodePointsToUnicodeRange(codepoints);
  // Should be a single range: U+0020-0407
  EXPECT_EQ(result, "U+0020-0407");
}

TEST(CodePointsToUnicodeRangeTest, AllDuplicates) {
  std::vector<uint32_t> codepoints = {0x41, 0x41, 0x41, 0x41};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints), "U+0041");
}

TEST(CodePointsToUnicodeRangeTest, CjkUnifiedIdeographs) {
  // CJK Unified Ideographs: U+4E00-9FFF (just a small range).
  std::vector<uint32_t> codepoints = {0x4E00, 0x4E01, 0x4E02, 0x4E10, 0x4E11};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints),
            "U+4E00-4E02,U+4E10-4E11");
}

TEST(CodePointsToUnicodeRangeTest, ZeroCodePoint) {
  std::vector<uint32_t> codepoints = {0x0000};
  EXPECT_EQ(FontGlyphScanner::CodePointsToUnicodeRange(codepoints), "U+0000");
}

TEST(FontGlyphResultTest, PopulatedResult) {
  FontGlyphResult result;
  FontUsage font;
  font.family = "Open Sans";
  font.used_glyphs = 42;
  result.fonts.push_back(font);
  // all_used_codepoints is a std::string (unicode-range format).
  result.all_used_codepoints = "U+0041-0043";
  result.total_text_nodes = 15;

  EXPECT_EQ(result.fonts.size(), 1u);
  EXPECT_EQ(result.all_used_codepoints, "U+0041-0043");
  EXPECT_EQ(result.total_text_nodes, 15u);
}

// ---- CDP-based Scan() tests ----

TEST_F(FontGlyphScannerCdpTest, SuccessfulScan) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html><body><p>Hello</p></body></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> result) {
                 scan_result_ = std::move(result);
                 scan_done_ = true;
               });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
    if (scan_done_) break;
  }

  // Send networkIdle.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (scan_done_) break;
  }

  ASSERT_TRUE(scan_done_);
  ASSERT_TRUE(scan_result_.ok()) << scan_result_.status().message();
  EXPECT_EQ(scan_result_->total_text_nodes, 3u);
  EXPECT_FALSE(scan_result_->all_used_codepoints.empty());
  ASSERT_EQ(scan_result_->fonts.size(), 1u);
  EXPECT_EQ(scan_result_->fonts[0].family, "Open Sans");
  EXPECT_EQ(scan_result_->fonts[0].src_url, "/fonts/open-sans.woff2");
  EXPECT_EQ(scan_result_->fonts[0].format, "woff2");
  EXPECT_EQ(scan_result_->fonts[0].weight, "400");
  EXPECT_EQ(scan_result_->fonts[0].style, "normal");
  EXPECT_EQ(scan_result_->fonts[0].used_glyphs, 5u);
}

// Enabling lifecycle events makes Chromium replay the blank
// tab's networkIdle before the document is written. The font scan must not
// collect on it (it would read about:blank): it collects only on a
// networkIdle that arrives after Page.setDocumentContent answered.
TEST_F(FontGlyphScannerCdpTest, IgnoresTheBlankTabsReplayedNetworkIdle) {
  replay_blank_lifecycle_ = true;
  FontGlyphScanner scanner(client_.get());
  scanner.Scan("<html><body><p>Hello</p></body></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  int content = -1;
  int evaluate = -1;
  for (size_t i = 0; i < received_commands_.size(); ++i) {
    const std::string method = received_commands_[i].value("method", "");
    if (method == "Page.setDocumentContent" && content < 0) {
      content = static_cast<int>(i);
    }
    if (method == "Runtime.evaluate" && evaluate < 0) {
      evaluate = static_cast<int>(i);
    }
  }
  ASSERT_GE(content, 0) << "the document was never written";
  EXPECT_EQ(evaluate, -1)
      << "collected on the blank tab's replayed networkIdle";
  EXPECT_FALSE(scan_done_);

  // The page's own networkIdle, after the write: now it collects.
  SendEvent("Page.lifecycleEvent",
            {{"name", "networkIdle"}, {"loaderId", "blank-loader"}}, "sess-1");
  for (int i = 0; i < 20 && !scan_done_; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  ASSERT_TRUE(scan_done_);
  EXPECT_TRUE(scan_result_.ok()) << scan_result_.status();
  evaluate = -1;
  for (size_t i = 0; i < received_commands_.size(); ++i) {
    if (received_commands_[i].value("method", "") == "Runtime.evaluate") {
      evaluate = static_cast<int>(i);
      break;
    }
  }
  EXPECT_GT(evaluate, content) << "the page is read after it was written";
}

// A page that keeps a request in flight at least every 500 ms
// never reaches networkIdle. The font scan then collects settle_after_load_ms
// after the page's own load event, instead of running into its session
// timeout; the replayed blank-tab load does not start that wait. (The bound
// is shortened for the test.)
TEST_F(FontGlyphScannerCdpTest, CollectsAfterLoadWhenTheNetworkNeverGoesIdle) {
  replay_blank_lifecycle_ = true;
  FontGlyphScanner scanner(client_.get());
  scanner.set_settle_after_load_ms_for_testing(kTestSettleMs);
  scanner.Scan("<html><body><p>Hello</p></body></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });
  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }
  const int content = IndexOf("Page.setDocumentContent");
  ASSERT_GE(content, 0) << "the document was never written";
  EXPECT_FALSE(scan_done_) << "the replayed load started the settle wait";

  // The page's load, after the reply; no networkIdle follows.
  SendEvent("Page.lifecycleEvent", {{"name", "load"}}, "sess-1");
  PumpUntil([this] { return scan_done_; });
  ASSERT_TRUE(scan_done_) << "no collection after the settle bound";
  EXPECT_TRUE(scan_result_.ok()) << scan_result_.status();
  EXPECT_GT(IndexOf("Runtime.evaluate"), content);
}

// On a page with nothing left to fetch, the page's load arrives
// BEFORE the reply to setDocumentContent. It still starts the settle wait.
TEST_F(FontGlyphScannerCdpTest, LoadBeforeTheContentReplyStartsTheSettle) {
  replay_blank_lifecycle_ = true;
  load_before_content_reply_ = true;
  FontGlyphScanner scanner(client_.get());
  scanner.set_settle_after_load_ms_for_testing(kTestSettleMs);
  scanner.Scan("<html><body><p>Hello</p></body></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });
  PumpUntil([this] { return scan_done_; });
  ASSERT_TRUE(scan_done_) << "the load before the reply was dropped";
  EXPECT_TRUE(scan_result_.ok()) << scan_result_.status();
  EXPECT_GT(IndexOf("Runtime.evaluate"), IndexOf("Page.setDocumentContent"));
}

// The settle bound can run out before setDocumentContent is answered; the
// font scan then collects at the reply, never before the document is written.
TEST_F(FontGlyphScannerCdpTest, SettleBeforeTheContentReplyCollectsAtTheReply) {
  replay_blank_lifecycle_ = true;
  hold_content_reply_ = true;
  FontGlyphScanner scanner(client_.get());
  scanner.set_settle_after_load_ms_for_testing(kTestSettleMs);
  scanner.Scan("<html><body><p>Hello</p></body></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });
  // Wait for the command itself, not a fixed time: the setup round trips
  // before it can take longer than the settle bound on a busy host.
  PumpUntil([this] { return held_content_reply_id_ >= 0; });
  ASSERT_GE(held_content_reply_id_, 0) << "setDocumentContent was not sent";
  // The page's load went out with the command; the bound runs out before
  // the reply.
  RunOutTheSettleBound();
  EXPECT_EQ(IndexOf("Runtime.evaluate"), -1)
      << "collected before the document was written";
  EXPECT_FALSE(scan_done_);
  RespondToCommand(held_content_reply_id_, json::object());
  PumpUntil([this] { return scan_done_; });
  ASSERT_TRUE(scan_done_);
  EXPECT_TRUE(scan_result_.ok()) << scan_result_.status();
}

TEST_F(FontGlyphScannerCdpTest, CreateTargetFailure) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> result) {
                 scan_result_ = std::move(result);
                 scan_done_ = true;
               });

  RunLoop();
  RunLoopUntil([this] { return !received_commands_.empty(); });
  int id = received_commands_[0].value("id", 0);
  RespondError(id, -32000, "Target creation failed");

  RunLoopUntil([this] { return scan_done_; }, 50);

  ASSERT_TRUE(scan_done_);
  EXPECT_FALSE(scan_result_.ok());
}

TEST_F(FontGlyphScannerCdpTest, NetworkOfflineFailure) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> result) {
                 scan_result_ = std::move(result);
                 scan_done_ = true;
               });

  for (int i = 0; i < 15; ++i) {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Network.emulateNetworkConditions") {
        RespondError(id, -32000, "not supported");
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  RunLoopUntil([this] { return scan_done_; }, 50);

  ASSERT_TRUE(scan_done_);
  EXPECT_FALSE(scan_result_.ok());
}

TEST_F(FontGlyphScannerCdpTest, EvaluateReturnsEmpty) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html><body></body></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> result) {
                 scan_result_ = std::move(result);
                 scan_done_ = true;
               });

  // Custom responder returning empty evaluate result.
  auto custom = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        RespondToCommand(id, {{"result", {{"type", "string"}, {"value", ""}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
    if (scan_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    custom();
    RunLoop();
    if (scan_done_) break;
  }

  ASSERT_TRUE(scan_done_);
  EXPECT_FALSE(scan_result_.ok());
}

TEST_F(FontGlyphScannerCdpTest, EvaluateReturnsMalformedJson) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html><body></body></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> result) {
                 scan_result_ = std::move(result);
                 scan_done_ = true;
               });

  auto custom = [this]() {
    RunLoop();
    for (auto& cmd : received_commands_) {
      if (responded_ids_.count(cmd.value("id", 0))) continue;
      int id = cmd.value("id", 0);
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", "target-1"}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", "sess-1"}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "f1"}}}}}});
      } else if (method == "Runtime.evaluate") {
        RespondToCommand(
            id,
            {{"result", {{"type", "string"}, {"value", "{not valid json!!"}}}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  };

  for (int i = 0; i < 20; ++i) {
    custom();
    RunLoop();
    if (scan_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    custom();
    RunLoop();
    if (scan_done_) break;
  }

  ASSERT_TRUE(scan_done_);
  EXPECT_FALSE(scan_result_.ok());
}

TEST_F(FontGlyphScannerCdpTest, ScanWithNoFonts) {
  FontGlyphScanner scanner(client_.get());

  // Return result with codepoints but no fonts.
  json result;
  result["text_nodes"] = 5;
  result["codepoints"] = json::array({0x41, 0x42, 0x43});
  result["fonts"] = json::array();
  std::string eval_result = result.dump();

  scanner.Scan("<html><body><p>ABC</p></body></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll(eval_result);
    RunLoop();
    if (scan_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll(eval_result);
    RunLoop();
    if (scan_done_) break;
  }

  ASSERT_TRUE(scan_done_);
  ASSERT_TRUE(scan_result_.ok());
  EXPECT_EQ(scan_result_->total_text_nodes, 5u);
  EXPECT_TRUE(scan_result_->fonts.empty());
  EXPECT_EQ(scan_result_->all_used_codepoints, "U+0041-0043");
}

TEST_F(FontGlyphScannerCdpTest, ScanWithMultipleFonts) {
  FontGlyphScanner scanner(client_.get());

  json result;
  result["text_nodes"] = 10;
  result["codepoints"] = json::array({0x20, 0x41, 0x42, 0x61, 0x62});
  result["fonts"] = json::array({
      {{"family", "Roboto"},
       {"src_url", "/fonts/roboto.woff2"},
       {"format", "woff2"},
       {"weight", "400"},
       {"style", "normal"}},
      {{"family", "Roboto"},
       {"src_url", "/fonts/roboto-bold.woff2"},
       {"format", "woff2"},
       {"weight", "700"},
       {"style", "normal"}},
  });
  std::string eval_result = result.dump();

  scanner.Scan("<html><body>Hello</body></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll(eval_result);
    RunLoop();
    if (scan_done_) break;
  }

  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll(eval_result);
    RunLoop();
    if (scan_done_) break;
  }

  ASSERT_TRUE(scan_done_);
  ASSERT_TRUE(scan_result_.ok());
  EXPECT_EQ(scan_result_->total_text_nodes, 10u);
  ASSERT_EQ(scan_result_->fonts.size(), 2u);
  EXPECT_EQ(scan_result_->fonts[0].family, "Roboto");
  EXPECT_EQ(scan_result_->fonts[0].weight, "400");
  EXPECT_EQ(scan_result_->fonts[1].weight, "700");
  // Both fonts get same codepoint range.
  EXPECT_EQ(scan_result_->fonts[0].unicode_range,
            scan_result_->fonts[1].unicode_range);
  EXPECT_EQ(scan_result_->fonts[0].used_glyphs, 5u);
}

TEST_F(FontGlyphScannerCdpTest, FetchInterceptionBlocksRequests) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html><body>Test</body></html>", 1440, 900,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Simulate Fetch.requestPaused.
  SendEvent(
      "Fetch.requestPaused",
      {{"requestId", "req-1"}, {"request", {{"url", "http://evil.com/steal"}}}},
      "sess-1");

  for (int i = 0; i < 5; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify Fetch.failRequest was sent.
  bool found_fail = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Fetch.failRequest") {
      // Fetch.failRequest's parameter is `errorReason`: Chromium rejects a
      // `reason` with "Invalid parameters" and leaves the request paused, so
      // the page never reaches load or networkIdle.
      EXPECT_EQ(cmd.value("params", json::object()).value("errorReason", ""),
                "BlockedByClient");
      EXPECT_FALSE(cmd.value("params", json::object()).contains("reason"));
      found_fail = true;
      break;
    }
  }
  EXPECT_TRUE(found_fail);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (scan_done_) break;
  }
}

TEST_F(FontGlyphScannerCdpTest, SessionTimeout) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan(
      "<html><body>Test</body></html>", 1440, 900,
      [this](absl::StatusOr<FontGlyphResult> r) {
        scan_result_ = std::move(r);
        scan_done_ = true;
      },
      200);  // 200ms timeout

  bool guard_fired = false;
  uv_timer_t guard;
  uv_timer_init(loop_, &guard);
  guard.data = &guard_fired;
  uv_timer_start(
      &guard, [](uv_timer_t* t) { *static_cast<bool*>(t->data) = true; }, 2000,
      0);

  while (!scan_done_ && !guard_fired) {
    AutoRespondAll();
    uv_run(loop_, UV_RUN_NOWAIT);
    PollCommands();
    test::SleepUs(1000);  // 1ms
  }

  uv_timer_stop(&guard);
  uv_close(reinterpret_cast<uv_handle_t*>(&guard), nullptr);
  RunLoop();

  ASSERT_TRUE(scan_done_) << "Session timeout did not fire within 2s";
  EXPECT_FALSE(scan_result_.ok());
  EXPECT_TRUE(scan_result_.status().message().find("timeout") !=
              std::string::npos)
      << "Error: " << scan_result_.status().message();
}

TEST_F(FontGlyphScannerCdpTest, MobileViewportSet) {
  FontGlyphScanner scanner(client_.get());

  scanner.Scan("<html></html>", 375, 667,
               [this](absl::StatusOr<FontGlyphResult> r) {
                 scan_result_ = std::move(r);
                 scan_done_ = true;
               });

  for (int i = 0; i < 20; ++i) {
    AutoRespondAll();
    RunLoop();
  }

  // Verify viewport was set with mobile flag, and touch.
  bool found = false;
  bool found_touch = false;
  for (const auto& cmd : received_commands_) {
    if (cmd.value("method", "") == "Emulation.setDeviceMetricsOverride") {
      auto params = cmd.value("params", json::object());
      EXPECT_EQ(params.value("width", 0), 375);
      EXPECT_EQ(params.value("height", 0), 667);
      EXPECT_TRUE(params.value("mobile", false));
      found = true;
    }
    if (cmd.value("method", "") == "Emulation.setTouchEmulationEnabled") {
      EXPECT_TRUE(found) << "touch follows the device metrics";
      auto params = cmd.value("params", json::object());
      EXPECT_TRUE(params.value("enabled", false));
      EXPECT_EQ(params.value("maxTouchPoints", 0), 5);
      found_touch = true;
      break;
    }
  }
  EXPECT_TRUE(found);
  EXPECT_TRUE(found_touch);
  // Then the phone's user agent, and a fresh window.
  test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", 375, 667);

  // Clean up.
  SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
  for (int i = 0; i < 10; ++i) {
    AutoRespondAll();
    RunLoop();
    if (scan_done_) break;
  }
}

// The tablet sends its own user agent, the desktop none; both
// write the document into a fresh window.
TEST_F(FontGlyphScannerCdpTest, TabletAndDesktopSetupOrder) {
  for (const auto& [width, height] :
       {std::pair<uint32_t, uint32_t>{768, 1024}, {1440, 900}}) {
    received_commands_.clear();
    responded_ids_.clear();
    scan_done_ = false;
    FontGlyphScanner scanner(client_.get());
    scanner.Scan("<html></html>", width, height,
                 [this](absl::StatusOr<FontGlyphResult> r) {
                   scan_result_ = std::move(r);
                   scan_done_ = true;
                 });
    for (int i = 0; i < 20; ++i) {
      AutoRespondAll();
      RunLoop();
    }
    test::ExpectFreshWindowRenderSetup(received_commands_, "sess-1", width,
                                       height);
    SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}}, "sess-1");
    for (int i = 0; i < 10; ++i) {
      AutoRespondAll();
      RunLoop();
      if (scan_done_) break;
    }
    EXPECT_TRUE(scan_done_) << width;
  }
}

}  // namespace
}  // namespace pagespeed
