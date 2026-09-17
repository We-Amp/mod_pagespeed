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

// A tiny, self-contained HTTP/HTTPS server for CurlUrlAsyncFetcherTest.
//
// This replaces the test's former dependency on public internet hosts
// (httpbin.org, badssl.com, www.google.com), which made the unit-test job the
// repo's worst CI flake. It serves an httpbin-equivalent subset of endpoints
// on 127.0.0.1 over an ephemeral port, accepts an arbitrary number of
// concurrent connections, and can optionally wrap each connection in TLS using
// BoringSSL (the same library libcurl links against in this build).
//
// Endpoints (HTTP and HTTPS):
//   GET  /html         -> 200, text/html, body starts with "<!DOCTYPE html>"
//   GET  /image/png    -> 200, image/png, body starts with the PNG signature
//   GET  /image/jpeg   -> 200, image/jpeg, body starts with the JPEG signature
//   GET  /gzip         -> 200, gzip-encoded JSON body ("{...}")
//   GET  /status/204   -> 204, no body
//   GET  /get          -> 200, small JSON body
//   GET  /delay/<n>    -> sleeps long enough to outlast the fetcher timeout
//                         (used by the timeout test), then 200
//   POST /post         -> 200, echoes the request body inside a JSON envelope
//   <anything else>    -> 404
//
// TLS modes select which leaf certificate the HTTPS server presents:
//   kTrustedCa  -> leaf signed by the test CA (curl trusts it via CAINFO)
//   kSelfSigned -> a self-signed leaf (curl rejects it unless verification is
//                  disabled), used for the "fails for self-signed" cases.

#ifndef TEST_PAGESPEED_SYSTEM_CURL_TEST_SERVER_H_
#define TEST_PAGESPEED_SYSTEM_CURL_TEST_SERVER_H_

#include <atomic>
#include <memory>
#include <vector>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/condvar.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread.h"
#include "pagespeed/kernel/base/thread_system.h"

// Forward-declare BoringSSL's SSL_CTX so the header does not pull in
// <openssl/ssl.h> into every translation unit that merely starts the server.
extern "C" {
struct ssl_ctx_st;
}

namespace net_instaweb {

// In-process HTTP/HTTPS test server. Runs an accept loop on its own thread and
// spawns a short-lived worker thread per connection so many concurrent curl
// fetches (e.g. the BoringSSL EAGAIN regression test) are served in parallel.
class CurlTestServer : public ThreadSystem::Thread {
 public:
  enum Scheme { kHttp, kHttps };
  // Which certificate the HTTPS server presents.
  enum TlsCert { kTrustedCa, kSelfSigned };

  // For HTTP, tls_cert is ignored.
  CurlTestServer(Scheme scheme, TlsCert tls_cert, ThreadSystem* thread_system);
  ~CurlTestServer() override;

  // Starts the accept loop and blocks until the listening port is known.
  // Returns false on setup failure.
  bool StartAndWait();

  // Stops the accept loop and joins all worker threads.
  void ShutDown();

  // The port the server is listening on (valid after StartAndWait()).
  int port() const { return listen_port_; }

  // "127.0.0.1:<port>" — convenient for building test URLs.
  GoogleString host_port() const {
    return StrCat("127.0.0.1:", IntegerToString(listen_port_));
  }

 private:
  // Per-connection worker thread (defined in the .cc).
  class ConnectionThread;

  void Run() override;
  // Handles one client fd to completion (parse request, write response).
  void HandleConnection(int client_fd);
  // Writes one HTTP response for the parsed method+path over a generic writer.
  // The writer abstracts plain-socket vs TLS I/O.
  template <typename WriteFn>
  void ServeRequest(const GoogleString& method, const GoogleString& path,
                    const GoogleString& body, const WriteFn& write_all);
  // Lazily builds the BoringSSL server context (HTTPS only).
  bool EnsureSslCtx();

  Scheme scheme_;
  TlsCert tls_cert_;

  std::unique_ptr<ThreadSystem::CondvarCapableMutex> mutex_;
  std::unique_ptr<ThreadSystem::Condvar> ready_;
  int listen_fd_;
  int listen_port_;
  std::atomic<bool> terminating_;
  bool started_;
  bool shut_down_;

  ssl_ctx_st* ssl_ctx_;  // owned; nullptr for HTTP.

  // Worker threads, one per accepted connection. Joined in ShutDown().
  ThreadSystem* thread_system_;
  std::unique_ptr<AbstractMutex> workers_mutex_;
  std::vector<std::unique_ptr<ThreadSystem::Thread>> workers_;

  CurlTestServer(const CurlTestServer&) = delete;
  CurlTestServer& operator=(const CurlTestServer&) = delete;
};

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_CURL_TEST_SERVER_H_
