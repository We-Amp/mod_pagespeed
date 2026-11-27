/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

namespace {

// The `text/markdown` media-range present as a comma/space/tab-delimited token
// (tolerating a trailing `;q=...`), never matched by `*/*`. This presence test
// decides `Vary: Accept` only; selecting a Markdown body uses
// AcceptMarkdownPreference.
bool AcceptContainsMarkdown(StringPiece accept) {
  const StringPiece kMarkdown("text/markdown");
  size_t pos = 0;
  while (pos < accept.size()) {
    size_t found = accept.find(kMarkdown, pos);
    if (found == StringPiece::npos) {
      return false;
    }
    // Left boundary: start-of-string, comma, or whitespace.
    bool left_ok = (found == 0) || accept[found - 1] == ',' ||
                   accept[found - 1] == ' ' || accept[found - 1] == '\t';
    // Right boundary: end-of-string, comma, semicolon (q-params), whitespace.
    size_t end = found + kMarkdown.size();
    bool right_ok = (end >= accept.size()) || accept[end] == ',' ||
                    accept[end] == ';' || accept[end] == ' ' ||
                    accept[end] == '\t';
    if (left_ok && right_ok) {
      return true;
    }
    pos = found + 1;
  }
  return false;
}

// RFC 9110 section 5.6.2 tchar.
bool IsTchar(unsigned char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

void SkipOws(StringPiece s, size_t* pos) {
  while (*pos < s.size() && (s[*pos] == ' ' || s[*pos] == '\t')) {
    ++*pos;
  }
}

StringPiece ParseToken(StringPiece s, size_t* pos) {
  const size_t start = *pos;
  while (*pos < s.size() && IsTchar(static_cast<unsigned char>(s[*pos]))) {
    ++*pos;
  }
  return StringPiece(s.substr(start, *pos - start));
}

// RFC 9110 section 5.6.4 quoted-string; *pos is at the opening quote.
bool SkipQuotedString(StringPiece s, size_t* pos) {
  ++*pos;
  while (*pos < s.size()) {
    unsigned char c = static_cast<unsigned char>(s[*pos]);
    if (c == '"') {
      ++*pos;
      return true;
    }
    if (c == '\\') {
      if (*pos + 1 >= s.size()) {
        return false;
      }
      c = static_cast<unsigned char>(s[*pos + 1]);
      ++*pos;
    }
    // qdtext and quoted-pair: HTAB, SP, VCHAR, obs-text.
    if (c != '\t' && (c < 0x20 || c == 0x7f)) {
      return false;
    }
    ++*pos;
  }
  return false;
}

// RFC 9110 section 12.4.2:
//   qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] )
// Stores the value in thousandths.
bool ParseQValue(StringPiece v, int* q) {
  if (v.empty() || (v[0] != '0' && v[0] != '1')) {
    return false;
  }
  const int whole = v[0] - '0';
  int frac = 0;
  if (v.size() > 1) {
    if (v[1] != '.' || v.size() > 5) {
      return false;
    }
    for (size_t i = 2; i < 5; ++i) {
      int digit = 0;
      if (i < v.size()) {
        if (v[i] < '0' || v[i] > '9') {
          return false;
        }
        digit = v[i] - '0';
      }
      frac = frac * 10 + digit;
    }
  }
  if (whole == 1 && frac != 0) {
    return false;
  }
  *q = whole * 1000 + frac;
  return true;
}

void KeepMax(int q, int* slot) {
  if (q > *slot) {
    *slot = q;
  }
}

}  // namespace

// One media range with its parameters, starting at *pos. Returns false when it
// is malformed. RFC 9110 sections 5.6.6 and 12.5.1.
bool AcceptMarkdownPreference::ParseElement(StringPiece s, size_t* pos) {
  const StringPiece type = ParseToken(s, pos);
  if (type.empty() || *pos >= s.size() || s[*pos] != '/') {
    return false;
  }
  ++*pos;
  const StringPiece subtype = ParseToken(s, pos);
  if (subtype.empty()) {
    return false;
  }
  if (type == "*" && subtype != "*") {
    return false;
  }

  int q = 1000;
  bool have_q = false;
  for (;;) {
    const size_t before = *pos;
    SkipOws(s, pos);
    if (*pos >= s.size() || s[*pos] != ';') {
      *pos = before;
      break;
    }
    ++*pos;
    SkipOws(s, pos);
    // An empty parameter is allowed: parameters = *( OWS ";" OWS [ parameter ] )
    if (*pos >= s.size() || s[*pos] == ';' || s[*pos] == ',') {
      continue;
    }
    const StringPiece name = ParseToken(s, pos);
    if (name.empty() || *pos >= s.size() || s[*pos] != '=') {
      return false;
    }
    ++*pos;
    const bool is_q = StringCaseEqual(name, "q");
    if (*pos < s.size() && s[*pos] == '"') {
      // The weight is never quoted.
      if (is_q || !SkipQuotedString(s, pos)) {
        return false;
      }
      continue;
    }
    const StringPiece value = ParseToken(s, pos);
    if (value.empty()) {
      return false;
    }
    if (is_q) {
      if (have_q || !ParseQValue(value, &q)) {
        return false;
      }
      have_q = true;
    }
  }

  if (type == "*") {
    KeepMax(q, &any_q_);
  } else if (StringCaseEqual(type, "text")) {
    if (StringCaseEqual(subtype, "markdown")) {
      if (markdown_q_ >= 0 && markdown_q_ != q) {
        return false;
      }
      markdown_q_ = q;
    } else if (StringCaseEqual(subtype, "html")) {
      KeepMax(q, &html_q_);
    } else if (subtype == "*") {
      KeepMax(q, &text_any_q_);
    }
  }
  return true;
}

void AcceptMarkdownPreference::AddLine(StringPiece line) {
  size_t pos = 0;
  while (!malformed_) {
    // Empty list elements are allowed (RFC 9110 section 5.6.1).
    SkipOws(line, &pos);
    while (pos < line.size() && line[pos] == ',') {
      ++pos;
      SkipOws(line, &pos);
    }
    if (pos >= line.size()) {
      return;
    }
    if (!ParseElement(line, &pos)) {
      malformed_ = true;
      return;
    }
    SkipOws(line, &pos);
    if (pos >= line.size()) {
      return;
    }
    if (line[pos] != ',') {
      malformed_ = true;
    }
  }
}

bool AcceptMarkdownPreference::PrefersMarkdown() const {
  if (malformed_ || markdown_q_ <= 0) {
    return false;
  }
  int html_q = 0;
  if (html_q_ >= 0) {
    html_q = html_q_;
  } else if (text_any_q_ >= 0) {
    html_q = text_any_q_;
  } else if (any_q_ >= 0) {
    html_q = any_q_;
  }
  return markdown_q_ >= html_q;
}

AgentOptimizeVaryFilter::AgentOptimizeVaryFilter(RewriteDriver* driver)
    : CommonFilter(driver) {}

AgentOptimizeVaryFilter::~AgentOptimizeVaryFilter() {}

bool AgentOptimizeVaryFilter::RequestAcceptsMarkdown(
    const RequestHeaders* request_headers) {
  if (request_headers == nullptr) {
    return false;
  }
  ConstStringStarVector accept_values;
  if (!request_headers->Lookup(HttpAttributes::kAccept, &accept_values)) {
    return false;
  }
  for (const GoogleString* value : accept_values) {
    if (value != nullptr && AcceptContainsMarkdown(*value)) {
      return true;
    }
  }
  return false;
}

bool AgentOptimizeVaryFilter::RequestPrefersMarkdown(
    const RequestHeaders* request_headers) {
  if (request_headers == nullptr) {
    return false;
  }
  // The stored header lines, not Lookup(): Lookup() splits Accept at every
  // comma, including one inside a quoted parameter value.
  AcceptMarkdownPreference preference;
  for (int i = 0, n = request_headers->NumAttributes(); i < n; ++i) {
    if (StringCaseEqual(request_headers->Name(i), HttpAttributes::kAccept)) {
      preference.AddLine(request_headers->Value(i));
    }
  }
  return preference.PrefersMarkdown();
}

void AgentOptimizeVaryFilter::StartDocumentImpl() {
  // Response headers are mutable only until the first Flush; after that
  // mutable_response_headers() returns NULL and we must not touch them.
  ResponseHeaders* headers = driver()->mutable_response_headers();
  if (headers == nullptr) {
    return;
  }

  // This filter is only added when options()->agent_optimize() is set (see
  // rewrite_driver_filter_init.cc) — the operator flag is the whole gate; the
  // feature carries no license/entitlement check of its own. The remaining
  // condition is an Accept: text/markdown request, or an HTML cache mode
  // that can answer this URL with its Markdown copy: then the response
  // depends on Accept for every client, whatever this request sent.
  // The body is never changed here — the sole effect is the Vary: Accept
  // signal.
  // Every mode at or after serve-variants (the modes are ordered).
  const bool may_serve_markdown = driver()->options()->daemon_html_cache() >=
                                  RewriteOptions::kDaemonHtmlCacheServeVariants;
  if (!may_serve_markdown &&
      !RequestAcceptsMarkdown(driver()->request_headers())) {
    return;
  }

  // Idempotent: never duplicate an existing Vary: Accept (e.g. one already
  // added for image/WebP negotiation). Vary values are RFC 9110 tokens, so
  // the comparison is case-insensitive: an origin spelling it "accept" is
  // the same token and must not earn a second one.
  if (!headers->HasValueCaseInsensitive(HttpAttributes::kVary,
                                        HttpAttributes::kAccept)) {
    headers->Add(HttpAttributes::kVary, HttpAttributes::kAccept);
    // Add() marks the precomputed caching fields dirty, and the document's
    // headers were already computed before the parse began (e.g. by
    // ProxyFetch::AddPagespeedHeader). Restore the invariant that mutator
    // establishes, so later readers of the caching fields (IsBrowserCacheable
    // and friends) see valid data rather than tripping their DCHECKs.
    // Accept is a named token and never '*', which is the only Vary input
    // ComputeCaching() consults, so this recompute changes no cached value.
    headers->ComputeCaching();
  }
}

}  // namespace net_instaweb
