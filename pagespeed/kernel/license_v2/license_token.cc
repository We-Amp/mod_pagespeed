// GENERATED — DO NOT EDIT BY HAND.
// Vendored from ModPageSpeed 2.0 src/crypto/license_token.cc by tools/sync-crypto.sh.
// Canonical source: github.com/We-Amp/pagespeed-optimizer src/crypto/.
// Synced from commit ebc48ef98186a07300b2f42576e8a14602525368.
// To update: bump PINNED_MPS2_COMMIT in tools/sync-crypto.sh and re-run it.
// Drift guard: the crypto-drift CI check. See the design record.

// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/license_v2/license_token.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/numbers.h"
// NOLINTNEXTLINE(misc-include-cleaner) -- provides JsonEscape
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/string_util.h"

#ifndef PAGESPEED_LICENSE_NAMESPACE
#define PAGESPEED_LICENSE_NAMESPACE pagespeed
#endif

namespace PAGESPEED_LICENSE_NAMESPACE {

namespace {

using net_instaweb::JsonEscape;

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
bool ExtractJsonString(std::string_view json, std::string_view key,
                       std::string* value) {
  // Look for "key":"
  std::string needle = absl::StrCat("\"", key, "\":\"");
  auto pos = json.find(needle);
  if (pos == std::string_view::npos) return false;
  pos += needle.size();
  // Find the closing quote, skipping escaped quotes (backslash-quote).
  std::string unescaped;
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

bool ExtractJsonInt64(std::string_view json, std::string_view key,
                      int64_t* value) {
  // Look for "key":
  std::string needle = absl::StrCat("\"", key, "\":");
  auto pos = json.find(needle);
  if (pos == std::string_view::npos) return false;
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

bool ExtractJsonInt32(std::string_view json, std::string_view key,
                      int32_t* value) {
  int64_t v;
  if (!ExtractJsonInt64(json, key, &v)) return false;
  if (v < std::numeric_limits<int32_t>::min() ||
      v > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  *value = static_cast<int32_t>(v);
  return true;
}

// Extract a JSON string array: "key":["val1","val2"]
// Handles escaped strings (same rules as ExtractJsonString).
bool ExtractJsonStringArray(std::string_view json, std::string_view key,
                            std::vector<std::string>* values) {
  std::string needle = absl::StrCat("\"", key, "\":[");
  auto pos = json.find(needle);
  if (pos == std::string_view::npos) return false;
  pos += needle.size();

  values->clear();
  while (pos < json.size()) {
    // Skip whitespace and commas
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ',')) ++pos;
    if (pos >= json.size()) {
      values->clear();
      return false;  // Truncated — no closing ]
    }
    if (json[pos] == ']') return true;
    if (json[pos] != '"') return false;
    ++pos;
    // Parse string value (same as ExtractJsonString inner loop)
    std::string value;
    while (pos < json.size()) {
      char c = json[pos];
      if (c == '\\' && pos + 1 < json.size()) {
        char next = json[pos + 1];
        switch (next) {
          case '"':
            value.push_back('"');
            break;
          case '\\':
            value.push_back('\\');
            break;
          case 'n':
            value.push_back('\n');
            break;
          case 'r':
            value.push_back('\r');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'b':
            value.push_back('\b');
            break;
          case 'f':
            value.push_back('\f');
            break;
          default:
            value.push_back(next);
            break;
        }
        pos += 2;
        continue;
      }
      if (c == '"') {
        values->push_back(std::move(value));
        ++pos;
        break;
      }
      value.push_back(c);
      ++pos;
    }
  }
  values->clear();
  return false;  // Reached end of input without closing ]
}

}  // namespace

std::string Base64UrlEncode(std::string_view input) {
  std::string result;
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

bool Base64UrlDecode(std::string_view input, std::string* output) {
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

  // RFC 4648 §3.5: reject invalid remainder (1 char = 6 bits can't encode
  // a whole byte) and require trailing padding bits to be zero.
  if (bits == 6 || (bits > 0 && (accum & ((1 << bits) - 1)) != 0)) {
    output->clear();
    return false;
  }

  return true;
}

std::string SerializePayload(const LicensePayload& payload) {
  std::string json =
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
  if (!payload.products.empty()) {
    absl::StrAppend(&json, ",\"products\":[");
    for (size_t i = 0; i < payload.products.size(); ++i) {
      if (i > 0) absl::StrAppend(&json, ",");
      absl::StrAppend(&json, "\"", JsonEscape(payload.products[i]), "\"");
    }
    absl::StrAppend(&json, "]");
  }
  if (payload.max_instances > 0) {
    absl::StrAppend(&json, ",\"max_instances\":", payload.max_instances);
  }
  // v4: entitlements array, after max_instances.  Emitted only
  // when non-empty, so a token without it is byte-identical to a v3 token (the
  // signature covers these bytes — TS crypto.ts MUST emit in this exact order).
  if (!payload.entitlements.empty()) {
    absl::StrAppend(&json, ",\"entitlements\":[");
    for (size_t i = 0; i < payload.entitlements.size(); ++i) {
      if (i > 0) absl::StrAppend(&json, ",");
      absl::StrAppend(&json, "\"", JsonEscape(payload.entitlements[i]), "\"");
    }
    absl::StrAppend(&json, "]");
  }
  absl::StrAppend(&json, "}");
  return json;
}

bool ParsePayload(std::string_view json, LicensePayload* payload) {
  if (!ExtractJsonString(json, "sub", &payload->sub)) return false;
  if (!ExtractJsonString(json, "iss", &payload->iss)) return false;
  if (!ExtractJsonInt64(json, "iat", &payload->iat)) return false;
  if (!ExtractJsonString(json, "plan", &payload->plan)) return false;
  // Optional v2 fields — missing is OK (backward-compatible with v1).
  ExtractJsonInt64(json, "exp", &payload->exp);
  ExtractJsonString(json, "sid", &payload->sid);
  ExtractJsonString(json, "kid", &payload->kid);
  // Optional v3 fields.
  ExtractJsonStringArray(json, "products", &payload->products);
  ExtractJsonInt32(json, "max_instances", &payload->max_instances);
  if (payload->max_instances < 0) payload->max_instances = 0;
  // Optional v4 field — absence parses clean (empty list).
  ExtractJsonStringArray(json, "entitlements", &payload->entitlements);
  return true;
}

std::string BuildToken(std::string_view signature,
                       std::string_view json_payload) {
  std::string raw;
  raw.reserve(signature.size() + json_payload.size());
  raw.append(signature.data(), signature.size());
  raw.append(json_payload.data(), json_payload.size());
  return Base64UrlEncode(raw);
}

bool SplitToken(std::string_view token, std::string* signature,
                std::string* json_payload) {
  std::string decoded;
  if (!Base64UrlDecode(token, &decoded)) return false;
  if (decoded.size() <= kSignatureSize) return false;

  *signature = decoded.substr(0, kSignatureSize);
  *json_payload = decoded.substr(kSignatureSize);
  return true;
}

}  // namespace PAGESPEED_LICENSE_NAMESPACE
