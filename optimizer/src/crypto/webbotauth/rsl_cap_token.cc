// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed
// pagespeed/kernel/webbotauth/rsl_cap_token.cc @
// f6c24e8d34ba0c58acb22d626f6213d666d73e41 (2026-07-04; AgentPass A3).
// Keep logic in sync manually; the token wire format is frozen.

#include "src/crypto/webbotauth/rsl_cap_token.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "src/crypto/webbotauth/base64.h"

namespace pagespeed {
namespace webbotauth {

namespace {

// A deliberately small, STRICT extractor for the flat field shapes RSL-CAP
// uses. We do NOT use a general-purpose JSON parser on purpose: the token is a
// fixed, controlled artifact, and a minimal exact-match extractor keeps the
// attack surface tiny (no generic-JWT/JSON confusion, no recursive parsing of
// attacker-controlled documents). Anything the extractor cannot understand is
// rejected as malformed by the caller.
//
// Supported value shapes (whitespace-tolerant):
//   "key" : "string"
//   "key" : <integer>
//   "key" : [ "s1", "s2", ... ]   (array of strings)
//
// These helpers scan for the *first* occurrence of a top-level quoted key. The
// token bodies are small and flat (no nested objects), so a linear scan is
// sufficient and avoids pulling in a parser dependency.

// Skips ASCII whitespace starting at *pos within s.
void SkipWs(std::string_view s, size_t* pos) {
  while (*pos < s.size()) {
    char c = s[*pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++(*pos);
    } else {
      break;
    }
  }
}

// Parses a JSON string literal starting at s[*pos] (which MUST be the opening
// '"'). Writes the unescaped contents to *out and advances *pos past the
// closing quote. Returns false on malformed input. Supports the minimal escape
// set that can appear in our tokens (\" \\ \/ \n \r \t \b \f). Rejects \u and
// any other escape (we never emit them in RSL-CAP fields).
bool ParseJsonString(std::string_view s, size_t* pos, std::string* out) {
  if (*pos >= s.size() || s[*pos] != '"') {
    return false;
  }
  ++(*pos);  // consume opening quote
  out->clear();
  while (*pos < s.size()) {
    char c = s[*pos];
    if (c == '"') {
      ++(*pos);  // consume closing quote
      return true;
    }
    if (c == '\\') {
      ++(*pos);
      if (*pos >= s.size()) {
        return false;
      }
      char e = s[*pos];
      switch (e) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        default:
          return false;  // unsupported escape (incl. \u) => malformed
      }
      ++(*pos);
    } else {
      out->push_back(c);
      ++(*pos);
    }
  }
  return false;  // unterminated string
}

// Finds the value position for a top-level `"key"` in flat object `obj`,
// returning the index just after the ':' (whitespace not yet skipped). Returns
// false if the key is absent. `start` lets callers find later keys without
// matching a substring of an earlier value.
bool FindKeyValueStart(std::string_view obj, std::string_view key,
                       size_t* out_pos) {
  // Build the exact quoted key token: "key"
  std::string needle;
  needle.push_back('"');
  needle.append(key.data(), key.size());
  needle.push_back('"');
  // Scan for the needle, then require a ':' (optionally whitespace) after it.
  size_t from = 0;
  while (from < obj.size()) {
    size_t idx = obj.find(needle, from);
    if (idx == std::string_view::npos) {
      return false;
    }
    size_t after = idx + needle.size();
    SkipWs(obj, &after);
    if (after < obj.size() && obj[after] == ':') {
      *out_pos = after + 1;  // just past ':'
      return true;
    }
    from = idx + 1;  // keep scanning; this match wasn't a key
  }
  return false;
}

// Extracts a required/optional string field. Returns true and sets *out if the
// key is present and its value is a JSON string; false if the key is absent.
// Sets *malformed=true if the key is present but the value is not a valid
// string.
bool ExtractString(std::string_view obj, std::string_view key, std::string* out,
                   bool* malformed) {
  size_t pos;
  if (!FindKeyValueStart(obj, key, &pos)) {
    return false;
  }
  SkipWs(obj, &pos);
  if (!ParseJsonString(obj, &pos, out)) {
    *malformed = true;
    return false;
  }
  return true;
}

// Extracts an integer field. Returns true and sets *out if the key is present
// and parses as int64; false if absent. Sets *malformed=true if present but
// not a valid integer.
bool ExtractInt64(std::string_view obj, std::string_view key, int64_t* out,
                  bool* malformed) {
  size_t pos;
  if (!FindKeyValueStart(obj, key, &pos)) {
    return false;
  }
  SkipWs(obj, &pos);
  // Collect a run of [-0-9].
  size_t begin = pos;
  if (pos < obj.size() && (obj[pos] == '-' || obj[pos] == '+')) {
    ++pos;
  }
  size_t digits = 0;
  while (pos < obj.size() && obj[pos] >= '0' && obj[pos] <= '9') {
    ++pos;
    ++digits;
  }
  if (digits == 0) {
    *malformed = true;
    return false;
  }
  std::string_view num = obj.substr(begin, pos - begin);
  if (!absl::SimpleAtoi(num, out)) {
    *malformed = true;
    return false;
  }
  return true;
}

// Extracts an array-of-strings field into *out. Returns true if the key is
// present and is a (possibly empty) JSON array of strings; false if absent.
// Sets *malformed=true if present but not a valid string array.
bool ExtractStringArray(std::string_view obj, std::string_view key,
                        std::vector<std::string>* out, bool* malformed) {
  size_t pos;
  if (!FindKeyValueStart(obj, key, &pos)) {
    return false;
  }
  SkipWs(obj, &pos);
  if (pos >= obj.size() || obj[pos] != '[') {
    *malformed = true;
    return false;
  }
  ++pos;  // consume '['
  out->clear();
  SkipWs(obj, &pos);
  if (pos < obj.size() && obj[pos] == ']') {
    ++pos;
    return true;  // empty array
  }
  while (pos < obj.size()) {
    SkipWs(obj, &pos);
    std::string elem;
    if (!ParseJsonString(obj, &pos, &elem)) {
      *malformed = true;
      return false;
    }
    out->push_back(elem);
    SkipWs(obj, &pos);
    if (pos >= obj.size()) {
      *malformed = true;
      return false;
    }
    if (obj[pos] == ',') {
      ++pos;
      continue;
    }
    if (obj[pos] == ']') {
      ++pos;
      return true;
    }
    *malformed = true;
    return false;
  }
  *malformed = true;
  return false;  // unterminated array
}

}  // namespace

RslCapParseStatus ParseRslCapToken(std::string_view compact, RslCapToken* out) {
  *out = RslCapToken();

  // 1. Split on '.' into EXACTLY 3 parts. We do NOT omit empty strings, so an
  //    empty segment (e.g. "a..c") is preserved and will fail base64url decode
  //    or header parsing below, never collapsing to a 2-part token.
  std::vector<std::string_view> parts = absl::StrSplit(compact, '.');
  if (parts.size() != 3) {
    return RslCapParseStatus::kMalformed;
  }
  std::string_view header_b64 = parts[0];
  std::string_view payload_b64 = parts[1];
  std::string_view sig_b64 = parts[2];

  // 2. base64url-decode each segment.
  std::string header_json, payload_json, sig_bytes;
  if (!Base64UrlDecode(header_b64, &header_json) ||
      !Base64UrlDecode(payload_b64, &payload_json) ||
      !Base64UrlDecode(sig_b64, &sig_bytes)) {
    return RslCapParseStatus::kMalformed;
  }

  // 3. Strict header extraction. typ and alg are MANDATORY and must match
  //    exactly. This is the alg-confusion / generic-JWT defense: a token with
  //    alg "none", "HS256", or any non-"EdDSA" value is rejected here, before
  //    any key resolution or signature work.
  bool malformed = false;
  std::string typ, alg, kid;
  if (!ExtractString(header_json, "typ", &typ, &malformed) || malformed) {
    return RslCapParseStatus::kMalformed;
  }
  if (!ExtractString(header_json, "alg", &alg, &malformed) || malformed) {
    return RslCapParseStatus::kMalformed;
  }
  if (!ExtractString(header_json, "kid", &kid, &malformed) || malformed) {
    return RslCapParseStatus::kMalformed;
  }
  if (typ != "RSL-CAP" || alg != "EdDSA") {
    return RslCapParseStatus::kMalformed;
  }

  // 4. Strict payload extraction. exp is MANDATORY and must be an integer.
  std::string iss, sub;
  int64_t exp = 0, iat = 0;
  std::vector<std::string> lic, scope;
  // iss/sub are optional strings; if present they must be valid.
  ExtractString(payload_json, "iss", &iss, &malformed);
  if (malformed) return RslCapParseStatus::kMalformed;
  ExtractString(payload_json, "sub", &sub, &malformed);
  if (malformed) return RslCapParseStatus::kMalformed;
  if (!ExtractInt64(payload_json, "exp", &exp, &malformed) || malformed) {
    return RslCapParseStatus::kMalformed;  // exp mandatory
  }
  ExtractInt64(payload_json, "iat", &iat, &malformed);  // optional
  if (malformed) return RslCapParseStatus::kMalformed;
  ExtractStringArray(payload_json, "lic", &lic, &malformed);  // optional
  if (malformed) return RslCapParseStatus::kMalformed;
  ExtractStringArray(payload_json, "scope", &scope, &malformed);  // optional
  if (malformed) return RslCapParseStatus::kMalformed;

  // Populate the parsed token.
  out->typ = typ;
  out->alg = alg;
  out->kid = kid;
  out->iss = iss;
  out->sub = sub;
  out->exp = exp;
  out->iat = iat;
  out->lic = lic;
  out->scope = scope;

  // Preserve the ORIGINAL received segments for byte-exact signature
  // verification (never re-encode -- avoids canonicalization drift).
  std::string signing_input;
  signing_input.append(header_b64.data(), header_b64.size());
  signing_input.push_back('.');
  signing_input.append(payload_b64.data(), payload_b64.size());
  out->signing_input = signing_input;
  out->signature_bytes = sig_bytes;

  out->parsed = true;
  return RslCapParseStatus::kOk;
}

}  // namespace webbotauth
}  // namespace pagespeed
