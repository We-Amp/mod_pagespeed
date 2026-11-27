// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A scripted CDP peer: a fake "Chrome" on the far side of a real CdpClient.
//
// The client is wired to genuine libuv pipes, so everything under test runs its
// real transport, real framing, and real callback scheduling; only the browser
// is scripted.  Tests drive it by answering the commands the subject sends
// (`AutoRespondAll`, or `SetResponder` for a per-method override) and by
// pushing events at it (`SendEvent`).
//
// This is the fourth home of a fixture that was copy-pasted three times
// (visual_regression_gate_test, page_analysis_test, browser_css_extractor_test).
// New CDP tests belong here; the surviving copies are follow-up migrations.
//
// Not thread-safe and not meant to be: every method runs on the test thread,
// and the loop is only advanced when a test asks for it.

#ifndef TEST_TEST_UTIL_CDP_SCRIPTED_PEER_H_
#define TEST_TEST_UTIL_CDP_SCRIPTED_PEER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "test/test_util/cdp_pipe.h"
#include "uv.h"

namespace pagespeed {
namespace test {

// Minimal base64 encoder for building screenshot payloads.
inline std::string Base64Encode(const std::vector<uint8_t>& data) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((data.size() + 2) / 3) * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    uint32_t val = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < data.size()) val |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < data.size()) val |= static_cast<uint32_t>(data[i + 2]);
    result.push_back(kTable[(val >> 18) & 0x3F]);
    result.push_back(kTable[(val >> 12) & 0x3F]);
    result.push_back(i + 1 < data.size() ? kTable[(val >> 6) & 0x3F] : '=');
    result.push_back(i + 2 < data.size() ? kTable[val & 0x3F] : '=');
  }
  return result;
}

class ScriptedCdpPeer {
 public:
  using json = nlohmann::json;

  // Per-method override consulted BEFORE the default script.  Return true to
  // claim the command (the default script then leaves it alone).
  using Responder =
      std::function<bool(int id, const std::string& method, const json& cmd)>;

  ScriptedCdpPeer() = default;
  ~ScriptedCdpPeer() { Stop(); }

  ScriptedCdpPeer(const ScriptedCdpPeer&) = delete;
  ScriptedCdpPeer& operator=(const ScriptedCdpPeer&) = delete;

  // Returns false if any part of the plumbing could not be set up; callers in
  // a gtest SetUp() should ASSERT_TRUE on it.
  bool Start() {
    loop_ = new uv_loop_t;
    if (uv_loop_init(loop_) != 0) return false;
    if (!chrome_to_client_.Create()) return false;
    if (!client_to_chrome_.Create()) return false;

    chrome_write_fd_ = chrome_to_client_.write_fd;

    uv_pipe_init(loop_, &read_pipe_, 0);
    uv_pipe_open(&read_pipe_, chrome_to_client_.TakeReadFd());
    uv_pipe_init(loop_, &client_write_, 0);
    uv_pipe_open(&client_write_, client_to_chrome_.TakeWriteFd());

    test_read_fd_ = client_to_chrome_.read_fd;
    if (!SetNonBlocking(test_read_fd_)) return false;

    client_ = std::make_unique<CdpClient>(loop_);
    return client_->AttachPipes(&read_pipe_, &client_write_).ok();
  }

  void Stop() {
    if (loop_ == nullptr) return;
    client_.reset();
    uv_walk(
        loop_,
        [](uv_handle_t* h, void*) {
          if (!uv_is_closing(h)) uv_close(h, nullptr);
        },
        nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);
    chrome_to_client_.CloseWrite();
    client_to_chrome_.CloseRead();
    uv_loop_close(loop_);
    delete loop_;
    loop_ = nullptr;
  }

  CdpClient* client() { return client_.get(); }
  uv_loop_t* loop() { return loop_; }

  // The base64 PNG the default script answers Page.captureScreenshot with.
  // Tests that need per-capture payloads use SetResponder instead.
  void set_screenshot_b64(std::string b64) { screenshot_b64_ = std::move(b64); }
  void SetResponder(Responder r) { responder_ = std::move(r); }

  // ---- Chrome -> client ----------------------------------------------------

  void SendFromChrome(const std::string& json_str) {
    std::string msg = json_str;
    msg.push_back('\0');
    ssize_t n = PipeWrite(chrome_write_fd_, msg.data(), msg.size());
    (void)n;
  }

  void RespondToCommand(int id, const json& result) {
    SendFromChrome(json({{"id", id}, {"result", result}}).dump());
  }

  void RespondError(int id, int code, const std::string& msg) {
    SendFromChrome(
        json({{"id", id}, {"error", {{"code", code}, {"message", msg}}}})
            .dump());
  }

  void SendEvent(const std::string& method, const json& params,
                 const std::string& session_id = "") {
    json event = {{"method", method}, {"params", params}};
    if (!session_id.empty()) event["sessionId"] = session_id;
    SendFromChrome(event.dump());
  }

  // ---- client -> Chrome ----------------------------------------------------

  void PollCommands() {
    char buf[4096];
    while (true) {
      ssize_t n = PipeRead(test_read_fd_, buf, sizeof(buf));
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

  const std::vector<json>& received_commands() const {
    return received_commands_;
  }

  // Number of commands seen so far whose "method" equals `method`.
  size_t CountCommands(const std::string& method) const {
    size_t n = 0;
    for (const auto& cmd : received_commands_) {
      if (cmd.value("method", "") == method) ++n;
    }
    return n;
  }

  bool SawCommand(const std::string& method) const {
    return CountCommands(method) > 0;
  }

  // ---- loop driving --------------------------------------------------------

  void RunLoop() {
    for (int i = 0; i < 5; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
    PollCommands();
  }

  // Uses UV_RUN_NOWAIT + a short sleep rather than UV_RUN_ONCE: the latter
  // blocks on Linux in Docker (epoll + pipe fd interaction).
  void RunLoopUntil(const std::function<bool()>& condition,
                    int max_iterations = 5000) {
    for (int i = 0; i < max_iterations; i++) {
      if (condition()) return;
      uv_run(loop_, UV_RUN_NOWAIT);
      PollCommands();
      SleepUs(1000);
    }
  }

  // Answer every not-yet-answered command: the custom responder first, then the
  // default script (create/attach/frame-tree/screenshot, empty result for the
  // rest).
  void AutoRespondAll() {
    RunLoop();
    for (const auto& cmd : received_commands_) {
      int id = cmd.value("id", 0);
      if (responded_ids_.count(id)) continue;
      std::string method = cmd.value("method", "");
      responded_ids_.insert(id);

      if (responder_ && responder_(id, method, cmd)) continue;

      if (method == "Target.createTarget") {
        RespondToCommand(id, {{"targetId", NextTargetId()}});
      } else if (method == "Target.attachToTarget") {
        RespondToCommand(id, {{"sessionId", NextSessionId()}});
      } else if (method == "Page.getFrameTree") {
        RespondToCommand(id, {{"frameTree", {{"frame", {{"id", "frame-1"}}}}}});
      } else if (method == "Page.captureScreenshot") {
        RespondToCommand(id, {{"data", screenshot_b64_}});
      } else {
        RespondToCommand(id, json::object());
      }
    }
  }

  // Pump AutoRespondAll until `condition` holds or the budget runs out.  The
  // common shape for a capture that also needs a lifecycle event pushed at it.
  void PumpUntil(const std::function<bool()>& condition, int iterations = 40) {
    for (int i = 0; i < iterations; i++) {
      if (condition()) return;
      AutoRespondAll();
      RunLoop();
      SleepUs(500);
    }
  }

  // The session id the Nth (1-based) Target.attachToTarget was answered with.
  // Sequential captures each attach to their own session; an event addressed to
  // the wrong one is silently dropped by the subject, which is a hang, not a
  // failure — so tests must address the right session.
  std::string session_id(int nth = 1) const {
    return "sess-" + std::to_string(nth);
  }

 private:
  std::string NextTargetId() { return "target-" + std::to_string(++targets_); }
  std::string NextSessionId() { return "sess-" + std::to_string(++sessions_); }

  uv_loop_t* loop_ = nullptr;
  PipePair chrome_to_client_;
  PipePair client_to_chrome_;
  uv_pipe_t read_pipe_{};
  uv_pipe_t client_write_{};
  int test_read_fd_ = -1;
  int chrome_write_fd_ = -1;
  std::unique_ptr<CdpClient> client_;
  std::string screenshot_b64_;
  std::vector<json> received_commands_;
  std::string received_data_;
  std::set<int> responded_ids_;
  Responder responder_;
  int targets_ = 0;
  int sessions_ = 0;
};

}  // namespace test
}  // namespace pagespeed

#endif  // TEST_TEST_UTIL_CDP_SCRIPTED_PEER_H_
