// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CDP Client Implementation
//
// Chrome DevTools Protocol JSON-RPC client using null-byte delimited pipe
// transport. See cdp_client.h for design decisions and usage.

#include "src/browser/cdp_client.h"

#include <cstddef>
#include <utility>

#include "absl/strings/str_cat.h"

namespace pagespeed {

namespace {

// Write context for uv_write requests.
struct WriteContext {
  uv_write_t req;
  std::string data;  // Owns the serialized message.
  uv_buf_t buf;
  std::shared_ptr<bool> alive;
  CdpClient* client;
};

// Timer context for per-command timeout (M2).
struct TimerData {
  CdpClient* client;
  uint32_t command_id;
};

// Context for off-thread JSON parsing (M10).
struct ParseWorkContext {
  uv_work_t req;
  CdpClient* client;
  std::shared_ptr<bool> alive;
  std::string raw_message;
  nlohmann::json parsed;
  bool parse_error = false;
};

// Threshold for off-thread JSON parsing (M10).
constexpr size_t kLargeMessageThreshold = static_cast<size_t>(64 * 1024);

}  // namespace

CdpClient::CdpClient(uv_loop_t* loop)
    : loop_(loop), alive_(std::make_shared<bool>(true)) {}

CdpClient::~CdpClient() {
  *alive_ = false;
  // Stop reading and null out data pointer to prevent use-after-free
  // from any already-queued OnRead callbacks.
  if (read_pipe_) {
    if (connected_) {
      uv_read_stop(reinterpret_cast<uv_stream_t*>(read_pipe_));
    }
    read_pipe_->data = nullptr;
  }
  connected_ = false;
  // Clean up timers without invoking callbacks.
  for (auto& [id, cmd] : pending_commands_) {
    if (cmd.timer) {
      uv_timer_stop(cmd.timer);
      auto* td = static_cast<TimerData*>(cmd.timer->data);
      delete td;
      uv_close(reinterpret_cast<uv_handle_t*>(cmd.timer),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    }
    // Do NOT invoke cmd.callback — caller should use
    // CancelAll() before destruction for that.
  }
  pending_commands_.clear();
}

absl::Status CdpClient::AttachPipes(uv_pipe_t* read_pipe,
                                    uv_pipe_t* write_pipe) {
  if (connected_) {
    return absl::FailedPreconditionError("Already connected");
  }
  if (!read_pipe || !write_pipe) {
    return absl::InvalidArgumentError("Null pipe handle");
  }

  read_pipe_ = read_pipe;
  write_pipe_ = write_pipe;
  read_pipe_->data = this;

  int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(read_pipe_), OnAlloc,
                         OnRead);
  if (rc != 0) {
    return absl::InternalError(
        absl::StrCat("uv_read_start failed: ", uv_strerror(rc)));
  }

  connected_ = true;
  return absl::OkStatus();
}

absl::StatusOr<int> CdpClient::SendCommand(const CdpCommand& command,
                                           CdpResponseCallback callback) {
  if (!connected_) {
    return absl::FailedPreconditionError("Not connected");
  }

  uint32_t id = next_id_++;
  if (next_id_ == 0) next_id_ = 1;  // Skip 0 on wrap.

  // Build JSON-RPC message.
  nlohmann::json msg;
  msg["id"] = id;
  msg["method"] = command.method;
  if (!command.params.is_null()) {
    msg["params"] = command.params;
  }
  if (!command.session_id.empty()) {
    msg["sessionId"] = command.session_id;
  }

  auto* ctx = new WriteContext();
  ctx->data = msg.dump();
  ctx->data.push_back('\0');  // Null-byte delimiter.
  ctx->buf = uv_buf_init(ctx->data.data(),
                         static_cast<unsigned int>(ctx->data.size()));
  ctx->alive = alive_;
  ctx->client = this;
  ctx->req.data = ctx;

  int rc = uv_write(&ctx->req, reinterpret_cast<uv_stream_t*>(write_pipe_),
                    &ctx->buf, 1, OnWriteDone);
  if (rc != 0) {
    delete ctx;
    return absl::InternalError(
        absl::StrCat("uv_write failed: ", uv_strerror(rc)));
  }

  // Register pending command with timeout.
  PendingCommand pending;
  pending.callback = std::move(callback);
  pending.id = id;

  if (command.timeout_ms > 0) {
    auto* timer = new uv_timer_t();
    int trc = uv_timer_init(loop_, timer);
    if (trc != 0) {
      delete timer;
    } else {
      auto* td = new TimerData{this, id};
      timer->data = td;
      trc = uv_timer_start(timer, OnCommandTimeout, command.timeout_ms, 0);
      if (trc != 0) {
        delete td;
        uv_close(reinterpret_cast<uv_handle_t*>(timer), [](uv_handle_t* h) {
          delete reinterpret_cast<uv_timer_t*>(h);
        });
      } else {
        pending.timer = timer;
      }
    }
  }

  pending_commands_[id] = std::move(pending);
  return id;
}

void CdpClient::SetEventCallback(CdpEventCallback callback) {
  event_callback_ = std::move(callback);
}

void CdpClient::CancelAll(absl::Status reason) {
  if (read_pipe_ && connected_) {
    uv_read_stop(reinterpret_cast<uv_stream_t*>(read_pipe_));
  }
  connected_ = false;

  // Move pending commands out to avoid mutation during iteration.
  auto pending = std::move(pending_commands_);
  pending_commands_.clear();

  for (auto& [id, cmd] : pending) {
    if (cmd.timer) {
      uv_timer_stop(cmd.timer);
      auto* td = static_cast<TimerData*>(cmd.timer->data);
      delete td;
      uv_close(reinterpret_cast<uv_handle_t*>(cmd.timer),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    }
    if (cmd.callback) {
      cmd.callback(reason);
    }
  }
}

// static
void CdpClient::OnAlloc(uv_handle_t* handle, size_t suggested_size,
                        uv_buf_t* buf) {
  auto* client = static_cast<CdpClient*>(handle->data);
  if (!client) {
    buf->base = nullptr;
    buf->len = 0;
    return;
  }
  if (client->alloc_buffer_.size() < suggested_size) {
    client->alloc_buffer_.resize(suggested_size);
  }
  buf->base = client->alloc_buffer_.data();
  buf->len = static_cast<unsigned int>(client->alloc_buffer_.size());
}

// static
void CdpClient::OnRead(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* buf) {
  auto* client = static_cast<CdpClient*>(stream->data);

  // Guard against use-after-free: if CdpClient was destroyed,
  // stream->data is nullptr.
  if (!client) {
    return;
  }

  if (nread < 0) {
    // Pipe EOF or error — cancel all pending (M3).
    if (nread == UV_EOF) {
      client->CancelAll(absl::UnavailableError("Chrome pipe closed (EOF)"));
    } else {
      client->CancelAll(absl::InternalError(
          absl::StrCat("Pipe read error: ", uv_strerror(nread))));
    }
    return;
  }

  if (nread == 0) {
    return;
  }

  // Append data and process complete messages.
  client->read_buffer_.append(buf->base, nread);

  // Messages are null-byte delimited. Use running offset to avoid
  // O(n²) from repeated erase(0, ...).
  size_t start = 0;
  size_t pos;
  while ((pos = client->read_buffer_.find('\0', start)) != std::string::npos) {
    size_t len = pos - start;
    if (len > 0 && len <= kMaxMessageSize) {
      client->ProcessMessage(client->read_buffer_.substr(start, len));
    }
    start = pos + 1;
  }
  // Remove all processed data in one operation.
  if (start > 0) {
    client->read_buffer_.erase(0, start);
  }

  // Guard against buffer overflow from incomplete messages.
  if (client->read_buffer_.size() > kMaxMessageSize) {
    client->read_buffer_.clear();
    client->CancelAll(
        absl::ResourceExhaustedError("CDP message exceeded size limit"));
  }
}

// static
void CdpClient::OnWriteDone(uv_write_t* req, int status) {
  auto* ctx = static_cast<WriteContext*>(req->data);
  if (*ctx->alive && status != 0 && ctx->client->connected_) {
    ctx->client->CancelAll(absl::InternalError(
        absl::StrCat("CDP write failed: ", uv_strerror(status))));
  }
  delete ctx;
}

// static
void CdpClient::OnCommandTimeout(uv_timer_t* timer) {
  auto* td = static_cast<TimerData*>(timer->data);
  auto* client = td->client;
  uint32_t command_id = td->command_id;

  auto it = client->pending_commands_.find(command_id);
  if (it == client->pending_commands_.end()) {
    // Already resolved.
    delete td;
    uv_close(reinterpret_cast<uv_handle_t*>(timer),
             [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    return;
  }

  auto callback = std::move(it->second.callback);
  // Timer is already firing; don't try to stop/close via CancelAll.
  it->second.timer = nullptr;
  client->pending_commands_.erase(it);

  delete td;
  uv_close(reinterpret_cast<uv_handle_t*>(timer),
           [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });

  if (callback) {
    callback(absl::DeadlineExceededError(
        absl::StrCat("CDP command ", command_id, " timed out")));
  }
}

void CdpClient::ProcessMessage(const std::string& message) {
  if (message.size() >= kLargeMessageThreshold) {
    // Parse large messages off the event loop (M10).
    auto* ctx = new ParseWorkContext();
    ctx->client = this;
    ctx->alive = alive_;
    // Copy message into ctx; defer moving until uv_queue_work succeeds.
    // If uv_queue_work fails, we fall back to synchronous parsing using
    // the original 'message' which must remain valid.
    ctx->raw_message = message;
    ctx->req.data = ctx;

    int rc = uv_queue_work(
        loop_, &ctx->req,
        [](uv_work_t* req) {
          auto* wctx = static_cast<ParseWorkContext*>(req->data);
          try {
            wctx->parsed = nlohmann::json::parse(wctx->raw_message);
          } catch (const nlohmann::json::exception&) {
            wctx->parse_error = true;
          }
        },
        [](uv_work_t* req, int /*status*/) {
          auto* wctx = static_cast<ParseWorkContext*>(req->data);
          if (!*wctx->alive) {
            delete wctx;
            return;
          }
          if (!wctx->parse_error) {
            try {
              wctx->client->HandleParsedMessage(std::move(wctx->parsed));
            } catch (const nlohmann::json::  // NOLINT(bugprone-empty-catch)
                     exception&) {
              // Malformed JSON field types.
            }
          }
          delete wctx;
        });

    if (rc != 0) {
      delete ctx;
      // Fall back to synchronous parsing using the original message.
      try {
        auto msg = nlohmann::json::parse(message);
        HandleParsedMessage(std::move(msg));
      } catch (  // NOLINT(bugprone-empty-catch)
          const nlohmann::json::exception&) {
      }
    }
    return;
  }

  // Parse small messages synchronously.
  try {
    auto msg = nlohmann::json::parse(message);
    HandleParsedMessage(std::move(msg));
  } catch (const nlohmann::json::exception&) {  // NOLINT(bugprone-empty-catch)
  }
}

void CdpClient::HandleParsedMessage(nlohmann::json msg) {
  // Response: has "id" field.
  if (msg.contains("id") && msg["id"].is_number()) {
    uint32_t id = msg["id"].get<uint32_t>();

    auto it = pending_commands_.find(id);
    if (it == pending_commands_.end()) {
      return;  // Already timed out or cancelled.
    }

    auto callback = std::move(it->second.callback);

    // Stop and close timeout timer.
    if (it->second.timer) {
      uv_timer_stop(it->second.timer);
      auto* td = static_cast<TimerData*>(it->second.timer->data);
      delete td;
      uv_close(reinterpret_cast<uv_handle_t*>(it->second.timer),
               [](uv_handle_t* h) { delete reinterpret_cast<uv_timer_t*>(h); });
    }

    pending_commands_.erase(it);

    CdpResponse response;
    response.id = id;

    if (msg.contains("error") && msg["error"].is_object()) {
      auto& err = msg["error"];
      if (err.contains("code") && err["code"].is_number()) {
        response.error_code = err["code"].get<int>();
      }
      if (err.contains("message") && err["message"].is_string()) {
        response.error_message = err["message"].get<std::string>();
      }
    } else if (msg.contains("result")) {
      response.result = std::move(msg["result"]);
    }

    if (callback) {
      callback(std::move(response));
    }
    return;
  }

  // Event: has "method" but no "id".
  if (msg.contains("method") && msg["method"].is_string()) {
    // Hold a local strong copy before dispatch: the callback may (via a Send
    // failure -> Finish) reassign event_callback_ to nullptr and drop the last
    // strong ref to the session WHILE this call is on the stack — which would
    // destroy the std::function we are executing (UB) and free the session
    // mid-call. The local copy keeps both alive until the call returns; a
    // re-assignment during dispatch now only touches the member, not `cb`.
    auto cb = event_callback_;
    if (cb) {
      CdpEvent event;
      event.method = msg["method"].get<std::string>();
      if (msg.contains("params")) {
        event.params = std::move(msg["params"]);
      }
      if (msg.contains("sessionId") && msg["sessionId"].is_string()) {
        event.session_id = msg["sessionId"].get<std::string>();
      }
      cb(event);
    }
  }
}

}  // namespace pagespeed
