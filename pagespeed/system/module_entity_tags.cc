// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/system/module_entity_tags.h"

#include <cstddef>

namespace net_instaweb {

const char kModuleHtmlEntityTagPrefix[] = "psh-";
const char kModuleMarkdownEntityTagPrefix[] = "psm-";

namespace {

bool IsOws(char c) { return c == ' ' || c == '\t'; }

// etagc = %x21 / %x23-7E / obs-text (RFC 9110 8.8.3).
bool IsEtagc(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u == 0x21 || (u >= 0x23 && u <= 0x7E) || u >= 0x80;
}

bool IsModuleOpaque(StringPiece opaque) {
  return opaque.starts_with(kModuleHtmlEntityTagPrefix) ||
         opaque.starts_with(kModuleMarkdownEntityTagPrefix);
}

bool MentionsModulePrefix(StringPiece line) {
  return line.find(kModuleHtmlEntityTagPrefix) != StringPiece::npos ||
         line.find(kModuleMarkdownEntityTagPrefix) != StringPiece::npos;
}

enum class LineScan { kNoModuleTag, kModuleTag, kMalformed };

// One field line as `*` / #entity-tag (RFC 9110 13.1.2, list rule 5.6.1).
LineScan ScanLine(StringPiece line) {
  const size_t n = line.size();
  // `*` as the whole field value.
  {
    size_t b = 0, e = n;
    while (b < e && IsOws(line[b])) ++b;
    while (e > b && IsOws(line[e - 1])) --e;
    if (e - b == 1 && line[b] == '*') {
      return LineScan::kNoModuleTag;
    }
  }
  size_t pos = 0;
  while (true) {
    while (pos < n && (IsOws(line[pos]) || line[pos] == ',')) ++pos;
    if (pos >= n) {
      return LineScan::kNoModuleTag;
    }
    if (n - pos >= 2 && line[pos] == 'W' && line[pos + 1] == '/') {
      pos += 2;
    }
    if (pos >= n || line[pos] != '"') {
      return LineScan::kMalformed;
    }
    const size_t open = ++pos;
    while (pos < n && line[pos] != '"') {
      if (!IsEtagc(line[pos])) {
        return LineScan::kMalformed;
      }
      ++pos;
    }
    if (pos >= n) {
      return LineScan::kMalformed;  // unterminated
    }
    if (IsModuleOpaque(line.substr(open, pos - open))) {
      return LineScan::kModuleTag;
    }
    ++pos;  // the closing quote
    while (pos < n && IsOws(line[pos])) ++pos;
    if (pos < n && line[pos] != ',') {
      return LineScan::kMalformed;
    }
  }
}

void JoinLines(const StringPieceVector& lines, GoogleString* out) {
  out->clear();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i > 0) {
      out->append(", ");
    }
    lines[i].AppendToString(out);
  }
}

}  // namespace

bool ConditionalsCarryModuleTag(const StringPieceVector& if_none_match_lines) {
  for (StringPiece line : if_none_match_lines) {
    switch (ScanLine(line)) {
      case LineScan::kModuleTag:
        return true;
      case LineScan::kMalformed:
        // A list a strict parser rejects: decided by the prefixes alone.
        if (MentionsModulePrefix(line)) {
          return true;
        }
        break;
      case LineScan::kNoModuleTag:
        break;
    }
  }
  return false;
}

bool StashClientConditionals(const StringPieceVector& if_none_match_lines,
                             const StringPieceVector& if_modified_since_lines,
                             ClientConditionalStash* stash) {
  stash->has_if_none_match = !if_none_match_lines.empty();
  JoinLines(if_none_match_lines, &stash->if_none_match);
  stash->has_if_modified_since = !if_modified_since_lines.empty();
  JoinLines(if_modified_since_lines, &stash->if_modified_since);
  stash->stripped = ConditionalsCarryModuleTag(if_none_match_lines);
  return stash->stripped;
}

}  // namespace net_instaweb
