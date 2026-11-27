// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed
// pagespeed/kernel/webbotauth/header_parser.cc @ 626ddfa95 (2026-07-03). Keep
// logic in sync manually; the RFC 9421/8941 core is standards-frozen. Extends
// the original core (upstreamed to 1.15): tag
// selection, multi-signature dictionaries, and HTTP field covered components
// (see header_parser.h).

#include "src/crypto/webbotauth/header_parser.h"

#include <string>
#include <string_view>
#include <vector>

#include "src/crypto/webbotauth/signature_base.h"

namespace pagespeed {
namespace webbotauth {

namespace {

// RFC 9110 token characters, restricted to lowercase (RFC 9421 requires
// component identifiers to be lowercase field names).
bool IsLowercaseFieldNameChar(char c) {
  if (c >= 'a' && c <= 'z') return true;
  if (c >= '0' && c <= '9') return true;
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

void SetReason(std::string* reason, const char* msg) {
  if (reason != nullptr) *reason = msg;
}

// Does this member's EFFECTIVE tag equal "web-bot-auth"? RFC 8941 dictionary
// parameters are last-wins, so a repeated tag param resolves to the LAST
// occurrence before matching (`;tag="web-bot-auth";tag="x"` has effective tag
// "x" -- NOT web-bot-auth material). The tag must be an sf-string (RFC 9421);
// a token/integer/boolean effective tag is not a match.
bool HasWebBotAuthTag(const SfvInnerList& inner) {
  const SfvParam* last_tag = nullptr;
  for (const SfvParam& p : inner.params) {
    if (p.name == "tag") last_tag = &p;
  }
  return last_tag != nullptr && last_tag->type == SfvParam::kString &&
         last_tag->str_value == kWebBotAuthTag;
}

}  // namespace

bool IsSupportedComponent(std::string_view c) {
  if (c == "@method" || c == "@authority" || c == "@path") return true;
  // RFC 9421 section 2.1 HTTP field component: a lowercase field name.
  // Bounded length; '@' names are derived components and never valid here.
  if (c.empty() || c.size() > kMaxFieldComponentNameLen) return false;
  for (char ch : c) {
    if (!IsLowercaseFieldNameChar(ch)) return false;
  }
  return true;
}

ParseStatus ParseSignatureHeaders(std::string_view signature_input,
                                  std::string_view signature,
                                  SignatureRequest* out, std::string* reason) {
  std::vector<SfvDictMember> members;
  if (!ParseSignatureInputDict(signature_input, &members) || members.empty()) {
    SetReason(reason, "malformed-signature-input");
    return ParseStatus::kMalformed;
  }

  // Select the FIRST member tagged "web-bot-auth" (deterministic: at most one
  // ed25519 verification per request, regardless of how many signatures the
  // request carries). A tagged member that then fails the profile is genuine
  // web-bot-auth material that does not validate -> kMalformed (fail closed),
  // never "try the next one".
  const SfvDictMember* selected = nullptr;
  for (const SfvDictMember& m : members) {
    if (HasWebBotAuthTag(m.inner)) {
      selected = &m;
      break;
    }
  }
  if (selected == nullptr) {
    SetReason(reason, "no-web-bot-auth-tag");
    return ParseStatus::kNoWebBotAuthSignature;
  }
  const SfvInnerList& list = selected->inner;

  if (selected->label.empty()) {
    SetReason(reason, "empty-label");
    return ParseStatus::kMalformed;
  }
  if (list.components.empty()) {
    SetReason(reason, "empty-covered-components");
    return ParseStatus::kMalformed;
  }
  if (list.components.size() > kMaxCoveredComponents) {
    SetReason(reason, "too-many-covered-components");
    return ParseStatus::kMalformed;
  }
  if (list.any_component_params) {
    SetReason(reason, "unsupported-component-params");
    return ParseStatus::kMalformed;
  }

  out->label = selected->label;
  out->covered.clear();
  for (const std::string& c : list.components) {
    if (!IsSupportedComponent(c)) {
      SetReason(reason, "unsupported-component");
      return ParseStatus::kMalformed;
    }
    // RFC 9421: the signature base must not contain the same component
    // identifier twice. Quadratic but bounded by kMaxCoveredComponents.
    for (const std::string& prev : out->covered) {
      if (prev == c) {
        SetReason(reason, "duplicate-covered-component");
        return ParseStatus::kMalformed;
      }
    }
    out->covered.push_back(c);
  }

  // Web Bot Auth architecture draft section 4.2: agents MUST cover
  // "@authority". Without it (and with no nonce cache in A1), a captured
  // signature would carry no request identity at all and replay against any
  // host for the whole freshness window. Fail closed.
  bool have_authority = false;
  for (const std::string& c : out->covered) {
    if (c == "@authority") {
      have_authority = true;
      break;
    }
  }
  if (!have_authority) {
    SetReason(reason, "authority-not-covered");
    return ParseStatus::kMalformed;
  }

  // Walk params, preserving order. Required: keyid, alg==ed25519.
  bool have_keyid = false;
  bool have_alg = false;
  for (const SfvParam& p : list.params) {
    if (p.name == "keyid") {
      if (p.type != SfvParam::kString) {
        SetReason(reason, "keyid-not-string");
        return ParseStatus::kMalformed;
      }
      out->keyid = p.str_value;
      have_keyid = true;
    } else if (p.name == "alg") {
      if (p.type != SfvParam::kString) {
        SetReason(reason, "alg-not-string");
        return ParseStatus::kMalformed;
      }
      out->alg = p.str_value;
      have_alg = true;
    } else if (p.name == "created") {
      if (p.type != SfvParam::kInteger) {
        SetReason(reason, "created-not-integer");
        return ParseStatus::kMalformed;
      }
      out->created = p.int_value;
      out->has_created = true;
    } else if (p.name == "expires") {
      if (p.type != SfvParam::kInteger) {
        SetReason(reason, "expires-not-integer");
        return ParseStatus::kMalformed;
      }
      out->expires = p.int_value;
      out->has_expires = true;
    } else if (p.name == "tag" && p.type == SfvParam::kString) {
      out->tag = p.str_value;  // selection already guaranteed "web-bot-auth"
    }
    // Unknown params (e.g. nonce) are ignored for validation but still
    // rendered in the @signature-params line (byte-exact serializer).
  }

  if (!have_keyid || out->keyid.empty()) {
    SetReason(reason, "missing-keyid");
    return ParseStatus::kMalformed;
  }
  if (!have_alg || out->alg != "ed25519") {
    SetReason(reason, "alg-not-ed25519");
    return ParseStatus::kMalformed;
  }

  // Reconstruct the @signature-params inner-list serialization byte-exactly
  // (param order preserved) from the parsed structure.
  out->signature_params_value = SerializeSignatureParams(list);

  // Decode the signature bytes for the selected label.
  if (!ParseSignatureBytes(signature, selected->label, &out->signature_bytes)) {
    SetReason(reason, "malformed-signature");
    return ParseStatus::kMalformed;
  }
  if (out->signature_bytes.size() != 64) {
    SetReason(reason, "signature-not-64-bytes");
    return ParseStatus::kMalformed;
  }

  return ParseStatus::kOk;
}

}  // namespace webbotauth
}  // namespace pagespeed
