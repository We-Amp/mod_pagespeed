// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_serve.h"

#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

namespace {

// The server core's own lines, which a Markdown answer keeps.
const char* const kTransportLines[] = {"Date", "Server", "Connection",
                                       "Keep-Alive", "Transfer-Encoding"};

bool IsTransportLine(StringPiece name) {
  for (const char* line : kTransportLines) {
    if (StringCaseEqual(name, line)) return true;
  }
  return false;
}

bool HasLine(const DaemonHtmlHeaderLines& lines, StringPiece name) {
  for (const auto& line : lines) {
    if (StringCaseEqual(line.first, name)) return true;
  }
  return false;
}

}  // namespace

std::wstring IisWidenRawTarget(const char* raw, size_t length) {
  std::wstring out;
  if (raw == nullptr) return out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(raw[i])));
  }
  return out;
}

bool IisPristineTarget(const wchar_t* value, size_t length, GoogleString* out) {
  out->clear();
  if (value == nullptr || length == 0) return false;
  GoogleString target;
  target.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    if (value[i] == L'\0' || value[i] > 0xFF) return false;
    target.push_back(static_cast<char>(value[i]));
  }
  if (target[0] != '/') {
    const size_t scheme_end = target.find("://");
    const size_t path_start = scheme_end == GoogleString::npos
                                  ? GoogleString::npos
                                  : target.find('/', scheme_end + 3);
    if (path_start == GoogleString::npos) {
      *out = "/";
      return true;
    }
    target.erase(0, path_start);
  }
  out->swap(target);
  return true;
}

bool IisHtmlCachePathIsCanonical(StringPiece target) {
  StringPiece path = target;
  const size_t query = path.find('?');
  if (query != StringPiece::npos) {
    // The query is not read, except that it is a valid target: no raw byte
    // outside printable ASCII.
    for (const char ch : target.substr(query)) {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (c < 0x21 || c >= 0x7F) return false;
    }
    path = path.substr(0, query);
  }
  if (path.empty() || path[0] != '/') return false;
  size_t segment_start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const StringPiece segment =
          path.substr(segment_start, i - segment_start);
      const bool last = i == path.size();
      // An empty segment is a merged slash, except a directory's trailing
      // one (and the root's).
      if (segment.empty() && !last) return false;
      if (segment == "." || segment == "..") return false;
      if (!segment.empty() && segment[segment.size() - 1] == '.') {
        return false;
      }
      segment_start = i + 1;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(path[i]);
    // No percent-escape: every escaped spelling has another IIS reads as
    // the same file.
    if (c == '%') return false;
    if (c < 0x21 || c >= 0x7F) return false;
    if (c >= 'A' && c <= 'Z') return false;
    switch (c) {
      case '\\':
      case ';':
      case ':':
      case '~':
      case '*':
      case '"':
      case '<':
      case '>':
      case '|':
        return false;
      default:
        break;
    }
  }
  return true;
}

GoogleString IisDaemonHtmlAjaxMarker(
    const std::function<bool(const char* name, StringPiece* value)>& header) {
  static const char* const kNames[] = {"X-Requested-With", "X-MicrosoftAjax",
                                       "X-Prototype-Version"};
  for (const char* name : kNames) {
    StringPiece value;
    if (!header(name, &value)) continue;
    const char* marker = DaemonHtmlAjaxMarker(name, value);
    if (marker != nullptr) return marker;
  }
  return GoogleString();
}

GoogleString IisVaryWithAccept(StringPiece existing) {
  StringPieceVector tokens;
  SplitStringPieceToVector(existing, ",", &tokens, true);
  for (StringPiece t : tokens) {
    TrimWhitespace(&t);
    if (StringCaseEqual(t, "Accept") || t == "*") return existing.as_string();
  }
  StringPiece trimmed = existing;
  TrimWhitespace(&trimmed);
  if (trimmed.empty()) return "Accept";
  return StrCat(existing, ", Accept");
}

IisMarkdownFinalPlan PlanIisMarkdownFinal(int status, int substatus,
                                          size_t entity_chunks,
                                          const DaemonHtmlHeaderLines& current,
                                          const IisMarkdownAnswer& answer) {
  IisMarkdownFinalPlan plan;
  if (answer.status != 200 && answer.status != 304) return plan;
  // Still the empty response the main registration left: no other module
  // gave it a status, a body or a type of its own.
  if (status != kIisMarkdownPendingStatus ||
      substatus != kIisMarkdownPendingSubStatus || entity_chunks != 0 ||
      HasLine(current, HttpAttributes::kContentType)) {
    return plan;
  }
  for (const auto& line : current) {
    if (StringCaseEqual(line.first, HttpAttributes::kContentEncoding) ||
        StringCaseEqual(line.first, "Content-Range")) {
      // The body is written after the stage that set this: it would not
      // describe it.
      plan.transport_conflict = true;
      return plan;
    }
  }
  plan.finalize = true;
  const bool has_length =
      HasLine(answer.headers, HttpAttributes::kContentLength);
  for (const auto& line : current) {
    const GoogleString& name = line.first;
    if (IsTransportLine(name) &&
        !(has_length &&
          StringCaseEqual(name, HttpAttributes::kTransferEncoding))) {
      continue;
    }
    bool listed = false;
    for (const GoogleString& r : plan.remove) {
      listed = listed || StringCaseEqual(r, name);
    }
    if (!listed) plan.remove.push_back(name);
  }
  return plan;
}

}  // namespace net_instaweb
