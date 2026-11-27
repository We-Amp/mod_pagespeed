// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for CdpClient using real pipe pairs.

#include "src/browser/cdp_client.h"

#include <cstring>
#include <functional>
#include <string>

#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "test/test_util/cdp_pipe.h"
#include "uv.h"

namespace pagespeed {
namespace {

// Test fixture providing a libuv loop and connected pipe pair.
//
// Pipe layout:
//   chrome_to_client_[1] --write--> chrome_to_client_[0] = client_read_
//   client_write_ --write--> client_to_chrome_[0] (test reads)
class CdpClientTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    ASSERT_EQ(0, uv_loop_init(loop_));

    ASSERT_TRUE(chrome_to_client_.Create());
    ASSERT_TRUE(client_to_chrome_.Create());

    // Client-side pipe handles.
    ASSERT_EQ(0, uv_pipe_init(loop_, &client_read_, 0));
    ASSERT_EQ(0, uv_pipe_open(&client_read_, chrome_to_client_.TakeReadFd()));

    ASSERT_EQ(0, uv_pipe_init(loop_, &client_write_, 0));
    ASSERT_EQ(0, uv_pipe_open(&client_write_, client_to_chrome_.TakeWriteFd()));

    // Make test's read FD non-blocking for verification.
    ASSERT_TRUE(test::SetNonBlocking(client_to_chrome_.read_fd));

    client_ = std::make_unique<CdpClient>(loop_);
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
    // Run loop to completion: processes close callbacks AND drains
    // any in-flight uv_queue_work requests from the threadpool.
    // UV_RUN_DEFAULT blocks until ref count reaches zero.
    uv_run(loop_, UV_RUN_DEFAULT);

    // Close test-owned FDs (not owned by libuv).
    chrome_to_client_.CloseWrite();
    client_to_chrome_.CloseRead();

    uv_loop_close(loop_);
    delete loop_;
  }

  // Send a CDP message from "Chrome" to the client.
  void SendFromChrome(const std::string& json_str) const {
    std::string msg = json_str;
    msg.push_back('\0');
    ssize_t n = test::PipeWrite(chrome_write_fd_, msg.data(), msg.size());
    (void)n;
  }

  // Run the event loop briefly to process I/O.
  void RunLoop() {
    for (int i = 0; i < 5; i++) {
      if (uv_run(loop_, UV_RUN_NOWAIT) == 0) break;
    }
  }

  // Run the event loop until a condition is met or timeout.
  // Uses UV_RUN_ONCE which blocks until at least one event fires.
  void RunLoopUntil(std::function<bool()> condition, int max_iterations = 200) {
    for (int i = 0; i < max_iterations; i++) {
      if (condition()) return;
      uv_run(loop_, UV_RUN_ONCE);
    }
  }

  // Write a large message from "Chrome" to the client.
  // Handles backpressure by writing in chunks and running the
  // loop between writes (pipe buffer is ~16KB on macOS).
  void WriteLargeFromChrome(const std::string& data) const {
    // Make write FD non-blocking.
    test::SetNonBlocking(chrome_write_fd_);

    size_t offset = 0;
    int stall_count = 0;
    while (offset < data.size()) {
      ssize_t n = test::PipeWrite(chrome_write_fd_, data.data() + offset,
                                  data.size() - offset);
      if (n > 0) {
        offset += n;
        stall_count = 0;
      } else {
        stall_count++;
        if (stall_count > 1000) break;  // Safety valve.
      }
      // Run loop to let client read (frees pipe buffer).
      // UV_RUN_ONCE blocks until at least one event fires, ensuring
      // reads are processed on Windows overlapped pipes.
      uv_run(loop_, UV_RUN_ONCE);
    }

    // Note: on POSIX, restoring blocking mode would require fcntl;
    // on Windows, SetNonBlocking is a no-op. For test purposes the
    // write FD remains in its current mode after this call.
  }

  // Read what the client sent to "Chrome".
  std::string ReadFromChrome() {
    RunLoop();
    char buf[4096];
    ssize_t n = test::PipeRead(test_read_fd_, buf, sizeof(buf));
    if (n <= 0) return "";
    return std::string(buf, n);
  }

  uv_loop_t* loop_ = nullptr;
  test::PipePair chrome_to_client_;  // read=client reads, write=test writes
  test::PipePair client_to_chrome_;  // read=test reads, write=client writes
  int& chrome_write_fd_ = chrome_to_client_.write_fd;
  int& test_read_fd_ = client_to_chrome_.read_fd;
  uv_pipe_t client_read_{};
  uv_pipe_t client_write_{};
  std::unique_ptr<CdpClient> client_;
};

TEST_F(CdpClientTest, AttachPipesSucceeds) {
  auto status = client_->AttachPipes(&client_read_, &client_write_);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(client_->connected());
}

TEST_F(CdpClientTest, AttachPipesRejectsNull) {
  auto status = client_->AttachPipes(nullptr, &client_write_);
  EXPECT_FALSE(status.ok());
  EXPECT_FALSE(client_->connected());
}

TEST_F(CdpClientTest, AttachPipesRejectsDouble) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());
  auto status = client_->AttachPipes(&client_read_, &client_write_);
  EXPECT_FALSE(status.ok());
}

TEST_F(CdpClientTest, SendCommandReturnsId) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpCommand cmd;
  cmd.method = "Page.enable";
  cmd.timeout_ms = 0;

  auto result = client_->SendCommand(cmd, [](auto) {});
  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_EQ(1, *result);
  EXPECT_EQ(1u, client_->pending_count());
}

TEST_F(CdpClientTest, SendCommandFailsWhenNotConnected) {
  CdpCommand cmd;
  cmd.method = "Page.enable";

  auto result = client_->SendCommand(cmd, [](auto) {});
  EXPECT_FALSE(result.ok());
}

TEST_F(CdpClientTest, SendCommandIncrementsIds) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  auto id1 = client_->SendCommand(cmd, [](auto) {});
  auto id2 = client_->SendCommand(cmd, [](auto) {});

  ASSERT_TRUE(id1.ok());
  ASSERT_TRUE(id2.ok());
  EXPECT_EQ(1, *id1);
  EXPECT_EQ(2, *id2);
}

TEST_F(CdpClientTest, ResponseResolvesCallback) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "Page.enable";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok());

  nlohmann::json response;
  response["id"] = *id;
  response["result"] = nlohmann::json::object();
  SendFromChrome(response.dump());

  RunLoop();

  EXPECT_TRUE(called);
  EXPECT_EQ(*id, received.id);
  EXPECT_FALSE(received.is_error());
  EXPECT_EQ(0u, client_->pending_count());
}

TEST_F(CdpClientTest, ErrorResponseSetsErrorFields) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "Bad.method";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok());

  nlohmann::json response;
  response["id"] = *id;
  response["error"]["code"] = -32601;
  response["error"]["message"] = "Method not found";
  SendFromChrome(response.dump());

  RunLoop();

  EXPECT_TRUE(received.is_error());
  EXPECT_EQ(-32601, received.error_code);
  EXPECT_EQ("Method not found", received.error_message);
}

TEST_F(CdpClientTest, EventCallbackInvoked) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool event_received = false;
  CdpEvent received_event;

  client_->SetEventCallback([&](const CdpEvent& e) {
    event_received = true;
    received_event = e;
  });

  nlohmann::json event;
  event["method"] = "Page.loadEventFired";
  event["params"]["timestamp"] = 12345.0;
  SendFromChrome(event.dump());

  RunLoop();

  EXPECT_TRUE(event_received);
  EXPECT_EQ("Page.loadEventFired", received_event.method);
  EXPECT_DOUBLE_EQ(12345.0, received_event.params["timestamp"].get<double>());
}

TEST_F(CdpClientTest, EventWithSessionId) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpEvent received_event;
  client_->SetEventCallback([&](const CdpEvent& e) { received_event = e; });

  nlohmann::json event;
  event["method"] = "Network.requestWillBeSent";
  event["params"] = nlohmann::json::object();
  event["sessionId"] = "session-123";
  SendFromChrome(event.dump());

  RunLoop();

  EXPECT_EQ("session-123", received_event.session_id);
}

TEST_F(CdpClientTest, CancelAllResolvesAllPending) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  int error_count = 0;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  for (int i = 0; i < 3; i++) {
    auto id = client_->SendCommand(cmd, [&](auto result) {
      if (!result.ok()) error_count++;
    });
    ASSERT_TRUE(id.ok()) << id.status().message();
  }

  EXPECT_EQ(3u, client_->pending_count());

  client_->CancelAll(absl::UnavailableError("Chrome crashed"));

  EXPECT_EQ(3, error_count);
  EXPECT_EQ(0u, client_->pending_count());
  EXPECT_FALSE(client_->connected());
}

TEST_F(CdpClientTest, SendCommandWithSessionId) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpCommand cmd;
  cmd.method = "Runtime.evaluate";
  cmd.params = {{"expression", "1+1"}};
  cmd.session_id = "target-session";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [](auto) {});
  ASSERT_TRUE(id.ok());

  // Read the command that was sent.
  std::string sent = ReadFromChrome();
  ASSERT_FALSE(sent.empty());

  // Remove null byte delimiter.
  if (!sent.empty() && sent.back() == '\0') {
    sent.pop_back();
  }
  auto json = nlohmann::json::parse(sent);

  EXPECT_EQ("target-session", json["sessionId"].get<std::string>());
  EXPECT_EQ("Runtime.evaluate", json["method"].get<std::string>());
  EXPECT_EQ("1+1", json["params"]["expression"].get<std::string>());
}

TEST_F(CdpClientTest, MultipleMessagesInOneRead) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  std::vector<CdpResponse> responses;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  auto id1 = client_->SendCommand(cmd, [&](auto result) {
    if (result.ok()) responses.push_back(*result);
  });
  auto id2 = client_->SendCommand(cmd, [&](auto result) {
    if (result.ok()) responses.push_back(*result);
  });
  ASSERT_TRUE(id1.ok()) << id1.status().message();
  ASSERT_TRUE(id2.ok()) << id2.status().message();

  // Send both responses concatenated (tests framing).
  nlohmann::json r1;
  r1["id"] = *id1;
  r1["result"]["value"] = "first";

  nlohmann::json r2;
  r2["id"] = *id2;
  r2["result"]["value"] = "second";

  std::string combined = r1.dump();
  combined.push_back('\0');
  combined += r2.dump();
  combined.push_back('\0');

  ssize_t n =
      test::PipeWrite(chrome_write_fd_, combined.data(), combined.size());
  (void)n;

  RunLoop();

  ASSERT_EQ(2u, responses.size());
  EXPECT_EQ("first", responses[0].result["value"].get<std::string>());
  EXPECT_EQ("second", responses[1].result["value"].get<std::string>());
}

TEST_F(CdpClientTest, PipeEofCancelsAll) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool got_error = false;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    if (!result.ok()) got_error = true;
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  // Close Chrome's write end → triggers EOF on client.
  chrome_to_client_.CloseWrite();

  RunLoop();

  EXPECT_TRUE(got_error);
  EXPECT_FALSE(client_->connected());
}

TEST_F(CdpClientTest, CommandWithResultData) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "DOM.getDocument";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    if (result.ok()) received = *result;
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  nlohmann::json response;
  response["id"] = *id;
  response["result"]["root"]["nodeId"] = 1;
  response["result"]["root"]["nodeName"] = "#document";
  SendFromChrome(response.dump());

  RunLoop();

  EXPECT_EQ(1, received.result["root"]["nodeId"].get<int>());
  EXPECT_EQ("#document",
            received.result["root"]["nodeName"].get<std::string>());
}

TEST_F(CdpClientTest, IgnoresUnknownResponseId) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  // Send a response for an ID we never sent.
  nlohmann::json response;
  response["id"] = 999;
  response["result"] = nlohmann::json::object();
  SendFromChrome(response.dump());

  RunLoop();  // Should not crash.
}

TEST_F(CdpClientTest, IgnoresInvalidJson) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  std::string garbage = "not valid json";
  garbage.push_back('\0');
  ssize_t n = test::PipeWrite(chrome_write_fd_, garbage.data(), garbage.size());
  (void)n;

  RunLoop();  // Should not crash.
  EXPECT_TRUE(client_->connected());
}

// --- T1: Per-command timeout tests ---

TEST_F(CdpClientTest, CommandTimesOutWithDeadlineExceeded) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  absl::Status received_status;

  CdpCommand cmd;
  cmd.method = "Slow.method";
  cmd.timeout_ms = 50;  // Short timeout.

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (!result.ok()) {
      received_status = result.status();
    }
  });
  ASSERT_TRUE(id.ok()) << id.status().message();
  EXPECT_EQ(1u, client_->pending_count());

  // Run loop until timeout fires (no response sent).
  RunLoopUntil([&] { return called; });

  EXPECT_TRUE(called);
  EXPECT_TRUE(absl::IsDeadlineExceeded(received_status)) << received_status;
  EXPECT_EQ(0u, client_->pending_count());
  // Client should still be connected (timeout != disconnection).
  EXPECT_TRUE(client_->connected());
}

TEST_F(CdpClientTest, ResponseBeforeTimeoutCancelsTimer) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "Fast.method";
  cmd.timeout_ms = 5000;  // Long timeout.

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  // Respond immediately (before timeout fires).
  nlohmann::json response;
  response["id"] = *id;
  response["result"]["value"] = "quick";
  SendFromChrome(response.dump());

  RunLoop();

  EXPECT_TRUE(called);
  EXPECT_FALSE(received.is_error());
  EXPECT_EQ("quick", received.result["value"].get<std::string>());
  EXPECT_EQ(0u, client_->pending_count());
}

TEST_F(CdpClientTest, CancelAllWithActiveTimers) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  int error_count = 0;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 5000;  // Long timeout.

  for (int i = 0; i < 3; i++) {
    auto id = client_->SendCommand(cmd, [&](auto result) {
      if (!result.ok()) error_count++;
    });
    ASSERT_TRUE(id.ok()) << id.status().message();
  }

  EXPECT_EQ(3u, client_->pending_count());

  // CancelAll should stop timers and invoke all callbacks.
  client_->CancelAll(absl::CancelledError("shutting down"));

  EXPECT_EQ(3, error_count);
  EXPECT_EQ(0u, client_->pending_count());

  // Run loop to process timer close callbacks (should not crash).
  RunLoop();
}

TEST_F(CdpClientTest, MultipleCommandsDifferentTimeouts) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool slow_timed_out = false;
  bool fast_resolved = false;

  CdpCommand slow_cmd;
  slow_cmd.method = "Slow";
  slow_cmd.timeout_ms = 50;

  CdpCommand fast_cmd;
  fast_cmd.method = "Fast";
  fast_cmd.timeout_ms = 0;  // No timeout.

  auto slow_id = client_->SendCommand(slow_cmd, [&](auto result) {
    if (!result.ok()) slow_timed_out = true;
  });
  auto fast_id = client_->SendCommand(fast_cmd, [&](auto result) {
    if (result.ok()) fast_resolved = true;
  });
  ASSERT_TRUE(slow_id.ok()) << slow_id.status().message();
  ASSERT_TRUE(fast_id.ok()) << fast_id.status().message();

  // Respond to the fast command only.
  nlohmann::json response;
  response["id"] = *fast_id;
  response["result"] = nlohmann::json::object();
  SendFromChrome(response.dump());

  // Run until the slow command times out.
  RunLoopUntil([&] { return slow_timed_out && fast_resolved; });

  EXPECT_TRUE(fast_resolved);
  EXPECT_TRUE(slow_timed_out);
  EXPECT_EQ(0u, client_->pending_count());
}

// --- T2: Large message parsing tests ---

TEST_F(CdpClientTest, LargeResponseParsedCorrectly) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "DOM.getDocument";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  // Build a response larger than kLargeMessageThreshold (64KB).
  // This triggers the uv_queue_work off-thread parsing path.
  std::string large_data(static_cast<size_t>(70) * 1024, 'x');
  nlohmann::json response;
  response["id"] = *id;
  response["result"]["data"] = large_data;
  response["result"]["marker"] = "large_test";

  std::string serialized = response.dump();
  ASSERT_GT(serialized.size(), 64u * 1024)
      << "Message must exceed kLargeMessageThreshold";
  serialized.push_back('\0');

  // Use chunked write (pipe buffer is ~16KB on macOS).
  WriteLargeFromChrome(serialized);

  // Run loop — uv_queue_work needs UV_RUN_ONCE to process
  // both the work and the after-work callback.
  RunLoopUntil([&] { return called; });

  EXPECT_TRUE(called);
  EXPECT_EQ(*id, received.id);
  EXPECT_FALSE(received.is_error());
  EXPECT_EQ("large_test", received.result["marker"].get<std::string>());
  EXPECT_EQ(large_data, received.result["data"].get<std::string>());
}

TEST_F(CdpClientTest, LargeInvalidJsonHandledGracefully) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  // Send >64KB of invalid JSON (triggers off-thread parse path).
  std::string large_garbage(static_cast<size_t>(70) * 1024, '{');
  large_garbage.push_back('\0');

  // Use chunked write (pipe buffer is ~16KB on macOS).
  // WriteLargeFromChrome runs the loop during writes, so by the
  // time it returns the message has been consumed and parsed.
  WriteLargeFromChrome(large_garbage);

  // Brief loop run to process any deferred callbacks.
  RunLoop();

  // Client should still be connected (parse error != disconnect).
  EXPECT_TRUE(client_->connected());
}

TEST_F(CdpClientTest, LargeEventParsedCorrectly) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool event_received = false;
  CdpEvent received_event;

  client_->SetEventCallback([&](const CdpEvent& e) {
    event_received = true;
    received_event = e;
  });

  // Build a large event (>64KB).
  std::string large_payload(static_cast<size_t>(70) * 1024, 'y');
  nlohmann::json event;
  event["method"] = "Network.dataReceived";
  event["params"]["data"] = large_payload;

  std::string serialized = event.dump();
  ASSERT_GT(serialized.size(), 64u * 1024);
  serialized.push_back('\0');

  // Use chunked write (pipe buffer is ~16KB on macOS).
  WriteLargeFromChrome(serialized);

  RunLoopUntil([&] { return event_received; });

  EXPECT_TRUE(event_received);
  EXPECT_EQ("Network.dataReceived", received_event.method);
  EXPECT_EQ(large_payload, received_event.params["data"].get<std::string>());
}

// --- T4: Recovery-after-bad-input tests ---

TEST_F(CdpClientTest, ValidMessageAfterInvalidJson) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  // Send invalid JSON first, then a valid response.
  std::string garbage = "{{{{not json}}}}";
  garbage.push_back('\0');

  nlohmann::json response;
  response["id"] = *id;
  response["result"]["status"] = "recovered";

  std::string valid = response.dump();
  valid.push_back('\0');

  std::string combined = garbage + valid;
  ssize_t n =
      test::PipeWrite(chrome_write_fd_, combined.data(), combined.size());
  (void)n;

  RunLoop();

  EXPECT_TRUE(called);
  EXPECT_FALSE(received.is_error());
  EXPECT_EQ("recovered", received.result["status"].get<std::string>());
}

TEST_F(CdpClientTest, ValidResponseAfterUnknownId) {
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;
  CdpResponse received;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 0;

  auto id = client_->SendCommand(cmd, [&](auto result) {
    called = true;
    if (result.ok()) {
      received = *result;
    }
  });
  ASSERT_TRUE(id.ok()) << id.status().message();

  // Send a response for an unknown ID, then for the real ID.
  nlohmann::json unknown;
  unknown["id"] = 9999;
  unknown["result"]["value"] = "wrong";

  nlohmann::json correct;
  correct["id"] = *id;
  correct["result"]["value"] = "right";

  std::string combined = unknown.dump();
  combined.push_back('\0');
  combined += correct.dump();
  combined.push_back('\0');

  ssize_t n =
      test::PipeWrite(chrome_write_fd_, combined.data(), combined.size());
  (void)n;

  RunLoop();

  EXPECT_TRUE(called);
  EXPECT_FALSE(received.is_error());
  EXPECT_EQ("right", received.result["value"].get<std::string>());
}

TEST_F(CdpClientTest, DestructorCleansUpWithoutCallbacks) {
  // Verify that destroying the client with pending commands does
  // not invoke callbacks (C3 fix).
  ASSERT_TRUE(client_->AttachPipes(&client_read_, &client_write_).ok());

  bool called = false;

  CdpCommand cmd;
  cmd.method = "Test";
  cmd.timeout_ms = 5000;

  auto id = client_->SendCommand(cmd, [&](auto) { called = true; });
  ASSERT_TRUE(id.ok()) << id.status().message();
  EXPECT_EQ(1u, client_->pending_count());

  // Destroy the client without calling CancelAll.
  client_.reset();

  // Run loop to process any deferred callbacks.
  RunLoop();

  // Callback should NOT have been invoked.
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace pagespeed
