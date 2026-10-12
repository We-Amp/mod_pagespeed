// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_RULE_KIND_H_
#define PAGESPEED_KERNEL_PACKS_RULE_KIND_H_

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/packs/pack.h"

namespace net_instaweb {
class HtmlElement;
class HtmlNode;
class HtmlParse;
}  // namespace net_instaweb

namespace pagespeed::packs {

// The page-time types live in planner_common.h; the interface only needs
// their names.
struct ElementFact;
struct ElementView;
struct PlanInput;
struct PlanOp;
struct PlanState;
struct RulePlan;

// Where in the document a kind's rules are decided.
enum class DecisionPoint {
  kHeadEnd,      // when the head closes
  kDocumentEnd,  // when the document ends
};

class RuleKind;

// The page-wide value of a head kind that a later kind may quote in its own
// value (the {canonical}, {title} and {description} placeholders of a jsonld
// template). kNone for every other kind.
enum class HeadSlot : std::uint8_t {
  kNone,
  kCanonical,
  kTitle,
  kDescription,
};

// What a kind handler may look at while the loader parses a rule's `value`
// object.
struct LoadContext {
  // The pack so far. The tables are complete when rules are parsed; the rules
  // are not.
  const Pack* pack = nullptr;
  // The rule being loaded: its kind, id and match scope are filled in, its
  // value is the one being parsed.
  const Rule* rule = nullptr;
  // JSON path of the `value` object (`rules[2].value`); messages about a
  // table entry name it as the user of the entry.
  std::string value_path;
  // (table name, kind) pairs whose entries were already checked, so a table
  // shared by many rules is walked once. Owned by the loader.
  std::set<std::pair<std::string, const RuleKind*>>* checked_tables = nullptr;
};

// What happens to an element of a kind that first appears after the kind's
// decision point (a late element).
struct LateAction {
  enum Type : std::uint8_t {
    kLeave,           // no change
    kRemove,          // delete the element
    kRemoveRelToken,  // keep the element, drop this kind's rel token
  };
  Type type = kLeave;
  // The late element contradicts the value the decision left in the
  // document (as opposed to repeating it, or being unusable).
  bool conflict = false;
  // How the late element's value compares (for the decision's `before`
  // hash); empty when it has none.
  std::string norm;
};

// One rule kind. A kind is a stateless handler: a single static instance per
// kind lives in the registry (kind_registry.h), and Rule::kind points at it.
//
// Loading: ParseValue(). Page time: the filter calls Observe() for every
// element of the main document, Plan() once at the kind's decision point,
// OnLateElement() for each element the kind observes after that point, and
// BuildInsert() for each insert the plan applies.
class RuleKind {
 public:
  virtual ~RuleKind() = default;

  // The `kind` string in a pack file ("canonical").
  virtual std::string_view name() const = 0;
  virtual DecisionPoint decision_point() const = 0;
  // Prefix of the defect ids an SEO-defects checker uses for this kind; the
  // decision records carry them in `defect`.
  virtual std::string_view lens_defect_prefix() const = 0;

  // Loader hook: validates the kind-specific `value` object and compiles it
  // into `*out`. Errors are kInvalidArgument with the message
  // "<json path>: <reason>"; the loader adds the source name.
  virtual absl::Status ParseValue(const Json::Value& value,
                                  const LoadContext& context,
                                  RuleValue* out) const = 0;

  // Streaming: does `element` belong to this kind? If so, fills `*fact`
  // (value, rel tokens, the text mode) and returns true. Called for
  // main-document elements only; the filter keeps svg, math, template,
  // noscript, noembed and noframes subtrees away from the kinds.
  virtual bool Observe(const ElementView& element, ElementFact* fact) const = 0;

  // Pure planning at the decision point: what the rule does with the facts
  // observed so far. Never touches HTML. Leaves `out->planned` false when
  // the kind has nothing to say (no decision is recorded then).
  virtual void Plan(const PlanInput& input, RulePlan* out) const = 0;

  // An element of this kind first seen after the decision point. `state`
  // describes the decision that was made and the elements seen before.
  virtual LateAction OnLateElement(const PlanState& state,
                                   const ElementFact& late) const = 0;

  // Builds the node(s) an insert op creates, with `parent` as their parent.
  // A node in `*out` whose parent is an earlier node of `*out` is appended
  // to that node once it is in the event stream; the others are inserted, in
  // order, where the plan puts the kind.
  virtual void BuildInsert(net_instaweb::HtmlParse* parser,
                           net_instaweb::HtmlElement* parent, const PlanOp& op,
                           std::vector<net_instaweb::HtmlNode*>* out) const = 0;

  // The placeholder this kind's final value feeds, if any.
  virtual HeadSlot head_slot() const { return HeadSlot::kNone; }
  // The value the document ends with for this kind, as text a placeholder
  // quotes: the value `plan` writes (when it writes one), else the first
  // usable element in <head> that the plan does not remove. Empty when there
  // is none. `plan` may be null (nothing was, or will be, applied). Only
  // kinds with a head_slot() have a value.
  virtual std::string FinalValue(const RulePlan* plan,
                                 const std::vector<ElementFact>& facts,
                                 std::string_view base) const {
    return std::string();
  }

  // True when an enforcing rule of this kind with `on_present` may change a
  // URL in the document (the filter reports it through CanModifyUrls()).
  virtual bool ModifiesUrls(OnPresent on_present) const { return false; }
  // True when an enforcing rule of this kind may add a script element.
  virtual bool MayInjectScripts() const { return false; }
  // True when a rule of this kind may carry `link_header`.
  virtual bool SupportsLinkHeader() const { return false; }
};

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_RULE_KIND_H_
