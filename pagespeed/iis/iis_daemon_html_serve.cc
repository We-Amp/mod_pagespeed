// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/iis/iis_daemon_html_serve.h"

#include "pagespeed/kernel/http/http_names.h"

namespace net_instaweb {

namespace {

int UpperHexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

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

bool IisHtmlCachePathIsCanonical(StringPiece target) {
  StringPiece path = target;
  const size_t query = path.find('?');
  if (query != StringPiece::npos) path = path.substr(0, query);
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
    if (c == '%') {
      if (i + 2 >= path.size()) return false;  // Not two digits left.
      const int hi = UpperHexValue(path[i + 1]);
      const int lo = UpperHexValue(path[i + 2]);
      if (hi < 0 || lo < 0 || hi * 16 + lo < 0x80) return false;
      i += 2;
      continue;
    }
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
  plan.finalize = true;
  const bool has_length =
      HasLine(answer.headers, HttpAttributes::kContentLength);
  for (const auto& line : current) {
    const GoogleString& name = line.first;
    if (StringCaseEqual(name, HttpAttributes::kContentEncoding) ||
        StringCaseEqual(name, "Content-Range")) {
      plan.transport_conflict = true;
      continue;
    }
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
