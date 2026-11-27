// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - WebSocket Protocol Implementation

#include "src/worker/websocket.h"

#include <cstddef>
#include <cstring>

namespace pagespeed {

// ---------------------------------------------------------------------------
// Minimal SHA-1 (RFC 3174) — only used for WebSocket accept key.
// ---------------------------------------------------------------------------

namespace {

struct Sha1Context {
  uint32_t state[5];
  uint64_t count;
  uint8_t buffer[64];
};

static void Sha1Init(Sha1Context* ctx) {
  ctx->state[0] = 0x67452301;
  ctx->state[1] = 0xEFCDAB89;
  ctx->state[2] = 0x98BADCFE;
  ctx->state[3] = 0x10325476;
  ctx->state[4] = 0xC3D2E1F0;
  ctx->count = 0;
}

static uint32_t RotLeft(uint32_t v, int n) {
  return (v << n) | (v >> (32 - n));
}

static void Sha1Transform(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(block[static_cast<ptrdiff_t>(i * 4)]) << 24) |
           (uint32_t(block[i * 4 + 1]) << 16) |
           (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
  }
  for (int i = 16; i < 80; ++i) {
    w[i] = RotLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }

  uint32_t a = state[0], b = state[1], c = state[2];
  uint32_t d = state[3], e = state[4];

  for (int i = 0; i < 80; ++i) {
    uint32_t f, k;
    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    uint32_t temp = RotLeft(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = RotLeft(b, 30);
    b = a;
    a = temp;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

static void Sha1Update(Sha1Context* ctx, const uint8_t* data, size_t len) {
  auto idx = static_cast<size_t>(ctx->count % 64);
  ctx->count += len;

  for (size_t i = 0; i < len; ++i) {
    ctx->buffer[idx++] = data[i];
    if (idx == 64) {
      Sha1Transform(ctx->state, ctx->buffer);
      idx = 0;
    }
  }
}

static void Sha1Final(Sha1Context* ctx, uint8_t digest[20]) {
  uint64_t bit_count = ctx->count * 8;
  uint8_t pad = 0x80;
  Sha1Update(ctx, &pad, 1);

  pad = 0;
  while (ctx->count % 64 != 56) {
    Sha1Update(ctx, &pad, 1);
  }

  uint8_t len_be[8];
  for (int i = 7; i >= 0; --i) {
    len_be[i] = static_cast<uint8_t>(bit_count & 0xFF);
    bit_count >>= 8;
  }
  Sha1Update(ctx, len_be, 8);

  for (int i = 0; i < 5; ++i) {
    digest[static_cast<ptrdiff_t>(i * 4)] =
        static_cast<uint8_t>(ctx->state[i] >> 24);
    digest[i * 4 + 1] = static_cast<uint8_t>(ctx->state[i] >> 16);
    digest[i * 4 + 2] = static_cast<uint8_t>(ctx->state[i] >> 8);
    digest[i * 4 + 3] = static_cast<uint8_t>(ctx->state[i]);
  }
}

// Standard base64 encoding (with padding, not URL-safe).
static std::string Base64Encode(const uint8_t* data, size_t len) {
  static constexpr char kAlpha[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((len + 2) / 3 * 4);

  for (size_t i = 0; i < len; i += 3) {
    uint32_t triple = uint32_t(data[i]) << 16;
    if (i + 1 < len) triple |= uint32_t(data[i + 1]) << 8;
    if (i + 2 < len) triple |= uint32_t(data[i + 2]);

    result += kAlpha[(triple >> 18) & 0x3F];
    result += kAlpha[(triple >> 12) & 0x3F];
    result += (i + 1 < len) ? kAlpha[(triple >> 6) & 0x3F] : '=';
    result += (i + 2 < len) ? kAlpha[triple & 0x3F] : '=';
  }
  return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// WsFrameParser
// ---------------------------------------------------------------------------

void WsFrameParser::Reset() {
  state_ = State::kHeader;
  frame_ = WsFrame{};
  header_read_ = 0;
  masked_ = false;
  memset(mask_key_, 0, sizeof(mask_key_));
  mask_read_ = 0;
  payload_len_ = 0;
  ext_len_read_ = 0;
  ext_len_needed_ = 0;
}

WsParseResult WsFrameParser::Feed(std::string_view data, size_t& consumed) {
  consumed = 0;

  while (consumed < data.size()) {
    auto byte = static_cast<uint8_t>(data[consumed]);

    switch (state_) {
      case State::kHeader: {
        header_[header_read_++] = byte;
        consumed++;
        if (header_read_ < 2) break;

        // RSV1-3 must be zero (no extensions negotiated, RFC 6455 §5.2).
        if ((header_[0] & 0x70) != 0) return WsParseResult::kError;

        frame_.fin = (header_[0] & 0x80) != 0;
        frame_.opcode = static_cast<WsOpcode>(header_[0] & 0x0F);
        masked_ = (header_[1] & 0x80) != 0;
        payload_len_ = header_[1] & 0x7F;

        // RFC 6455 §5.1: client-to-server frames MUST be masked.
        if (!masked_) return WsParseResult::kError;

        // RFC 6455 §5.5: control frames MUST NOT exceed 125 bytes
        // and MUST NOT be fragmented.
        bool is_control = (static_cast<uint8_t>(frame_.opcode) & 0x08) != 0;
        if (is_control && (payload_len_ > 125 || !frame_.fin)) {
          return WsParseResult::kError;
        }

        if (payload_len_ == 126) {
          state_ = State::kExtLen16;
          ext_len_needed_ = 2;
          ext_len_read_ = 0;
        } else if (payload_len_ == 127) {
          state_ = State::kExtLen64;
          ext_len_needed_ = 8;
          ext_len_read_ = 0;
        } else if (masked_) {
          state_ = State::kMaskKey;
          mask_read_ = 0;
        } else if (payload_len_ > 0) {
          if (payload_len_ > kMaxPayloadSize) return WsParseResult::kError;
          frame_.payload.reserve(static_cast<size_t>(payload_len_));
          state_ = State::kPayload;
        } else {
          state_ = State::kDone;
          return WsParseResult::kFrame;
        }
        break;
      }

      case State::kExtLen16:
      case State::kExtLen64: {
        ext_len_buf_[ext_len_read_++] = byte;
        consumed++;
        if (ext_len_read_ < ext_len_needed_) break;

        payload_len_ = 0;
        for (size_t i = 0; i < ext_len_needed_; ++i) {
          payload_len_ = (payload_len_ << 8) | ext_len_buf_[i];
        }
        if (payload_len_ > kMaxPayloadSize) return WsParseResult::kError;

        if (masked_) {
          state_ = State::kMaskKey;
          mask_read_ = 0;
        } else if (payload_len_ > 0) {
          frame_.payload.reserve(static_cast<size_t>(payload_len_));
          state_ = State::kPayload;
        } else {
          state_ = State::kDone;
          return WsParseResult::kFrame;
        }
        break;
      }

      case State::kMaskKey: {
        mask_key_[mask_read_++] = byte;
        consumed++;
        if (mask_read_ < 4) break;

        if (payload_len_ > 0) {
          frame_.payload.reserve(static_cast<size_t>(payload_len_));
          state_ = State::kPayload;
        } else {
          state_ = State::kDone;
          return WsParseResult::kFrame;
        }
        break;
      }

      case State::kPayload: {
        size_t remaining_payload =
            static_cast<size_t>(payload_len_) - frame_.payload.size();
        size_t available = data.size() - consumed;
        size_t to_read = std::min(remaining_payload, available);

        size_t offset = frame_.payload.size();
        frame_.payload.append(data.data() + consumed, to_read);
        consumed += to_read;

        // Unmask in place.
        if (masked_) {
          for (size_t i = 0; i < to_read; ++i) {
            frame_.payload[offset + i] ^= mask_key_[(offset + i) % 4];
          }
        }

        if (frame_.payload.size() == static_cast<size_t>(payload_len_)) {
          // Parse close code if this is a close frame.
          if (frame_.opcode == WsOpcode::kClose) {
            // RFC 6455 §5.5.1: close payload must be 0 or >= 2 bytes.
            if (frame_.payload.size() == 1) {
              return WsParseResult::kError;
            }
            if (frame_.payload.size() >= 2) {
              frame_.close_code = (uint16_t(uint8_t(frame_.payload[0])) << 8) |
                                  uint16_t(uint8_t(frame_.payload[1]));
              frame_.payload = frame_.payload.substr(2);
            }
          }
          state_ = State::kDone;
          return WsParseResult::kFrame;
        }
        break;
      }

      case State::kDone:
        return WsParseResult::kFrame;
    }
  }

  return WsParseResult::kNeedMore;
}

// ---------------------------------------------------------------------------
// Frame construction (server→client, always unmasked)
// ---------------------------------------------------------------------------

static std::string BuildFrame(WsOpcode opcode, std::string_view payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    frame.push_back(static_cast<char>(127));
    uint64_t len = payload.size();
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }

  frame.append(payload);
  return frame;
}

std::string WsBuildTextFrame(std::string_view payload) {
  return BuildFrame(WsOpcode::kText, payload);
}

std::string WsBuildCloseFrame(uint16_t code) {
  char payload[2];
  payload[0] = static_cast<char>((code >> 8) & 0xFF);
  payload[1] = static_cast<char>(code & 0xFF);
  return BuildFrame(WsOpcode::kClose, std::string_view(payload, 2));
}

std::string WsBuildPingFrame(std::string_view payload) {
  return BuildFrame(WsOpcode::kPing, payload);
}

std::string WsBuildPongFrame(std::string_view payload) {
  return BuildFrame(WsOpcode::kPong, payload);
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

static constexpr std::string_view kWsGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string WsComputeAcceptKey(std::string_view client_key) {
  std::string input;
  input.reserve(client_key.size() + kWsGuid.size());
  input.append(client_key);
  input.append(kWsGuid);

  Sha1Context ctx;
  Sha1Init(&ctx);
  Sha1Update(&ctx, reinterpret_cast<const uint8_t*>(input.data()),
             input.size());
  uint8_t digest[20];
  Sha1Final(&ctx, digest);

  return Base64Encode(digest, 20);
}

std::string WsBuildHandshakeResponse(std::string_view client_key) {
  std::string accept = WsComputeAcceptKey(client_key);
  std::string response;
  response += "HTTP/1.1 101 Switching Protocols\r\n";
  response += "Upgrade: websocket\r\n";
  response += "Connection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: ";
  response += accept;
  response += "\r\n\r\n";
  return response;
}

}  // namespace pagespeed
