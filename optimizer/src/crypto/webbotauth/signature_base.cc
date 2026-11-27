// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// In sync with mod_pagespeed
// pagespeed/kernel/webbotauth/signature_base.cc @ 626ddfa95 (2026-07-03). Keep
// logic in sync manually; the RFC 9421/8941 core is standards-frozen. Extends
// the original core (upstreamed to 1.15): RFC 9421
// section 2.1 HTTP field components (see signature_base.h).

#include "src/crypto/webbotauth/signature_base.h"

#include <string>
#include <string_view>
#include <vector>

#include "absl/strings/str_cat.h"

namespace pagespeed {
namespace webbotauth {

namespace {

// Serialize an sf-string per RFC 8941 section 4.1.3: surround with quotes and
// backslash-escape '"' and '\'.
std::string SerializeString(std::string_view s) {
  std::string out;
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

std::string SerializeParam(const SfvParam& p) {
  switch (p.type) {
    case SfvParam::kInteger:
      return absl::StrCat(";", p.name, "=", p.int_value);
    case SfvParam::kString:
      return absl::StrCat(";", p.name, "=", SerializeString(p.str_value));
    case SfvParam::kToken:
      return absl::StrCat(";", p.name, "=", p.str_value);
    case SfvParam::kBoolean:
      // Boolean true is serialized as a bare key (no "=?1").
      return absl::StrCat(";", p.name);
  }
  return std::string();
}

}  // namespace

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

std::string SerializeSignatureParams(const SfvInnerList& list) {
  std::string out = "(";
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

namespace {

// RFC 9421 section 2.1: strip leading/trailing optional whitespace (SP/HTAB)
// from each field line value.
std::string_view TrimOws(std::string_view v) {
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
    v.remove_prefix(1);
  }
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
    v.remove_suffix(1);
  }
  return v;
}

}  // namespace

bool BuildSignatureBase(const BaseRequestView& req,
                        const std::vector<std::string>& covered,
                        std::string_view params_serialization,
                        std::string* out) {
  std::string base;
  for (const std::string& comp : covered) {
    if (!comp.empty() && comp[0] == '@') {
      std::string_view value;
      if (comp == "@method") {
        value = req.method;
      } else if (comp == "@authority") {
        value = req.authority;
      } else if (comp == "@path") {
        value = req.path;
      } else {
        // Unknown derived component: the caller validates components first,
        // so this is unreachable in practice — fail closed regardless.
        return false;
      }
      // RFC 9421 component line: "<id>": <value>\n  (no component params in
      // the supported profile).
      absl::StrAppend(&base, "\"", comp, "\": ", value, "\n");
      continue;
    }
    // RFC 9421 section 2.1 HTTP field component: every field line whose name
    // matches the covered id, in wire order, OWS-trimmed per line and joined
    // with ", ". A covered field with NO matching line in the request fails
    // base construction (fail-closed; the signature could never verify
    // honestly anyway).
    absl::StrAppend(&base, "\"", comp, "\": ");
    bool found = false;
    for (const HeaderField& field : req.fields) {
      if (!EqualsIgnoreAsciiCase(field.name, comp)) continue;
      if (found) base += ", ";
      absl::StrAppend(&base, TrimOws(field.value));
      found = true;
    }
    if (!found) return false;
    base.push_back('\n');
  }
  // Trailing @signature-params line (NO terminating newline).
  absl::StrAppend(&base, "\"@signature-params\": ", params_serialization);
  *out = std::move(base);
  return true;
}

}  // namespace webbotauth
}  // namespace pagespeed
