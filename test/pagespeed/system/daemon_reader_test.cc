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

// Unit tests for UdsDaemonReader: HTTP/1.1 over a unix-domain socket via the
// curl fetcher's FetchOverUnixSocket.

#include "pagespeed/system/uds_daemon_reader.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/request_context.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/system/curl_url_async_fetcher.h"
#include "test/pagespeed/kernel/base/gmock.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

namespace net_instaweb {
namespace {

// Minimal HTTP/1.1 server on a unix-domain socket.  Accepts one connection
// at a time, records the request head, and either answers with a canned
// response or (hang_=true) holds the connection open until Stop().
class UdsTestServer {
 public:
  explicit UdsTestServer(const GoogleString& socket_path)
      : socket_path_(socket_path) {}

  ~UdsTestServer() { Stop(); }

  bool Start() {
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
      return false;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
      return false;
    }
    memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());
    unlink(socket_path_.c_str());
    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr),
             sizeof(addr)) != 0) {
      return false;
    }
    if (listen(listen_fd_, 4) != 0) {
      return false;
    }
    thread_ = std::thread([this]() { ServeLoop(); });
    return true;
  }

  void Stop() {
    stopping_.store(true);
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (client_fd_ >= 0) {
        shutdown(client_fd_, SHUT_RDWR);
      }
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (listen_fd_ >= 0) {
      close(listen_fd_);
      listen_fd_ = -1;
    }
    unlink(socket_path_.c_str());
  }

  GoogleString last_request() {
    std::lock_guard<std::mutex> lock(mu_);
    return last_request_;
  }

  // Response configuration.
  GoogleString status_line_ = "200 OK";
  GoogleString content_type_ = "application/json";
  GoogleString body_ = "{\"status\":\"ok\"}";
  bool hang_ = false;

 private:
  void ServeLoop() {
    while (!stopping_.load()) {
      int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) {
        break;  // shutdown() in Stop()
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        client_fd_ = fd;
      }
      char buf[4096];
      GoogleString request;
      while (request.find("\r\n\r\n") == GoogleString::npos) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        request.append(buf, n);
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        last_request_ = request;
      }
      if (hang_) {
        while (!stopping_.load()) {
          usleep(10000);
        }
      } else {
        GoogleString response = StrCat(
            "HTTP/1.1 ", status_line_, "\r\nContent-Type: ", content_type_,
            "\r\nContent-Length: ", IntegerToString(body_.size()), "\r\n\r\n",
            body_);
        const char* p = response.c_str();
        size_t left = response.size();
        while (left > 0) {
          ssize_t n = write(fd, p, left);
          if (n <= 0) {
            break;
          }
          p += n;
          left -= n;
        }
      }
      close(fd);
      {
        std::lock_guard<std::mutex> lock(mu_);
        client_fd_ = -1;
      }
    }
  }

  GoogleString socket_path_;
  int listen_fd_ = -1;
  int client_fd_ = -1;  // guarded by mu_
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex mu_;
  GoogleString last_request_;  // guarded by mu_
};

// AsyncFetch with a thread-safe done/success flag (the curl poll thread
// completes fetches on its own thread).
class ReaderTestFetch : public AsyncFetch {
 public:
  explicit ReaderTestFetch(const RequestContextPtr& ctx)
      : AsyncFetch(ctx) {}

  bool HandleWrite(const StringPiece& content,
                   MessageHandler* handler) override {
    content.AppendToString(&buffer_);
    return true;
  }
  bool HandleFlush(MessageHandler* handler) override { return true; }
  void HandleHeadersComplete() override {}
  void HandleDone(bool success) override {
    success_.store(success);
    done_.store(true);
  }

  bool done() const { return done_.load(); }
  bool success() const { return success_.load(); }
  const GoogleString& buffer() const { return buffer_; }

 private:
  std::atomic<bool> done_{false};
  std::atomic<bool> success_{false};
  GoogleString buffer_;
};

class UdsDaemonReaderTest : public ::testing::Test {
 protected:
  UdsDaemonReaderTest()
      : thread_system_(Platform::CreateThreadSystem()),
        timer_(Platform::CreateTimer()),
        message_handler_(thread_system_->NewMutex()) {}

  // Waits for the fetch to complete (bounded by the reader's 5s upstream
  // timeout plus slack).
  void WaitForDone(const ReaderTestFetch& fetch) {
    int64 deadline_ms =
        timer_->NowMs() + UdsDaemonReader::kUpstreamTimeoutMs + 5000;
    while (!fetch.done() && timer_->NowMs() < deadline_ms) {
      usleep(1000);
    }
  }

  GoogleString SocketPath(StringPiece name) {
    return StrCat(GTestTempDir(), "/uds_daemon_reader_test_", name, ".sock");
  }

  std::unique_ptr<ThreadSystem> thread_system_;
  std::unique_ptr<Timer> timer_;
  MockMessageHandler message_handler_;
};

TEST_F(UdsDaemonReaderTest, ProxyIsSuppressedForUnixSocket) {
  // A fetcher configured with a dead proxy must still serve a unix-socket
  // fetch: the proxy is suppressed explicitly for UDS connections, on any
  // libcurl version.
  UdsTestServer server(SocketPath("proxy"));
  ASSERT_TRUE(server.Start());
  CurlUrlAsyncFetcher proxied_fetcher("127.0.0.1:1" /* dead proxy */,
                                      thread_system_.get(),
                                      nullptr /* statistics */, timer_.get(),
                                      5000, &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  proxied_fetcher.FetchOverUnixSocket(
      "http://localhost/v1/health", SocketPath("proxy"),
      UdsDaemonReader::kUpstreamTimeoutMs,
      UdsDaemonReader::kMaxResponseBodyBytes, &message_handler_, &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_TRUE(fetch.success());
  EXPECT_EQ(HttpStatus::kOK, fetch.response_headers()->status_code());
}

TEST_F(UdsDaemonReaderTest, GetOverUnixSocket) {
  UdsTestServer server(SocketPath("get"));
  ASSERT_TRUE(server.Start());
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         SocketPath("get"), &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  reader.Get("/v1/health", &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_TRUE(fetch.success());
  EXPECT_EQ(HttpStatus::kOK, fetch.response_headers()->status_code());
  EXPECT_EQ("{\"status\":\"ok\"}", fetch.buffer());
  const char* content_type =
      fetch.response_headers()->Lookup1(HttpAttributes::kContentType);
  ASSERT_TRUE(content_type != nullptr);
  EXPECT_STREQ("application/json", content_type);
  // The upstream request is a plain GET of the allow-listed path.
  EXPECT_THAT(server.last_request(),
              ::testing::HasSubstr("GET /v1/health HTTP/1.1"));
  EXPECT_EQ(GoogleString::npos, server.last_request().find('?'));
}

TEST_F(UdsDaemonReaderTest, HeadOverUnixSocket) {
  UdsTestServer server(SocketPath("head"));
  ASSERT_TRUE(server.Start());
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         SocketPath("head"), &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  fetch.request_headers()->set_method(RequestHeaders::kHead);
  reader.Get("/v1/health", &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_TRUE(fetch.success());
  EXPECT_EQ(HttpStatus::kOK, fetch.response_headers()->status_code());
  EXPECT_TRUE(fetch.buffer().empty());
  EXPECT_THAT(server.last_request(),
              ::testing::HasSubstr("HEAD /v1/health HTTP/1.1"));
}

TEST_F(UdsDaemonReaderTest, EmptySocketPathFailsImmediately) {
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         "" /* socket_path */, &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  reader.Get("/v1/health", &fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_FALSE(fetch.success());
}

TEST_F(UdsDaemonReaderTest, UnreachableSocketFails) {
  // No server is listening at this path.
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         SocketPath("absent"), &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  reader.Get("/v1/health", &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_FALSE(fetch.success());
}

TEST_F(UdsDaemonReaderTest, OversizedResponseFails) {
  UdsTestServer server(SocketPath("big"));
  server.body_ = GoogleString(UdsDaemonReader::kMaxResponseBodyBytes + 1, 'x');
  ASSERT_TRUE(server.Start());
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         SocketPath("big"), &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  reader.Get("/v1/stats", &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_FALSE(fetch.success());
}

TEST_F(UdsDaemonReaderTest, TimeoutFails) {
  UdsTestServer server(SocketPath("hang"));
  server.hang_ = true;
  ASSERT_TRUE(server.Start());
  UdsDaemonReader reader(thread_system_.get(), timer_.get(),
                         SocketPath("hang"), &message_handler_);
  ReaderTestFetch fetch(
      RequestContext::NewTestRequestContext(thread_system_.get()));
  int64 start_ms = timer_->NowMs();
  reader.Get("/v1/stats", &fetch);
  WaitForDone(fetch);
  ASSERT_TRUE(fetch.done());
  EXPECT_FALSE(fetch.success());
  // The reader's own timeout ended the fetch.
  EXPECT_GE(timer_->NowMs() - start_ms,
            UdsDaemonReader::kUpstreamTimeoutMs - 1000);
}

}  // namespace
}  // namespace net_instaweb
