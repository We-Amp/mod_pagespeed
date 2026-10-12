// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_TEMPLATE_H_
#define PAGESPEED_KERNEL_PACKS_TEMPLATE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace pagespeed::packs {

// Placeholders a template may contain. Nothing else is expanded: no
// expressions, no conditionals, no nesting.
//   {scheme} {host} {path} {query} {url} {1}..{9}
//   {title} {description} {canonical}   (jsonld templates only)
enum class Placeholder {
  kScheme,
  kHost,
  kPath,
  kQuery,    // with a leading '?', or empty when the URL has no query
  kUrl,      // scheme://host/path?query
  kCapture,  // {1}..{9}: RE2 capture groups of the rule's path_regex
  kTitle,
  kDescription,
  kCanonical,
};

struct TemplateSegment {
  bool is_literal = true;
  std::string literal;                             // when is_literal
  Placeholder placeholder = Placeholder::kScheme;  // otherwise
  int capture = 0;                                 // 1..9 when kCapture
};

// A parsed template. A '{' that does not start a "{identifier}" token is a
// literal character, so JSON text such as {"a":1} needs no escaping. Any
// "{identifier}" token that is not one of the placeholders above is an
// error, so a typo cannot silently expand to nothing.
struct Template {
  std::string source;
  std::vector<TemplateSegment> segments;
};

struct TemplateParseOptions {
  // {title}, {description} and {canonical} are accepted only when true.
  bool allow_page_values = false;
  // Number of capturing groups of the rule's path_regex (0 when none). A
  // {n} placeholder with n above this is rejected.
  int capture_groups = 0;
};

// Parses `text`. The error message is the reason only (no file or path).
absl::StatusOr<Template> ParseTemplate(std::string_view text,
                                       const TemplateParseOptions& options);

struct PageUrl {
  std::string scheme;  // "http" or "https"
  std::string host;    // lowercased, port stripped
  std::string path;    // starts with '/'
  std::string query;   // without the leading '?'; may be empty
};

struct ExpandContext {
  PageUrl url;
  // captures[i] holds group i+1 of path_regex. A group beyond the vector, or
  // one that did not participate in the match, expands to the empty string.
  std::vector<std::string> captures;
  // Final values of the document after earlier rules ran; empty when none.
  std::string title;
  std::string description;
  std::string canonical;
};

// How placeholder VALUES are escaped (literal template text never is).
enum class EscapeContext {
  kNone,        // attribute values: the writer's attribute quoting applies
  kHtmlText,    // title text: & < > are entity-escaped in the WHOLE result,
                // literal template text included
  kJsonString,  // JSON-LD: values are JSON-string-escaped, and every '<' in
                // the whole result is emitted as < so "</script" can
                // never appear
};

// Expands `tpl`. Fails with kResourceExhausted when the result would exceed
// `max_bytes` (measured after escaping). The cap is checked before each piece
// is escaped or appended, so an oversize value is never copied.
absl::StatusOr<std::string> ExpandTemplate(const Template& tpl,
                                           const ExpandContext& ctx,
                                           EscapeContext escape,
                                           size_t max_bytes);

// Entity-escapes & < > for use as HTML element text. This is the one helper
// every caller must use for text that comes from a table value (or anywhere
// else outside ExpandTemplate) before it becomes the content of an element.
std::string EscapeForHtmlText(std::string_view s);
std::string EscapeJsonString(std::string_view s);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_TEMPLATE_H_
