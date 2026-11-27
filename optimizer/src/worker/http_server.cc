// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP Management API Server Implementation

#include "src/worker/http_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <optional>
#include <utility>

#include "absl/strings/str_cat.h"
#include "lib/base/message_handler.h"
#include "lib/base/string_util.h"
#include "src/worker/posix_compat.h"
#include "src/worker/uv_helpers.h"

namespace pagespeed {

using net_instaweb::AsciiToLowerInPlace;
using net_instaweb::LowerChar;

// ---------------------------------------------------------------------------
// ApiErrorCode helpers
// ---------------------------------------------------------------------------

const char* ApiErrorCodeString(ApiErrorCode code) {
  switch (code) {
    case ApiErrorCode::kBadRequest:
      return "BAD_REQUEST";
    case ApiErrorCode::kUnauthorized:
      return "UNAUTHORIZED";
    case ApiErrorCode::kForbidden:
      return "FORBIDDEN";
    case ApiErrorCode::kNotFound:
      return "NOT_FOUND";
    case ApiErrorCode::kMethodNotAllowed:
      return "METHOD_NOT_ALLOWED";
    case ApiErrorCode::kPayloadTooLarge:
      return "PAYLOAD_TOO_LARGE";
    case ApiErrorCode::kUnsupportedMediaType:
      return "UNSUPPORTED_MEDIA_TYPE";
    case ApiErrorCode::kTooManyRequests:
      return "TOO_MANY_REQUESTS";
    case ApiErrorCode::kInternalError:
      return "INTERNAL_ERROR";
    case ApiErrorCode::kServiceUnavailable:
      return "SERVICE_UNAVAILABLE";
    case ApiErrorCode::kGatewayTimeout:
      return "GATEWAY_TIMEOUT";
    case ApiErrorCode::kBadGateway:
      return "BAD_GATEWAY";
  }
  return "UNKNOWN";
}

int ApiErrorCodeStatus(ApiErrorCode code) {
  switch (code) {
    case ApiErrorCode::kBadRequest:
      return 400;
    case ApiErrorCode::kUnauthorized:
      return 401;
    case ApiErrorCode::kForbidden:
      return 403;
    case ApiErrorCode::kNotFound:
      return 404;
    case ApiErrorCode::kMethodNotAllowed:
      return 405;
    case ApiErrorCode::kPayloadTooLarge:
      return 413;
    case ApiErrorCode::kUnsupportedMediaType:
      return 415;
    case ApiErrorCode::kTooManyRequests:
      return 429;
    case ApiErrorCode::kInternalError:
      return 500;
    case ApiErrorCode::kServiceUnavailable:
      return 503;
    case ApiErrorCode::kGatewayTimeout:
      return 504;
    case ApiErrorCode::kBadGateway:
      return 502;
  }
  return 500;
}

// ---------------------------------------------------------------------------
// Bind-address classification
// ---------------------------------------------------------------------------

namespace {

// Strict four-decimal-octet parse: exactly a.b.c.d, each 1-3 digits, 0-255,
// no trailing characters.  This deliberately matches what uv_ip4_addr will
// accept, so a bind address that classifies here is a bind address that will
// actually bind.  Writes the first octet when `first_octet` is non-null.
bool ParseIpv4ApiBind(std::string_view address, int* first_octet) {
  int octets[4] = {-1, -1, -1, -1};
  size_t pos = 0;
  for (int i = 0; i < 4; ++i) {
    if (pos >= address.size()) return false;
    int value = 0;
    size_t digits = 0;
    while (pos < address.size() && address[pos] >= '0' && address[pos] <= '9') {
      value = value * 10 + (address[pos] - '0');
      if (value > 255) return false;
      ++pos;
      ++digits;
    }
    if (digits == 0 || digits > 3) return false;
    octets[i] = value;
    if (i < 3) {
      if (pos >= address.size() || address[pos] != '.') return false;
      ++pos;
    }
  }
  if (pos != address.size()) return false;
  if (first_octet != nullptr) *first_octet = octets[0];
  return true;
}

}  // namespace

bool IsIpv4LiteralApiBind(std::string_view address) {
  // "" means "use the compiled default", which is the IPv4 literal 127.0.0.1.
  if (address.empty()) return true;
  return ParseIpv4ApiBind(address, nullptr);
}

bool IsLoopbackApiBind(std::string_view address) {
  if (address.empty()) return true;  // the compiled default, 127.0.0.1
  // IPv4 loopback is the whole 127.0.0.0/8 block, not just 127.0.0.1.
  // Hostnames and IPv6 literals never reach here as "loopback": the server
  // binds through uv_ip4_addr, which takes an IPv4 literal only, so anything
  // else is refused at config parse with a named message rather than
  // silently classified.  An address we cannot classify is never safe.
  int first_octet = -1;
  if (!ParseIpv4ApiBind(address, &first_octet)) return false;
  return first_octet == 127;
}

const char* HttpStatusText(int status_code) {
  switch (status_code) {
    case 200:
      return "OK";
    case 204:
      return "No Content";
    case 301:
      return "Moved Permanently";
    case 304:
      return "Not Modified";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 413:
      return "Payload Too Large";
    case 415:
      return "Unsupported Media Type";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Timeout";
    default:
      return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// URL decoding
// ---------------------------------------------------------------------------

// Internal helper: decode percent-encoding.  If decode_plus is true,
// '+' is converted to space (for query string values).
static std::string UrlDecodeImpl(std::string_view input, bool decode_plus) {
  using net_instaweb::TransformPercentEncoded;
  return TransformPercentEncoded(
      input,
      [](std::string& out, char decoded, int, int) { out.push_back(decoded); },
      [decode_plus](std::string& out, char c) {
        out.push_back(decode_plus && c == '+' ? ' ' : c);
      });
}

std::string UrlDecode(std::string_view input) {
  return UrlDecodeImpl(input, /*decode_plus=*/true);
}

std::string UrlDecodePath(std::string_view input) {
  return UrlDecodeImpl(input, /*decode_plus=*/false);
}

// ---------------------------------------------------------------------------
// HttpRequest
// ---------------------------------------------------------------------------

std::string_view HttpRequest::Header(std::string_view name) const {
  std::string lower(name);
  AsciiToLowerInPlace(lower);
  auto it = headers.find(lower);
  if (it != headers.end()) return it->second;
  return {};
}

std::string_view HttpRequest::QueryParam(std::string_view name) const {
  auto it = query_params.find(std::string(name));
  if (it != query_params.end()) return it->second;
  return {};
}

void HttpRequest::ParseQueryString() {
  query_params.clear();
  if (query_string.empty()) return;

  std::string_view remaining = query_string;
  while (!remaining.empty()) {
    auto amp = remaining.find('&');
    std::string_view pair =
        (amp != std::string_view::npos) ? remaining.substr(0, amp) : remaining;
    remaining =
        (amp != std::string_view::npos) ? remaining.substr(amp + 1) : "";

    auto eq = pair.find('=');
    if (eq != std::string_view::npos) {
      std::string key = UrlDecode(pair.substr(0, eq));
      std::string value = UrlDecode(pair.substr(eq + 1));
      query_params[std::move(key)] = std::move(value);
    } else if (!pair.empty()) {
      query_params[UrlDecode(pair)] = "";
    }
  }
}

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------

HttpResponse& HttpResponse::Status(int status) {
  status_code_ = status;
  return *this;
}

HttpResponse& HttpResponse::SetHeader(std::string key, std::string value) {
  // Strip CR/LF to prevent HTTP response splitting (header injection).
  std::erase(key, '\r');
  std::erase(key, '\n');
  std::erase(value, '\r');
  std::erase(value, '\n');
  headers_.emplace_back(std::move(key), std::move(value));
  return *this;
}

HttpResponse& HttpResponse::ContentType(std::string_view type) {
  return SetHeader("Content-Type", std::string(type));
}

HttpResponse& HttpResponse::Body(std::string body) {
  body_ = std::move(body);
  return *this;
}

HttpResponse& HttpResponse::Json(std::string json_body) {
  body_ = std::move(json_body);
  return SetHeader("Content-Type", "application/json");
}

HttpResponse HttpResponse::Error(ApiErrorCode code, std::string_view message,
                                 std::string_view details_json) {
  HttpResponse resp;
  resp.Status(ApiErrorCodeStatus(code));

  std::string escaped = net_instaweb::JsonEscapeMinimal(message);

  std::string json =
      absl::StrCat("{\"error\":{\"code\":\"", ApiErrorCodeString(code),
                   "\",\"message\":\"", escaped, "\"");
  if (!details_json.empty()) {
    absl::StrAppend(&json, ",\"details\":", details_json);
  }
  absl::StrAppend(&json, "}}");

  resp.Json(std::move(json));
  return resp;
}

std::string HttpResponse::Serialize(bool omit_body) const {
  std::string result;
  absl::StrAppend(&result, "HTTP/1.1 ", status_code_, " ",
                  HttpStatusText(status_code_), "\r\n");

  bool has_content_length = false;
  bool has_connection = false;
  bool has_cache_control = false;
  bool has_xcto = false;
  for (const auto& [key, value] : headers_) {
    absl::StrAppend(&result, key, ": ", value, "\r\n");
    if (key == "Content-Length") has_content_length = true;
    if (key == "Connection") has_connection = true;
    if (key == "Cache-Control") has_cache_control = true;
    if (key == "X-Content-Type-Options") has_xcto = true;
  }

  if (!has_content_length) {
    absl::StrAppend(&result, "Content-Length: ", body_.size(), "\r\n");
  }
  if (!has_connection) {
    absl::StrAppend(&result, "Connection: close\r\n");
  }
  // Security defaults: prevent MIME sniffing and browser caching of API data.
  // Handlers that set their own Cache-Control or X-Content-Type-Options
  // (e.g. static file handler) take precedence.
  if (!has_xcto) {
    absl::StrAppend(&result, "X-Content-Type-Options: nosniff\r\n");
  }
  if (!has_cache_control) {
    absl::StrAppend(&result, "Cache-Control: no-store\r\n");
  }
  absl::StrAppend(&result, "\r\n");
  // HEAD responses carry headers (including the GET Content-Length) but no body.
  if (!omit_body) {
    absl::StrAppend(&result, body_);
  }

  return result;
}

// ---------------------------------------------------------------------------
// HttpConnection — per-client state
// ---------------------------------------------------------------------------

struct HttpConnection {
  // uv_tcp_t or uv_pipe_t depending on the server's transport; every use
  // below goes through the uv_stream_t view, so the arm only matters at
  // init time.
  uv_any_handle handle;
  uv_timer_t timeout;
  HttpServer* server = nullptr;
  llhttp_t parser;
  HttpRequest request;
  uint64_t id = 0;  // Unique per connection; keys HttpServer::live_conns_.
  bool closing = false;
  bool response_sent = false;  // Guard against double SendResponse.
  bool async_pending = false;  // An async route owes this request a response.
  uint64_t deadline_ms = 0;    // Absolute uv_now() deadline for the request.
  bool keep_alive = false;     // Keep connection open after response.
  int requests_served = 0;   // Number of requests completed on this connection.
  std::string pending_data;  // Unconsumed bytes from pipelined requests.

  // Parser state tracking
  std::string current_header_field;
  std::string current_header_value;
  bool building_header_value = false;
  size_t total_header_bytes = 0;
};

// Write context — owns the serialized response data and the uv_write_t.
// Follows the same pattern as MgmtWriteContext in worker.cc.
struct HttpWriteContext {
  uv_write_t req;
  uv_buf_t buf;
  HttpConnection* conn;
  std::string data;  // Serialized HTTP response (buf points into this)
};

// ---------------------------------------------------------------------------
// HttpServer
// ---------------------------------------------------------------------------

HttpServer::HttpServer(uv_loop_t* loop, HttpServerConfig config,
                       MessageHandler* handler)
    : loop_(loop), config_(std::move(config)), handler_(handler) {
  llhttp_settings_init(&parser_settings_);
  parser_settings_.on_url = OnUrl;
  parser_settings_.on_header_field = OnHeaderField;
  parser_settings_.on_header_value = OnHeaderValue;
  parser_settings_.on_body = OnBody;
  parser_settings_.on_message_complete = OnMessageComplete;
}

HttpServer::~HttpServer() {
  if (listening_.load(std::memory_order_acquire)) {
    Stop();
  }
}

void HttpServer::SetUpgradeHandler(UpgradeHandler handler) {
  upgrade_handler_ = std::move(handler);
}

void HttpServer::AddRoute(std::string method, std::string path,
                          RouteHandler handler, RouteAuth auth) {
  Route route;
  route.method = std::move(method);
  if (path.size() > 1 && path.back() == '*') {
    route.path = path.substr(0, path.size() - 1);
    route.is_prefix = true;
  } else {
    route.path = std::move(path);
    route.is_prefix = false;
  }
  route.handler = std::move(handler);
  route.auth = auth;
  routes_.push_back(std::move(route));
}

void HttpServer::AddAsyncRoute(std::string method, std::string path,
                               AsyncRouteHandler handler, RouteAuth auth) {
  AddRoute(std::move(method), std::move(path), RouteHandler{}, auth);
  routes_.back().async_handler = std::move(handler);
}

bool HttpServer::Start() {
  if (listening_.load(std::memory_order_acquire)) return false;

  if (is_pipe()) return StartPipe();

  int r = uv_tcp_init(loop_, &server_handle_.tcp);
  if (r != 0) {
    handler_->Error("HTTP: uv_tcp_init failed: %s", uv_strerror(r));
    return false;
  }
  server_handle_.handle.data = this;

  struct sockaddr_in addr;
  r = uv_ip4_addr(config_.bind_address.c_str(), config_.port, &addr);
  if (r != 0) {
    handler_->Error("HTTP: invalid bind address: %s",
                    config_.bind_address.c_str());
    uv_close(&server_handle_.handle, nullptr);
    return false;
  }

  r = uv_tcp_bind(&server_handle_.tcp, reinterpret_cast<const sockaddr*>(&addr),
                  0);
  if (r != 0) {
    handler_->Error("HTTP: bind failed on %s:%d: %s",
                    config_.bind_address.c_str(), config_.port, uv_strerror(r));
    uv_close(&server_handle_.handle, nullptr);
    return false;
  }

  // Retrieve actual bound port (important when port=0 for tests).
  struct sockaddr_storage bound_addr;
  int namelen = sizeof(bound_addr);
  uv_tcp_getsockname(&server_handle_.tcp,
                     reinterpret_cast<struct sockaddr*>(&bound_addr), &namelen);
  bound_port_.store(
      ntohs(reinterpret_cast<struct sockaddr_in*>(&bound_addr)->sin_port),
      std::memory_order_relaxed);

  r = uv_listen(&server_handle_.stream, 128, OnConnection);
  if (r != 0) {
    handler_->Error("HTTP: listen failed: %s", uv_strerror(r));
    uv_close(&server_handle_.handle, nullptr);
    return false;
  }

  listening_.store(true, std::memory_order_release);
  handler_->Info("HTTP API listening on %s:%d", config_.bind_address.c_str(),
                 bound_port_.load(std::memory_order_relaxed));
  return true;
}

// Remove OUR socket file, and only if it is still a socket.  Between bind
// and teardown the path could have been replaced; deleting whatever is there
// by then would make shutdown a destructive operation on someone else's file.
void HttpServer::UnlinkOwnSocket() {
#ifndef _WIN32
  if (config_.socket_path.empty()) return;
  struct stat st{};
  if (::lstat(config_.socket_path.c_str(), &st) != 0) return;
  if (!S_ISSOCK(st.st_mode)) return;
  ::unlink(config_.socket_path.c_str());
#endif
}

// HTTP/1.1 over a group-scoped unix socket.  The mode is set
// with an explicit chmod after bind and is FATAL if it fails -- a management
// socket whose scope we cannot pin is a management socket any local user can
// purge the cache through.  The umask guard closes the instant between bind
// and chmod in which the socket exists at a wider mode, exactly as the
// daemon's other three listeners do.
bool HttpServer::StartPipe() {
#ifdef _WIN32
  handler_->Error("HTTP: --api-socket is not supported on this platform");
  return false;
#else
  int r = uv_pipe_init(loop_, &server_handle_.pipe, 0);
  if (r != 0) {
    handler_->Error("HTTP: uv_pipe_init failed: %s", uv_strerror(r));
    return false;
  }
  server_handle_.handle.data = this;

  // Only ever remove a stale SOCKET.  lstat, not stat: a symlink planted at
  // this path must be refused, not followed and not deleted-through.  If
  // something else is sitting here -- a regular file, a directory, someone
  // else's symlink -- unlinking it would be this daemon destroying a file it
  // was never asked to touch, so refuse and say which path and what it is.
  {
    struct stat st{};
    if (::lstat(config_.socket_path.c_str(), &st) == 0) {
      if (!S_ISSOCK(st.st_mode)) {
        handler_->Error(
            "HTTP: refusing to use API socket path %s: it already exists and "
            "is not a socket (mode %o) — remove it yourself or choose another "
            "--api-socket path",
            config_.socket_path.c_str(),
            static_cast<unsigned>(st.st_mode & 07777));
        uv_close(&server_handle_.handle, nullptr);
        return false;
      }
      if (::unlink(config_.socket_path.c_str()) != 0) {
        handler_->Error("HTTP: could not remove the stale API socket %s: %s",
                        config_.socket_path.c_str(), strerror(errno));
        uv_close(&server_handle_.handle, nullptr);
        return false;
      }
    } else if (errno != ENOENT) {
      handler_->Error("HTTP: cannot stat the API socket path %s: %s",
                      config_.socket_path.c_str(), strerror(errno));
      uv_close(&server_handle_.handle, nullptr);
      return false;
    }
  }
  {
    const mode_t previous = ::umask(0117);
    r = uv_pipe_bind(&server_handle_.pipe, config_.socket_path.c_str());
    ::umask(previous);
  }
  if (r != 0) {
    handler_->Error("HTTP: bind failed on %s: %s", config_.socket_path.c_str(),
                    uv_strerror(r));
    uv_close(&server_handle_.handle, nullptr);
    return false;
  }

  if (::chmod(config_.socket_path.c_str(), 0660) != 0) {
    handler_->Error("HTTP: failed to chmod 0660 API socket %s: %s",
                    config_.socket_path.c_str(), strerror(errno));
    uv_close(&server_handle_.handle, nullptr);
    UnlinkOwnSocket();
    return false;
  }

  r = uv_listen(&server_handle_.stream, 128, OnConnection);
  if (r != 0) {
    handler_->Error("HTTP: listen failed: %s", uv_strerror(r));
    uv_close(&server_handle_.handle, nullptr);
    UnlinkOwnSocket();
    return false;
  }

  bound_port_.store(0, std::memory_order_relaxed);
  listening_.store(true, std::memory_order_release);
  handler_->Info("HTTP API listening on unix socket %s (mode 0660)",
                 config_.socket_path.c_str());
  return true;
#endif
}

void HttpServer::Stop() {
  if (!listening_.load(std::memory_order_acquire)) return;
  listening_.store(false, std::memory_order_release);

  SafeClose(&server_handle_.handle);
#ifndef _WIN32
  if (is_pipe()) UnlinkOwnSocket();
#endif

  // Snapshot connections_ under the mutex so we don't race with OnClose
  // callbacks (running on the event loop thread) that erase from the vector.
  std::vector<HttpConnection*> conns;
  {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    conns = connections_;
  }

  // Stop reading on all connections first so no new request data is
  // processed between the server close and individual connection close.
  for (auto* conn : conns) {
    if (!conn->closing) {
      uv_read_stop(&conn->handle.stream);
    }
  }

  // Close all active connections.
  for (auto* conn : conns) {
    CloseConnection(conn);
  }
}

// ---------------------------------------------------------------------------
// libuv callbacks
// ---------------------------------------------------------------------------

void HttpServer::OnConnection(uv_stream_t* server, int status) {
  auto* self = static_cast<HttpServer*>(server->data);
  if (status < 0) {
    self->handler_->Warning("HTTP: connection error: %s", uv_strerror(status));
    return;
  }

  if (self->active_connections_.load(std::memory_order_relaxed) >=
      self->config_.max_connections) {
    auto* temp = new uv_any_handle;
    self->InitStream(temp);
    uv_accept(server, &temp->stream);
    uv_close(&temp->handle, [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
    return;
  }

  auto* conn = new HttpConnection;
  conn->server = self;
  conn->closing = false;
  conn->building_header_value = false;

  self->InitStream(&conn->handle);
  conn->handle.handle.data = conn;

  if (uv_accept(server, &conn->handle.stream) != 0) {
    // The handle was already uv_*_init'd by InitStream() above, so it must
    // be uv_close()'d rather than deleted directly (as every other accept
    // site in this daemon does): a bare delete would leave the handle
    // registered with the loop.
    uv_close(&conn->handle.handle, [](uv_handle_t* h) {
      delete static_cast<HttpConnection*>(h->data);
    });
    return;
  }

  self->active_connections_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(self->connections_mutex_);
    self->connections_.push_back(conn);
  }
  conn->id = self->next_conn_id_++;
  self->live_conns_[conn->id] = conn;

  // Start request timeout and record the absolute deadline.
  uv_timer_init(self->loop_, &conn->timeout);
  conn->timeout.data = conn;
  conn->deadline_ms = uv_now(self->loop_) + self->config_.request_timeout_ms;
  uv_timer_start(&conn->timeout, OnTimeout, self->config_.request_timeout_ms,
                 0);

  // Initialize llhttp parser for this connection.
  llhttp_init(&conn->parser, HTTP_REQUEST, &self->parser_settings_);
  conn->parser.data = conn;

  uv_read_start(&conn->handle.stream, OnAlloc, OnRead);
}

void HttpServer::OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size,
                         uv_buf_t* buf) {
  size_t size = std::min(suggested_size, size_t{65536});
  buf->base = new char[size];
  buf->len = size;
}

void HttpServer::OnRead(uv_stream_t* stream, ssize_t nread,
                        const uv_buf_t* buf) {
  auto* conn = static_cast<HttpConnection*>(stream->data);
  auto* self = conn->server;

  if (nread < 0) {
    delete[] buf->base;
    self->CloseConnection(conn);
    return;
  }

  if (nread == 0) {
    delete[] buf->base;
    return;
  }

  // Check body size limit (safe arithmetic to avoid overflow).
  size_t body_limit = self->config_.max_request_body;
  if (body_limit <= SIZE_MAX - 4096) body_limit += 4096;
  if (static_cast<size_t>(nread) > body_limit ||
      conn->request.body.size() > body_limit - static_cast<size_t>(nread)) {
    delete[] buf->base;
    self->SendResponse(conn, HttpResponse::Error(ApiErrorCode::kPayloadTooLarge,
                                                 "Request body too large"));
    return;
  }

  // Feed data to llhttp parser.
  llhttp_errno_t err =
      llhttp_execute(&conn->parser, buf->base, static_cast<size_t>(nread));

  if (err == HPE_PAUSED) {
    // Parser paused in OnMessageComplete — save any pipelined data.
    // Must copy before freeing buf->base since error_pos points into it.
    const char* pos = llhttp_get_error_pos(&conn->parser);
    auto consumed = static_cast<size_t>(pos - buf->base);
    if (consumed < static_cast<size_t>(nread)) {
      conn->pending_data.assign(pos, static_cast<size_t>(nread) - consumed);
    }
    delete[] buf->base;
    return;  // Response is being written; OnWriteDone handles next steps.
  }

  delete[] buf->base;

  if (err != HPE_OK) {
    self->SendResponse(conn, HttpResponse::Error(ApiErrorCode::kBadRequest,
                                                 "Malformed HTTP request"));
    return;
  }

  // Restart the inactivity timer while body data is being uploaded,
  // preventing slowloris attacks that trickle bytes to hold connections.
  // Cap to the overall request deadline so slow-but-steady clients
  // cannot hold a connection indefinitely.
  if (!conn->request.body.empty() && !conn->closing && !conn->response_sent) {
    uint64_t now = uv_now(self->loop_);
    uint64_t remaining =
        (now < conn->deadline_ms) ? conn->deadline_ms - now : 0;
    uint64_t timeout = std::min(
        static_cast<uint64_t>(self->config_.body_read_timeout_ms), remaining);
    if (timeout == 0) timeout = 1;  // Ensure timer fires promptly.
    uv_timer_start(&conn->timeout, OnTimeout, timeout, 0);
  }
}

void HttpServer::OnWriteDone(uv_write_t* req, int status) {
  auto* wctx = reinterpret_cast<HttpWriteContext*>(req);
  HttpConnection* conn = wctx->conn;
  HttpServer* self = conn->server;
  delete wctx;
  if (conn->closing) return;
  uv_timer_stop(&conn->timeout);

  // Write error or no keep-alive — close the connection.
  if (status != 0 || !conn->keep_alive) {
    self->CloseConnection(conn);
    return;
  }

  // Reset connection state for the next request on this connection.
  conn->request = HttpRequest{};
  conn->response_sent = false;
  conn->keep_alive = false;
  conn->building_header_value = false;
  conn->current_header_field.clear();
  conn->current_header_value.clear();
  conn->total_header_bytes = 0;

  // Resume the parser (paused in OnMessageComplete, not re-initialized
  // because llhttp tracks keep-alive state internally).
  llhttp_resume(&conn->parser);

  // Restart request timeout.
  conn->deadline_ms = uv_now(self->loop_) + self->config_.request_timeout_ms;
  uv_timer_start(&conn->timeout, OnTimeout, self->config_.request_timeout_ms,
                 0);

  // Replay any pipelined data before starting socket reads, so that
  // bytes arrive in order and uv_read_start is only called if no
  // new response was already triggered.
  if (!conn->pending_data.empty()) {
    std::string data = std::move(conn->pending_data);
    conn->pending_data.clear();
    llhttp_errno_t err =
        llhttp_execute(&conn->parser, data.data(), data.size());
    if (err == HPE_PAUSED) {
      const char* pos = llhttp_get_error_pos(&conn->parser);
      auto consumed = static_cast<size_t>(pos - data.data());
      if (consumed < data.size()) {
        conn->pending_data.assign(pos, data.size() - consumed);
      }
      // Response was sent for the pipelined request; OnWriteDone
      // will fire again for that response.  Do not restart reads.
      return;
    } else if (err != HPE_OK && !conn->response_sent) {
      self->SendResponse(conn, HttpResponse::Error(ApiErrorCode::kBadRequest,
                                                   "Malformed HTTP request"));
      return;
    }
  }

  // No pending response — start reading from the socket.
  if (!conn->response_sent) {
    uv_read_start(&conn->handle.stream, OnAlloc, OnRead);
  }
}

void HttpServer::OnClose(uv_handle_t* handle) {
  auto* conn = static_cast<HttpConnection*>(handle->data);
  if (handle == reinterpret_cast<uv_handle_t*>(&conn->handle)) {
    // TCP handle closed — now close the timer.
    SafeClose(&conn->timeout, [](uv_handle_t* h) {
      auto* c = static_cast<HttpConnection*>(h->data);
      auto* srv = c->server;
      srv->live_conns_.erase(c->id);
      srv->active_connections_.fetch_sub(1, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(srv->connections_mutex_);
        auto& v = srv->connections_;
        v.erase(std::remove(v.begin(), v.end(), c), v.end());
      }
      delete c;
    });
  } else {
    // Timer closed — delete connection.
    auto* srv = conn->server;
    srv->live_conns_.erase(conn->id);
    srv->active_connections_.fetch_sub(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lock(srv->connections_mutex_);
      auto& v = srv->connections_;
      v.erase(std::remove(v.begin(), v.end(), conn), v.end());
    }
    delete conn;
  }
}

void HttpServer::OnTimeout(uv_timer_t* timer) {
  auto* conn = static_cast<HttpConnection*>(timer->data);
  // A timed-out async request gets its 504 and then the connection closes:
  // the late Responder must never find the connection reused for a new request.
  if (conn->async_pending) conn->keep_alive = false;
  conn->server->SendResponse(
      conn,
      HttpResponse::Error(ApiErrorCode::kGatewayTimeout, "Request timeout"));
}

// ---------------------------------------------------------------------------
// llhttp parser callbacks
// ---------------------------------------------------------------------------

int HttpServer::OnUrl(llhttp_t* parser, const char* at, size_t length) {
  auto* conn = static_cast<HttpConnection*>(parser->data);
  conn->request.raw_url.append(at, length);
  // Reject early if URL exceeds limit (don't wait for OnMessageComplete).
  auto* self = conn->server;
  if (conn->request.raw_url.size() > self->config_.max_url_length) {
    return HPE_USER;
  }
  return 0;
}

int HttpServer::OnHeaderField(llhttp_t* parser, const char* at, size_t length) {
  auto* conn = static_cast<HttpConnection*>(parser->data);
  auto* self = conn->server;

  conn->total_header_bytes += length;
  if (conn->total_header_bytes > self->config_.max_header_bytes) {
    return HPE_USER;
  }

  if (conn->building_header_value) {
    AsciiToLowerInPlace(conn->current_header_field);
    conn->request.headers[conn->current_header_field] =
        std::move(conn->current_header_value);
    conn->current_header_field.clear();
    conn->current_header_value.clear();
    conn->building_header_value = false;
  }

  conn->current_header_field.append(at, length);
  return 0;
}

int HttpServer::OnHeaderValue(llhttp_t* parser, const char* at, size_t length) {
  auto* conn = static_cast<HttpConnection*>(parser->data);
  auto* self = conn->server;

  conn->total_header_bytes += length;
  if (conn->total_header_bytes > self->config_.max_header_bytes) {
    return HPE_USER;
  }

  conn->current_header_value.append(at, length);
  conn->building_header_value = true;
  return 0;
}

int HttpServer::OnBody(llhttp_t* parser, const char* at, size_t length) {
  auto* conn = static_cast<HttpConnection*>(parser->data);
  auto* self = conn->server;

  if (length > self->config_.max_request_body ||
      conn->request.body.size() > self->config_.max_request_body - length) {
    return HPE_USER;
  }
  conn->request.body.append(at, length);
  return 0;
}

bool HttpServer::IsWsOriginAllowed(std::string_view origin,
                                   std::string_view origin_allowlist) {
  // No allowlist (or wildcard) configured: impose no restriction.
  if (origin_allowlist.empty() || origin_allowlist == "*") {
    return true;
  }
  // Absent Origin: a non-browser client (browsers always send Origin on a WS
  // handshake), so it is not a cross-site hijacking vector.
  if (origin.empty()) {
    return true;
  }
  return origin == origin_allowlist;
}

int HttpServer::OnMessageComplete(llhttp_t* parser) {
  auto* conn = static_cast<HttpConnection*>(parser->data);
  auto* self = conn->server;

  // Flush last header if any.
  if (conn->building_header_value) {
    AsciiToLowerInPlace(conn->current_header_field);
    conn->request.headers[conn->current_header_field] =
        std::move(conn->current_header_value);
    conn->building_header_value = false;
  }

  // Set method from parser.
  conn->request.method =
      llhttp_method_name(static_cast<llhttp_method_t>(parser->method));

  // Parse URL into path and query string.
  std::string_view url = conn->request.raw_url;

  if (url.size() > self->config_.max_url_length) {
    self->SendResponse(
        conn, HttpResponse::Error(ApiErrorCode::kBadRequest, "URL too long"));
    return HPE_PAUSED;
  }

  auto qpos = url.find('?');
  if (qpos != std::string_view::npos) {
    conn->request.path = UrlDecodePath(url.substr(0, qpos));
    conn->request.query_string = std::string(url.substr(qpos + 1));
  } else {
    conn->request.path = UrlDecodePath(url);
  }
  conn->request.ParseQueryString();

  // Reject null bytes in path (null-byte injection prevention).
  if (conn->request.path.find('\0') != std::string::npos) {
    self->SendResponse(
        conn, HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid path"));
    return HPE_PAUSED;
  }

  // Reject path traversal.
  if (conn->request.path.find("..") != std::string::npos) {
    self->SendResponse(
        conn, HttpResponse::Error(ApiErrorCode::kBadRequest, "Invalid path"));
    return HPE_PAUSED;
  }

  // Stop the timeout timer.
  uv_timer_stop(&conn->timeout);

  // Detect WebSocket upgrade requests.
  if (self->upgrade_handler_ && conn->request.method == "GET") {
    std::string_view conn_hdr = conn->request.Header("connection");
    std::string_view upgrade_hdr = conn->request.Header("upgrade");
    std::string_view ws_key = conn->request.Header("sec-websocket-key");

    // Case-insensitive check for "upgrade" as a token in the Connection
    // header (comma-separated list per RFC 7230 §6.1) and "websocket"
    // in the Upgrade header (RFC 6455 §4.2.1).
    bool has_upgrade = false;
    bool has_websocket = false;
    {
      // Tokenize Connection header on commas and match whole tokens.
      std::string lower_conn(conn_hdr);
      AsciiToLowerInPlace(lower_conn);
      std::string_view remaining = lower_conn;
      while (!remaining.empty()) {
        auto comma = remaining.find(',');
        std::string_view token = (comma != std::string_view::npos)
                                     ? remaining.substr(0, comma)
                                     : remaining;
        remaining = (comma != std::string_view::npos)
                        ? remaining.substr(comma + 1)
                        : "";
        // Trim whitespace.
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (token == "upgrade") {
          has_upgrade = true;
          break;
        }
      }

      std::string lower_upg(upgrade_hdr);
      AsciiToLowerInPlace(lower_upg);
      has_websocket = (lower_upg == "websocket");
    }

    // RFC 6455 §4.2.1: Sec-WebSocket-Version MUST be 13.
    std::string_view ws_version = conn->request.Header("sec-websocket-version");

    if (has_upgrade && has_websocket && !ws_key.empty() && ws_version == "13" &&
        ws_key.size() == 24) {
      // Opt-in cross-site WebSocket hijacking defense: when an Origin allowlist
      // is configured (config_.cors_origin), reject an upgrade whose
      // browser-supplied Origin does not match. CORS headers (ApplyCors) do not
      // protect WebSockets, and in read_open / no-token mode the handshake is
      // otherwise unauthenticated. Default (empty cors_origin) imposes no check.
      if (!IsWsOriginAllowed(conn->request.Header("origin"),
                             self->config_.cors_origin)) {
        self->SendResponse(conn, HttpResponse::Error(ApiErrorCode::kForbidden,
                                                     "Origin not allowed"));
        return HPE_PAUSED;
      }

      // WebSocket upgrades require auth when a token is set, unless read_open
      // mode is enabled (WS streams are read-only).  A tokenless server that
      // was not explicitly opened with --api-no-auth refuses the handshake
      // for the same reason CheckAuth does -- and, for the same reason again,
      // the unix socket's group scope authenticates the peer outright.
      if (self->is_pipe()) {
        // Authenticated by transport; fall through to the upgrade.
      } else if (self->config_.auth_token.empty() &&
                 !self->config_.allow_unauthenticated) {
        self->SendResponse(conn,
                           HttpResponse::Error(ApiErrorCode::kUnauthorized,
                                               "Authentication required"));
        return HPE_PAUSED;
      }
      if (!self->is_pipe() && !self->config_.auth_token.empty() &&
          !self->config_.read_open) {
        std::string_view auth_header = conn->request.Header("authorization");
        constexpr std::string_view kBearer = "Bearer ";
        if (auth_header.empty() || !auth_header.starts_with(kBearer) ||
            !net_instaweb::ConstantTimeCompare(
                auth_header.substr(kBearer.size()), self->config_.auth_token)) {
          self->SendResponse(
              conn, HttpResponse::Error(
                        auth_header.empty() ? ApiErrorCode::kUnauthorized
                                            : ApiErrorCode::kForbidden,
                        auth_header.empty() ? "Authentication required"
                                            : "Invalid token"));
          return HPE_PAUSED;
        }
      }

      // Move the request out before detaching — DetachConnection
      // schedules conn for async deletion.
      HttpRequest request = std::move(conn->request);
      uv_stream_t* detached = self->DetachConnection(conn);
      if (detached) {
        self->upgrade_handler_(std::move(request), detached);
      }
      return HPE_PAUSED;
    }
  }

  // Determine keep-alive: respect the client's preference and enforce
  // the server's per-connection request limit.
  conn->requests_served++;
  conn->keep_alive =
      llhttp_should_keep_alive(parser) &&
      conn->requests_served < self->config_.max_requests_per_connection;

  std::optional<HttpResponse> dispatched =
      self->DispatchRequest(conn->request, conn);
  if (!dispatched.has_value()) {
    // Async route: the response comes later through CompleteAsync.  Stop
    // reading (as SendResponse does) and keep the parser paused so a
    // pipelined request waits for this one; start a fresh full request
    // timeout for the deferred response, so a handler that never answers is
    // bounded.  The receive deadline is not reused: it includes idle time on
    // a keep-alive connection and may be nearly spent already.
    // (A handler that answered inline has already sent; nothing to arm.)
    if (conn->async_pending) {
      uv_read_stop(&conn->handle.stream);
      conn->deadline_ms =
          uv_now(self->loop_) + self->config_.request_timeout_ms;
      uv_timer_start(&conn->timeout, OnTimeout,
                     self->config_.request_timeout_ms, 0);
    }
    return HPE_PAUSED;
  }
  HttpResponse response = std::move(*dispatched);
  if (conn->keep_alive) {
    response.SetHeader("Connection", "keep-alive");
  }
  self->ApplyCors(conn->request, response);
  self->SendResponse(conn, response);

  // Pause the parser to prevent processing pipelined data in this
  // llhttp_execute call.  OnWriteDone will resume when ready.
  return HPE_PAUSED;
}

// ---------------------------------------------------------------------------
// Request dispatch and middleware
// ---------------------------------------------------------------------------

std::optional<HttpResponse> HttpServer::DispatchRequest(
    const HttpRequest& request, HttpConnection* conn) {
  // Handle CORS preflight.
  if (request.method == "OPTIONS" && !config_.cors_origin.empty()) {
    return HttpResponse()
        .Status(204)
        .SetHeader("Access-Control-Allow-Methods",
                   "GET, POST, PATCH, DELETE, OPTIONS")
        .SetHeader("Access-Control-Allow-Headers",
                   "Content-Type, Authorization, X-Requested-With")
        .SetHeader("Access-Control-Max-Age", "86400");
  }

  // The health endpoint is always unauthenticated.
  bool skip_auth = (request.path == "/v1/health");

  // The console's static bundle carries no operational data, so GET/HEAD to
  // /console and /console/* are exempt from the bearer token too.  The
  // exemption is deliberately narrower than the bundle's prefix route: it
  // covers exactly the /console path segment ("/consoles" is not exempt) and
  // only read methods — a POST to a console path still requires the token,
  // and every /v1/* data endpoint stays behind it regardless of method.
  if (!skip_auth && (request.method == "GET" || request.method == "HEAD") &&
      (request.path == "/console" || request.path.starts_with("/console/"))) {
    skip_auth = true;
  }

  if (!skip_auth) {
    auto auth_error = CheckAuth(request, ReadOpenApplies(request));
    if (auth_error.has_value()) {
      return auth_error;
    }
  }

  // CSRF protection for state-changing data/admin endpoints reachable via the
  // embedded console proxy (/v1/cache/*, /v1/capture/*, /v1/config). These run
  // with skip_auth == false, so when no API token is configured (the default
  // in-process ASP.NET host) CheckAuth() lets them through without a credential.
  // Require X-Requested-With: XMLHttpRequest — a non-simple header that forces a
  // CORS preflight, which the worker (no CORS response headers) fails, blocking
  // cross-origin POST/PATCH. The SPA sends this header on every POST/PATCH
  // (workbench direct-transport.ts; PR #435). Skipped on TCP when an API token
  // is configured, since the bearer token is then the authenticating
  // credential.
  // KEPT ON for the unix socket.  A browser cannot address an AF_UNIX path
  // directly, but the intended topology puts a reverse proxy in front of this
  // socket to serve the console -- and through that proxy a browser CAN reach
  // it, cross-origin, with no credential anywhere on the path, because the
  // socket transport deliberately asks for none.  That makes this header the
  // only thing left standing between a cross-origin POST and cache purge, so
  // it is exactly where the backstop has to stay.  Peers on the socket send
  // it on mutating requests; the console SPA already does.
  if ((is_pipe() ||
       (config_.auth_token.empty() && config_.allow_unauthenticated)) &&
      (request.method == "POST" || request.method == "PATCH") &&
      request.path.starts_with("/v1/")) {
    std::string_view x_requested_with = request.Header("x-requested-with");
    if (x_requested_with != "XMLHttpRequest") {
      return HttpResponse::Error(
          ApiErrorCode::kBadRequest,
          "Missing or invalid CSRF header — state-changing requests require "
          "X-Requested-With: XMLHttpRequest");
    }
  }

  // Reject non-JSON POST/PATCH bodies to prevent accidental
  // form-encoded or plaintext submissions to the JSON API.
  if ((request.method == "POST" || request.method == "PATCH") &&
      !request.body.empty() && request.path.starts_with("/v1/")) {
    std::string_view content_type = request.Header("content-type");
    if (!content_type.starts_with("application/json")) {
      return HttpResponse::Error(ApiErrorCode::kUnsupportedMediaType,
                                 "Content-Type must be application/json");
    }
  }

  // Route matching.  HEAD is served by the matching GET route (RFC 9110 §9.3.2:
  // every GET resource also answers HEAD with identical headers and no body);
  // the framing layer drops the body for HEAD when the response is written.
  std::string_view route_method =
      (request.method == "HEAD") ? std::string_view("GET") : request.method;
  for (const auto& route : routes_) {
    bool method_match = (route.method == route_method);
    bool path_match = route.is_prefix ? request.path.starts_with(route.path)
                                      : (request.path == route.path);

    if (path_match && method_match) {
      // Last-resort net: a handler that fails answers 500 instead of
      // stopping the optimizer. The exception text itself is never echoed
      // into the response or the log, only a fixed, generic message, so a
      // handler bug can't turn into an information leak.
      try {
        if (route.async_handler) {
          // Mark the request as awaiting its response BEFORE calling the
          // handler, so a handler that answers inline still gets through.
          conn->async_pending = true;
          uint64_t id = conn->id;
          route.async_handler(request, [this, id](HttpResponse response) {
            CompleteAsync(id, std::move(response));
          });
          return std::nullopt;
        }
        return route.handler(request);
      } catch (const std::exception&) {
        conn->async_pending = false;
        handler_->Warning("HTTP: route handler threw; answering 500");
        return HttpResponse::Error(ApiErrorCode::kInternalError,
                                   "Internal server error");
      }
    }
  }

  // Check if path matches any route with different method (405 vs 404).
  for (const auto& route : routes_) {
    bool path_match = route.is_prefix ? request.path.starts_with(route.path)
                                      : (request.path == route.path);
    if (path_match) {
      return HttpResponse::Error(ApiErrorCode::kMethodNotAllowed,
                                 "Method not allowed");
    }
  }

  return HttpResponse::Error(ApiErrorCode::kNotFound, "Not found");
}

void HttpServer::CompleteAsync(uint64_t conn_id, HttpResponse response) {
  auto it = live_conns_.find(conn_id);
  if (it == live_conns_.end()) return;
  HttpConnection* conn = it->second;
  if (conn->closing || conn->response_sent || !conn->async_pending) return;
  conn->async_pending = false;
  if (conn->keep_alive) {
    response.SetHeader("Connection", "keep-alive");
  }
  ApplyCors(conn->request, response);
  SendResponse(conn, response);
}

std::optional<HttpResponse> HttpServer::CheckAuth(const HttpRequest& request,
                                                  bool read_open_applies) {
  // On the unix-socket transport the FILESYSTEM is
  // the credential.  Reaching this socket at all means passing its 0660
  // `pagespeed` group scope, which is the same boundary that already governs
  // the cache volume and the management socket -- so a peer that got a
  // connection accepted here is an authenticated peer, and asking it for a
  // bearer token on top would hand the web-server user a credential to store
  // and rotate for no added boundary.  That removal is the entire reason the
  // socket transport was chosen over 127.0.0.1 + token.
  //
  // TCP is unchanged: a token is required there unless the operator passed
  // --api-no-auth.
  if (is_pipe()) {
    return std::nullopt;
  }

  // An empty token no longer means "no auth required".  It used to return
  // "no error" for EVERY request, which is what left POST /v1/cache/purge
  // and the whole read surface reachable with no credential whenever the API
  // was on.  Failing closed here is the second line: the config-parse
  // invariant already refuses to start in this state unless the operator
  // asked for it with --api-no-auth.
  if (config_.auth_token.empty()) {
    if (config_.allow_unauthenticated) {
      return std::nullopt;
    }
    return HttpResponse::Error(ApiErrorCode::kUnauthorized,
                               "Authentication required");
  }

  // Read-open mode: an explicitly allow-listed GET/HEAD route doesn't
  // require auth.  HEAD is the read-only sibling of GET (same auth
  // semantics, no body) and is what monitoring tools (UptimeRobot, curl
  // health checks) use, so it must follow the same bypass as GET —
  // otherwise HEAD returns a false 401 while GET to the same endpoint
  // succeeds.  A route NOT registered with RouteAuth::kReadOpenOk (the
  // default) keeps the token here, read-open or not.
  if (config_.read_open && read_open_applies &&
      (request.method == "GET" || request.method == "HEAD")) {
    return std::nullopt;
  }

  std::string_view auth_header = request.Header("authorization");
  if (auth_header.empty()) {
    return HttpResponse::Error(ApiErrorCode::kUnauthorized,
                               "Authentication required");
  }

  constexpr std::string_view kBearerPrefix = "Bearer ";
  if (!auth_header.starts_with(kBearerPrefix)) {
    return HttpResponse::Error(ApiErrorCode::kUnauthorized,
                               "Invalid authorization scheme");
  }

  std::string_view token = auth_header.substr(kBearerPrefix.size());
  if (!net_instaweb::ConstantTimeCompare(token, config_.auth_token)) {
    return HttpResponse::Error(ApiErrorCode::kForbidden, "Invalid token");
  }

  return std::nullopt;
}

bool HttpServer::ReadOpenApplies(const HttpRequest& request) const {
  // Explicit allow-list: only a route registered with RouteAuth::kReadOpenOk
  // bypasses the token under --api-read-open.  Everything else -- a route
  // left at the default, and a path that matches no registered route at
  // all -- fails closed, so a new route that forgets to opt in does not
  // silently join the public read-only surface.
  //
  // This mirrors DispatchRequest's own route-matching loop exactly (first
  // match in registration order, HEAD normalized to GET), and answers about
  // THAT route specifically -- not "does any kReadOpenOk route happen to
  // match this path".  The distinction matters once a prefix route exists:
  // scanning for any allow-listed match, ignoring order, would fail OPEN
  // for a request whose path a kReadOpenOk prefix matches but whose more
  // specific, earlier-registered kToken route is what dispatch actually
  // picks.  Checking the same first match dispatch would pick keeps the two
  // in lockstep by construction.
  const std::string_view method = (request.method == "HEAD")
                                      ? std::string_view("GET")
                                      : std::string_view(request.method);
  for (const auto& route : routes_) {
    const bool path_match = route.is_prefix
                                ? request.path.starts_with(route.path)
                                : (request.path == route.path);
    if (path_match && route.method == method) {
      return route.auth == RouteAuth::kReadOpenOk;
    }
  }
  return false;
}

void HttpServer::ApplyCors(const HttpRequest& request, HttpResponse& response) {
  if (config_.cors_origin.empty()) return;

  std::string_view origin = request.Header("origin");
  if (origin.empty()) return;

  if (config_.cors_origin == "*") {
    // Wildcard CORS: allow any origin but WITHOUT credentials.  Reflecting
    // an arbitrary Origin with Access-Control-Allow-Credentials: true is a
    // classic CORS misconfiguration that lets any site make credentialed
    // cross-origin requests.  Use literal "*" instead.
    response.SetHeader("Access-Control-Allow-Origin", "*");
    // No Access-Control-Allow-Credentials — browsers enforce this correctly:
    // credentialed requests are rejected with wildcard origin.
    response.SetHeader("Vary", "Origin");
  } else if (config_.cors_origin == std::string(origin)) {
    // Specific origin configured: safe to enable credentials.
    response.SetHeader("Access-Control-Allow-Origin", std::string(origin));
    response.SetHeader("Access-Control-Allow-Credentials", "true");
    response.SetHeader("Vary", "Origin");
  }
}

// ---------------------------------------------------------------------------
// Response sending and connection lifecycle
// ---------------------------------------------------------------------------

void HttpServer::SendResponse(HttpConnection* conn,
                              const HttpResponse& response) {
  if (conn->closing || conn->response_sent) return;
  conn->response_sent = true;

  // Stop reading so we don't process more data on this connection.
  uv_read_stop(&conn->handle.stream);

  auto* wctx = new HttpWriteContext;
  wctx->conn = conn;
  // HEAD responses send headers (and the GET Content-Length) but no body.
  const bool omit_body = (conn->request.method == "HEAD");
  wctx->data = response.Serialize(omit_body);
  wctx->buf = uv_buf_init(wctx->data.data(), wctx->data.size());

  // Restart the timeout as a write timeout: if the client reads too slowly,
  // the connection is closed after config_.timeout_ms to prevent slow-read
  // DoS that exhausts the max_connections limit.
  uv_timer_stop(&conn->timeout);
  uv_timer_start(&conn->timeout, OnTimeout, config_.request_timeout_ms, 0);

  int r =
      uv_write(&wctx->req, &conn->handle.stream, &wctx->buf, 1, OnWriteDone);
  if (r != 0) {
    delete wctx;
    CloseConnection(conn);
  }
}

uv_stream_t* HttpServer::DetachConnection(HttpConnection* conn) {
  if (conn->closing) return nullptr;
  conn->closing = true;

  // Stop reading and cancel timeout on the original handle.
  uv_read_stop(&conn->handle.stream);
  uv_timer_stop(&conn->timeout);

  // Extract the OS socket fd from the existing handle.
  uv_os_fd_t fd;
  if (uv_fileno(reinterpret_cast<uv_handle_t*>(&conn->handle), &fd) != 0) {
    conn->closing = false;
    CloseConnection(conn);
    return nullptr;
  }

  // Duplicate the fd so the new handle owns an independent copy.
  // The original fd stays with the HttpConnection and will be closed
  // when we uv_close its handle below.
#ifdef _WIN32
  // On Windows, uv_os_fd_t is HANDLE. Use WSADuplicateSocket + WSASocket.
  WSAPROTOCOL_INFOW info;
  if (WSADuplicateSocketW(reinterpret_cast<SOCKET>(fd), GetCurrentProcessId(),
                          &info) != 0) {
    conn->closing = false;
    CloseConnection(conn);
    return nullptr;
  }
  SOCKET new_sock = WSASocketW(info.iAddressFamily, info.iSocketType,
                               info.iProtocol, &info, 0, WSA_FLAG_OVERLAPPED);
  if (new_sock == INVALID_SOCKET) {
    conn->closing = false;
    CloseConnection(conn);
    return nullptr;
  }
  uv_os_sock_t new_fd = new_sock;
#else
  int new_fd = dup(fd);
  if (new_fd < 0) {
    conn->closing = false;
    CloseConnection(conn);
    return nullptr;
  }
#endif

  // Allocate a new heap handle and open the duplicated socket on it.  Always
  // a uv_any_handle so the owner can free it without knowing which arm it is.
  auto* new_handle = new uv_any_handle;
  InitStream(new_handle);
  const int open_rc = is_pipe() ? uv_pipe_open(&new_handle->pipe, new_fd)
                                : uv_tcp_open(&new_handle->tcp, new_fd);
  if (open_rc != 0) {
#ifdef _WIN32
    closesocket(new_fd);
#else
    close(new_fd);
#endif
    uv_close(&new_handle->handle, [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
    conn->closing = false;
    CloseConnection(conn);
    return nullptr;
  }

  // Close the old HttpConnection handle (closes the original fd).
  uv_close(&conn->handle.handle, OnClose);

  return &new_handle->stream;
}

void HttpServer::InitStream(uv_any_handle* handle) {
  if (is_pipe()) {
    uv_pipe_init(loop_, &handle->pipe, 0);
  } else {
    uv_tcp_init(loop_, &handle->tcp);
  }
}

void HttpServer::CloseConnection(HttpConnection* conn) {
  if (conn->closing) return;
  conn->closing = true;

  uv_read_stop(&conn->handle.stream);
  uv_timer_stop(&conn->timeout);
  uv_close(&conn->handle.handle, OnClose);
}

}  // namespace pagespeed
