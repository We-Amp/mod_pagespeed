// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/head_singleton_kind.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/entity_decode.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {
namespace {

// ---------------------------------------------------------------------------
// The kind's traits, as the ladder needs them.
// ---------------------------------------------------------------------------

struct Traits {
  const RuleKind* kind = nullptr;
  bool url = false;        // the value is a URL
  bool html_text = false;  // the value is HTML text content (escaped)
  net_instaweb::HtmlName::Keyword attr = net_instaweb::HtmlName::kNotAKeyword;
  size_t max_length = 0;
  bool unusable_defect = false;
  std::string prefix;  // defect id prefix
};

std::string MissingDefect(const Traits& k) {
  return absl::StrCat(k.prefix, "-missing");
}
std::string MultipleDefect(const Traits& k) {
  return absl::StrCat(k.prefix, "-multiple");
}
// Only some kinds have a defect for an unusable element; for the others the
// same situation is "missing".
std::string UnusableDefect(const Traits& k) {
  return k.unusable_defect ? absl::StrCat(k.prefix, "-unusable")
                           : MissingDefect(k);
}

// ---------------------------------------------------------------------------
// Target values
// ---------------------------------------------------------------------------

enum class TargetStatus : std::uint8_t { kOk, kNoValue, kInvalid };

struct Target {
  TargetStatus status = TargetStatus::kNoValue;
  // What gets written: the href / content, or for a title the HTML-escaped
  // text.
  std::string value;
  // What it compares as: NormUrl() for a URL, the whitespace-collapsed
  // decoded text otherwise.
  std::string norm;
  // The value as text, for the decision log.
  std::string logical;
};

std::string LogicalText(const Traits& k, std::string_view value) {
  if (k.html_text) return CollapseWhitespace(SafeDecodeHtmlEntities(value));
  return CollapseWhitespace(value);
}

Target ComputeTarget(const Traits& k, const PlanInput& in) {
  Target t;
  const Rule& rule = *in.rule;
  ExpandContext ex;
  ex.url = in.url;
  ex.captures = in.captures;
  const EscapeContext esc =
      k.html_text ? EscapeContext::kHtmlText : EscapeContext::kNone;
  // Text limits are in characters; a UTF-8 character is at most 4 bytes and
  // escaping can grow it further, hence the generous byte cap.
  const size_t max_bytes = k.url ? k.max_length : k.max_length * 8;

  std::string raw;
  switch (rule.value.source) {
    case ValueSource::kTemplate: {
      auto s = ExpandTemplate(rule.value.tpl, ex, esc, max_bytes);
      if (!s.ok()) {
        t.status = TargetStatus::kInvalid;
        return t;
      }
      raw = *std::move(s);
      break;
    }
    case ValueSource::kTable: {
      if (in.table_value != nullptr) {
        raw =
            k.html_text ? EscapeForHtmlText(*in.table_value) : *in.table_value;
      } else if (rule.value.fallback_tpl.has_value()) {
        auto s = ExpandTemplate(*rule.value.fallback_tpl, ex, esc, max_bytes);
        if (!s.ok()) {
          t.status = TargetStatus::kInvalid;
          return t;
        }
        raw = *std::move(s);
      } else {
        t.status = TargetStatus::kNoValue;
        return t;
      }
      break;
    }
    case ValueSource::kPattern:
      t.status = TargetStatus::kNoValue;
      return t;
  }

  if (k.url) {
    if (!CheckUrlValue(raw).ok()) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
    std::optional<std::string> n = NormUrl(raw);
    if (!n.has_value()) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
    t.norm = *std::move(n);
  } else {
    t.norm = LogicalText(k, raw);
    if (t.norm.empty() || HasControlChars(t.norm) ||
        Utf8Length(t.norm) > k.max_length) {
      t.status = TargetStatus::kInvalid;
      return t;
    }
  }
  t.logical = k.url ? raw : t.norm;
  t.value = std::move(raw);
  t.status = TargetStatus::kOk;
  return t;
}

// ---------------------------------------------------------------------------
// Candidates
// ---------------------------------------------------------------------------

struct Cand {
  const ElementFact* fact = nullptr;
  std::string norm;     // empty when blank or unparsable
  bool blank = false;   // no value at all
  bool usable = false;  // in <head> and carries a valid value
  // In <head> and safe to rewrite in place: an element with other rel tokens
  // (an alternate link, say) is never reused, because its other meaning would
  // change with its href.
  bool reusable = false;
};

std::string NormOfFact(const Traits& k, const ElementFact& f,
                       std::string_view base, bool* blank) {
  if (k.url) {
    const std::string trimmed = CollapseWhitespace(f.value);
    *blank = trimmed.empty();
    if (*blank) return std::string();
    return NormUrl(ResolveUrl(base, trimmed)).value_or(std::string());
  }
  std::string norm = CollapseWhitespace(f.value);
  *blank = norm.empty();
  return norm;
}

std::vector<Cand> MakeCands(const Traits& k,
                            const std::vector<ElementFact>& facts,
                            std::string_view base) {
  std::vector<Cand> out;
  out.reserve(facts.size());
  for (const ElementFact& f : facts) {
    Cand c;
    c.fact = &f;
    c.norm = NormOfFact(k, f, base, &c.blank);
    c.usable = f.in_head && !c.norm.empty();
    c.reusable = f.in_head && f.other_rel_tokens.empty();
    out.push_back(std::move(c));
  }
  return out;
}

Reason UnusableReason(const Cand& c) {
  if (c.blank) return Reason::kEmpty;
  return c.fact->in_head ? Reason::kUnusable : Reason::kOutOfHead;
}

// Why a new element is inserted although one exists.
Reason InsertReason(const Cand& c) {
  if (c.blank) return Reason::kEmpty;
  return c.fact->in_head ? Reason::kPresent : Reason::kOutOfHead;
}

std::string JoinTokens(const std::vector<std::string>& tokens) {
  std::string out;
  for (const std::string& t : tokens) {
    if (!out.empty()) out += ' ';
    out += t;
  }
  return out;
}

PlanOp RemoveOp(const Traits& k, const Cand& c) {
  PlanOp op;
  op.kind = k.kind;
  op.target = *c.fact;
  if (c.fact->other_rel_tokens.empty()) {
    op.type = OpType::kRemove;
  } else {
    // The element has other meanings: keep it, drop only this kind's token.
    op.type = OpType::kRemoveRelToken;
    op.value = JoinTokens(c.fact->other_rel_tokens);
  }
  return op;
}

PlanOp SetOp(const Traits& k, const Cand& c, const Target& t) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = k.kind;
  op.target = *c.fact;
  op.value = t.value;
  op.attr = k.attr;
  return op;
}

PlanOp InsertOp(const Traits& k, const Target& t) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = k.kind;
  op.value = t.value;
  op.attr = k.attr;
  return op;
}

size_t DistinctNorms(const std::vector<Cand>& cands) {
  std::map<std::string, int> seen;
  for (const Cand& c : cands) {
    if (!c.norm.empty()) seen[c.norm]++;
  }
  return seen.size();
}

// Records the hash of the first existing value as the "old" value.
void SetOldHash(RulePlan* rp, const std::vector<Cand>& cands) {
  for (const Cand& c : cands) {
    if (!c.norm.empty()) {
      rp->decision.before_hash = HashValue(c.norm);
      return;
    }
  }
}

// keep: act only when the kind is absent from the whole document; otherwise
// remove exact duplicates and nothing else.
void PlanKeep(const Traits& k, const std::vector<Cand>& cands, const Target& t,
              RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(k, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(k));
    return;
  }
  // Within a group of equal values keep one (the first in <head>, else the
  // first), remove the rest.
  std::map<std::string, const Cand*> keeper;
  for (const Cand& c : cands) {
    if (c.norm.empty()) continue;
    auto [it, inserted] = keeper.emplace(c.norm, &c);
    if (!inserted && !it->second->fact->in_head && c.fact->in_head) {
      it->second = &c;
    }
  }
  size_t removed = 0;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (c.norm.empty() || keeper[c.norm] == &c) continue;
    rp->ops.push_back(RemoveOp(k, c));
    if (old_norm.empty()) old_norm = c.norm;
    ++removed;
  }
  const size_t distinct = keeper.size();
  if (removed > 0) {
    rp->decision.before_hash = HashValue(old_norm);
    Finish(rp, Action::kDedupe, Reason::kDuplicate,
           distinct > 1 ? MultipleDefect(k) : std::string());
  } else if (distinct > 1) {
    Finish(rp, Action::kNone, Reason::kConflict, MultipleDefect(k));
  } else if (distinct == 0) {
    // Only blank elements: keep stands down, as for any present element.
    Finish(rp, Action::kNone, Reason::kPresent, UnusableDefect(k));
  } else if (keeper.begin()->first == t.norm) {
    Finish(rp, Action::kNone, Reason::kEqual, std::string());
  } else {
    Finish(rp, Action::kNone, Reason::kPresent, std::string());
  }
}

// repair: make sure one usable element exists; leave a usable one alone.
void PlanRepair(const Traits& k, const std::vector<Cand>& cands,
                const Target& t, RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(k, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(k));
    return;
  }
  const Cand* first_usable = nullptr;
  for (const Cand& c : cands) {
    if (c.usable) {
      first_usable = &c;
      break;
    }
  }
  if (first_usable == nullptr) {
    // Nothing usable. Rewrite the first element that sits in <head> in
    // place; otherwise add one. Everything else goes.
    const Cand* reuse = nullptr;
    for (const Cand& c : cands) {
      if (c.reusable) {
        reuse = &c;
        break;
      }
    }
    for (const Cand& c : cands) {
      if (&c != reuse) rp->ops.push_back(RemoveOp(k, c));
    }
    if (reuse != nullptr) {
      rp->ops.insert(rp->ops.begin(), SetOp(k, *reuse, t));
      if (!reuse->norm.empty()) {
        rp->decision.before_hash = HashValue(reuse->norm);
      }
      Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
             UnusableReason(*reuse), UnusableDefect(k));
    } else {
      rp->ops.push_back(InsertOp(k, t));
      SetOldHash(rp, cands);
      Finish(rp, Action::kReplace, InsertReason(cands.front()),
             UnusableDefect(k));
    }
    return;
  }
  // A usable element exists: it stays. Duplicates, conflicts and unusable
  // leftovers are removed.
  bool conflict = false;
  bool unusable = false;
  Reason unusable_reason = Reason::kUnusable;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (&c == first_usable) continue;
    if (c.usable) {
      if (c.norm != first_usable->norm) conflict = true;
    } else if (!unusable) {
      unusable = true;
      unusable_reason = UnusableReason(c);
    }
    if (old_norm.empty()) old_norm = c.norm;
    rp->ops.push_back(RemoveOp(k, c));
  }
  if (rp->ops.empty()) {
    Finish(rp, Action::kNone,
           first_usable->norm == t.norm ? Reason::kEqual : Reason::kPresent,
           std::string());
    return;
  }
  rp->decision.before_hash = HashValue(old_norm);
  if (conflict) {
    Finish(rp, Action::kRemove, Reason::kConflict, MultipleDefect(k));
  } else if (unusable) {
    Finish(rp, Action::kRemove, unusable_reason,
           k.unusable_defect ? UnusableDefect(k) : std::string());
  } else {
    Finish(rp, Action::kDedupe, Reason::kDuplicate, std::string());
  }
}

// replace: the document ends with exactly the rule's value.
void PlanReplace(const Traits& k, const std::vector<Cand>& cands,
                 const Target& t, RulePlan* rp) {
  if (cands.empty()) {
    rp->ops.push_back(InsertOp(k, t));
    Finish(rp, Action::kInsert, Reason::kAbsent, MissingDefect(k));
    return;
  }
  const Cand* keeper = nullptr;  // an element already equal to the target
  const Cand* reuse = nullptr;   // else the first element in <head>
  for (const Cand& c : cands) {
    if (keeper == nullptr && c.fact->in_head && c.norm == t.norm) keeper = &c;
    if (reuse == nullptr && c.reusable) reuse = &c;
  }
  const Cand* kept = keeper != nullptr ? keeper : reuse;
  bool distinct_removed = false;
  std::string old_norm;
  for (const Cand& c : cands) {
    if (&c == kept) continue;
    if (!c.norm.empty() && c.norm != t.norm) distinct_removed = true;
    if (old_norm.empty()) old_norm = c.norm;
    rp->ops.push_back(RemoveOp(k, c));
  }
  const bool multiple = DistinctNorms(cands) > 1;
  if (keeper != nullptr) {
    if (rp->ops.empty()) {
      Finish(rp, Action::kNone, Reason::kEqual, std::string());
    } else {
      rp->decision.before_hash = HashValue(old_norm);
      Finish(rp, distinct_removed ? Action::kRemove : Action::kDedupe,
             distinct_removed ? Reason::kConflict : Reason::kDuplicate,
             distinct_removed ? MultipleDefect(k) : std::string());
    }
    return;
  }
  if (reuse != nullptr) {
    rp->ops.insert(rp->ops.begin(), SetOp(k, *reuse, t));
    if (!reuse->norm.empty()) rp->decision.before_hash = HashValue(reuse->norm);
    std::string defect = multiple ? MultipleDefect(k) : std::string();
    if (defect.empty() && !reuse->usable) defect = UnusableDefect(k);
    Finish(rp, reuse->blank ? Action::kFill : Action::kReplace,
           reuse->blank ? Reason::kEmpty : Reason::kPresent, std::move(defect));
    return;
  }
  rp->ops.push_back(InsertOp(k, t));
  SetOldHash(rp, cands);
  // (Only elements that cannot be reused get here: they sit outside <head>
  // or carry other rel tokens; a usable value is not a defect.)
  Finish(rp, Action::kReplace, InsertReason(cands.front()),
         cands.front().usable ? std::string() : UnusableDefect(k));
}

// The canonical URL the document ends with once `rp`'s ops are applied.
void SetFinal(const Traits& k, const std::vector<Cand>& cands, const Target& t,
              std::string_view base, RulePlan* rp) {
  if (!k.url) return;
  std::set<const net_instaweb::HtmlElement*> gone;
  for (const PlanOp& op : rp->ops) {
    if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
      rp->final_norm = t.norm;
      rp->final_url = t.value;
      return;
    }
    gone.insert(op.target.element);
  }
  // Only elements in <head> with a usable value count; a document whose
  // surviving canonicals disagree has no single answer.
  const Cand* survivor = nullptr;
  for (const Cand& c : cands) {
    if (gone.count(c.fact->element) > 0 || !c.usable) continue;
    if (survivor == nullptr) {
      survivor = &c;
    } else if (survivor->norm != c.norm) {
      return;
    }
  }
  if (survivor == nullptr) return;
  rp->final_norm = survivor->norm;
  rp->final_url = ResolveUrl(base, CollapseWhitespace(survivor->fact->value));
}

}  // namespace

std::string HeadSingletonKind::NormOf(const ElementFact& fact,
                                      std::string_view base) const {
  Traits k;
  k.url = value_type() == ValueType::kUrl;
  bool blank = false;
  return NormOfFact(k, fact, base, &blank);
}

void HeadSingletonKind::Plan(const PlanInput& input, RulePlan* out) const {
  Traits k;
  k.kind = this;
  k.url = value_type() == ValueType::kUrl;
  k.html_text = value_type() == ValueType::kHtmlText;
  k.attr = value_attribute();
  k.max_length = max_value_length();
  k.unusable_defect = has_unusable_defect();
  k.prefix = std::string(lens_defect_prefix());

  const Target target = ComputeTarget(k, input);
  if (target.status != TargetStatus::kOk) {
    Finish(out, Action::kNone,
           target.status == TargetStatus::kNoValue ? Reason::kNoValue
                                                   : Reason::kInvalidValue,
           std::string());
    return;
  }
  out->has_target = true;
  out->target_norm = target.norm;
  const std::vector<Cand> cands = MakeCands(k, *input.facts, input.base);
  switch (input.rule->on_present) {
    case OnPresent::kKeep:
      PlanKeep(k, cands, target, out);
      break;
    case OnPresent::kRepair:
      PlanRepair(k, cands, target, out);
      break;
    case OnPresent::kReplace:
      PlanReplace(k, cands, target, out);
      break;
  }
  for (const PlanOp& op : out->ops) {
    if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
      out->decision.after = TruncateUtf8(target.logical, kMaxAfterBytes);
    }
  }
  SetFinal(k, cands, target, input.base, out);
}

std::string HeadSingletonKind::FinalValue(const RulePlan* plan,
                                          const std::vector<ElementFact>& facts,
                                          std::string_view base) const {
  const bool url = value_type() == ValueType::kUrl;
  const bool html_text = value_type() == ValueType::kHtmlText;
  std::set<const net_instaweb::HtmlElement*> gone;
  if (plan != nullptr) {
    for (const PlanOp& op : plan->ops) {
      if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
        if (html_text) {
          return CollapseWhitespace(SafeDecodeHtmlEntities(op.value));
        }
        return CollapseWhitespace(op.value);
      }
      gone.insert(op.target.element);
    }
  }
  for (const ElementFact& f : facts) {
    if (!f.in_head || gone.count(f.element) != 0) continue;
    std::string v = CollapseWhitespace(f.value);
    if (v.empty()) continue;
    if (url) {
      std::string absolute = ResolveUrl(base, v);
      if (!NormUrl(absolute).has_value()) continue;
      return absolute;
    }
    return v;
  }
  return std::string();
}

LateAction HeadSingletonKind::OnLateElement(const PlanState& state,
                                            const ElementFact& late) const {
  LateAction leave;
  const RulePlan& plan = *state.plan;
  // A rule without a value for this page changes nothing, late or not.
  if (!plan.has_target || plan.rule == nullptr) return leave;
  LateAction remove;
  remove.type = late.other_rel_tokens.empty() ? LateAction::kRemove
                                              : LateAction::kRemoveRelToken;
  const std::string norm = NormOf(late, state.base);
  remove.norm = norm;
  switch (plan.rule->on_present) {
    case OnPresent::kRepair:
    case OnPresent::kReplace:
      // Out of <head>, so never usable: the ladder removes it, exactly as it
      // would have had the element been seen before the decision.
      return remove;
    case OnPresent::kKeep:
      break;
  }
  if (state.inserted) {
    // The pack put an element of the kind into the head because none was
    // there; a second one now would make the page worse than the input
    // (two values where there was one). Search engines ignore it outside the
    // head anyway.
    remove.conflict = norm != plan.target_norm;
    return remove;
  }
  // keep, with an element in the document already: remove exact duplicates
  // only, as keep does before the decision point.
  if (norm.empty()) return leave;
  if (state.norms == nullptr) {
    for (const ElementFact& e : state.earlier) {
      if (NormOf(e, state.base) == norm) return remove;
    }
    return leave;
  }
  // Each earlier element is normalized once per page, not once per late
  // element: a page with many late elements stays linear.
  NormCache& cache = *state.norms;
  for (; cache.folded < state.earlier.size(); ++cache.folded) {
    std::string n = NormOf(state.earlier[cache.folded], state.base);
    if (!n.empty()) cache.norms.insert(std::move(n));
  }
  return cache.norms.count(norm) > 0 ? remove : leave;
}

void HeadSingletonKind::BuildInsert(
    net_instaweb::HtmlParse* parser, net_instaweb::HtmlElement* parent,
    const PlanOp& op, std::vector<net_instaweb::HtmlNode*>* out) const {
  net_instaweb::HtmlElement* fresh = CreateElement(parser, parent);
  out->push_back(fresh);
  if (value_attribute() == net_instaweb::HtmlName::kNotAKeyword) {
    out->push_back(parser->NewCharactersNode(fresh, op.value));
  } else {
    parser->AddEscapedAttribute(fresh, value_attribute(),
                                EscapeAttributeValue(op.value));
  }
}

}  // namespace pagespeed::packs
