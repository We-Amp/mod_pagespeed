// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed pagespeed/kernel/webbotauth/sfv.cc @
// 626ddfa95 (2026-07-03). Keep logic in sync manually; the RFC 9421/8941 core
// is standards-frozen. Extends the original core (upstreamed to 1.15 in
// the 1.15 line): bounded multi-member Signature-Input dictionary
// parsing + component-parameter tolerance (see sfv.h).

#include "src/crypto/webbotauth/sfv.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/base64.h"

namespace pagespeed {
namespace webbotauth {

namespace {

// PARSER LAXNESS (intentional, fail-closed): this parser accepts a handful
// of inputs strict RFC 8941 would reject -- adjacent inner-list items
// without a separating SP, HTAB where the grammar wants SP, bytes >= 0x80
// inside sf-strings, leading-zero integers, unchecked base64 padding
// counts.  All of them are harmless by construction: the verifier
// re-serializes the parsed structure STRICTLY and verifies over that
// serialization, so any input whose strict re-serialization differs from
// what the signer signed yields a signature-base mismatch and the request
// fail-closes to kUnknown at crypto time.  Laxness here can never widen
// trust.

// A tiny forward cursor over a std::string_view. All accessors are
// bounds-checked.
class Scanner {
 public:
  explicit Scanner(std::string_view s) : s_(s), pos_(0) {}

  bool Eof() const { return pos_ >= s_.size(); }
  char Peek() const { return Eof() ? '\0' : s_[pos_]; }
  char Next() { return Eof() ? '\0' : s_[pos_++]; }
  size_t pos() const { return pos_; }

  void SkipSpaces() {
    while (!Eof() && (s_[pos_] == ' ' || s_[pos_] == '\t')) ++pos_;
  }

  bool Consume(char c) {
    if (Peek() == c) {
      ++pos_;
      return true;
    }
    return false;
  }

 private:
  std::string_view s_;
  size_t pos_;
};

bool IsTokenStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '*';
}
bool IsTokenChar(char c) {
  // RFC 8941 token chars (subset sufficient here): alnum and a few specials.
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ||
         c == '.' || c == ':' || c == '/' || c == '*';
}
bool IsKeyStart(char c) { return (c >= 'a' && c <= 'z') || c == '*'; }
bool IsKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '-' || c == '.' || c == '*';
}

// Parse an sf-string: a double-quoted string with backslash escaping of '"'
// and '\'. On success advances past the closing quote and fills `out`.
bool ParseString(Scanner* sc, std::string* out) {
  if (!sc->Consume('"')) return false;
  out->clear();
  while (!sc->Eof()) {
    char c = sc->Next();
    if (c == '\\') {
      char e = sc->Next();
      if (e != '"' && e != '\\') return false;
      out->push_back(e);
    } else if (c == '"') {
      return true;
    } else if (static_cast<unsigned char>(c) < 0x20 ||
               static_cast<unsigned char>(c) == 0x7f) {
      return false;  // control chars not allowed in sf-string
    } else {
      out->push_back(c);
    }
  }
  return false;  // unterminated
}

// Parse an sf-token (unquoted).
bool ParseToken(Scanner* sc, std::string* out) {
  if (!IsTokenStart(sc->Peek())) return false;
  out->clear();
  out->push_back(sc->Next());
  while (IsTokenChar(sc->Peek())) out->push_back(sc->Next());
  return true;
}

// Parse an sf-integer (optionally negative).
bool ParseInteger(Scanner* sc, int64_t* out) {
  std::string digits;
  bool neg = false;
  if (sc->Peek() == '-') {
    neg = true;
    sc->Next();
  }
  if (!std::isdigit(static_cast<unsigned char>(sc->Peek()))) return false;
  while (std::isdigit(static_cast<unsigned char>(sc->Peek()))) {
    digits.push_back(sc->Next());
    if (digits.size() > 15) return false;  // RFC 8941 integer max 15 digits
  }
  int64_t v = std::strtoll(digits.c_str(), nullptr, 10);
  *out = neg ? -v : v;
  return true;
}

// Parse a parameter key.
bool ParseKey(Scanner* sc, std::string* out) {
  if (!IsKeyStart(sc->Peek())) return false;
  out->clear();
  out->push_back(sc->Next());
  while (IsKeyChar(sc->Peek())) out->push_back(sc->Next());
  return true;
}

// Parse the parameter list following a member/item: zero or more
// `;key[=value]`. Order is preserved.
bool ParseParams(Scanner* sc, std::vector<SfvParam>* params) {
  while (sc->Peek() == ';') {
    sc->Next();  // consume ';'
    sc->SkipSpaces();
    SfvParam p;
    if (!ParseKey(sc, &p.name)) return false;
    if (sc->Consume('=')) {
      char c = sc->Peek();
      if (c == '"') {
        if (!ParseString(sc, &p.str_value)) return false;
        p.type = SfvParam::kString;
      } else if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        // Integer (we do not need decimals for created/expires).
        if (!ParseInteger(sc, &p.int_value)) return false;
        p.type = SfvParam::kInteger;
      } else if (IsTokenStart(c)) {
        if (!ParseToken(sc, &p.str_value)) return false;
        p.type = SfvParam::kToken;
      } else {
        return false;
      }
    } else {
      // Bare parameter => boolean true.
      p.type = SfvParam::kBoolean;
      p.bool_value = true;
    }
    params->push_back(p);
  }
  return true;
}

// Parse an inner list: ( item item ... ) followed by optional params.
bool ParseInnerList(Scanner* sc, SfvInnerList* out) {
  if (!sc->Consume('(')) return false;
  sc->SkipSpaces();
  while (sc->Peek() != ')') {
    if (sc->Eof()) return false;
    // Each component identifier is an sf-string (RFC 9421 component ids are
    // serialized as quoted strings).
    std::string comp;
    if (sc->Peek() == '"') {
      if (!ParseString(sc, &comp)) return false;
    } else {
      return false;  // we require quoted component identifiers
    }
    // A component may carry its own params (e.g. ;name=...). We do not
    // support component parameters, but the parse stays TOTAL: an unselected
    // dictionary member with component params must not poison the whole
    // header. The parameter content is dropped; the flag lets the header
    // parser reject the member if it is the one selected for verification.
    std::vector<SfvParam> item_params;
    if (!ParseParams(sc, &item_params)) return false;
    if (!item_params.empty()) {
      out->any_component_params = true;
    }
    out->components.push_back(comp);
    sc->SkipSpaces();
  }
  if (!sc->Consume(')')) return false;
  // Inner-list-level params (created/keyid/alg/expires) follow.
  return ParseParams(sc, &out->params);
}

}  // namespace

bool ParseSignatureInputDict(std::string_view value,
                             std::vector<SfvDictMember>* out) {
  out->clear();
  Scanner sc(value);
  while (true) {
    sc.SkipSpaces();
    SfvDictMember member;
    // dictionary member: key "=" inner-list
    if (!ParseKey(&sc, &member.label)) return false;
    if (!sc.Consume('=')) return false;
    if (!ParseInnerList(&sc, &member.inner)) return false;
    // Duplicate labels are ambiguous signature material (RFC 8941 last-wins
    // does not apply cleanly to signatures): fail closed. The scan is
    // quadratic but bounded by kMaxSignatureInputMembers.
    for (const SfvDictMember& prev : *out) {
      if (prev.label == member.label) return false;
    }
    if (out->size() >= kMaxSignatureInputMembers) return false;
    out->push_back(std::move(member));
    sc.SkipSpaces();
    if (sc.Eof()) return true;
    // Another dictionary member must follow a comma; anything else is
    // trailing garbage.
    if (!sc.Consume(',')) return false;
  }
}

bool ParseSignatureInput(std::string_view value, std::string* label,
                         SfvInnerList* out) {
  std::vector<SfvDictMember> members;
  if (!ParseSignatureInputDict(value, &members)) return false;
  if (members.size() != 1) return false;  // single-label contract
  // Preserve the original single-label contract: components carrying their
  // own parameters are unsupported here.
  if (members[0].inner.any_component_params) return false;
  *label = std::move(members[0].label);
  *out = std::move(members[0].inner);
  return true;
}

bool ParseSignatureBytes(std::string_view value, std::string_view label,
                         std::string* raw_bytes_out) {
  Scanner sc(value);
  // Dictionary of one-or-more members; find the one matching `label`.
  while (!sc.Eof()) {
    sc.SkipSpaces();
    std::string key;
    if (!ParseKey(&sc, &key)) return false;
    if (!sc.Consume('=')) return false;
    // Byte sequence value: :base64:
    std::string b64;
    bool is_byteseq = false;
    if (sc.Consume(':')) {
      is_byteseq = true;
      while (sc.Peek() != ':' && !sc.Eof()) b64.push_back(sc.Next());
      if (!sc.Consume(':')) return false;
    } else {
      return false;  // we only accept byte-sequence Signature values
    }
    // Skip any params on this member.
    std::vector<SfvParam> ignored;
    if (!ParseParams(&sc, &ignored)) return false;

    bool match = (key == label);
    if (match && is_byteseq) {
      return Base64Decode(b64, raw_bytes_out);
    }
    sc.SkipSpaces();
    if (sc.Consume(',')) {
      continue;  // next dictionary member
    }
    if (sc.Eof()) break;
    return false;
  }
  return false;  // label not found
}

}  // namespace webbotauth
}  // namespace pagespeed
