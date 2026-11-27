// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Ported from mod_pagespeed pagespeed/kernel/webbotauth/jwks.cc
// @ 8e15a19ce (2026-07-02). Keep logic in sync manually; the RFC 9421/8941
// core is standards-frozen.

#include "src/crypto/webbotauth/jwks.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/crypto/webbotauth/base64.h"

namespace pagespeed {
namespace webbotauth {

namespace {

// A minimal, tolerant JSON scanner sufficient for the JWK/JWKS shape. It does
// NOT validate the whole document; it extracts string members and walks object
// boundaries. It is deliberately conservative and total (never throws).
class JsonScanner {
 public:
  explicit JsonScanner(std::string_view s) : s_(s), pos_(0) {}

  bool Eof() const { return pos_ >= s_.size(); }
  char Peek() const { return Eof() ? '\0' : s_[pos_]; }

  void SkipWs() {
    while (!Eof()) {
      char c = s_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool Consume(char c) {
    SkipWs();
    if (Peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  // Parse a JSON string (assumes current non-ws char is '"'). Handles the
  // common escapes; rejects on malformed input.
  bool ParseString(std::string* out) {
    SkipWs();
    if (Peek() != '"') return false;
    ++pos_;
    out->clear();
    while (!Eof()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (Eof()) return false;
        char e = s_[pos_++];
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
          case 't':
            out->push_back('\t');
            break;
          case 'r':
            out->push_back('\r');
            break;
          case 'b':
            out->push_back('\b');
            break;
          case 'f':
            out->push_back('\f');
            break;
          case 'u': {
            // Skip the 4 hex digits; we only need ASCII JWK members so a basic
            // passthrough of the literal sequence is acceptable. Reject if
            // fewer than 4 remain.
            if (pos_ + 4 > s_.size()) return false;
            pos_ += 4;  // do not interpret; JWK members we read are ASCII
            break;
          }
          default:
            return false;
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        return false;  // unescaped control char
      } else {
        out->push_back(c);
      }
    }
    return false;  // unterminated
  }

  // Skip a JSON value (string, number, object, array, literal). Returns false
  // on malformed nesting.
  bool SkipValue() {
    SkipWs();
    char c = Peek();
    if (c == '"') {
      std::string ignore;
      return ParseString(&ignore);
    }
    if (c == '{') return SkipBalanced('{', '}');
    if (c == '[') return SkipBalanced('[', ']');
    // number / true / false / null
    while (!Eof()) {
      char d = s_[pos_];
      if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
          d == '\n' || d == '\r') {
        break;
      }
      ++pos_;
    }
    return true;
  }

  bool SkipBalanced(char open, char close) {
    SkipWs();
    if (Peek() != open) return false;
    int depth = 0;
    bool in_string = false;
    while (!Eof()) {
      char c = s_[pos_++];
      if (in_string) {
        if (c == '\\') {
          if (Eof()) return false;
          ++pos_;  // skip escaped char
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == open) {
        ++depth;
      } else if (c == close) {
        --depth;
        if (depth == 0) return true;
      }
    }
    return false;
  }

  size_t pos() const { return pos_; }
  void set_pos(size_t p) { pos_ = p; }
  std::string_view s() const { return s_; }

 private:
  std::string_view s_;
  size_t pos_;
};

// Parse a single JWK object (cursor positioned at the opening '{'). On success
// fills kty/crv/kid/x members it recognises and advances past the object.
struct Jwk {
  std::string kty, crv, kid, x;
};

bool ParseJwkObject(JsonScanner* sc, Jwk* jwk) {
  if (!sc->Consume('{')) return false;
  sc->SkipWs();
  if (sc->Consume('}')) return true;  // empty object
  while (true) {
    std::string key;
    if (!sc->ParseString(&key)) return false;
    if (!sc->Consume(':')) return false;
    sc->SkipWs();
    if (sc->Peek() == '"') {
      std::string val;
      if (!sc->ParseString(&val)) return false;
      if (key == "kty")
        jwk->kty = val;
      else if (key == "crv")
        jwk->crv = val;
      else if (key == "kid")
        jwk->kid = val;
      else if (key == "x")
        jwk->x = val;
    } else {
      if (!sc->SkipValue()) return false;
    }
    if (sc->Consume(',')) continue;
    if (sc->Consume('}')) return true;
    return false;
  }
}

bool JwkMatches(const Jwk& jwk, std::string_view kid, std::string* raw_key_32,
                bool allow_kidless) {
  if (jwk.kty != "OKP") return false;
  if (jwk.crv != "Ed25519") return false;
  // If the JWK specifies a kid it must equal the requested keyid. A kid-less
  // JWK is accepted only when allow_kidless is set, i.e. a single-JWK directory
  // (one key, host already allowlisted). In a JWKS *array* a kid-less entry
  // must NOT match an arbitrary requested keyid: that would let the holder of
  // a low-trust kid-less key impersonate any registered (e.g. verified-bot)
  // keyid -- the key-directory fail-open the lookup is required to prevent.
  if (jwk.kid.empty()) {
    if (!allow_kidless) return false;
  } else if (jwk.kid != kid) {
    return false;
  }
  if (jwk.x.empty()) return false;
  std::string raw;
  if (!Base64UrlDecode(jwk.x, &raw)) return false;
  if (raw.size() != 32) return false;
  *raw_key_32 = raw;
  return true;
}

// Validate a parsed JWK as a kid-BEARING OKP/Ed25519 key and, if valid, output
// its kid + raw 32-byte key. Used by the enumeration path (warm-fetch); a kid
// is REQUIRED here because the store is keyed by (host, kid).
bool JwkToKidKey(const Jwk& jwk, std::string* kid_out,
                 std::string* raw_key_32) {
  if (jwk.kty != "OKP") return false;
  if (jwk.crv != "Ed25519") return false;
  if (jwk.kid.empty()) return false;  // cannot store-key a kid-less entry
  if (jwk.x.empty()) return false;
  std::string raw;
  if (!Base64UrlDecode(jwk.x, &raw)) return false;
  if (raw.size() != 32) return false;
  *kid_out = jwk.kid;
  *raw_key_32 = raw;
  return true;
}

}  // namespace

bool ExtractEd25519Key(std::string_view jwks_document, std::string_view kid,
                       std::string* raw_key_32) {
  JsonScanner sc(jwks_document);
  sc.SkipWs();
  if (sc.Peek() != '{') return false;

  // Two accepted shapes:
  //   (1) JWKS: { "keys": [ {jwk}, ... ] }
  //   (2) single JWK: { "kty":"OKP", ... }
  // Try to find a top-level "keys" array; if present, iterate it. Otherwise,
  // treat the top-level object as a single JWK.
  size_t start = sc.pos();

  // Attempt shape (1): scan members looking for "keys".
  if (!sc.Consume('{')) return false;
  sc.SkipWs();
  bool found_keys = false;
  if (!sc.Consume('}')) {
    while (true) {
      std::string key;
      if (!sc.ParseString(&key)) break;
      if (!sc.Consume(':')) break;
      if (key == "keys") {
        found_keys = true;
        // Parse array of JWK objects.
        if (!sc.Consume('[')) return false;
        sc.SkipWs();
        if (sc.Consume(']')) return false;  // empty keys
        while (true) {
          Jwk jwk;
          if (!ParseJwkObject(&sc, &jwk)) return false;
          // Array shape: a kid is required; a kid-less entry must not
          // wildcard.
          if (JwkMatches(jwk, kid, raw_key_32, /*allow_kidless=*/false)) {
            return true;
          }
          if (sc.Consume(',')) continue;
          if (sc.Consume(']')) break;
          return false;
        }
        return false;  // walked all keys, no match
      } else {
        if (!sc.SkipValue()) break;
      }
      if (sc.Consume(',')) continue;
      break;
    }
  }

  if (found_keys) return false;

  // Shape (2): single JWK object.
  JsonScanner sc2(jwks_document);
  sc2.set_pos(start);
  Jwk jwk;
  if (!ParseJwkObject(&sc2, &jwk)) return false;
  // Single-JWK shape: a missing kid is the documented operator convenience.
  return JwkMatches(jwk, kid, raw_key_32, /*allow_kidless=*/true);
}

size_t ExtractAllEd25519Keys(
    std::string_view jwks_document,
    std::vector<std::pair<std::string, std::string>>* out) {
  const size_t before = out->size();
  JsonScanner sc(jwks_document);
  sc.SkipWs();
  if (sc.Peek() != '{') return 0;
  size_t start = sc.pos();

  // Shape (1): { "keys": [ {jwk}, ... ] } -- enumerate every kid-bearing key.
  if (!sc.Consume('{')) return 0;
  sc.SkipWs();
  bool found_keys = false;
  if (!sc.Consume('}')) {
    while (true) {
      std::string key;
      if (!sc.ParseString(&key)) break;
      if (!sc.Consume(':')) break;
      if (key == "keys") {
        found_keys = true;
        if (!sc.Consume('[')) return out->size() - before;
        sc.SkipWs();
        if (sc.Consume(']')) return out->size() - before;  // empty keys
        while (true) {
          Jwk jwk;
          if (!ParseJwkObject(&sc, &jwk)) return out->size() - before;
          std::string kid, raw;
          if (JwkToKidKey(jwk, &kid, &raw)) {
            out->push_back(std::make_pair(kid, raw));
          }
          if (sc.Consume(',')) continue;
          if (sc.Consume(']')) break;
          return out->size() - before;
        }
        return out->size() - before;
      } else {
        if (!sc.SkipValue()) break;
      }
      if (sc.Consume(',')) continue;
      break;
    }
  }

  if (found_keys) return out->size() - before;

  // Shape (2): single JWK object. Only emit if it carries a kid (a kid-less
  // single JWK cannot be store-keyed -- it stays a local-file-only
  // convenience).
  JsonScanner sc2(jwks_document);
  sc2.set_pos(start);
  Jwk jwk;
  if (!ParseJwkObject(&sc2, &jwk)) return 0;
  std::string kid, raw;
  if (JwkToKidKey(jwk, &kid, &raw)) {
    out->push_back(std::make_pair(kid, raw));
  }
  return out->size() - before;
}

}  // namespace webbotauth
}  // namespace pagespeed
