// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - WebSocket Endpoint Handlers
//
// Manages WebSocket connections for real-time streaming:
//   /v1/ws/stats  — Stats snapshot + periodic deltas
//   /v1/ws/events — Batched event stream
//   /v1/ws/logs   — Real-time log streaming with ring buffer
//
// Runs entirely on the libuv event loop.  Thread-safe event
// posting via PostEvent() and PostLog().

#ifndef PAGESPEED_SRC_WORKER_WS_HANDLERS_H_
#define PAGESPEED_SRC_WORKER_WS_HANDLERS_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/worker/websocket.h"
#include "uv.h"

namespace pagespeed {

class MessageHandler;

// Longest log message the ring keeps, in bytes of UTF-8.  A longer message is
// cut on a character boundary and ends in "…[truncated N bytes]".
inline constexpr size_t kMaxLogMessageBytes = 4096;

// Largest serialized GET /v1/logs page, envelope included.  A page stops
// before it would exceed this, whatever its entry limit says, so a page of
// long lines is shorter, never larger.
inline constexpr size_t kMaxLogsPageBytes = 512 * 1024;

// Makes a log message safe to store and to serve as JSON: every byte that
// does not begin or continue a well-formed UTF-8 sequence becomes U+FFFD,
// then a message longer than kMaxLogMessageBytes keeps its longest prefix of
// at most kMaxLogMessageBytes bytes that ends on a character boundary,
// followed by "…[truncated N bytes]" (N = the number of bytes removed).
// Pure; callable from any thread.
std::string SanitizeLogMessage(std::string message);

// Configuration for WebSocket endpoints.
struct WsConfig {
  int max_connections = 8;       // Max simultaneous WS connections.
  int ping_interval_ms = 30000;  // Server ping interval.
  int pong_timeout_ms = 10000;   // Close if pong not received.
  int auth_timeout_ms = 2000;    // Close if auth not received.
  int stats_interval_ms = 1000;  // Stats push interval (default).
  int stats_min_interval_ms = 100;
  int stats_max_interval_ms = 60000;
  int event_batch_ms = 100;  // Event batch interval.
  // Per-client event buffer high-water.  Also the most bytes of log entries
  // sent to a log-stream client in one write (one entry is always sent).
  size_t event_hwm = 65536;
  std::string auth_token;  // Empty = no credential configured.
  // An empty auth_token is only "no auth required" when the
  // operator deliberately opted out (--api-no-auth).  Otherwise a tokenless
  // server fails closed, here as in HttpServer::CheckAuth.
  bool allow_unauthenticated = false;
  // Skip WS auth under --api-read-open, but ONLY for a stream name in
  // read_open_streams -- an explicit allow-list, matching
  // RouteAuth::kReadOpenOk on the HTTP side.  A stream not in this set
  // (including one AcceptUpgrade has never heard of) keeps the token even
  // under read_open, the same as the log stream does today.
  bool read_open = false;
  std::unordered_set<std::string> read_open_streams = {"stats", "events"};
  // Bounds on the connections simultaneously waiting for in-band
  // authentication (the WS handshake has no header a browser could carry a
  // bearer token on, so the token -- when one is required -- is sent as the
  // connection's first text frame instead).  An upgrade that would exceed
  // either bound is refused the same way exceeding max_connections is.
  // Authenticated connections, and pre-authenticated read-open-stream
  // connections, never count against this budget.
  //
  // The per-peer bound (keyed by remote address) is what lets a client open
  // several streams at once -- all three, or two browser tabs of the bundled
  // console, which opens stats and logs per tab -- while a single
  // never-authenticating peer can no longer pin the whole budget for
  // everyone else.  The global bound caps the pre-auth state no matter how
  // many distinct peers arrive; max_connections counts these connections
  // too, so the global bound only takes effect when max_connections is
  // above it (at the defaults, 8 is reached first).  A transport without a
  // peer address counts against the global bound only.  (The worker's unix
  // socket carries no token, so its connections never wait for in-band
  // authentication and never reach the budget at all.)
  int max_preauth_connections = 16;
  int max_preauth_connections_per_peer = 4;
};

// Metrics for WebSocket subsystem.
struct WsMetrics {
  std::atomic<int> stats_connections{0};
  std::atomic<int> events_connections{0};
  std::atomic<int> logs_connections{0};
  std::atomic<uint64_t> messages_sent{0};
  // Messages not delivered to a client: events over a client's buffer limit,
  // frames refused at the pending-write limit, and log entries that left the
  // ring before a slow log-stream client could be sent them (the client is
  // told how many in an "overflow" message).
  std::atomic<uint64_t> messages_dropped{0};
  // Log entries serialized for the live stream.  One per entry sent, however
  // many clients received it.
  std::atomic<uint64_t> log_entries_serialized{0};
};

// Rate-limits the WS auth-timeout warning to at most one emission per
// window (60s by default), reporting how many timeouts were suppressed
// since the last emission.  Pure and stateful-but-clockless: the caller
// supplies "now" (steady-clock milliseconds), so this is testable with
// synthetic timestamps and needs no real sleep.  Not thread-safe; callers
// on WsManager serialize on the event loop thread, as with every other
// WsManager field.
class WsAuthTimeoutWarningLimiter {
 public:
  explicit WsAuthTimeoutWarningLimiter(int64_t window_ms = 60000)
      : window_ms_(window_ms) {}

  // Call once per auth timeout with the current time.  Returns the number
  // of PRIOR timeouts suppressed since the last emission (0 on the very
  // first call, or the first call after a window has fully elapsed) when
  // THIS timeout should be logged now; returns std::nullopt when this
  // timeout itself is suppressed (less than window_ms since the last
  // emission) -- the caller logs nothing, but the connection still closes.
  std::optional<int> RecordTimeout(int64_t now_ms) {
    if (!last_emit_ms_.has_value() || now_ms - *last_emit_ms_ >= window_ms_) {
      const int suppressed = suppressed_since_last_;
      suppressed_since_last_ = 0;
      last_emit_ms_ = now_ms;
      return suppressed;
    }
    ++suppressed_since_last_;
    return std::nullopt;
  }

 private:
  int64_t window_ms_;
  std::optional<int64_t> last_emit_ms_;
  int suppressed_since_last_ = 0;
};

// Bounds the connections simultaneously waiting for in-band authentication,
// in the two dimensions WsConfig describes: a global cap and a per-peer cap
// keyed by the caller-supplied peer key (an empty key counts against the
// global cap only).  A refused TryAcquire records nothing; a successful one
// must be paired with exactly one Release of the same key.  Not thread-safe;
// WsManager calls it on the event loop thread only, like every other
// WsManager field.  Pure and clockless, so tests can drive it with synthetic
// keys.
class WsPreauthBudget {
 public:
  // Which bound a TryAcquire tripped, so the refusal can name it.
  enum class Verdict : std::uint8_t {
    kAdmitted,
    kRefusedGlobal,
    kRefusedPerPeer
  };

  WsPreauthBudget(int global_max, int per_peer_max)
      : global_max_(global_max), per_peer_max_(per_peer_max) {}

  Verdict TryAcquire(std::string_view peer_key) {
    if (pending_ >= global_max_) return Verdict::kRefusedGlobal;
    if (peer_key.empty()) {
      ++pending_;
      return Verdict::kAdmitted;
    }
    auto [it, inserted] = by_peer_.try_emplace(std::string(peer_key), 0);
    if (it->second >= per_peer_max_) {
      if (inserted) by_peer_.erase(it);
      return Verdict::kRefusedPerPeer;
    }
    ++it->second;
    ++pending_;
    return Verdict::kAdmitted;
  }

  void Release(const std::string& peer_key) {
    --pending_;
    if (peer_key.empty()) return;
    auto it = by_peer_.find(peer_key);
    if (it != by_peer_.end() && --it->second == 0) by_peer_.erase(it);
  }

  int pending() const { return pending_; }

  // Slots currently held under `peer_key` (0 for a key holding none).
  int pending_for(const std::string& peer_key) const {
    auto it = by_peer_.find(peer_key);
    return it == by_peer_.end() ? 0 : it->second;
  }

 private:
  int global_max_;
  int per_peer_max_;
  int pending_ = 0;
  std::unordered_map<std::string, int> by_peer_;
};

// Forward declaration — connection state is internal.
struct WsConnection;

// WebSocket endpoint manager.
//
// Usage:
//   WsManager mgr(loop, config, handler);
//   mgr.SetStatsProvider([&]() { return BuildStatsJson(); });
//   mgr.Start();
//   // From any thread:
//   mgr.PostEvent("variant_written", detail_json);
//   // On shutdown:
//   mgr.Stop();
class WsManager {
 public:
  WsManager(uv_loop_t* loop, WsConfig config, MessageHandler* handler);
  ~WsManager();

  WsManager(const WsManager&) = delete;
  WsManager& operator=(const WsManager&) = delete;

  // Set the callback that produces the full stats JSON snapshot.
  // Called on the event loop thread during stats push.
  void SetStatsProvider(std::function<nlohmann::json()> provider);

  // Start timers (ping, stats, event batch).
  void Start();

  // Stop all timers and close all connections.
  void Stop();

  // Attempt to upgrade an HTTP connection to WebSocket.
  // Called from the HTTP server's OnMessageComplete when the path
  // matches /v1/ws/stats, /v1/ws/events, or /v1/ws/logs.
  // Takes ownership of the handle, which the HTTP server allocated as a
  // uv_any_handle (a TCP socket or, under --api-socket, a unix pipe) and
  // hands over as a uv_stream_t*.  It must be freed as a uv_any_handle*.
  // `endpoint` is "stats", "events", or "logs".
  // `client_key` is the Sec-WebSocket-Key header value.
  // `interval_ms` is the client-requested stats interval (0 = default).
  void AcceptUpgrade(uv_stream_t* handle, std::string_view endpoint,
                     std::string_view client_key, int interval_ms = 0);

  // Post an event from any thread.  Thread-safe.
  // `type` is the event name (e.g., "variant_written").
  // `detail` is the event payload JSON object.
  void PostEvent(std::string_view type, nlohmann::json detail);

  // Post a log entry from any thread.  Thread-safe.
  // `level` is "debug", "info", "warning", or "error".
  // `source` is the originating subsystem (e.g., "worker").
  // `module` is a finer-grained tag (e.g., "image", "html").
  // `message` is the formatted log line.
  void PostLog(std::string_view level, std::string_view source,
               std::string_view module, std::string message);

  // Build the GET /v1/logs response document from the log ring.
  // LOOP THREAD ONLY: log_ring_ is confined to the event loop (mutated only
  // by DrainPendingLogs), and route handlers run on that loop, so this reads
  // the ring with no lock -- log_mutex_ guards pending_logs_, not the ring.
  // Never blocks.  With has_since, the oldest entries newer than `since`;
  // otherwise the newest entries.  At most `limit` entries and at most
  // kMaxLogsPageBytes serialized, but at least one entry when one matches.
  nlohmann::json BuildLogsResponse(uint64_t since, bool has_since,
                                   size_t limit) const;

  // This process's log stream identity: 16 lowercase hex digits (64 random
  // bits) drawn when the manager is constructed.  Sequence numbers start
  // again at 1 in a new process; a reader that sees this change knows it.
  const std::string& stream_id() const { return stream_id_; }

  // Access metrics.
  const WsMetrics& metrics() const { return metrics_; }

  // The pre-authentication budget, for tests that check a slot is returned
  // on every path.  Loop thread only, like the budget itself (or with the
  // loop stopped).
  const WsPreauthBudget& preauth_budget() const { return preauth_budget_; }

  // Total active WebSocket connections.
  int active_connections() const;

 private:
  // Timer callbacks.
  static void OnPingTimer(uv_timer_t* timer);
  static void OnStatsTimer(uv_timer_t* timer);
  static void OnEventBatchTimer(uv_timer_t* timer);

  // libuv callbacks for WS connections.
  static void OnAlloc(uv_handle_t* handle, size_t suggested_size,
                      uv_buf_t* buf);
  static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
  static void OnWriteDone(uv_write_t* req, int status);
  static void OnClose(uv_handle_t* handle);
  static void OnAuthTimeout(uv_timer_t* timer);
  static void OnPongTimeout(uv_timer_t* timer);

  // Async callbacks for cross-thread posting.
  static void OnAsyncEvent(uv_async_t* async);
  static void OnAsyncLog(uv_async_t* async);

  // Log batch timer callback.
  static void OnLogBatchTimer(uv_timer_t* timer);

  // Internal helpers.
  void HandleFrame(WsConnection* conn, const WsFrame& frame);
  void HandleAuthMessage(WsConnection* conn, std::string_view payload);
  void SendText(WsConnection* conn, std::string_view text);
  void SendFrame(WsConnection* conn, const std::string& frame_data);
  void CloseWs(WsConnection* conn, uint16_t code);
  void RemoveConnection(WsConnection* conn);
  void PushStats();
  void FlushEventBatch();
  void DrainPendingEvents();
  void DrainPendingLogs();
  void FlushLogBatch();
  void PumpLogStream(WsConnection* conn);
  const std::string& LogFrame(size_t index);
  void TrimLogFrames();
  void SendLogSnapshot(WsConnection* conn);

  uv_loop_t* loop_;
  WsConfig config_;
  MessageHandler* handler_;
  const std::string stream_id_;  // see stream_id()
  WsMetrics metrics_;
  std::atomic<bool> running_ = false;

  // Stats provider callback.
  std::function<nlohmann::json()> stats_provider_;

  // Previous stats snapshot for delta computation.
  nlohmann::json prev_stats_;
  uint64_t stats_seq_ = 0;
  uint64_t events_seq_ = 0;

  // Timers.
  uv_timer_t ping_timer_;
  uv_timer_t stats_timer_;
  uv_timer_t event_batch_timer_;

  // Cross-thread event posting.
  uv_async_t async_event_;
  std::mutex event_mutex_;
  std::vector<nlohmann::json> pending_events_;

  // Cross-thread log posting.
  uv_async_t async_log_;
  uv_timer_t log_batch_timer_;
  std::mutex log_mutex_;
  std::vector<nlohmann::json> pending_logs_;
  std::deque<nlohmann::json> log_ring_;
  // Parallel to log_ring_ (same length, same order): the serialized stream
  // frame of each entry, empty until a stream client first needs it and
  // emptied again once every client is past it.  See LogFrame, TrimLogFrames.
  std::deque<std::string> log_frames_;
  // Every frame below this position is known to be empty.
  uint64_t log_frames_trimmed_to_ = 0;
  uint64_t log_total_ = 0;  // Total logs ever posted (for overflow count).
  // Posts dropped at a full pending queue.  They never receive a seq (no
  // phantom gap); the count lets a reader say that entries were not kept.
  std::atomic<uint64_t> log_shed_total_{0};
  static constexpr size_t kMaxLogRingSize = 2000;
  static constexpr size_t kMaxPendingQueueSize = 10000;

  // Active connections (vector mutated on loop thread only).
  std::vector<WsConnection*> connections_;
  // Atomic mirror of connections_.size() for thread-safe reads.
  std::atomic<int> connection_count_{0};

  // Connections currently waiting for in-band authentication (loop thread
  // only, like connections_).  Bounded globally and per peer by the config's
  // two max_preauth_connections values; see AcceptUpgrade for the single
  // acquire site and HandleAuthMessage / CloseWs for the exactly-once
  // release.
  WsPreauthBudget preauth_budget_{config_.max_preauth_connections,
                                  config_.max_preauth_connections_per_peer};

  // Rate limiter for the "WS: auth timeout" warning (see OnAuthTimeout).
  WsAuthTimeoutWarningLimiter auth_timeout_warning_limiter_;

  // Rate limiter for the "WS: max connections awaiting authentication"
  // refusal warning (see AcceptUpgrade). A separate instance from the one
  // above: a refusal costs a client nothing (no 2s wait), so its line rate
  // is bounded only by how fast upgrades arrive, independently of timeouts.
  WsAuthTimeoutWarningLimiter preauth_reject_warning_limiter_;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_WS_HANDLERS_H_
