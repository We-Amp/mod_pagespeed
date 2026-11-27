// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - <noscript> removal for the browser-analysis renders
//
// The coverage render (BrowserCssExtractor) and the critical-CSS validation
// render (VisualRegressionGate) run with script execution disabled, an SSRF
// and determinism defence that stays. With scripting off, Blink parses
// <noscript> content as markup and renders it: a no-JS banner takes the top of
// the fold, a <noscript><style> applies, and the fold those renders measure is
// the one a no-JS client sees. The optimized page is served to
// clients running scripts, which parse <noscript> content as raw text that
// never renders. Removing the <noscript> elements, contents and all, from the
// documents those two renders load makes them render what a scripting browser
// renders, without running a single script. The renders that DO run scripts
// (page analysis, script coverage, the agent render) already parse <noscript>
// as raw text and get the page unchanged.
//
// WHICH <noscript> IS ONE, AND WHERE IT ENDS, is decided the way the HTML
// parser decides it with the scripting flag set, not by searching for the
// string: a tokenizer that follows the spec's states for tags and attributes
// (a quote opens a value only right after `=`), comments (`<!-->`, `<!--->`,
// `--!>`), raw text and RCDATA (script, style, textarea, title, xmp, iframe,
// noembed, noframes; plaintext to the end), script data with its escaped and
// double-escaped states, and CDATA in foreign content; and a light tree model
// for what the tokenizer cannot know alone: inline <svg>/<math> (where
// <noscript> is a foreign element that renders the same either way, and
// title/style/script are not raw text), the breakout tags that leave foreign
// content, the integration points that are HTML again (foreignObject, desc,
// title; annotation-xml with an HTML encoding; mi, mo, mn, ms, mtext), and the
// end tags that close an <svg> implicitly, which follow the in-body end tag
// rules: scopes, special elements (`</span>` across an open <div> is
// ignored), headings (`</h1>` closes an open <h2>), </form>, and the adoption
// agency algorithm's effect for formatting elements; and the
// start tags that close open elements: "close a p element" (not for
// <table> without a doctype), <a> and <nobr> in an open one, <button> in an
// open one, and a second <form> while the form element pointer is set.
// A <noscript> in HTML content is raw
// text up to the first `</noscript` followed by whitespace, `/` or `>`: a
// nested one ends at the first end tag, and the end tag is matched
// case-insensitively. Every byte outside a removed element is kept.
//
// FAIL CLOSED. A removed range that runs to the end of the input with markup
// after the start tag (an unclosed <noscript>, or a desync this tokenizer did
// not foresee) could take the rest of the page with it, and two blank renders
// compare equal. `reliable` is then false, and so it is when the tree model
// gives up (nesting too deep). Callers must not render such a document: the
// validator refuses, and the coverage render's result is discarded. A
// genuinely unclosed <noscript> blanks the page for scripting clients too, so
// nothing is lost. As a second, independent check, `rendered_tags` lists the
// start tags left that HtmlScanner would collect into `elements`, and
// NoscriptStripAgreesWithScan compares the two element by element where the
// fold is, not just by count.
//
// Only for documents a browser renders with script execution disabled. Never
// apply it to the markup the scanner reads to assemble the combined
// stylesheet: the loadCSS rule needs the <noscript> twin, and
// the validation hash is computed over what that assembly produces.

#ifndef PAGESPEED_SRC_WORKER_NOSCRIPT_STRIP_H_
#define PAGESPEED_SRC_WORKER_NOSCRIPT_STRIP_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/worker/html_scanner.h"

namespace pagespeed {

struct NoscriptStripResult {
  // The document without its <noscript> elements.
  std::string html;
  // Number of <noscript> elements removed.
  size_t removed = 0;
  // The removed byte ranges of the input, [begin, end), in order.
  std::vector<std::pair<size_t, size_t>> removed_ranges;
  // False when `html` may not be what a scripting browser parses, and must
  // not be rendered (see FAIL CLOSED above). `unreliable_reason` says why.
  bool reliable = true;
  std::string unreliable_reason;
  // Start tags left in `html` that HtmlScanner would collect into its
  // `elements`: not inside <template> content or a foreign <noscript>, not a
  // <template>, <noembed> or <noframes> itself, and not one the worker
  // injected (data-pagespeed-critical, -hint, -async-fallback, -async-loader).
  size_t rendered_start_tags = 0;
  // Their tag names, lowercase, in document order.
  std::vector<std::string> rendered_tags;

  // Where offset `pos` of the input lands in `html`. An offset inside a
  // removed range lands where that range stood.
  [[nodiscard]] size_t MapOffset(size_t pos) const;
};

// Removes every <noscript> element, contents and all, as described above.
NoscriptStripResult StripNoscriptElements(std::string_view html);

// How many elements after <body> must match exactly in the cross-check.
inline constexpr size_t kExactElementsAfterBody = 200;
// The longest run of elements on one side only that the cross-check still
// takes for an isolated difference between the two readings.
inline constexpr size_t kMaxIsolatedRun = 2;
// A removal that went wrong only deletes: elements the scanner has and the
// removal lacks may number at most kMaxDeletionRun per run and kMaxDeletions
// in all, counted gross: an element read as another counts as
// a deletion too.
inline constexpr size_t kMaxDeletionRun = 1;
inline constexpr size_t kMaxDeletions = 3;

// The cross-check: HtmlScanner, an independent reading of the same input by
// the HtmlLexer, collected `scanned` (its `elements`, which leave out what a
// scripting browser does not render). From the first <body> on, the first
// kExactElementsAfterBody elements the removal kept must be the same tag
// names in the same order (zero tolerance: that is where the fold usually
// is). Across the whole document the two lists are aligned by a shortest
// edit script: a run of more than kMaxIsolatedRun elements on one side only
// between matching ones (what a removal that went wrong leaves, wherever the
// fold is) is a disagreement, and the edits may number at most
// 2 * max(3, 1%) (an element read as another counts two). Deletions
// (elements the scanner has and the removal lacks, counted gross, so an
// element read as another is one) are held to a stricter budget:
// kMaxDeletionRun per run, kMaxDeletions in all, since a removal that went
// wrong only ever deletes. When they disagree, one of the two readings has gone
// wrong and the stripped document must not be rendered.
bool NoscriptStripAgreesWithScan(const NoscriptStripResult& result,
                                 const std::vector<CollectedElement>& scanned);

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_NOSCRIPT_STRIP_H_
