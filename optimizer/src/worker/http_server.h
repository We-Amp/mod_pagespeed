// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP Management API Server
//
// Lightweight HTTP/1.1 server embedded in the worker process for the
// PageSpeed Workbench (web console + VS Code extension).  Runs on
// the libuv event loop alongside the existing Unix socket listeners.
// Uses llhttp for request parsing.
//
// Serves both the JSON API (/v1/*) and the web console (/console/*).

#ifndef PAGESPEED_SRC_WORKER_HTTP_SERVER_H_
#define PAGESPEED_SRC_WORKER_HTTP_SERVER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "llhttp.h"
#include "uv.h"

namespace pagespeed {

class MessageHandler;

// Standard error codes for JSON API responses.
enum class ApiErrorCode : std::uint8_t {
  kBadRequest,
  kUnauthorized,
  kForbidden,
  kNotFound,
  kMethodNotAllowed,
  kPayloadTooLarge,
  kUnsupportedMediaType,
  kTooManyRequests,
  kInternalError,
  kServiceUnavailable,
  kGatewayTimeout,
  kBadGateway,
};

// Returns the string name for an error code (e.g., "BAD_REQUEST").
const char* ApiErrorCodeString(ApiErrorCode code);

// Returns the HTTP status code for an error code (e.g., 400).
int ApiErrorCodeStatus(ApiErrorCode code);

// Parsed HTTP request.
struct HttpRequest {
  std::string method;        // "GET", "POST", "PATCH", etc.
  std::string path;          // URL path (decoded, no query string)
  std::string query_string;  // Raw query string (after '?')
  std::string body;          // Request body
  std::string raw_url;       // Original URL as received

  // Parsed headers (lowercased keys).
  std::unordered_map<std::string, std::string> headers;

  // Parsed query parameters (decoded).
  std::unordered_map<std::string, std::string> query_params;

  // Helper to get a header value (empty string if not present).
  std::string_view Header(std::string_view name) const;

  // Helper to get a query parameter (empty string if not present).
  std::string_view QueryParam(std::string_view name) const;

  // Parse query string into query_params map.
  void ParseQueryString();
};

// HTTP response builder.
class HttpResponse {
 public:
  HttpResponse() = default;

  // Set status code (default 200).
  HttpResponse& Status(int status);

  // Set a response header.
  HttpResponse& SetHeader(std::string key, std::string value);

  // Set Content-Type header.
  HttpResponse& ContentType(std::string_view type);

  // Set body.
  HttpResponse& Body(std::string body);

  // Set JSON body (also sets Content-Type to application/json).
  HttpResponse& Json(std::string json_body);

  // Build standard JSON error response.
  static HttpResponse Error(ApiErrorCode code, std::string_view message,
                            std::string_view details_json = "");

  // Serialize to HTTP/1.1 wire format.  When omit_body is true (HEAD requests)
  // the body is left off the wire but Content-Length still reflects the body
  // that a GET would have returned, per RFC 9110 §9.3.2.
  std::string Serialize(bool omit_body = false) const;

  int status_code() const { return status_code_; }
  const std::string& body() const { return body_; }

 private:
  int status_code_ = 200;
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string body_;
};

// Route handler callback.
// Return an HttpResponse for the given request.
using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

// Deferred-response callback handed to an async route handler.  Calling it
// sends the response on the connection the request arrived on.  It must be
// called on the event-loop thread, at most once, and only while the server is
// alive.  If the connection has gone away (closed by the peer, timed out, or
// the server stopped) in the meantime the response is dropped.
using Responder = std::function<void(HttpResponse)>;

// Async route handler: receives the request and a Responder and may return
// before the response exists.  The server sends nothing until the Responder
// is called, and holds back the next pipelined request on the connection in
// the meantime.  The connection's request timeout still applies and answers
// 504 (closing the connection) if the Responder is not called in time.
using AsyncRouteHandler = std::function<void(const HttpRequest&, Responder)>;

// WebSocket upgrade handler callback.
// Called when the HTTP server detects a valid WebSocket upgrade request.
// The handler receives the parsed request (moved out of the connection) and a
// heap-allocated uv_any_handle (a uv_tcp_t or a uv_pipe_t, depending on the
// transport) that owns the connection socket, presented as a uv_stream_t*.
// The handler takes full ownership: it must eventually uv_close it and free
// it as a uv_any_handle* (never as one arm of the union — the arms differ in
// size).
using UpgradeHandler =
    std::function<void(HttpRequest request, uv_stream_t* handle)>;

// How a route's authentication relates to --api-read-open.
//
// This is an explicit allow-list, not an opt-out: kToken is the default, so
// a route that is registered without naming this parameter requires the
// bearer token on TCP regardless of --api-read-open.  A route must opt in
// with kReadOpenOk to become readable without the token under read-open --
// a new GET route added later and left at the default stays closed, rather
// than silently becoming part of the public read-only surface.
enum class RouteAuth : uint8_t {
  // The bearer token is required on TCP, --api-read-open or not.  The
  // default; also correct for every mutating route (POST/PATCH/DELETE),
  // since --api-read-open only ever bypasses GET/HEAD.
  kToken,
  // Opts this GET/HEAD route out of the token requirement specifically
  // under --api-read-open, for read-only data judged safe for a public
  // read-only console.  Nothing else differs: the unix socket and
  // --api-no-auth treat it like any route.
  kReadOpenOk,
};

// A single route registration.
struct Route {
  std::string method;      // HTTP method ("GET", "POST", etc.)
  std::string path;        // Exact path or prefix (ending with *)
  bool is_prefix = false;  // True if path ends with *
  RouteAuth auth = RouteAuth::kToken;
  RouteHandler handler;
  // Set instead of `handler` for a route registered with AddAsyncRoute.
  AsyncRouteHandler async_handler;
};

// The packaged unix-socket path for the management API.
// Lives in the daemon's RuntimeDirectory, which the unit creates 0750
// pagespeed:pagespeed; the socket itself is chmod 0660 after bind.
inline constexpr const char* kDefaultApiSocketPath =
    "/run/pagespeed-optimizer/api.sock";

// Configuration for the HTTP server.
struct HttpServerConfig {
  // TCP transport.  Used when socket_path is empty.
  std::string bind_address = "127.0.0.1";
  int port = 9880;
  // AF_UNIX transport.  When non-empty the server speaks
  // HTTP/1.1 over a unix socket at this path INSTEAD of binding TCP, and the
  // socket's 0660 group scope is the authentication for local peers: no
  // credential is provisioned to the module, and "binds localhost only" is
  // true by binding no port at all.
  std::string socket_path;
  int max_connections = 32;
  size_t max_request_body = 64 * 1024;  // 64KB
  size_t max_url_length = 8192;
  size_t max_header_bytes = 64 * 1024;  // 64KB total headers
  int request_timeout_ms = 10000;       // 10s
  int body_read_timeout_ms = 5000;      // 5s inactivity during body upload
  int max_requests_per_connection =
      100;  // Keep-alive request limit per TCP conn

  // Authentication token.  Empty means "no credential is configured", which
  // is only a legal state when allow_unauthenticated is also set.
  std::string auth_token;

  // Whether an empty auth_token is permitted to answer
  // requests.  FALSE by default: an unset token fails closed rather than
  // opening every endpoint, cache purge included.  The daemon sets it from
  // the deliberate --api-no-auth opt-out, which the config-parse invariant
  // only accepts for a loopback or unix-socket transport.
  bool allow_unauthenticated = false;

  // CORS origin allowlist (empty = CORS disabled).
  std::string cors_origin;

  // Whether GET endpoints are open without auth.
  bool read_open = false;
};

// HTTP connection state (per-client parser and buffer).
struct HttpConnection;

// Lightweight HTTP/1.1 server using libuv + llhttp.
//
// Usage:
//   HttpServer server(loop, config, handler);
//   server.AddRoute("GET", "/v1/health", [](const HttpRequest& req) {
//     return HttpResponse().Json("{\"status\":\"ok\"}");
//   });
//   server.Start();
//   // ... run loop ...
//   server.Stop();
class HttpServer {
 public:
  // The server does not own the loop or handler.
  HttpServer(uv_loop_t* loop, HttpServerConfig config, MessageHandler* handler);
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Register a route handler.
  // Path may end with '*' for prefix matching.  `auth` opts a GET/HEAD route
  // INTO --api-read-open (see RouteAuth); the default keeps the token
  // required regardless of read-open.
  void AddRoute(std::string method, std::string path, RouteHandler handler,
                RouteAuth auth = RouteAuth::kToken);

  // Register a route whose response may be produced later.  Authentication,
  // CSRF and content-type checks run exactly as for AddRoute; see
  // AsyncRouteHandler for the response contract.
  void AddAsyncRoute(std::string method, std::string path,
                     AsyncRouteHandler handler,
                     RouteAuth auth = RouteAuth::kToken);

  // Set a handler for WebSocket upgrade requests.
  // When a request carries "Connection: Upgrade" + "Upgrade: websocket"
  // headers and a valid Sec-WebSocket-Key, the server detaches the TCP
  // connection and hands it to this callback instead of routing normally.
  void SetUpgradeHandler(UpgradeHandler handler);

  // Start listening (TCP, or a unix socket when config.socket_path is set).
  // Returns true on success.
  bool Start();

  // Stop accepting connections and close all active connections.
  void Stop();

  // Get the number of active connections.
  int active_connections() const {
    return active_connections_.load(std::memory_order_relaxed);
  }

  // Get the actual bound port (useful when port=0 for tests).
  int bound_port() const { return bound_port_.load(std::memory_order_relaxed); }

  // Opt-in cross-site WebSocket hijacking defense. Returns true if a WebSocket
  // upgrade carrying browser-supplied |origin| should be allowed given the
  // configured |origin_allowlist| (HttpServerConfig::cors_origin). An empty or
  // "*" allowlist imposes no restriction (default behavior), and an absent
  // Origin is allowed (non-browser clients, which are not a CSWSH vector);
  // otherwise the Origin must match the allowlist exactly. Exposed for testing.
  static bool IsWsOriginAllowed(std::string_view origin,
                                std::string_view origin_allowlist);

 private:
  // libuv callbacks
  static void OnConnection(uv_stream_t* server, int status);
  static void OnAlloc(uv_handle_t* handle, size_t suggested_size,
                      uv_buf_t* buf);
  static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
  static void OnWriteDone(uv_write_t* req, int status);
  static void OnClose(uv_handle_t* handle);
  static void OnTimeout(uv_timer_t* timer);

  // llhttp parser callbacks
  static int OnUrl(llhttp_t* parser, const char* at, size_t length);
  static int OnHeaderField(llhttp_t* parser, const char* at, size_t length);
  static int OnHeaderValue(llhttp_t* parser, const char* at, size_t length);
  static int OnBody(llhttp_t* parser, const char* at, size_t length);
  static int OnMessageComplete(llhttp_t* parser);

  // Dispatch a parsed request to the router.  Returns nullopt when the
  // request matched an async route: the handler now owns the Responder and
  // the response is sent later through CompleteAsync.
  std::optional<HttpResponse> DispatchRequest(const HttpRequest& request,
                                              HttpConnection* conn);

  // Deliver a deferred response.  Looks the connection up by id and drops the
  // response if it is gone, closing, or no longer waiting for this response.
  void CompleteAsync(uint64_t conn_id, HttpResponse response);

  // Check authentication for a request.  `read_open_applies` is true only
  // when the request targets a route registered with RouteAuth::kReadOpenOk.
  // Returns nullopt if auth is OK, or an error response if not.
  std::optional<HttpResponse> CheckAuth(const HttpRequest& request,
                                        bool read_open_applies);

  // True only when the request's method and path match a route registered
  // with RouteAuth::kReadOpenOk (HEAD matching GET, as routing does).  False
  // by default -- including for a path that matches no registered route at
  // all -- so a route (or non-route) that never named the option stays
  // closed under --api-read-open.
  bool ReadOpenApplies(const HttpRequest& request) const;

  // Check CORS and add headers to response.
  void ApplyCors(const HttpRequest& request, HttpResponse& response);

  // Send a response on a connection.  Keep-alive connections are
  // reset for the next request in OnWriteDone; others are closed.
  void SendResponse(HttpConnection* conn, const HttpResponse& response);

  // Close a connection.
  void CloseConnection(HttpConnection* conn);

  // Detach a connection for WebSocket upgrade.
  // Transfers the socket fd to a new heap-allocated uv_any_handle,
  // cleans up the HttpConnection, and returns it as a uv_stream_t*.
  // Returns nullptr on failure.
  uv_stream_t* DetachConnection(HttpConnection* conn);

  // AF_UNIX listener setup.  Bind, explicit chmod 0660,
  // listen; any failure is fatal to Start().
  bool StartPipe();

  // Initialize `handle` as the arm this server's transport uses.
  void InitStream(uv_any_handle* handle);

  // Remove the API socket file, but only while it is still a socket.
  void UnlinkOwnSocket();

  // True when this server is listening on a unix socket rather than TCP.
  bool is_pipe() const { return !config_.socket_path.empty(); }

  uv_loop_t* loop_;
  HttpServerConfig config_;
  MessageHandler* handler_;
  // Either a uv_tcp_t or a uv_pipe_t; is_pipe() says which.  A union rather
  // than two members so every stream-level call site stays single-path.
  uv_any_handle server_handle_;
  std::atomic<bool> listening_{false};
  std::atomic<int> active_connections_{0};
  std::atomic<int> bound_port_{0};

  std::vector<Route> routes_;
  UpgradeHandler upgrade_handler_;

  // Active connections (for clean shutdown in Stop()).
  // Protected by connections_mutex_ — accessed from both the libuv event
  // loop thread (OnConnection, OnClose) and the caller thread (Stop()).
  std::mutex connections_mutex_;
  std::vector<HttpConnection*> connections_;

  // Live connections by id, for deferred responses.  Event-loop thread only.
  // An id is never reused, so a stale Responder cannot reach a newer
  // connection that happens to share an address.
  uint64_t next_conn_id_ = 1;
  std::unordered_map<uint64_t, HttpConnection*> live_conns_;

  // llhttp parser settings (shared across all connections).
  llhttp_settings_t parser_settings_;
};

// Quick scan for excessive JSON nesting depth (prevents stack overflow
// in nlohmann/json which has no built-in depth limit).  Approximate:
// does not account for braces inside string literals, but erring on the
// side of rejection is acceptable for untrusted input.
inline bool JsonNestingTooDeep(std::string_view s, int max_depth = 32) {
  int depth = 0;
  bool in_string = false;
  bool escape_next = false;
  for (char c : s) {
    if (escape_next) {
      escape_next = false;
      continue;
    }
    if (in_string) {
      if (c == '\\') {
        escape_next = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      if (++depth > max_depth) return true;
    } else if (c == '}' || c == ']') {
      if (--depth < 0) return true;  // Malformed: more closes than opens.
    }
  }
  return false;
}

// Is `address` an IPv4 literal (or empty, meaning the
// compiled default)?  The management API binds through uv_ip4_addr, so a
// hostname or an IPv6 literal cannot be bound at all; config parse refuses
// those with a named message instead of letting the bind fail later.
bool IsIpv4LiteralApiBind(std::string_view address);

// Is `address` a loopback bind for the management API?  True for 127.0.0.0/8
// and for the empty default; false for everything else, including the
// wildcard 0.0.0.0, hostnames, and IPv6 literals.  An address this cannot
// parse is reported as NON-loopback — an unparseable bind is never safe.
bool IsLoopbackApiBind(std::string_view address);

// URL-decode a percent-encoded string (+ is converted to space).
// Use for query string values (application/x-www-form-urlencoded).
std::string UrlDecode(std::string_view input);

// URL-decode a percent-encoded path (+ is kept literal).
// Use for URL path components where + is not a space substitute.
std::string UrlDecodePath(std::string_view input);

// Get the HTTP status text for a status code.
const char* HttpStatusText(int status_code);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_HTTP_SERVER_H_
