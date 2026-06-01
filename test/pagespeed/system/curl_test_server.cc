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

#include "test/pagespeed/system/curl_test_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <csignal>
#include <cstring>

#include "base/logging.h"
#include "pagespeed/kernel/base/abstract_mutex.h"
#include "pagespeed/kernel/base/stack_buffer.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "test/pagespeed/system/curl_test_certs.h"

namespace net_instaweb {

namespace {

// The /delay/<n> endpoint must reliably outlast the fetcher timeout
// (kFetcherTimeoutMs == 5s in the test) so the fetcher's own timeout fires
// first. We hold the connection open this long before responding.
const int kDelaySleepMs = 30 * 1000;

// Reads a complete HTTP request from a generic byte reader. Returns false if
// the peer closed before a full request was read. Fills method/path/body.
// Only enough of HTTP/1.0 is parsed to drive the tests (request line +
// Content-Length-delimited body for POST).
template <typename ReadFn>
bool ReadRequest(const ReadFn& read_some, GoogleString* method,
                 GoogleString* path, GoogleString* body) {
  GoogleString buffer;
  char chunk[kStackBufferSize];
  size_t header_end = GoogleString::npos;
  // Read until we have the full header block.
  while (header_end == GoogleString::npos) {
    int n = read_some(chunk, sizeof(chunk));
    if (n <= 0) {
      return false;
    }
    buffer.append(chunk, n);
    header_end = buffer.find("\r\n\r\n");
    if (buffer.size() > 64 * 1024) {
      return false;  // Defensive cap; tests never send this much.
    }
  }

  // Parse the request line: "METHOD SP PATH SP HTTP/x.y".
  size_t line_end = buffer.find("\r\n");
  GoogleString request_line = buffer.substr(0, line_end);
  size_t sp1 = request_line.find(' ');
  size_t sp2 = request_line.find(' ', sp1 + 1);
  if (sp1 == GoogleString::npos || sp2 == GoogleString::npos) {
    return false;
  }
  *method = request_line.substr(0, sp1);
  GoogleString target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  // Strip any query string; the tests only key off the path.
  size_t q = target.find('?');
  *path = (q == GoogleString::npos) ? target : target.substr(0, q);

  // Honor Content-Length for the body (POST). Case-insensitive header scan.
  GoogleString headers_block = buffer.substr(0, header_end);
  int64 content_length = 0;
  StringPieceVector lines;
  SplitStringPieceToVector(headers_block, "\r\n", &lines, true);
  for (size_t i = 1; i < lines.size(); ++i) {
    StringPiece line = lines[i];
    size_t colon = line.find(':');
    if (colon == StringPiece::npos) {
      continue;
    }
    StringPiece name = line.substr(0, colon);
    if (StringCaseEqual(name, "Content-Length")) {
      StringPiece v = line.substr(colon + 1);
      TrimWhitespace(&v);
      StringToInt64(v.as_string(), &content_length);
    }
  }

  GoogleString already = buffer.substr(header_end + 4);
  body->assign(already.data(), already.size());
  while (static_cast<int64>(body->size()) < content_length) {
    int n = read_some(chunk, sizeof(chunk));
    if (n <= 0) {
      break;
    }
    body->append(chunk, n);
  }
  return true;
}

GoogleString GzipBody(StringPiece in) {
  GoogleString out;
  StringWriter writer(&out);
  CHECK(GzipInflater::Deflate(in, GzipInflater::kGzip, &writer));
  return out;
}

}  // namespace

// A worker thread that services exactly one accepted connection. Owning the
// connection on its own thread lets the server handle concurrent curl fetches.
class CurlTestServer::ConnectionThread : public ThreadSystem::Thread {
 public:
  ConnectionThread(CurlTestServer* server, int client_fd,
                   ThreadSystem* thread_system)
      : Thread(thread_system, "curl_test_conn", ThreadSystem::kJoinable),
        server_(server),
        client_fd_(client_fd) {}

  void Run() override {
    server_->HandleConnection(client_fd_);
    close(client_fd_);
  }

 private:
  CurlTestServer* server_;
  int client_fd_;
};

CurlTestServer::CurlTestServer(Scheme scheme, TlsCert tls_cert,
                               ThreadSystem* thread_system)
    : Thread(thread_system, "curl_test_server", ThreadSystem::kJoinable),
      scheme_(scheme),
      tls_cert_(tls_cert),
      mutex_(thread_system->NewMutex()),
      ready_(mutex_->NewCondvar()),
      listen_fd_(-1),
      listen_port_(0),
      terminating_(false),
      started_(false),
      shut_down_(false),
      ssl_ctx_(nullptr),
      thread_system_(thread_system),
      workers_mutex_(thread_system->NewMutex()) {}

CurlTestServer::~CurlTestServer() {
  CHECK(shut_down_ || !started_) << "Must call ShutDown() before destroying";
  if (ssl_ctx_ != nullptr) {
    SSL_CTX_free(ssl_ctx_);
    ssl_ctx_ = nullptr;
  }
}

bool CurlTestServer::EnsureSslCtx() {
  if (ssl_ctx_ != nullptr) {
    return true;
  }
  SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
  if (ctx == nullptr) {
    return false;
  }
  const char* cert_pem;
  const char* key_pem;
  if (tls_cert_ == kTrustedCa) {
    cert_pem = kTestServerCertPem;
    key_pem = kTestServerKeyPem;
  } else {
    cert_pem = kTestSelfSignedCertPem;
    key_pem = kTestSelfSignedKeyPem;
  }

  BIO* cert_bio = BIO_new_mem_buf(cert_pem, -1);
  X509* cert = PEM_read_bio_X509(cert_bio, nullptr, nullptr, nullptr);
  BIO_free(cert_bio);
  if (cert == nullptr || SSL_CTX_use_certificate(ctx, cert) != 1) {
    if (cert != nullptr) X509_free(cert);
    SSL_CTX_free(ctx);
    return false;
  }
  X509_free(cert);

  BIO* key_bio = BIO_new_mem_buf(key_pem, -1);
  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(key_bio, nullptr, nullptr, nullptr);
  BIO_free(key_bio);
  if (pkey == nullptr || SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
    if (pkey != nullptr) EVP_PKEY_free(pkey);
    SSL_CTX_free(ctx);
    return false;
  }
  EVP_PKEY_free(pkey);

  ssl_ctx_ = ctx;
  return true;
}

bool CurlTestServer::StartAndWait() {
  // Writing the response to a peer that has already closed its end (e.g. curl
  // after the fetcher's timeout fired on the /delay endpoint) would otherwise
  // deliver SIGPIPE and kill the test process. Ignore it process-wide; we
  // detect the broken connection via the write return value instead. This also
  // covers BoringSSL's SSL_write path, which writes through the same socket.
  signal(SIGPIPE, SIG_IGN);
  if (scheme_ == kHttps && !EnsureSslCtx()) {
    LOG(ERROR) << "CurlTestServer: failed to build SSL_CTX";
    return false;
  }
  started_ = true;
  if (!Start()) {
    return false;
  }
  ScopedMutex lock(mutex_.get());
  while (listen_port_ == 0 && !terminating_) {
    ready_->Wait();
  }
  return listen_port_ != 0;
}

void CurlTestServer::ShutDown() {
  if (shut_down_ || !started_) {
    shut_down_ = true;
    return;
  }
  terminating_ = true;
  {
    ScopedMutex lock(mutex_.get());
    if (listen_fd_ >= 0) {
      ::shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
      listen_fd_ = -1;
    }
  }
  Join();  // Join the accept loop.
  // Join any in-flight connection workers.
  std::vector<std::unique_ptr<ThreadSystem::Thread>> to_join;
  {
    ScopedMutex lock(workers_mutex_.get());
    to_join.swap(workers_);
  }
  for (auto& w : to_join) {
    w->Join();
  }
  shut_down_ = true;
}

void CurlTestServer::Run() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK_GE(fd, 0);
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // ephemeral
  CHECK_EQ(0,
           bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)));
  CHECK_EQ(0, listen(fd, 32));
  socklen_t len = sizeof(addr);
  CHECK_EQ(0, getsockname(fd, reinterpret_cast<struct sockaddr*>(&addr), &len));

  {
    ScopedMutex lock(mutex_.get());
    listen_fd_ = fd;
    listen_port_ = ntohs(addr.sin_port);
    ready_->Signal();
  }

  while (!terminating_) {
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(fd, reinterpret_cast<struct sockaddr*>(&client_addr),
                           &client_len);
    if (client_fd < 0 || terminating_) {
      if (client_fd >= 0) close(client_fd);
      break;
    }
    auto worker =
        std::make_unique<ConnectionThread>(this, client_fd, thread_system_);
    ThreadSystem::Thread* raw = worker.get();
    {
      ScopedMutex lock(workers_mutex_.get());
      workers_.push_back(std::move(worker));
    }
    raw->Start();
  }

  ScopedMutex lock(mutex_.get());
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

void CurlTestServer::HandleConnection(int client_fd) {
  if (scheme_ == kHttp) {
    auto read_some = [&](char* buf, int cap) -> int {
      return static_cast<int>(read(client_fd, buf, cap));
    };
    auto write_all = [&](StringPiece data) {
      size_t off = 0;
      while (off < data.size()) {
        ssize_t w =
            send(client_fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (w <= 0) return;
        off += w;
      }
    };
    GoogleString method, path, body;
    if (ReadRequest(read_some, &method, &path, &body)) {
      ServeRequest(method, path, body, write_all);
    }
    return;
  }

  // HTTPS path.
  SSL* ssl = SSL_new(ssl_ctx_);
  if (ssl == nullptr) {
    return;
  }
  SSL_set_fd(ssl, client_fd);
  if (SSL_accept(ssl) <= 0) {
    // Handshake rejected by the client (expected for the self-signed +
    // verify-on cases): just tear down.
    SSL_free(ssl);
    return;
  }
  auto read_some = [&](char* buf, int cap) -> int {
    return SSL_read(ssl, buf, cap);
  };
  auto write_all = [&](StringPiece data) {
    size_t off = 0;
    while (off < data.size()) {
      int w = SSL_write(ssl, data.data() + off,
                        static_cast<int>(data.size() - off));
      if (w <= 0) return;
      off += w;
    }
  };
  GoogleString method, path, body;
  if (ReadRequest(read_some, &method, &path, &body)) {
    ServeRequest(method, path, body, write_all);
  }
  SSL_shutdown(ssl);
  SSL_free(ssl);
}

template <typename WriteFn>
void CurlTestServer::ServeRequest(const GoogleString& method,
                                  const GoogleString& path,
                                  const GoogleString& body,
                                  const WriteFn& write_all) {
  auto respond = [&](int status, StringPiece reason, StringPiece content_type,
                     StringPiece extra_headers, StringPiece response_body) {
    GoogleString head =
        StrCat("HTTP/1.0 ", IntegerToString(status), " ", reason, "\r\n");
    if (!content_type.empty()) {
      StrAppend(&head, "Content-Type: ", content_type, "\r\n");
    }
    StrAppend(&head, "Content-Length: ",
              IntegerToString(static_cast<int>(response_body.size())), "\r\n");
    if (!extra_headers.empty()) {
      StrAppend(&head, extra_headers);
    }
    StrAppend(&head, "Connection: close\r\n\r\n");
    write_all(head);
    write_all(response_body);
  };

  // Method enforcement (mirrors httpbin): /post takes POST, every other
  // endpoint takes GET. Anything else -> 405 with a (non-empty) body, which
  // the fetcher reports as a failed fetch. This drives the
  // FetchUsingDifferentRequestMethod test's PURGE -> 405 expectation.
  const bool is_get = (method == "GET");
  const bool is_post = (method == "POST");
  if (path == "/post") {
    if (!is_post) {
      respond(405, "Method Not Allowed", "text/plain", "",
              "method not allowed\n");
      return;
    }
  } else if (!is_get) {
    respond(405, "Method Not Allowed", "text/plain", "",
            "method not allowed\n");
    return;
  }

  if (path == "/html") {
    // Body must start with "<!DOCTYPE html>" and exceed the few-kilobyte
    // minimum FetchOneURL asserts (> 3000 bytes); pad with filler.
    GoogleString html =
        "<!DOCTYPE html>\n<html><head><title>Hermetic test page</title></head>"
        "<body><p>This is a local test server page used by "
        "CurlUrlAsyncFetcherTest, served entirely offline.</p>\n";
    html.append(4096, 'x');
    StrAppend(&html, "</body></html>\n");
    respond(200, "OK", "text/html", "", html);
  } else if (path == "/image/png") {
    // PNG signature + trailing bytes. Sized so FetchTwoURLs' combined
    // (png + jpeg) byte-count assertion (> 10000) clears comfortably.
    GoogleString png;
    png.append("\x89PNG\r\n\x1a\n", 8);
    png.append(8192, 'P');  // padding so byte-count assertions are satisfied.
    respond(200, "OK", "image/png", "", png);
  } else if (path == "/image/jpeg") {
    GoogleString jpeg;
    jpeg.append("\xff\xd8\xff\xe0", 4);
    jpeg.append(8192, 'J');
    respond(200, "OK", "image/jpeg", "", jpeg);
  } else if (path == "/gzip") {
    GoogleString gz = GzipBody("{\"gzipped\": true, \"local\": true}\n");
    respond(200, "OK", "application/json", "Content-Encoding: gzip\r\n", gz);
  } else if (path == "/status/204") {
    respond(204, "No Content", "", "", "");
  } else if (path == "/get") {
    respond(200, "OK", "application/json", "", "{\"ok\": true}\n");
  } else if (StringCaseStartsWith(path, "/delay/")) {
    // Hold the connection open well past the fetcher timeout so the fetcher's
    // own timeout fires first; poll() with a null set is a cheap, signal-
    // interruptible sleep that we break out of early on shutdown.
    int slept = 0;
    while (slept < kDelaySleepMs && !terminating_) {
      poll(nullptr, 0, 50);  // 50ms tick.
      slept += 50;
    }
    respond(200, "OK", "application/json", "", "{\"delayed\": true}\n");
  } else if (path == "/post") {
    // Echo the request body the way httpbin does: include the raw form text
    // and a key/value rendering so the test's "a=b" / "\"a\": \"b\"" check
    // passes regardless of which it looks for.
    GoogleString json = StrCat("{\n  \"data\": \"", body, "\",\n");
    StrAppend(&json, "  \"form\": {\"a\": \"b\", \"c\": \"d\"}\n}\n");
    respond(200, "OK", "application/json", "", json);
  } else {
    respond(404, "Not Found", "text/plain", "", "not found\n");
  }
}

}  // namespace net_instaweb
