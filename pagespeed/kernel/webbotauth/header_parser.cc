// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/header_parser.h"

#include "pagespeed/kernel/webbotauth/signature_base.h"

namespace net_instaweb {
namespace webbotauth {

bool IsSupportedComponent(StringPiece c) {
  return c == "@method" || c == "@authority" || c == "@path";
}

namespace {
void SetReason(GoogleString* reason, const char* msg) {
  if (reason != nullptr) *reason = msg;
}
}  // namespace

bool ParseSignatureHeaders(StringPiece signature_input, StringPiece signature,
                           SignatureRequest* out, GoogleString* reason) {
  SfvInnerList list;
  GoogleString label;
  if (!ParseSignatureInput(signature_input, &label, &list)) {
    SetReason(reason, "malformed-signature-input");
    return false;
  }
  if (label.empty()) {
    SetReason(reason, "empty-label");
    return false;
  }
  if (list.components.empty()) {
    SetReason(reason, "empty-covered-components");
    return false;
  }

  out->label = label;
  out->covered.clear();
  for (const GoogleString& c : list.components) {
    if (!IsSupportedComponent(c)) {
      SetReason(reason, "unsupported-component");
      return false;
    }
    out->covered.push_back(c);
  }

  // Walk params, preserving order. Required: keyid, alg==ed25519.
  bool have_keyid = false;
  bool have_alg = false;
  for (const SfvParam& p : list.params) {
    if (p.name == "keyid") {
      if (p.type != SfvParam::kString) {
        SetReason(reason, "keyid-not-string");
        return false;
      }
      out->keyid = p.str_value;
      have_keyid = true;
    } else if (p.name == "alg") {
      if (p.type != SfvParam::kString) {
        SetReason(reason, "alg-not-string");
        return false;
      }
      out->alg = p.str_value;
      have_alg = true;
    } else if (p.name == "created") {
      if (p.type != SfvParam::kInteger) {
        SetReason(reason, "created-not-integer");
        return false;
      }
      out->created = p.int_value;
      out->has_created = true;
    } else if (p.name == "expires") {
      if (p.type != SfvParam::kInteger) {
        SetReason(reason, "expires-not-integer");
        return false;
      }
      out->expires = p.int_value;
      out->has_expires = true;
    }
    // Unknown params are ignored for validation but still rendered in the
    // @signature-params line (handled by the byte-exact serializer).
  }

  if (!have_keyid || out->keyid.empty()) {
    SetReason(reason, "missing-keyid");
    return false;
  }
  if (!have_alg || out->alg != "ed25519") {
    SetReason(reason, "alg-not-ed25519");
    return false;
  }

  // Reconstruct the @signature-params inner-list serialization byte-exactly
  // (param order preserved) from the parsed structure.
  out->signature_params_value = SerializeSignatureParams(list);

  // Decode the signature bytes for this label.
  if (!ParseSignatureBytes(signature, label, &out->signature_bytes)) {
    SetReason(reason, "malformed-signature");
    return false;
  }
  if (out->signature_bytes.size() != 64) {
    SetReason(reason, "signature-not-64-bytes");
    return false;
  }

  return true;
}

}  // namespace webbotauth
}  // namespace net_instaweb
