// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/llms_txt_formatter.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

namespace {

// Output caps (self-bounded; the builder also caps entry counts).
constexpr std::size_t kMaxTitleBytes = 200;
constexpr std::size_t kMaxSummaryBytes = 400;
constexpr std::size_t kMaxUrlBytes = 2048;

// Collapse to a single line: tabs/newlines/other control bytes become spaces,
// runs of spaces collapse, leading/trailing spaces trimmed, length-capped.
// Strips a page's ability to inject newlines (hence new markdown blocks).
std::string OneLine(std::string_view s, std::size_t cap) {
  std::string out;
  out.reserve(s.size() < cap ? s.size() : cap);
  bool pending_space = false;
  bool started = false;
  for (unsigned char c : s) {
    bool is_space = (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                     c == '\f' || c == '\v');
    if (is_space) {
      if (started) pending_space = true;
      continue;
    }
    if (c < 0x20 || c == 0x7F) continue;  // drop other control bytes
    if (pending_space) {
      if (out.size() + 1 >= cap) break;
      out.push_back(' ');
      pending_space = false;
    }
    if (out.size() >= cap) break;
    out.push_back(static_cast<char>(c));
    started = true;
  }
  return out;
}

// Escape markdown link-text delimiters so a title cannot break out of [...]
// or forge a code span. Runs after OneLine.
std::string EscapeLinkText(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '[' || c == ']' || c == '\\' || c == '`') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// A hex digit for percent-encoding.
char HexDigit(unsigned v) {
  return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10));
}

// Validate + sanitize a URL for emission inside markdown (url). Requires an
// http(s) scheme; percent-encodes bytes that would break the (url) delimiter
// or markdown structure (space, parens, angle brackets, control, non-ASCII).
// Returns "" if the URL is not http(s) (the caller then drops the entry).
std::string SanitizeUrl(std::string_view s) {
  if (s.size() > kMaxUrlBytes) return std::string();
  std::string lower;
  lower.reserve(8);
  for (std::size_t i = 0; i < s.size() && i < 8; ++i) {
    char c = s[i];
    lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                           : c);
  }
  if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
    return std::string();
  }
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    const bool safe =
        (c > 0x20 && c < 0x7F && c != '(' && c != ')' && c != '<' && c != '>' &&
         c != '"' && c != '`' && c != '\\' && c != ' ');
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HexDigit(c >> 4));
      out.push_back(HexDigit(c & 0x0F));
    }
  }
  return out;
}

}  // namespace

std::string FormatLlmsTxt(std::string_view site_title,
                          std::string_view site_summary,
                          const std::vector<LlmsTxtSection>& sections) {
  std::string out;

  // H1 site title (always present; fall back to a neutral label if empty).
  std::string title = OneLine(site_title, kMaxTitleBytes);
  out += "# ";
  out += title.empty() ? "Site index" : title;
  out += "\n";

  // Optional blockquote one-line summary. Escape markdown delimiters too — a
  // summary is attacker-influenced page content and must not forge inline links
  // or code spans into the LLM-consumed file (the H1 above is the trusted host).
  std::string summary = EscapeLinkText(OneLine(site_summary, kMaxSummaryBytes));
  if (!summary.empty()) {
    out += "\n> ";
    out += summary;
    out += "\n";
  }

  for (const LlmsTxtSection& section : sections) {
    // Collect the section's valid links first so we can skip empty sections.
    std::string body;
    for (const LlmsTxtLink& link : section.links) {
      std::string url = SanitizeUrl(link.url);
      if (url.empty()) continue;  // non-http(s) / oversized — drop
      std::string text = EscapeLinkText(OneLine(link.title, kMaxTitleBytes));
      if (text.empty()) text = url;  // never emit an empty link label
      body += "- [";
      body += text;
      body += "](";
      body += url;
      body += ")";
      std::string s = EscapeLinkText(OneLine(link.summary, kMaxSummaryBytes));
      if (!s.empty()) {
        body += ": ";
        body += s;
      }
      body += "\n";
    }
    if (body.empty()) continue;  // skip a section with no emittable links

    std::string heading =
        EscapeLinkText(OneLine(section.heading, kMaxTitleBytes));
    out += "\n## ";
    out += heading.empty() ? "Pages" : heading;
    out += "\n\n";
    out += body;
  }
  return out;
}

}  // namespace net_instaweb
