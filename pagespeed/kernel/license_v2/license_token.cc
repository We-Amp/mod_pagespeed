// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_token.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

namespace {

// Escape a string for safe embedding inside a JSON double-quoted value.
// Handles the characters required by RFC 8259 section 7.
GoogleString JsonEscape(StringPiece s) {
  GoogleString out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Control character: emit \u00XX.
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x",
                   static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out.append(buf);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

constexpr int kSignatureSize = 64;

// Decode table: maps ASCII byte to 6-bit value, -1 for invalid.
constexpr std::array<int8_t, 256> MakeDecodeTable() {
  std::array<int8_t, 256> table{};
  for (auto& v : table) v = -1;
  for (int i = 0; i < 64; ++i) {
    table[static_cast<unsigned char>(kBase64UrlAlphabet[i])] =
        static_cast<int8_t>(i);
  }
  return table;
}

constexpr auto kDecodeTable = MakeDecodeTable();

// Minimal JSON string extraction: find "key":"value" in JSON object.
// Only handles simple flat JSON with string values and integer iat.
bool ExtractJsonString(StringPiece json, StringPiece key,
                       GoogleString* value) {
  // Look for "key":"
  GoogleString needle = absl::StrCat("\"", key, "\":\"");
  auto pos = json.find(needle);
  if (pos == StringPiece::npos) return false;
  pos += needle.size();
  // Find the closing quote, skipping escaped quotes (backslash-quote).
  GoogleString unescaped;
  while (pos < json.size()) {
    char c = json[pos];
    if (c == '\\' && pos + 1 < json.size()) {
      char next = json[pos + 1];
      switch (next) {
        case '"':
          unescaped.push_back('"');
          break;
        case '\\':
          unescaped.push_back('\\');
          break;
        case 'n':
          unescaped.push_back('\n');
          break;
        case 'r':
          unescaped.push_back('\r');
          break;
        case 't':
          unescaped.push_back('\t');
          break;
        case 'b':
          unescaped.push_back('\b');
          break;
        case 'f':
          unescaped.push_back('\f');
          break;
        default:
          unescaped.push_back(next);
          break;
      }
      pos += 2;
      continue;
    }
    if (c == '"') {
      *value = std::move(unescaped);
      return true;
    }
    unescaped.push_back(c);
    ++pos;
  }
  return false;
}

bool ExtractJsonInt64(StringPiece json, StringPiece key, int64_t* value) {
  // Look for "key":
  GoogleString needle = absl::StrCat("\"", key, "\":");
  auto pos = json.find(needle);
  if (pos == StringPiece::npos) return false;
  pos += needle.size();
  // Skip whitespace
  while (pos < json.size() && json[pos] == ' ') ++pos;
  // Find end of number
  auto end = pos;
  if (end < json.size() && json[end] == '-') ++end;
  while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end == pos) return false;
  return absl::SimpleAtoi(json.substr(pos, end - pos), value);
}

}  // namespace

GoogleString Base64UrlEncode(StringPiece input) {
  GoogleString result;
  result.reserve((input.size() * 4 + 2) / 3);

  size_t i = 0;
  while (i + 2 < input.size()) {
    uint32_t triple = (static_cast<uint8_t>(input[i]) << 16) |
                      (static_cast<uint8_t>(input[i + 1]) << 8) |
                      static_cast<uint8_t>(input[i + 2]);
    result += kBase64UrlAlphabet[(triple >> 18) & 0x3F];
    result += kBase64UrlAlphabet[(triple >> 12) & 0x3F];
    result += kBase64UrlAlphabet[(triple >> 6) & 0x3F];
    result += kBase64UrlAlphabet[triple & 0x3F];
    i += 3;
  }

  if (i + 1 == input.size()) {
    uint32_t val = static_cast<uint8_t>(input[i]);
    result += kBase64UrlAlphabet[(val >> 2) & 0x3F];
    result += kBase64UrlAlphabet[(val << 4) & 0x3F];
  } else if (i + 2 == input.size()) {
    uint32_t val = (static_cast<uint8_t>(input[i]) << 8) |
                   static_cast<uint8_t>(input[i + 1]);
    result += kBase64UrlAlphabet[(val >> 10) & 0x3F];
    result += kBase64UrlAlphabet[(val >> 4) & 0x3F];
    result += kBase64UrlAlphabet[(val << 2) & 0x3F];
  }

  return result;
}

bool Base64UrlDecode(StringPiece input, GoogleString* output) {
  // Skip padding if present
  size_t len = input.size();
  while (len > 0 && input[len - 1] == '=') --len;
  input = input.substr(0, len);

  output->clear();
  output->reserve((len * 3) / 4);

  uint32_t accum = 0;
  int bits = 0;

  for (char c : input) {
    int8_t val = kDecodeTable[static_cast<unsigned char>(c)];
    if (val < 0) return false;
    accum = (accum << 6) | static_cast<uint32_t>(val);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output->push_back(static_cast<char>((accum >> bits) & 0xFF));
    }
  }

  return true;
}

GoogleString SerializePayload(const LicensePayload& payload) {
  GoogleString json =
      absl::StrCat("{\"sub\":\"", JsonEscape(payload.sub), "\",\"iss\":\"",
                   JsonEscape(payload.iss), "\",\"iat\":", payload.iat,
                   ",\"plan\":\"", JsonEscape(payload.plan), "\"");
  if (payload.exp != 0) {
    absl::StrAppend(&json, ",\"exp\":", payload.exp);
  }
  if (!payload.sid.empty()) {
    absl::StrAppend(&json, ",\"sid\":\"", JsonEscape(payload.sid), "\"");
  }
  if (!payload.kid.empty()) {
    absl::StrAppend(&json, ",\"kid\":\"", JsonEscape(payload.kid), "\"");
  }
  absl::StrAppend(&json, "}");
  return json;
}

bool ParsePayload(StringPiece json, LicensePayload* payload) {
  if (!ExtractJsonString(json, "sub", &payload->sub)) return false;
  if (!ExtractJsonString(json, "iss", &payload->iss)) return false;
  if (!ExtractJsonInt64(json, "iat", &payload->iat)) return false;
  if (!ExtractJsonString(json, "plan", &payload->plan)) return false;
  // Optional v2 fields -- missing is OK (backward-compatible with v1).
  ExtractJsonInt64(json, "exp", &payload->exp);
  ExtractJsonString(json, "sid", &payload->sid);
  ExtractJsonString(json, "kid", &payload->kid);
  return true;
}

GoogleString BuildToken(StringPiece signature, StringPiece json_payload) {
  GoogleString raw;
  raw.reserve(signature.size() + json_payload.size());
  raw.append(signature.data(), signature.size());
  raw.append(json_payload.data(), json_payload.size());
  return Base64UrlEncode(raw);
}

bool SplitToken(StringPiece token, GoogleString* signature,
                GoogleString* json_payload) {
  GoogleString decoded;
  if (!Base64UrlDecode(token, &decoded)) return false;
  if (decoded.size() <= kSignatureSize) return false;

  *signature = decoded.substr(0, kSignatureSize);
  *json_payload = decoded.substr(kSignatureSize);
  return true;
}

}  // namespace net_instaweb
