// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_RULE_PACK_FILTER_H_
#define PAGESPEED_KERNEL_PACKS_RULE_PACK_FILTER_H_

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/matcher.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/page_facts.h"
#include "pagespeed/kernel/packs/planner.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {

struct RulePackFilterOptions {
  // Bound on the bytes the pack may add to one page.
  size_t max_added_bytes = kMaxAddedBytesPerPage;
};

// Applies the rules of a rule pack to an HTML document as it streams through
// an HtmlParse, one flush window at a time.
//
// Decision points. Each kind decides once per page, at its decision point
// (RuleKind::decision_point()). The head kinds (canonical, title,
// description, hreflang) decide when the head closes: the first of </head> (written or
// implied by the parser), <body>, and the end of the document. Everything
// observed by then is in view, including elements between the logical end of
// the head (the first element that cannot be head content) and </head>, which
// count as outside the head. Inserts go before the logical end of the head,
// else at the end of <head>.
//
// The kinds that decide at the end of the document (jsonld) wait for
// EndDocument: they see every element of their kind, in the head or the
// body, and the head values the document ends with. A new element is
// appended to <body> before </body> while the body is still in the current
// flush window, else to <html>, else the plan is dropped (not_rewritable).
// Their removals and rewrites go through the element's own events, which only
// exist while the element is in the current window.
//
// Atomicity. Before changing anything at a decision point the filter checks
// every node an enforcing op touches: the target (and its text) must still be
// in the current flush window, and an insert needs its anchor there. If one
// check fails the whole plan of that point is dropped: nothing changes, and
// the decisions of the rules that would have acted say `not_rewritable`. So a
// head split by a flush still gets the elements it lacks (the anchor is the
// current event or the still-open <head>), while an edit of an element the
// server already sent fails closed.
//
// Late elements. An element of a kind first seen after the kind decided (a
// canonical link in <body>, say) is handed to RuleKind::OnLateElement(),
// which removes it where the ladder would have (repair, replace, exact
// duplicates under keep) and, under keep, when the pack inserted the kind on
// this page: a second value would make the page worse than the input. The
// decisions are recorded at the end of the document.
//
// Response headers. A canonical rule with `link_header: match` also keeps
// the `Link: <url>; rel="canonical"` response header in line with the
// canonical the document ends up with. That is decided with the head and
// needs the headers to be writable: once the first flush window has gone out
// the action is skipped and recorded as `headers_flushed`. The headers are
// read (to report a conflicting origin `Link`) in report mode too, but only
// an enforcing rule changes them.
//
// Several packs on one host. The host runs one filter per pack, in load
// order, over the same document. When the filter selects a rule for the page
// (DetermineEnabled) it claims the rule's target through PackHost::Claim():
// the rule's kind and, for a `link_header: match` rule, the `Link` header. A
// target an earlier pack already holds is not this filter's on the page: the
// rule is dropped from the selection and records action `none`, reason
// `claimed` and the holder's id, whatever the modes, and a claim stays held
// even when the holder's plan is later dropped. A host that runs one pack
// never answers a claim with another pack, so it never sees `claimed`.
//
// A rule changes the HTML only when its effective mode is `enforce` (the
// lowest of the directive mode, the matching site's mode and the rule's own
// opt-in). Otherwise the filter records what the rule would do and leaves the
// document byte-identical to the same chain without the filter.
//
// The filter holds the pack alive for the duration of the parse. Its output
// depends only on the page URL, the pack and the HTML. Every failure leaves
// the page as the origin sent it.
class RulePackFilter : public net_instaweb::EmptyHtmlFilter {
 public:
  // `pack` must not be null: a server without a usable pack does not build the
  // filter. `host` must outlive the filter. The page URL is read from the parser for every document, so
  // one filter serves a parser that is reused.
  RulePackFilter(net_instaweb::HtmlParse* parser,
                 std::shared_ptr<const Pack> pack, Mode directive_mode,
                 PackHost* host, RulePackFilterOptions options = {});
  ~RulePackFilter() override;

  // Off for a URL the pack cannot apply to: an unparsable URL, a host the
  // pack does not list ("rule_packs: host not listed"), or a path no rule
  // matches.
  void DetermineEnabled(GoogleString* disabled_reason) override;
  void StartDocument() override;
  void StartElement(net_instaweb::HtmlElement* element) override;
  void EndElement(net_instaweb::HtmlElement* element) override;
  void Characters(net_instaweb::HtmlCharactersNode* characters) override;
  // Called at the end of every flush window; the first call means the
  // response headers have been sent.
  void Flush() override;
  void EndDocument() override;
  // True when an enforcing rule may change a URL in the document.
  bool CanModifyUrls() override { return can_modify_urls_; }
  ScriptUsage GetScriptUsage() const override {
    return may_inject_scripts_ ? kMayInjectScripts : kNeverInjectsScripts;
  }
  const char* Name() const override { return "RulePackFilter"; }

  // What happened to the last document.
  //
  // True when the filter changed the document.
  bool modified() const { return modified_; }
  // True when a rule has something to do on the page that it did not do
  // because it only reports.
  bool would_modify() const { return would_modify_; }
  // True when the filter changed the response headers (the `Link` header of
  // a rule with `link_header: match`).
  bool headers_modified() const { return headers_modified_; }
  // Why the page (or its head) was left alone as a whole; kNone when it was
  // evaluated and changed as planned.
  SkipReason skip_reason() const { return skip_reason_; }
  // One decision per applicable rule (and one more for a rule that removed
  // late elements after inserting), in application order.
  const std::vector<PackDecision>& decisions() const { return decisions_; }
  const PageUrl& url() const { return url_; }
  // The first flush window has been sent.
  bool headers_flushed() const { return headers_flushed_; }

 private:
  struct LateState {
    bool seen = false;         // an element of the kind came late
    bool acted = false;        // OnLateElement asked for a change
    bool conflict = false;     // ... because it contradicted the decision
    bool unremovable = false;  // a change could not be made
    std::string before;        // norm of the first late element acted on
  };

  void BeginPage();
  // Claims the targets of the selected rules (see the class comment); a rule
  // whose target another pack holds leaves the selection and gets its
  // `claimed` decision.
  void TakeClaims();
  void EndLogicalHead(net_instaweb::HtmlElement* ender);
  void DecideHead();
  void DecideDocumentEnd();
  bool InsertAnchorAvailable() const;
  // Where a kind that decides at the end of the document appends its element:
  // <body> while its end is still in the current window, else <html>, else
  // null.
  net_instaweb::HtmlElement* DocumentAnchor() const;
  bool Rewritable(const PagePlan& plan, DecisionPoint point) const;
  void Apply(const PagePlan& plan, DecisionPoint point);
  // The `Link: rel=canonical` header of the rules that ask for it, decided
  // together with the head. Never touches the document.
  void ApplyHeaders(const PagePlan& plan);
  void ApplyOp(const RulePlan& rule_plan, const PlanOp& op,
               DecisionPoint point);
  void InsertAtHeadEnd(net_instaweb::HtmlNode* node);
  // Puts `node` where the inserts of `point` go.
  void InsertAt(DecisionPoint point, net_instaweb::HtmlNode* node);
  void AddDebugComment(const RulePlan& rule_plan,
                       net_instaweb::HtmlElement* before, DecisionPoint point);
  // The text of a debug comment: what a rule did (`action=`) or, in report
  // mode, would do (`would=`), and its effective mode.
  std::string DebugText(const RulePlan& rule_plan, bool would,
                        Action action) const;
  // Report mode with debug comments on: a `would=` comment where each
  // report-only rule of `plan` would have changed the document, where the
  // document can still take one.
  void AnnotateReport(const PagePlan& plan, DecisionPoint point);
  void SetEscaped(net_instaweb::HtmlElement* element,
                  net_instaweb::HtmlName::Keyword keyword,
                  const std::string& escaped);
  void HandleLate(const RuleKind* kind, size_t fact_index);
  void FinishTextElement();
  void FinalizeHeadDecisions();
  void FinalizeDocumentDecisions();
  void Report();

  net_instaweb::HtmlParse* parser_;
  std::shared_ptr<const Pack> pack_;
  Mode directive_mode_;
  PackHost* host_;
  RulePackFilterOptions options_;
  bool can_modify_urls_ = false;
  bool may_inject_scripts_ = false;

  // Per document.
  bool active_ = false;
  PageUrl url_;
  Selection selection_;
  // A rule that decides at the end of the document applies on this page, so
  // the head kinds are observed even without a rule of their own.
  bool needs_head_values_ = false;
  PageFacts facts_;
  std::string base_;
  std::vector<net_instaweb::HtmlElement*> inert_;
  net_instaweb::HtmlElement* open_head_ = nullptr;
  // The element whose text is being collected, and where its fact is.
  net_instaweb::HtmlElement* text_element_ = nullptr;
  const RuleKind* text_kind_ = nullptr;
  size_t text_fact_ = 0;
  std::string text_raw_;

  int window_ = 0;
  int insert_before_window_ = -1;
  bool headers_flushed_ = false;

  bool head_decided_ = false;
  bool head_dropped_ = false;
  PagePlan head_plan_;
  // The kinds that decide at the end of the document.
  bool document_decided_ = false;
  bool document_dropped_ = false;
  PagePlan document_plan_;
  net_instaweb::HtmlElement* document_anchor_ = nullptr;
  // The rules that already have a debug comment (one per rule, however many
  // elements it writes).
  std::set<const RulePlan*> commented_;
  std::array<const RulePlan*, kKindCount> plan_for_{};
  std::array<bool, kKindCount> inserted_{};
  std::array<LateState, kKindCount> late_{};
  std::array<NormCache, kKindCount> norms_{};

  // The decision about a rule's response header, by kind: logged after the
  // rule's own decisions.
  std::array<std::optional<PackDecision>, kKindCount> header_decision_{};
  // The `claimed` decisions of the selected rules whose kind another pack
  // holds, logged after every other decision of the page.
  std::vector<PackDecision> claimed_;
  // By kind: the pack that holds the `Link` header of this pack's rule;
  // empty when the rule holds it (or has no header action).
  std::array<std::string, kKindCount> header_claimed_by_{};
  // Every selected rule was claimed: nothing to observe, only the claimed
  // decisions to log when the document ends.
  bool claimed_only_ = false;

  bool modified_ = false;
  bool headers_modified_ = false;
  // The host had response headers when the document began.
  bool has_headers_ = false;
  bool would_modify_ = false;
  SkipReason skip_reason_ = SkipReason::kNone;
  std::vector<PackDecision> decisions_;
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_RULE_PACK_FILTER_H_
