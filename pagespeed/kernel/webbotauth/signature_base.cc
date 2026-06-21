// Copyright 2026 We-Amp B.V.
// Licensed under the Apache License, Version 2.0 (the "License").

#include "pagespeed/kernel/webbotauth/signature_base.h"

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

GoogleString BuildSignatureBase(const BaseRequestView& req,
                                const std::vector<GoogleString>& covered,
                                StringPiece params_serialization) {
  GoogleString base;
  for (const GoogleString& comp : covered) {
    StringPiece value;
    if (comp == "@method") {
      value = req.method;
    } else if (comp == "@authority") {
      value = req.authority;
    } else if (comp == "@path") {
      value = req.path;
    } else {
      // Should never happen: caller validates components first. Fail-safe to an
      // empty value, which will simply fail verification.
      value = StringPiece();
    }
    // RFC 9421 component line: "<id>": <value>\n   (no params on derived comps)
    StrAppend(&base, "\"", comp, "\": ", value, "\n");
  }
  // Trailing @signature-params line (NO terminating newline).
  StrAppend(&base, "\"@signature-params\": ", params_serialization);
  return base;
}

}  // namespace webbotauth
}  // namespace net_instaweb
