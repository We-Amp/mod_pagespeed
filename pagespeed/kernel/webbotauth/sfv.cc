// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/sfv.h"

#include <cctype>
#include <cstdlib>

#include "pagespeed/kernel/webbotauth/base64.h"

namespace net_instaweb {
namespace webbotauth {

namespace {

// A tiny forward cursor over a StringPiece. All accessors are bounds-checked.
class Scanner {
 public:
  explicit Scanner(StringPiece s) : s_(s), pos_(0) {}

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
  StringPiece s_;
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
bool ParseString(Scanner* sc, GoogleString* out) {
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
bool ParseToken(Scanner* sc, GoogleString* out) {
  if (!IsTokenStart(sc->Peek())) return false;
  out->clear();
  out->push_back(sc->Next());
  while (IsTokenChar(sc->Peek())) out->push_back(sc->Next());
  return true;
}

// Parse an sf-integer (optionally negative).
bool ParseInteger(Scanner* sc, int64_t* out) {
  GoogleString digits;
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
bool ParseKey(Scanner* sc, GoogleString* out) {
  if (!IsKeyStart(sc->Peek())) return false;
  out->clear();
  out->push_back(sc->Next());
  while (IsKeyChar(sc->Peek())) out->push_back(sc->Next());
  return true;
}

// Parse the parameter list following a member/item: zero or more `;key[=value]`.
// Order is preserved.
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
    GoogleString comp;
    if (sc->Peek() == '"') {
      if (!ParseString(sc, &comp)) return false;
    } else {
      return false;  // we require quoted component identifiers
    }
    // A component may carry its own params (e.g. ;name=...); for the supported
    // derived components (@method/@authority/@path) there are none, and any
    // present params make it an unsupported component which the header_parser
    // will reject. We skip over per-item params here to keep parsing total.
    std::vector<SfvParam> item_params;
    if (!ParseParams(sc, &item_params)) return false;
    if (!item_params.empty()) {
      // Mark the component as carrying params by appending a sentinel the
      // header parser treats as unsupported. Simplest: reject now.
      return false;
    }
    out->components.push_back(comp);
    sc->SkipSpaces();
  }
  if (!sc->Consume(')')) return false;
  // Inner-list-level params (created/keyid/alg/expires) follow.
  return ParseParams(sc, &out->params);
}

}  // namespace

bool ParseSignatureInput(StringPiece value, GoogleString* label,
                         SfvInnerList* out) {
  Scanner sc(value);
  sc.SkipSpaces();
  // dictionary member: key "=" inner-list
  GoogleString key;
  if (!ParseKey(&sc, &key)) return false;
  if (!sc.Consume('=')) return false;
  if (!ParseInnerList(&sc, out)) return false;
  sc.SkipSpaces();
  // Only the single-label case is supported: reject a trailing dictionary
  // separator (another member) -> kUnknown.
  if (!sc.Eof()) {
    if (sc.Peek() == ',') return false;  // multiple labels unsupported
    return false;                        // trailing garbage
  }
  *label = key;
  return true;
}

bool ParseSignatureBytes(StringPiece value, StringPiece label,
                         GoogleString* raw_bytes_out) {
  Scanner sc(value);
  // Dictionary of one-or-more members; find the one matching `label`.
  while (!sc.Eof()) {
    sc.SkipSpaces();
    GoogleString key;
    if (!ParseKey(&sc, &key)) return false;
    if (!sc.Consume('=')) return false;
    // Byte sequence value: :base64:
    GoogleString b64;
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

    bool match = (key.size() == label.size() && StringPiece(key) == label);
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
}  // namespace net_instaweb
