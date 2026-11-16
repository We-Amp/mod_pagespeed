// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_MARKDOWN_EXTRACTOR_FILTER_H_
#define PAGESPEED_LIB_HTML_MARKDOWN_EXTRACTOR_FILTER_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "lib/html/empty_html_filter.h"

namespace net_instaweb {

class HtmlElement;
class HtmlCharactersNode;
class HtmlParse;

// First-cut SAX HTML -> markdown extractor for the agent_optimize render.
// PURE: it consumes the SAX event stream of the RENDERED
// outerHTML and accumulates markdown, maintaining its OWN explicit block / list /
// table / skip stacks — never a tree query (the §4 SAX invariant).
//
// SECURITY: the input is fully attacker-controlled (a hostile page's rendered DOM)
// and the output is consumed by an AI agent. The extractor is FAIL-SAFE and
// self-bounded: it caps its own output size + nesting depth (does not trust the
// caller's input cap), it ESCAPES emitted text/href/src/alt so a page cannot forge
// markdown STRUCTURE (headings, links, fenced code) into the agent feed, and the
// <main>/<article> (and role=main/article) content kill-guard is authoritative over
// the chrome strip. NOTE — agent-visible content is NOT guaranteed equal to
// human-visible content in either direction: class/stylesheet-driven hiding is
// invisible to a SAX pass (no computed style), so downstream consumers MUST treat
// the extracted markdown as untrusted, attacker-influenced input. Readability
// main-content node-scoring, class/id exclude vocabulary, front-matter, and the
// content-hash binding are P2.
//
// The filter NEVER fetches: <img> emits ![alt](src) from the element's own
// attributes (the kDenyKeepElement contract, lib/net/fetch_policy.h).
class MarkdownExtractorFilter : public EmptyHtmlFilter {
 public:
  // strip_soft_chrome: when true (default), nav/header/footer subtrees are dropped
  // as boilerplate. ExtractAgentMarkdown re-runs with this false (fail-open) if the
  // first pass dropped a <main>/<article> content island or produced nothing, so a
  // page that lays content inside chrome still yields content-complete markdown.
  // content_root_only: when true, ONLY text inside a <main>/<article>/role=main
  // content-root subtree is emitted — bare-<body> chrome (skip-links, promo
  // banners) that the nav/header/footer denylist misses is dropped (Issue F (4)).
  // The entry point only enables this when a content root is known to exist, so
  // landmark-free pages keep the fail-open two-pass behavior.
  explicit MarkdownExtractorFilter(bool strip_soft_chrome = true,
                                   bool content_root_only = false);
  ~MarkdownExtractorFilter() override;

  const char* Name() const override { return "MarkdownExtractor"; }

  void StartDocument() override;
  void EndDocument() override;
  void StartElement(HtmlElement* element) override;
  void EndElement(HtmlElement* element) override;
  void Characters(HtmlCharactersNode* characters) override;

  // The parser that feeds this filter. When it has switched to passing the
  // rest of a page through (a page nested past the depth limit, or past the
  // page limit), the text it still delivers is raw markup, not parsed content:
  // it is dropped and the copy is marked truncated. The caller must flush the
  // parser often enough (see RunExtractor) for the switch to be seen close to
  // where it happens.
  void SetParser(const HtmlParse* parser) { parser_ = parser; }

  // Page metadata captured by a separate ExtractHtmlMetadata pass over the same
  // HTML. When non-empty, EndDocument prepends a YAML front-matter block (Issue
  // F (2)) ahead of the body markdown. `date` is captured by THIS filter from the
  // first <time datetime> (set during parse), not by the metadata pass.
  void SetFrontMatter(std::string_view title, std::string_view description,
                      std::string_view canonical) {
    fm_title_ = std::string(title);
    fm_description_ = std::string(description);
    fm_canonical_ = std::string(canonical);
  }

  // The accumulated markdown (valid after FinishParse / EndDocument).
  const std::string& markdown() const { return markdown_; }
  // True iff a <main>/<article> (or role=main/article) content root was dropped
  // because it sat inside a soft-chrome (nav/header/footer) strip — the signal for
  // ExtractAgentMarkdown to fail open and re-extract without soft-chrome stripping.
  bool dropped_content_root() const { return dropped_content_root_; }
  bool truncated() const { return truncated_; }

 private:
  struct ListCtx {
    bool ordered;
    int counter;
  };

  // Self-contained safety bounds (do NOT trust the caller's input cap).
  static constexpr std::size_t kMaxMarkdownBytes = 8u
                                                   << 20;  // 8 MiB output cap
  static constexpr int kMaxListIndentLevels = 16;  // emission indent clamp
  static constexpr int kMaxBlockquoteDepth = 8;    // emission prefix clamp

  std::string* Out();
  void AppendText(std::string_view text);
  void FlushBlock();
  void AppendBlock(const std::string& body);
  void AppendListItem(const std::string& body);
  bool AtOutputCap();
  bool Skipping() const {
    return hard_skip_depth_ > 0 || soft_skip_depth_ > 0 || time_depth_ > 0;
  }
  // content_root_only gate: outside any content root, suppress emission.
  bool OutsideContentRoot() const {
    return content_root_only_ && content_root_depth_ == 0;
  }
  // True while a block element inside an open <a> must be flattened inline
  // instead of starting/ending a markdown block (Issue F (5)).
  bool InAnchorBlock() const { return anchor_depth_ > 0 && pre_depth_ == 0; }
  // Insert the ": " segment joiner before a new flattened block inside an anchor.
  void AnchorBlockJoin();
  // True while a block child of an open <li> must be flattened into the item
  // line instead of starting its own block (icon-badge / card feature lists).
  // Disabled inside <pre> and inside an open <a> (the anchor flatten owns that).
  bool InListItemBlock() const {
    return item_open_ && pre_depth_ == 0 && anchor_depth_ == 0;
  }
  // Separate a flattened block child from the prior item content by a single
  // space (trimming any inter-block whitespace first) so the item reads as one
  // citable line regardless of source formatting.
  void ListItemBlockJoin();
  // Prepend the YAML front-matter block (if any field is set) to markdown_.
  void PrependFrontMatter();

  std::string markdown_;  // final output
  std::string cur_;       // current block's inline buffer
  std::string cell_;      // current table-cell inline buffer
  std::string code_buf_;  // current inline <code> span (for adaptive fencing)
  std::string pre_language_;  // outermost <pre data-language> label (fence tag)
  std::string table_;         // current table accumulation
  std::vector<ListCtx> lists_;
  std::vector<std::string> row_cells_;
  // Front-matter (Issue F (2)): title/description/canonical from the metadata
  // pass; date from the first <time datetime> seen during this parse.
  std::string fm_title_;
  std::string fm_description_;
  std::string fm_canonical_;
  const HtmlParse* parser_ = nullptr;
  std::string fm_date_;
  // Block-in-anchor (Issue F (5)): byte offset in cur_ where the open anchor's
  // link text begins, so a wrapped <h2>/<p> flattens to one '[text](href)'.
  std::size_t anchor_text_start_ = 0;
  int blockquote_depth_ = 0;
  int pre_depth_ = 0;
  int heading_level_ = 0;
  int hard_skip_depth_ =
      0;  // inside script/style/noscript/template/head/svg/hidden/<time>
  int soft_skip_depth_ = 0;  // inside nav/header/footer (soft chrome)
  int time_depth_ = 0;       // inside <time> (text suppressed; datetime kept)
  int content_root_depth_ =
      0;                   // depth inside a <main>/<article>/role=main root
  int article_depth_ = 0;  // depth inside an <article>/role=article subtree
  // Per open <a>: the output offset of its '[' and whether any visible text or
  // image was emitted inside it, so a link with none can be dropped at </a>
  // instead of emitting '[](href)'.
  struct OpenAnchor {
    std::size_t pos;
    bool has_text;
  };
  std::vector<OpenAnchor> anchor_open_pos_;
  void MarkAnchorText() {
    for (OpenAnchor& a : anchor_open_pos_) a.has_text = true;
  }
  int anchor_depth_ = 0;  // open <a> nesting (Issue F (5))
  bool item_open_ = false;
  bool in_table_ = false;
  bool in_cell_ = false;
  bool in_inline_code_ = false;
  bool in_dt_ = false;  // inside a <dt> (definition term -> bold block)
  bool table_header_done_ = false;
  bool last_was_list_item_ = false;
  bool truncated_ = false;
  bool dropped_content_root_ = false;
  bool strip_soft_chrome_ = true;
  bool content_root_only_ = false;
};

// Pure entry point: parse `html` (the rendered outerHTML) and return clean
// markdown. `url` only satisfies the parser (relative-link resolution); the
// extractor emits sanitized href/src verbatim-ish. Two-pass fail-open: if the
// boilerplate-stripped pass drops a content root or yields nothing, re-extract
// keeping chrome. Returns "" on an unusable url / empty input.
std::string ExtractAgentMarkdown(std::string_view html, std::string_view url);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_MARKDOWN_EXTRACTOR_FILTER_H_
