// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/hreflang_kind.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_value_parser.h"
#include "pagespeed/kernel/packs/template.h"
#include "pagespeed/kernel/packs/url_norm.h"

namespace pagespeed::packs {
namespace {

enum class TargetStatus : std::uint8_t { kOk, kNoValue, kInvalid, kNoSelf };

struct Target {
  TargetStatus status = TargetStatus::kNoValue;
  std::vector<std::pair<std::string, std::string>> entries;  // by code
  std::string page_norm;
};

std::string PageUrlText(const PageUrl& url) {
  return absl::StrCat(url.scheme, "://", url.host, url.path,
                      url.query.empty() ? "" : "?", url.query);
}

Target ComputeTarget(const PlanInput& in) {
  Target t;
  t.page_norm = NormUrl(PageUrlText(in.url)).value_or(std::string());
  const Rule& rule = *in.rule;
  if (rule.value.source == ValueSource::kTable) {
    if (in.cluster == nullptr) return t;  // kNoValue
    t.entries = in.cluster->entries;
  } else if (rule.value.source == ValueSource::kPattern) {
    ExpandContext ex;
    ex.url = in.url;
    ex.captures = in.captures;
    for (const auto& [code, tpl] : rule.value.pattern) {
      auto s = ExpandTemplate(tpl, ex, EscapeContext::kNone, kMaxUrlBytes);
      if (!s.ok() || !CheckUrlValue(*s).ok() || !NormUrl(*s).has_value()) {
        t.status = TargetStatus::kInvalid;
        t.entries.clear();
        return t;
      }
      t.entries.emplace_back(code, *std::move(s));
    }
  } else {
    return t;  // kNoValue
  }
  bool self = false;
  for (const auto& e : t.entries) {
    if (NormUrl(e.second) == t.page_norm) self = true;
  }
  t.status = self ? TargetStatus::kOk : TargetStatus::kNoSelf;
  return t;
}

// The code as the page wrote it, when it is a valid code in its canonical
// spelling (lowercase, '-'); empty otherwise. "en_US" is not valid.
std::string ValidCode(std::string_view written) {
  const std::string lower = absl::AsciiStrToLower(CollapseWhitespace(written));
  std::optional<std::string> code = NormalizeHreflangCode(lower);
  return code.has_value() && *code == lower ? lower : std::string();
}

struct Cand {
  const ElementFact* fact = nullptr;
  std::string code;       // lowercase and valid as written; empty otherwise
  std::string href_norm;  // NormUrl() of the absolute href; empty otherwise
  bool blank = false;     // no href at all
  bool usable = false;    // in <head>, valid code, absolute href
  std::string key() const { return absl::StrCat(code, "=", href_norm); }
};

std::vector<Cand> MakeCands(const std::vector<ElementFact>& facts) {
  std::vector<Cand> out;
  out.reserve(facts.size());
  for (const ElementFact& f : facts) {
    Cand c;
    c.fact = &f;
    c.code = ValidCode(f.code);
    const std::string href = CollapseWhitespace(f.value);
    c.blank = href.empty();
    if (!c.blank) c.href_norm = NormUrl(href).value_or(std::string());
    c.usable = f.in_head && !c.code.empty() && !c.href_norm.empty();
    out.push_back(std::move(c));
  }
  return out;
}

Reason UnusableReason(const Cand& c) {
  if (c.blank) return Reason::kEmpty;
  if (c.code.empty() || c.href_norm.empty()) return Reason::kUnusable;
  return Reason::kOutOfHead;
}

// What the page had, for the "before" hash and the removed list (the code and
// href as written, collapsed; never raw markup).
std::string BeforeKey(const ElementFact& f, const Cand* usable) {
  if (usable != nullptr && usable->usable) return usable->key();
  // (Page text can hold bytes that are not UTF-8; the decision is UTF-8.)
  return absl::StrCat(ReplaceInvalidUtf8(CollapseWhitespace(f.code)), "=",
                      ReplaceInvalidUtf8(CollapseWhitespace(f.value)));
}
std::string BeforeKey(const Cand& c) { return BeforeKey(*c.fact, &c); }

PlanOp RemoveOp(const RuleKind* kind, const Cand& c) {
  PlanOp op;
  op.type = OpType::kRemove;
  op.kind = kind;
  op.target = *c.fact;
  return op;
}

PlanOp InsertOp(const RuleKind* kind, const std::string& code,
                const std::string& href) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = kind;
  op.code = code;
  op.value = href;
  op.attr = net_instaweb::HtmlName::kHref;
  return op;
}

PlanOp SetOp(const RuleKind* kind, const Cand& c, const std::string& href) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = kind;
  op.target = *c.fact;
  op.code = c.code;
  op.value = href;
  op.attr = net_instaweb::HtmlName::kHref;
  return op;
}

std::string AfterText(const RulePlan& rp) {
  std::string out;
  for (const PlanOp& op : rp.ops) {
    if (op.type == OpType::kRemove) continue;
    if (!out.empty()) out += ", ";
    absl::StrAppend(&out, op.code, "=", op.value);
  }
  return TruncateUtf8(out, kMaxAfterBytes);
}

// True when the usable elements are exactly the target entries.
bool EqualsTarget(const Target& t,
                  const std::map<std::string, const Cand*>& first) {
  if (first.size() != t.entries.size()) return false;
  for (const auto& e : t.entries) {
    auto it = first.find(e.first);
    if (it == first.end() ||
        it->second->href_norm != NormUrl(e.second).value_or(std::string())) {
      return false;
    }
  }
  return true;
}

void PlanKind(const RuleKind* kind, const Target& t, OnPresent on_present,
              const std::vector<Cand>& cands, RulePlan* rp) {
  const std::string prefix(kind->lens_defect_prefix());
  OutcomeFlags f;
  f.had_any = !cands.empty();
  f.self_defect = absl::StrCat(prefix, "-no-self");
  f.unusable_defect = absl::StrCat(prefix, "-invalid");

  // The element that survives per code: the first usable one in document
  // order, except that one pointing at the page itself wins, so that a
  // conflict never costs the page its own entry.
  std::map<std::string, const Cand*> first;
  for (const Cand& c : cands) {
    if (!c.usable) continue;
    auto [it, inserted] = first.emplace(c.code, &c);
    if (!inserted && it->second->href_norm != t.page_norm &&
        c.href_norm == t.page_norm) {
      it->second = &c;
    }
  }
  bool existing_self = false;
  for (const auto& [code, c] : first) {
    if (c->href_norm == t.page_norm) existing_self = true;
  }
  const bool self_missing = !cands.empty() && !existing_self;
  auto note_before = [&](const Cand& c) {
    if (rp->decision.before_hash.empty()) {
      rp->decision.before_hash = HashValue(BeforeKey(c));
    }
  };

  if (on_present == OnPresent::kKeep) {
    if (cands.empty()) {
      for (const auto& e : t.entries) {
        rp->ops.push_back(InsertOp(kind, e.first, e.second));
      }
      f.inserted = true;
      SummarizeOutcome(rp, f);
      return;
    }
    // Exact duplicates (same code and same href) are the only thing keep
    // removes; everything else is observed.
    std::set<std::string> seen;
    bool invalid = false;
    bool conflict = false;
    for (const Cand& c : cands) {
      if (!c.usable) {
        invalid = true;
        continue;
      }
      if (!seen.insert(c.key()).second) {
        rp->ops.push_back(RemoveOp(kind, c));
        NoteRemoved(rp, BeforeKey(c), ReasonName(Reason::kDuplicate));
        note_before(c);
        f.removed = f.duplicate = true;
      } else if (first[c.code] != &c) {
        conflict = true;
      }
    }
    const std::string observed =
        invalid ? f.unusable_defect
                : (self_missing ? f.self_defect : std::string());
    if (f.removed) {
      SummarizeOutcome(rp, f);
    } else if (conflict) {
      Finish(rp, Action::kNone, Reason::kConflict, observed);
    } else {
      f.idle_defect = observed;
      const bool equal = !invalid && !self_missing && EqualsTarget(t, first);
      f.idle_reason = equal ? Reason::kEqual : Reason::kPresent;
      SummarizeOutcome(rp, f);
    }
    return;
  }

  if (on_present == OnPresent::kRepair) {
    for (const Cand& c : cands) {
      Reason why;
      if (!c.usable) {
        why = UnusableReason(c);
        if (!f.has_unusable) {
          f.has_unusable = true;
          f.unusable_reason = why;
        }
      } else if (first[c.code] != &c) {
        if (first[c.code]->href_norm != c.href_norm) {
          f.conflict = true;
          why = Reason::kConflict;
        } else {
          f.duplicate = true;
          why = Reason::kDuplicate;
        }
      } else {
        continue;
      }
      rp->ops.push_back(RemoveOp(kind, c));
      NoteRemoved(rp, BeforeKey(c), ReasonName(why));
      note_before(c);
      f.removed = true;
    }
    bool self_inserted = false;
    for (const auto& e : t.entries) {
      if (first.count(e.first) != 0) continue;
      rp->ops.push_back(InsertOp(kind, e.first, e.second));
      f.inserted = true;
      if (NormUrl(e.second) == t.page_norm) self_inserted = true;
    }
    if (self_missing && !self_inserted) {
      // Every code of the page exists but points elsewhere: point the first
      // at the page.
      for (const auto& e : t.entries) {
        if (NormUrl(e.second) != t.page_norm) continue;
        auto it = first.find(e.first);
        if (it == first.end()) continue;
        rp->ops.push_back(SetOp(kind, *it->second, e.second));
        note_before(*it->second);
        f.set = true;
        break;
      }
    }
    f.self_missing = self_missing;
    f.idle_reason = EqualsTarget(t, first) ? Reason::kEqual : Reason::kPresent;
    SummarizeOutcome(rp, f);
    return;
  }

  // replace: the document ends with exactly the target entries.
  std::set<const Cand*> kept;
  for (const auto& e : t.entries) {
    const Cand* match = nullptr;
    const Cand* reuse = nullptr;
    const std::string want = NormUrl(e.second).value_or(std::string());
    for (const Cand& c : cands) {
      if (!c.usable || c.code != e.first || kept.count(&c) != 0) continue;
      if (c.href_norm == want) {
        match = &c;
        break;
      }
      if (reuse == nullptr) reuse = &c;
    }
    if (match != nullptr) {
      kept.insert(match);
    } else if (reuse != nullptr) {
      kept.insert(reuse);
      rp->ops.push_back(SetOp(kind, *reuse, e.second));
      note_before(*reuse);
      f.set = true;
    } else {
      rp->ops.push_back(InsertOp(kind, e.first, e.second));
      f.inserted = true;
    }
  }
  for (const Cand& c : cands) {
    if (kept.count(&c) != 0) continue;
    rp->ops.push_back(RemoveOp(kind, c));
    note_before(c);
    f.removed = true;
    Reason why;
    if (!c.usable) {
      why = UnusableReason(c);
      if (!f.has_unusable) {
        f.has_unusable = true;
        f.unusable_reason = why;
      }
    } else if (first[c.code] == &c) {
      f.conflict = true;  // a distinct code the target does not carry
      why = Reason::kConflict;
    } else {
      f.duplicate = true;
      why = Reason::kDuplicate;
    }
    NoteRemoved(rp, BeforeKey(c), ReasonName(why));
  }
  f.self_missing = self_missing;
  f.idle_reason = Reason::kEqual;
  SummarizeOutcome(rp, f);
}

}  // namespace

absl::Status HreflangKind::ParseValue(const Json::Value& value,
                                      const LoadContext& context,
                                      RuleValue* out) const {
  ValueSpec spec;
  spec.allow_template = false;
  spec.template_denied =
      "hreflang rules take \"table\" or \"pattern\", not a template";
  spec.allow_pattern = true;
  spec.hreflang_table = true;
  return ParseRuleValue(value, context, spec, out);
}

bool HreflangKind::Observe(const ElementView& view, ElementFact* fact) const {
  if (view.keyword != net_instaweb::HtmlName::kLink) return false;
  bool has_rel = false;
  const std::string rel =
      AttrValue(view.element, net_instaweb::HtmlName::kRel, &has_rel);
  if (!has_rel) return false;
  bool alternate = false;
  bool other = false;
  for (std::string_view tok :
       absl::StrSplit(rel, absl::ByAnyChar(" \t\n\r\f"), absl::SkipEmpty())) {
    if (absl::EqualsIgnoreCase(tok, "alternate")) {
      alternate = true;
    } else {
      other = true;
    }
  }
  if (!alternate || other) return false;
  const net_instaweb::HtmlElement::Attribute* attr = nullptr;
  for (const net_instaweb::HtmlElement::Attribute& a :
       view.element->attributes()) {
    if (absl::EqualsIgnoreCase(a.name_str(), "hreflang")) {
      attr = &a;
      break;
    }
  }
  if (attr == nullptr) return false;
  fact->element = view.element;
  fact->in_head = view.in_head;
  fact->code = attr->escaped_value() == nullptr
                   ? std::string()
                   : DecodeAttribute(attr->escaped_value());
  fact->value = AttrValue(view.element, net_instaweb::HtmlName::kHref,
                          &fact->has_value_attr);
  return true;
}

void HreflangKind::Plan(const PlanInput& input, RulePlan* out) const {
  const Target t = ComputeTarget(input);
  if (t.status != TargetStatus::kOk) {
    Reason why = Reason::kInvalidValue;
    if (t.status == TargetStatus::kNoValue) why = Reason::kNoValue;
    if (t.status == TargetStatus::kNoSelf) why = Reason::kClusterWithoutSelf;
    Finish(out, Action::kNone, why, std::string());
    return;
  }
  out->has_target = true;
  out->target_norm = t.page_norm;
  PlanKind(this, t, input.rule->on_present, MakeCands(*input.facts), out);
  out->decision.after = AfterText(*out);
}

LateAction HreflangKind::OnLateElement(const PlanState& state,
                                       const ElementFact& late) const {
  LateAction leave;
  const RulePlan& plan = *state.plan;
  // A rule without a usable cluster for this page changes nothing.
  if (!plan.has_target || plan.rule == nullptr) return leave;
  LateAction remove;
  remove.type = LateAction::kRemove;
  remove.norm = BeforeKey(late, nullptr);
  switch (plan.rule->on_present) {
    case OnPresent::kRepair:
    case OnPresent::kReplace:
      // Out of <head>, so never usable: the ladder removes it, exactly as it
      // would have had the link been seen before the decision.
      return remove;
    case OnPresent::kKeep:
      break;
  }
  if (!state.inserted) return leave;  // keep only observes what is there
  const std::string code = ValidCode(late.code);
  if (code.empty()) return leave;
  for (const PlanOp& op : plan.ops) {
    if (op.type != OpType::kInsert || op.code != code) continue;
    remove.conflict =
        NormUrl(CollapseWhitespace(late.value)) != NormUrl(op.value);
    return remove;
  }
  return leave;
}

void HreflangKind::BuildInsert(
    net_instaweb::HtmlParse* parser, net_instaweb::HtmlElement* parent,
    const PlanOp& op, std::vector<net_instaweb::HtmlNode*>* out) const {
  net_instaweb::HtmlElement* link =
      parser->NewElement(parent, net_instaweb::HtmlName::kLink);
  parser->AddEscapedAttribute(link, net_instaweb::HtmlName::kRel, "alternate");
  link->AddEscapedAttribute(parser->MakeName("hreflang"),
                            EscapeAttributeValue(op.code),
                            net_instaweb::HtmlElement::DOUBLE_QUOTE);
  parser->AddEscapedAttribute(link, net_instaweb::HtmlName::kHref,
                              EscapeAttributeValue(op.value));
  out->push_back(link);
}

}  // namespace pagespeed::packs
