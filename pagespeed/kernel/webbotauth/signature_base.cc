// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").
//
// Kept in sync manually with pagespeed-optimizer src/crypto/webbotauth/
// signature_base.cc (this change upstreams the optimizer's RFC 9421 section 2.1
// HTTP field components, see signature_base.h).

#include "pagespeed/kernel/webbotauth/signature_base.h"

#include <utility>

#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {
namespace webbotauth {

namespace {

// Serialize an sf-string per RFC 8941 section 4.1.3: surround with quotes and
// backslash-escape '"' and '\'.
GoogleString SerializeString(StringPiece s) {
  GoogleString out;
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

GoogleString SerializeParam(const SfvParam& p) {
  switch (p.type) {
    case SfvParam::kInteger:
      return StrCat(";", p.name, "=", Integer64ToString(p.int_value));
    case SfvParam::kString:
      return StrCat(";", p.name, "=", SerializeString(p.str_value));
    case SfvParam::kToken:
      return StrCat(";", p.name, "=", p.str_value);
    case SfvParam::kBoolean:
      // Boolean true is serialized as a bare key (no "=?1").
      return StrCat(";", p.name);
  }
  return GoogleString();
}

// RFC 9421 section 2.1: strip leading/trailing optional whitespace (SP/HTAB)
// from each field line value.
StringPiece TrimOws(StringPiece v) {
  while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) {
    v.remove_prefix(1);
  }
  while (!v.empty() && (v[v.size() - 1] == ' ' || v[v.size() - 1] == '\t')) {
    v.remove_suffix(1);
  }
  return v;
}

}  // namespace

GoogleString SerializeSignatureParams(const SfvInnerList& list) {
  GoogleString out = "(";
  for (size_t i = 0; i < list.components.size(); ++i) {
    if (i != 0) out.push_back(' ');
    out += SerializeString(list.components[i]);
  }
  out.push_back(')');
  for (const SfvParam& p : list.params) {
    out += SerializeParam(p);
  }
  return out;
}

bool BuildSignatureBase(const BaseRequestView& req,
                        const std::vector<GoogleString>& covered,
                        StringPiece params_serialization, GoogleString* out) {
  GoogleString base;
  for (const GoogleString& comp : covered) {
    if (!comp.empty() && comp[0] == '@') {
      StringPiece value;
      if (comp == "@method") {
        value = req.method;
      } else if (comp == "@authority") {
        value = req.authority;
      } else if (comp == "@path") {
        value = req.path;
      } else {
        // Unknown derived component: the caller validates components first,
        // so this is unreachable in practice -- fail closed regardless.
        return false;
      }
      // RFC 9421 component line: "<id>": <value>\n  (no component params in
      // the supported profile).
      StrAppend(&base, "\"", comp, "\": ", value, "\n");
      continue;
    }
    // RFC 9421 section 2.1 HTTP field component: every field line whose name
    // matches the covered id, in wire order, OWS-trimmed per line and joined
    // with ", ". A covered field with NO matching line in the request fails
    // base construction (fail-closed; the signature could never verify
    // honestly anyway).
    StrAppend(&base, "\"", comp, "\": ");
    bool found = false;
    for (const HeaderField& field : req.fields) {
      if (!StringCaseEqual(field.name, comp)) continue;
      if (found) base += ", ";
      StrAppend(&base, TrimOws(field.value));
      found = true;
    }
    if (!found) return false;
    base.push_back('\n');
  }
  // Trailing @signature-params line (NO terminating newline).
  StrAppend(&base, "\"@signature-params\": ", params_serialization);
  *out = std::move(base);
  return true;
}

}  // namespace webbotauth
}  // namespace net_instaweb
