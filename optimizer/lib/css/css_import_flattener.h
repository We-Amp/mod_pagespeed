// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSS @import Flattener
//
// Scans CSS for @import statements and replaces them with the
// imported CSS content read from cache, eliminating sequential
// blocking requests. Flattening is all-or-nothing per sheet: an
// @import that cannot be inlined (not cached, depth limit, unsafe
// media value, structurally unbalanced child, unparseable form) would
// land mid-sheet after inlined rules — a position where browsers must
// ignore @import (CSS Cascading L4) — silently dropping its styles.
// In that case the sheet is returned unchanged, so partial cache
// state never renders worse than the unflattened original.

#ifndef PAGESPEED_LIB_CSS_CSS_IMPORT_FLATTENER_H_
#define PAGESPEED_LIB_CSS_CSS_IMPORT_FLATTENER_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {
namespace css {

// Information about a single @import statement.
struct CssImport {
  std::string url;       // Extracted URL from @import
  std::string media;     // Media query if any (empty = all)
  size_t start_pos = 0;  // Start position of @import in CSS
  size_t end_pos = 0;    // End position (after semicolon)
};

// Extract all @import statements from the beginning of CSS.
// Per CSS Cascading L4/L5, @import must appear before any rules except
// @charset and @layer statements; an @layer block ends the import
// prelude. Stops at the first construct that ends the prelude.
std::vector<CssImport> ExtractImports(std::string_view css);

// Resolve relative URLs in CSS url() references and string-form
// @import URLs ("..."/'...') against a base URL. Given imported CSS
// from import_url, rewrites relative URLs so they are correct
// relative to parent_url's directory.
std::string ResolveUrlsInCss(std::string_view css, std::string_view import_url,
                             std::string_view parent_url);

// Callback: given a URL, returns CSS content if cached, nullopt if not.
using CssLookupFn =
    std::function<std::optional<std::string>(std::string_view url)>;

// Result of flattening @import statements.
struct FlattenResult {
  std::string css;           // Flattened CSS
  int imports_resolved = 0;  // Number of @imports replaced
  // At least 1 when flattening was skipped because an @import could not
  // be inlined (skipped_unresolved_import below); always 0 on success —
  // flattening is all-or-nothing.
  int imports_unresolved = 0;
  // True when flattening was skipped for the whole sheet because an
  // @import (at any depth) carries a non-media condition (cascade layer
  // or supports()).  css is the input, unchanged.
  bool skipped_non_media_condition = false;
  // True when flattening was skipped for the whole sheet because an
  // @import (at any depth) could not be inlined: not in cache, depth
  // limit reached, unsafe media value, output size cap, a structurally
  // unbalanced child (unterminated comment or string, unbalanced
  // braces), or an @import form the parser cannot process.  css is the
  // input, unchanged.
  bool skipped_unresolved_import = false;
};

// True when `media` can be written as the prelude of `@media <media> { ... }`
// without changing what the block contains: no braces, semicolons,
// backslash, `@`, angle brackets, quotes or comment opener, and balanced
// parentheses. Every valid media query list passes.
bool IsSafeMediaValue(std::string_view media);

// The media query a stylesheet's rules are scoped to when the sheet is
// appended to a combined stylesheet, for the stylesheet media `media` (the
// `media` attribute of its <link> or <style>):
//   - "" when the rules apply on every screen as written: no media, `all` or
//     `screen` (any case, surrounding whitespace ignored);
//   - the trimmed media query list otherwise (`print`, `(max-width: 600px)`),
//     which AppendMediaScopedCss writes as an @media block, so a rule from a
//     print sheet stays a print rule in the combined sheet;
//   - nullopt when the value cannot be written as an @media prelude safely
//     (IsSafeMediaValue). No valid media query list is refused. Such a sheet
//     belongs in no combined sheet, and a caller treats it as missing.
std::optional<std::string_view> CombinedSheetMediaScope(std::string_view media);

// The size AppendMediaScopedCss adds for `css_size` bytes under `scope`.
size_t MediaScopedCssSize(std::string_view scope, size_t css_size);

// True when `css` can be embedded in another sheet without changing how the
// surrounding rules parse: comments and strings are terminated, braces are
// balanced (never negative, zero at the end), and a trailing backslash does
// not escape the closing brace of a wrapper. Braces inside comments, strings,
// escapes and unquoted url() values do not count. Only such a sheet can be
// written inside an @media block without a stray `}` closing the block early,
// which would let the rules after it apply outside the media.
bool IsStructurallyBalanced(std::string_view css);

// Appends `css` to `*out`: as written when `scope` is empty, else as
// `@media <scope>{<css>}`. `scope` comes from CombinedSheetMediaScope.
// Returns false, appending nothing, when `scope` is not empty and `css`
// cannot be wrapped safely (IsStructurallyBalanced); a caller treats that sheet
// as missing.
bool AppendMediaScopedCss(std::string_view scope, std::string_view css,
                          std::string* out);

// Flatten @import statements by reading imported CSS from cache.
// lookup_fn: given a URL, returns CSS content if cached, nullopt if not.
// max_depth: recursion limit (default 5).
FlattenResult FlattenImports(std::string_view css, std::string_view css_url,
                             CssLookupFn lookup_fn, int max_depth = 5);

}  // namespace css
}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CSS_CSS_IMPORT_FLATTENER_H_
