// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/html/sitemap_parser.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/string_util.h"

namespace net_instaweb {

namespace {

// Decode the five XML predefined entities plus numeric character references.
// Unknown entities are left verbatim (fail-soft). Bounded by the input length;
// output never exceeds input length.
std::string DecodeXmlEntities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      ++i;
      continue;
    }
    std::size_t semi = s.find(';', i + 1);
    // Bound the entity scan: a real entity is short; a stray '&' must not make
    // us swallow the rest of the document looking for a ';'.
    if (semi == std::string_view::npos || semi - i > 12) {
      out.push_back('&');
      ++i;
      continue;
    }
    std::string_view ent = s.substr(i + 1, semi - (i + 1));  // between & and ;
    if (ent == "amp") {
      out.push_back('&');
    } else if (ent == "lt") {
      out.push_back('<');
    } else if (ent == "gt") {
      out.push_back('>');
    } else if (ent == "quot") {
      out.push_back('"');
    } else if (ent == "apos") {
      out.push_back('\'');
    } else if (!ent.empty() && ent[0] == '#') {
      // Numeric character reference: &#NN; (decimal) or &#xHH; (hex).
      unsigned long code = 0;
      bool ok = false;
      if (ent.size() >= 2 && (ent[1] == 'x' || ent[1] == 'X')) {
        ok = ent.size() > 2;
        for (std::size_t k = 2; k < ent.size() && ok; ++k) {
          char c = ent[k];
          code *= 16;
          if (c >= '0' && c <= '9') {
            code += static_cast<unsigned>(c - '0');
          } else if (c >= 'a' && c <= 'f') {
            code += static_cast<unsigned>(c - 'a' + 10);
          } else if (c >= 'A' && c <= 'F') {
            code += static_cast<unsigned>(c - 'A' + 10);
          } else {
            ok = false;
          }
          if (code > 0x10FFFF) ok = false;
        }
      } else {
        ok = ent.size() > 1;
        for (std::size_t k = 1; k < ent.size() && ok; ++k) {
          char c = ent[k];
          if (c >= '0' && c <= '9') {
            code = code * 10 + static_cast<unsigned>(c - '0');
          } else {
            ok = false;
          }
          if (code > 0x10FFFF) ok = false;
        }
      }
      // Reject UTF-16 surrogate code points (0xD800-0xDFFF): they are not valid
      // Unicode scalars and would otherwise encode to ill-formed UTF-8.
      if (code >= 0xD800 && code <= 0xDFFF) ok = false;
      if (!ok) {
        // Not a valid numeric ref — keep verbatim.
        out.push_back('&');
        ++i;
        continue;
      }
      // Encode the code point as UTF-8.
      if (code < 0x80) {
        out.push_back(static_cast<char>(code));
      } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
      }
    } else {
      // Unknown entity — keep verbatim.
      out.push_back('&');
      ++i;
      continue;
    }
    i = semi + 1;
  }
  return out;
}

// A conformant <loc> URL has no unescaped whitespace and no control bytes.
// Reject anything else (fail-closed: a malformed <loc> is dropped, not emitted).
bool IsValidLoc(std::string_view loc) {
  if (loc.empty()) return false;
  for (unsigned char c : loc) {
    if (c < 0x20 || c == 0x7F || c == ' ') return false;
  }
  return true;
}

// True iff the byte after a "<tag" match is a valid name terminator, so that
// "<loc" does not spuriously match "<location".
bool IsTagBoundary(char c) { return c == '>' || c == '/' || IsHtmlSpace(c); }

// Returns the position of the first occurrence of "<urlset" / "<sitemapindex"
// (case-insensitive) treated as a real element open, or npos.
std::string_view::size_type FindRootOpen(std::string_view xml,
                                         std::string_view tag) {
  std::string_view::size_type pos = 0;
  while (true) {
    auto hit = FindIgnoreCase(xml.substr(pos), tag);
    if (hit == std::string_view::npos) return std::string_view::npos;
    std::size_t abs = pos + hit;
    std::size_t after = abs + tag.size();
    if (after >= xml.size() || IsTagBoundary(xml[after])) return abs;
    pos = abs + 1;
  }
}

}  // namespace

ParsedSitemap ParseSitemap(std::string_view xml) {
  ParsedSitemap result;
  if (xml.size() > kSitemapMaxInputBytes) {
    xml = xml.substr(0, kSitemapMaxInputBytes);
  }

  // Determine the document kind from whichever root element appears first.
  auto urlset_pos = FindRootOpen(xml, "<urlset");
  auto index_pos = FindRootOpen(xml, "<sitemapindex");
  const bool is_index =
      index_pos != std::string_view::npos &&
      (urlset_pos == std::string_view::npos || index_pos < urlset_pos);
  const bool is_urlset = urlset_pos != std::string_view::npos && !is_index;
  if (!is_index && !is_urlset) return result;  // not a recognizable sitemap

  std::vector<std::string>& sink =
      is_index ? result.nested_sitemap_locs : result.page_locs;

  // Scan for every <loc>…</loc>.
  std::string_view::size_type pos = 0;
  while (sink.size() < kSitemapMaxEntries) {
    auto open = FindIgnoreCase(xml.substr(pos), "<loc");
    if (open == std::string_view::npos) break;
    std::size_t open_abs = pos + open;
    std::size_t name_end = open_abs + 4;  // after "<loc"
    if (name_end >= xml.size() || !IsTagBoundary(xml[name_end])) {
      pos = open_abs + 1;  // e.g. "<location" — not our element
      continue;
    }
    std::size_t gt = xml.find('>', name_end);
    if (gt == std::string_view::npos) break;
    if (gt > name_end && xml[gt - 1] == '/') {  // self-closing <loc/> — no text
      pos = gt + 1;
      continue;
    }
    std::size_t text_start = gt + 1;
    auto close = FindIgnoreCase(xml.substr(text_start), "</loc");
    if (close == std::string_view::npos) break;
    std::size_t text_end = text_start + close;
    std::string_view raw = xml.substr(text_start, text_end - text_start);

    // Advance past the closing tag's '>' for the next iteration.
    std::size_t close_gt = xml.find('>', text_end);
    pos = (close_gt == std::string_view::npos) ? xml.size() : close_gt + 1;

    // Trim, unwrap CDATA, decode entities, validate.
    TrimHtmlWhitespace(&raw);
    std::string loc;
    if (raw.size() >= 12 && raw.substr(0, 9) == "<![CDATA[" &&
        raw.substr(raw.size() - 3) == "]]>") {
      loc = std::string(raw.substr(9, raw.size() - 12));  // CDATA is literal
    } else {
      loc = DecodeXmlEntities(raw);
    }
    std::string_view loc_view(loc);
    TrimHtmlWhitespace(&loc_view);
    if (IsValidLoc(loc_view)) sink.emplace_back(loc_view);
  }
  if (sink.size() >= kSitemapMaxEntries) result.truncated = true;
  return result;
}

}  // namespace net_instaweb
