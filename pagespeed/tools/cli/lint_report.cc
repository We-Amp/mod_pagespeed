// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/tools/cli/lint_report.h"

#include <cstdio>

namespace net_instaweb {
namespace lint {

namespace {

void AppendHex(unsigned char c, const char* prefix, GoogleString* out) {
  static const char kHex[] = "0123456789abcdef";
  out->append(prefix);
  out->push_back(kHex[c >> 4]);
  out->push_back(kHex[c & 15]);
}

void AppendStringArray(const std::vector<GoogleString>& values,
                       GoogleString* out) {
  out->push_back('[');
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out->append(", ");
    }
    AppendJsonString(values[i], out);
  }
  out->push_back(']');
}

}  // namespace

const char* SeverityName(Severity severity) {
  switch (severity) {
    case Severity::kInfo:
      return "info";
    case Severity::kWarning:
      return "warning";
    case Severity::kError:
      return "error";
  }
  return "error";
}

bool ParseSeverity(StringPiece text, Severity* severity) {
  for (Severity s : {Severity::kInfo, Severity::kWarning, Severity::kError}) {
    if (text == SeverityName(s)) {
      *severity = s;
      return true;
    }
  }
  return false;
}

void AppendJsonString(StringPiece s, GoogleString* out) {
  out->push_back('"');
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(ch);
    } else if (c < 0x20 || c >= 0x7f) {
      AppendHex(c, "\\u00", out);
    } else {
      out->push_back(ch);
    }
  }
  out->push_back('"');
}

GoogleString PrintableText(StringPiece s) {
  GoogleString out;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c >= 0x7f || c == '\\') {
      AppendHex(c, "\\x", &out);
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

int Report::Count(Severity severity) const {
  int n = 0;
  for (const Finding& f : findings_) {
    if (f.severity == severity) {
      ++n;
    }
  }
  return n;
}

int Report::ExitCode(Severity fail_on) const {
  for (const Finding& f : findings_) {
    if (static_cast<int>(f.severity) >= static_cast<int>(fail_on)) {
      return kExitFindings;
    }
  }
  return kExitClean;
}

GoogleString Report::ToJson(StringPiece module_version) const {
  GoogleString o = "{\n  \"schema\": ";
  AppendJsonString(kSchema, &o);
  o += ",\n  \"tool_version\": ";
  AppendJsonString(kToolVersion, &o);
  o += ",\n  \"module_version\": ";
  AppendJsonString(module_version, &o);
  o += ",\n  \"inputs\": [";
  for (size_t i = 0; i < inputs_.size(); ++i) {
    const Input& in = inputs_[i];
    o += i > 0 ? ",\n    {" : "\n    {";
    o += "\"path\": ";
    AppendJsonString(in.path, &o);
    o += ", \"kind\": ";
    AppendJsonString(in.kind, &o);
    o += ", \"sha256\": ";
    AppendJsonString(in.sha256, &o);
    o += "}";
  }
  o += inputs_.empty() ? "],\n" : "\n  ],\n";
  o += StrCat("  \"summary\": {\"error\": ", Count(Severity::kError),
              ", \"warning\": ", Count(Severity::kWarning),
              ", \"info\": ", Count(Severity::kInfo), "},\n");
  o += "  \"findings\": [";
  for (size_t i = 0; i < findings_.size(); ++i) {
    const Finding& f = findings_[i];
    o += i > 0 ? ",\n    {" : "\n    {";
    o += "\"rule\": ";
    AppendJsonString(f.rule, &o);
    o += ", \"severity\": ";
    AppendJsonString(SeverityName(f.severity), &o);
    o += ", \"file\": ";
    AppendJsonString(f.file, &o);
    o += StrCat(", \"line\": ", f.line, ", \"scope\": ");
    AppendJsonString(f.scope, &o);
    o += ", \"directive\": ";
    AppendJsonString(f.directive, &o);
    o += ", \"args\": ";
    AppendStringArray(f.args, &o);
    o += ", \"message\": ";
    AppendJsonString(f.message, &o);
    o += "}";
  }
  o += findings_.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return o;
}

GoogleString Report::ToText() const {
  GoogleString o;
  for (const Finding& f : findings_) {
    o += PrintableText(f.file);
    if (f.line > 0) {
      o += StrCat(":", f.line);
    }
    o += StrCat(": ", SeverityName(f.severity), " ", PrintableText(f.rule),
                ": ", PrintableText(f.message));
    if (!f.directive.empty()) {
      o += StrCat(" [", PrintableText(f.directive));
      for (const GoogleString& a : f.args) {
        o += StrCat(" ", PrintableText(a));
      }
      o += "]";
    }
    o += "\n";
  }
  o += StrCat(Count(Severity::kError), " error(s), ", Count(Severity::kWarning),
              " warning(s), ", Count(Severity::kInfo), " info\n");
  return o;
}

}  // namespace lint
}  // namespace net_instaweb
