// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PLANNER_COMMON_H_
#define PAGESPEED_KERNEL_PACKS_PLANNER_COMMON_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "absl/types/span.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/rule_kind.h"
#include "pagespeed/kernel/packs/template.h"

namespace net_instaweb {
class HtmlElement;
}  // namespace net_instaweb

// The page-time types shared by the kinds, the planner and the filter, and the
// small pure helpers the kinds use. Nothing here depends on the kind registry,
// so the kind handlers can use it.

namespace pagespeed::packs {

// Why a page (or one decision point of it) was left alone as a whole.
enum class SkipReason : std::uint8_t {
  kNone,
  kHostNotListed,
  kNoHead,
  kMalformedHead,
  kNotRewritable,
  kSizeLimit,
};
std::string_view SkipReasonName(SkipReason reason);

enum class OpType : std::uint8_t {
  kInsert,          // add a new element where the kind's insert anchor is
  kRemove,          // delete the element
  kRemoveRelToken,  // keep the element, write `value` as its rel attribute
  kSetValue,        // set `attr` (or the text, see PlanOp::attr) to `value`
};

struct PlanOp {
  OpType type = OpType::kInsert;
  const RuleKind* kind = nullptr;
  ElementFact target;  // the existing element (not for kInsert)
  // kInsert, kSetValue: the attribute value (canonical, description, the href
  // of an hreflang link), the already HTML-escaped text (title) or the script
  // text (jsonld). kRemoveRelToken: the rel tokens that remain.
  std::string value;
  // hreflang, kInsert: the (normalized) language code of the new link.
  std::string code;
  // kInsert, kSetValue: the attribute that carries `value`; kNotAKeyword
  // means `value` is the element's text content.
  net_instaweb::HtmlName::Keyword attr = net_instaweb::HtmlName::kNotAKeyword;
};

// One rule's plan for one page.
struct RulePlan {
  const Rule* rule = nullptr;
  PackDecision decision;
  std::vector<PlanOp> ops;  // empty when the decision is a no-op
  // False when the kind recorded nothing for the rule.
  bool planned = true;
  // The rule had a valid value for this page (and `target_norm` is how that
  // value compares).
  bool has_target = false;
  std::string target_norm;
  // The canonical URL the document ends up with after this plan (url kinds):
  // the rule's value when the plan inserts or sets it, else the one usable
  // element that stays. Empty when there is none or when several differ. How
  // it compares (`final_norm`) and the absolute URL (`final_url`).
  std::string final_norm;
  std::string final_url;
};

// What Plan() gets: the rule, the page and the facts of the rule's kind.
// Every lookup the planner needs is done by the caller, so the kinds stay
// pure functions of their input.
struct PlanInput {
  const Pack* pack = nullptr;
  const Rule* rule = nullptr;
  PageUrl url;
  std::vector<std::string> captures;  // captures[i] = path_regex group i+1
  // The rule's kind's elements, in document order.
  const std::vector<ElementFact>* facts = nullptr;
  // What relative URLs in the page resolve against: <base href> when it is
  // usable, else the page URL.
  std::string base;
  // For a table-valued rule: the table entry for this page, or null on a
  // miss (the fallback template then applies).
  const std::string* table_value = nullptr;
  // For an hreflang rule over the hreflang table: the cluster that contains
  // the page, or null on a miss.
  const HreflangCluster* cluster = nullptr;
  // The values the document ends with for the kinds that have a head_slot(),
  // for kinds that decide at the end of the document (a jsonld template
  // quotes them). Empty when the page has none.
  std::string final_canonical;
  std::string final_title;
  std::string final_description;
};

// The norms of the elements of one kind seen so far, for the late-element
// duplicate check. A kind computes the norm of each element once, the first
// time it needs it: `folded` elements of PlanState::earlier are in `norms`.
// The norm depends on the page's base, which is fixed only at the decision
// point, so it cannot be computed when the element is observed.
struct NormCache {
  std::unordered_set<std::string> norms;
  size_t folded = 0;
};

// What OnLateElement() gets.
struct PlanState {
  // The decision made at the decision point.
  const RulePlan* plan = nullptr;
  // The plan inserted an element of the kind (or would have, in report mode).
  bool inserted = false;
  std::string base;
  // Every element of the kind seen before the late one, in document order.
  absl::Span<const ElementFact> earlier;
  // The caller's cache over `earlier`, kept across the late elements of one
  // kind on one page so each element is normalized once. May be null; the
  // kind then compares against `earlier` directly.
  NormCache* norms = nullptr;
};

// What Observe() gets: one element of the main document.
struct ElementView {
  net_instaweb::HtmlElement* element = nullptr;
  net_instaweb::HtmlName::Keyword keyword =
      net_instaweb::HtmlName::kNotAKeyword;
  std::string_view lower_name;  // the tag name, lowercased
  bool in_head = false;         // inside the logical <head>
};

// Escapes `s` for use as an attribute value (inside double quotes): & < > "
// and ' only, so UTF-8 passes through untouched.
std::string EscapeAttributeValue(std::string_view s);

// Whitespace-collapses (ASCII whitespace runs become one space, ends
// trimmed).
std::string CollapseWhitespace(std::string_view s);

// Resolves `ref` against the absolute URL `base` (RFC 3986, without
// normalization beyond dot segments). Returns `ref` unchanged when it is
// already absolute, and an empty string when `base` is not absolute.
std::string ResolveUrl(std::string_view base, std::string_view ref);

// `s` with every byte that is not part of a well-formed UTF-8 sequence
// replaced by U+FFFD. For page-written text that goes into a decision (the
// log lines are UTF-8).
std::string ReplaceInvalidUtf8(std::string_view s);

// Decodes an attribute value as written: numeric character references
// become UTF-8, named ones go through SafeDecodeHtmlEntities(). Used only to
// compare what the page says with what a rule says.
std::string DecodeAttribute(std::string_view escaped);

// Decoded value of an attribute, tolerant of non-ASCII bytes and entities.
// `present` is false when the attribute is absent or has no value.
std::string AttrValue(const net_instaweb::HtmlElement* element,
                      net_instaweb::HtmlName::Keyword keyword,
                      bool* present = nullptr);

// What a rule that works on several elements at once (hreflang, jsonld)
// did, boiled down to the one decision it logs.
struct OutcomeFlags {
  bool has_unusable = false;
  Reason unusable_reason = Reason::kUnusable;
  std::string unusable_defect;
  bool conflict = false;
  bool self_missing = false;
  std::string self_defect;
  bool set = false;
  bool inserted = false;
  bool duplicate = false;
  bool removed = false;
  // The defect to report when the rule acts because the kind is absent.
  std::string absent_defect;
  bool had_any = false;
  // Reported when nothing happens.
  Reason idle_reason = Reason::kPresent;
  std::string idle_defect;
};

// Writes the one decision of a multi-element rule from `flags`.
void SummarizeOutcome(RulePlan* plan, const OutcomeFlags& flags);

// Sets the decision's action, reason and defect.
void Finish(RulePlan* plan, Action action, Reason reason, std::string defect);

// Appends "item (reason)" to the decision's list of removed things.
void NoteRemoved(RulePlan* plan, std::string_view item,
                 std::string_view reason);

// Cuts the removed list to kMaxAfterBytes at an item boundary and says how
// many items did not fit: "a (x); b (y); ...(+3)". `total` is the number of
// items that were noted.
void CapRemoved(RulePlan* plan, size_t total);

// Bytes the plan adds to the page (inserts and value changes), with `extra`
// added per change (room for a debug comment).
size_t AddedBytes(const RulePlan& plan, size_t extra);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PLANNER_COMMON_H_
