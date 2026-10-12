// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/kinds/jsonld_kind.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/element_fact.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/planner_common.h"
#include "pagespeed/kernel/packs/rule_value_parser.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {
namespace {

void CollectNodeTypes(const Json::Value& node, bool allow_graph,
                      std::set<std::string>* out) {
  if (!node.isObject()) return;
  if (HasMember(node, "@type")) {
    const Json::Value& t = Member(node, "@type");
    if (t.isString()) {
      out->insert(AsString(t));
    } else if (t.isArray()) {
      for (Json::ArrayIndex i = 0; i < t.size(); ++i) {
        if (t[i].isString()) out->insert(AsString(t[i]));
      }
    }
  }
  if (allow_graph && HasMember(node, "@graph")) {
    const Json::Value& g = Member(node, "@graph");
    if (g.isArray()) {
      for (Json::ArrayIndex i = 0; i < g.size(); ++i) {
        CollectNodeTypes(g[i], false, out);
      }
    }
  }
}

// The set of top-level @type values, including the members of a top-level
// @graph.
std::set<std::string> TypesOf(const Json::Value& doc) {
  std::set<std::string> out;
  if (doc.isArray()) {
    for (Json::ArrayIndex i = 0; i < doc.size(); ++i) {
      CollectNodeTypes(doc[i], true, &out);
    }
  } else {
    CollectNodeTypes(doc, true, &out);
  }
  return out;
}

bool Intersects(const std::set<std::string>& a,
                const std::set<std::string>& b) {
  for (const std::string& x : a) {
    if (b.count(x) != 0) return true;
  }
  return false;
}

struct Cand {
  const ElementFact* fact = nullptr;
  bool valid = false;
  // Too large or too deeply nested to parse safely: neither valid nor
  // invalid, and never touched.
  bool unreadable = false;
  std::set<std::string> types;
  std::string dump;  // canonical form of a valid block
  std::string hash_key;
};

Cand MakeCand(const ElementFact& f) {
  Cand c;
  c.fact = &f;
  if (f.unreadable || JsonNestingExceeds(f.value, kMaxJsonLdDepth)) {
    c.unreadable = true;
    return c;
  }
  Json::Value doc;
  // A browser keeps the last of two equal keys; so does this parse.
  if (ParseStrictJson(f.value, kMaxJsonLdDepth, false, &doc)) {
    c.valid = true;
    c.types = TypesOf(doc);
    c.dump = CompactJson(doc);
    c.hash_key = c.dump;
  } else {
    c.hash_key = CollapseWhitespace(f.value);
  }
  return c;
}

struct Target {
  bool ok = false;
  std::string text;  // what gets written
  std::set<std::string> types;
  std::string dump;
};

// Expands the rule's template with the values the document ends with. The
// text must parse with the strict settings the pack loader uses (a repeated
// key is an error too), hold an object or array, and carry a type.
Target ComputeTarget(const PlanInput& in) {
  Target t;
  ExpandContext ex;
  ex.url = in.url;
  ex.captures = in.captures;
  ex.title = in.final_title;
  ex.description = in.final_description;
  ex.canonical = in.final_canonical;
  auto s = ExpandTemplate(in.rule->value.tpl, ex, EscapeContext::kJsonString,
                          kMaxJsonLdBytes);
  if (!s.ok()) return t;
  Json::Value doc;
  if (!ParseStrictJson(*s, kMaxJsonLdDepth, true, &doc)) return t;
  t.types = TypesOf(doc);
  if (t.types.empty()) return t;
  t.dump = CompactJson(doc);
  t.text = *std::move(s);
  t.ok = true;
  return t;
}

PlanOp RemoveOp(const RuleKind* kind, const Cand& c) {
  PlanOp op;
  op.type = OpType::kRemove;
  op.kind = kind;
  op.target = *c.fact;
  return op;
}

PlanOp SetOp(const RuleKind* kind, const Cand& c, const std::string& text) {
  PlanOp op;
  op.type = OpType::kSetValue;
  op.kind = kind;
  op.target = *c.fact;
  op.value = text;
  return op;
}

PlanOp InsertOp(const RuleKind* kind, const std::string& text) {
  PlanOp op;
  op.type = OpType::kInsert;
  op.kind = kind;
  op.value = text;
  return op;
}

Reason UnusableReason(const Cand& c) {
  return CollapseWhitespace(c.fact->value).empty() ? Reason::kEmpty
                                                   : Reason::kUnusable;
}

std::string TypesText(const Cand& c) {
  if (!c.valid) return "unparsable block";
  std::string out;
  for (const std::string& ty : c.types) {
    if (!out.empty()) out += ",";
    out += ty;
  }
  return out.empty() ? "untyped block" : out;
}

std::string AfterText(const Target& t) {
  std::string out;
  for (const std::string& ty : t.types) {
    if (!out.empty()) out += ", ";
    out += ty;
  }
  return TruncateUtf8(out, kMaxAfterBytes);
}

void PlanKind(const RuleKind* kind, const Target& t, OnPresent on_present,
              const std::vector<Cand>& cands, RulePlan* rp) {
  OutcomeFlags f;
  f.had_any = !cands.empty();
  f.unusable_defect = absl::StrCat(kind->lens_defect_prefix(), "-invalid");
  auto matching = [&](const Cand& c) {
    return c.valid && Intersects(c.types, t.types);
  };
  auto note_before = [&](const Cand& c) {
    if (rp->decision.before_hash.empty()) {
      rp->decision.before_hash = HashValue(c.hash_key);
    }
  };

  bool any_matching = false;
  bool any_invalid = false;
  const Cand* equal_block = nullptr;
  const Cand* first_invalid = nullptr;
  for (const Cand& c : cands) {
    if (!c.valid) {
      any_invalid = true;
      if (first_invalid == nullptr) first_invalid = &c;
    } else if (matching(c)) {
      any_matching = true;
      if (equal_block == nullptr && c.dump == t.dump) equal_block = &c;
    }
  }

  // Exact duplicates among the blocks that carry the rule's types.
  std::set<std::string> seen;
  std::set<const Cand*> duplicates;
  for (const Cand& c : cands) {
    if (matching(c) && !seen.insert(c.dump).second) duplicates.insert(&c);
  }

  if (on_present == OnPresent::kKeep) {
    if (!any_matching && !any_invalid) {
      rp->ops.push_back(InsertOp(kind, t.text));
      f.inserted = true;
      SummarizeOutcome(rp, f);
      return;
    }
    for (const Cand& c : cands) {
      if (duplicates.count(&c) == 0) continue;
      rp->ops.push_back(RemoveOp(kind, c));
      NoteRemoved(rp, TypesText(c), ReasonName(Reason::kDuplicate));
      note_before(c);
      f.removed = f.duplicate = true;
    }
    f.idle_defect = any_invalid ? f.unusable_defect : std::string();
    f.idle_reason = equal_block != nullptr ? Reason::kEqual : Reason::kPresent;
    SummarizeOutcome(rp, f);
    return;
  }

  if (on_present == OnPresent::kRepair) {
    for (const Cand& c : cands) {
      if (!c.valid) {
        rp->ops.push_back(RemoveOp(kind, c));
        NoteRemoved(rp, TypesText(c), ReasonName(UnusableReason(c)));
        note_before(c);
        f.removed = true;
        if (!f.has_unusable) {
          f.has_unusable = true;
          f.unusable_reason = UnusableReason(c);
        }
      } else if (duplicates.count(&c) != 0) {
        rp->ops.push_back(RemoveOp(kind, c));
        NoteRemoved(rp, TypesText(c), ReasonName(Reason::kDuplicate));
        note_before(c);
        f.removed = f.duplicate = true;
      }
    }
    if (!any_matching) {
      rp->ops.push_back(InsertOp(kind, t.text));
      f.inserted = true;
    }
    f.idle_reason = equal_block != nullptr ? Reason::kEqual : Reason::kPresent;
    SummarizeOutcome(rp, f);
    return;
  }

  // replace: exactly one block with the rule's content. Only blocks whose
  // types are all among the rule's are the rule's to rewrite or remove. A
  // block that shares a type but carries others (a @graph with a WebPage and
  // an Organization, a ["Organization", "LocalBusiness"] block) would lose
  // data, so the rule stands down for the page.
  auto subset = [&](const Cand& c) {
    if (!c.valid || c.types.empty()) return false;
    for (const std::string& ty : c.types) {
      if (t.types.count(ty) == 0) return false;
    }
    return true;
  };
  for (const Cand& c : cands) {
    if (matching(c) && !subset(c)) {
      Finish(rp, Action::kNone, Reason::kConflict, std::string());
      return;
    }
  }
  const Cand* first_owned = nullptr;
  const Cand* equal_owned = nullptr;
  std::set<std::string> seen_owned;
  std::set<const Cand*> dup_owned;
  for (const Cand& c : cands) {
    if (!subset(c)) continue;
    if (first_owned == nullptr) first_owned = &c;
    if (equal_owned == nullptr && c.dump == t.dump) equal_owned = &c;
    if (!seen_owned.insert(c.dump).second) dup_owned.insert(&c);
  }
  const Cand* kept = equal_owned;
  const Cand* reuse = nullptr;
  if (kept == nullptr) {
    reuse = first_owned != nullptr ? first_owned : first_invalid;
  }
  if (reuse != nullptr) {
    rp->ops.push_back(SetOp(kind, *reuse, t.text));
    note_before(*reuse);
    f.set = true;
  }
  for (const Cand& c : cands) {
    if (&c == kept || &c == reuse) continue;
    if (c.valid && !subset(c)) continue;
    rp->ops.push_back(RemoveOp(kind, c));
    note_before(c);
    f.removed = true;
    Reason why;
    if (!c.valid) {
      why = UnusableReason(c);
      if (!f.has_unusable) {
        f.has_unusable = true;
        f.unusable_reason = why;
      }
    } else if (c.dump == t.dump || dup_owned.count(&c) != 0) {
      f.duplicate = true;
      why = Reason::kDuplicate;
    } else {
      f.conflict = true;
      why = Reason::kConflict;
    }
    NoteRemoved(rp, TypesText(c), ReasonName(why));
  }
  if (kept == nullptr && reuse == nullptr) {
    rp->ops.push_back(InsertOp(kind, t.text));
    f.inserted = true;
  }
  f.idle_reason = Reason::kEqual;
  SummarizeOutcome(rp, f);
}

}  // namespace

absl::Status JsonLdKind::ParseValue(const Json::Value& value,
                                    const LoadContext& context,
                                    RuleValue* out) const {
  ValueSpec spec;
  spec.allow_table = false;
  spec.table_denied = "jsonld rules take a \"template\"";
  spec.json_template = true;
  return ParseRuleValue(value, context, spec, out);
}

bool JsonLdKind::Observe(const ElementView& view, ElementFact* fact) const {
  if (view.keyword != net_instaweb::HtmlName::kScript) return false;
  // Exactly this media type, ASCII case-insensitive; a parameter
  // ("; charset=utf-8") or surrounding space is not JSON-LD to a lens.
  const std::string type =
      AttrValue(view.element, net_instaweb::HtmlName::kType);
  if (!absl::EqualsIgnoreCase(type, "application/ld+json")) return false;
  fact->element = view.element;
  fact->in_head = view.in_head;
  fact->text_mode = TextMode::kRaw;
  return true;
}

void JsonLdKind::Plan(const PlanInput& input, RulePlan* out) const {
  // Blocks too large or too deeply nested to read: the page's JSON-LD is left
  // alone.
  std::vector<Cand> cands;
  cands.reserve(input.facts->size());
  bool unreadable = false;
  for (const ElementFact& f : *input.facts) {
    cands.push_back(MakeCand(f));
    if (cands.back().unreadable) unreadable = true;
  }
  const Target t = ComputeTarget(input);
  if (!t.ok) {
    Finish(out, Action::kNone, Reason::kInvalidValue, std::string());
    return;
  }
  out->has_target = true;
  out->target_norm = t.dump;
  if (unreadable) {
    Finish(out, Action::kNone, Reason::kPresent, std::string());
    return;
  }
  PlanKind(this, t, input.rule->on_present, cands, out);
  for (const PlanOp& op : out->ops) {
    if (op.type == OpType::kInsert || op.type == OpType::kSetValue) {
      out->decision.after = AfterText(t);
    }
  }
}

void JsonLdKind::BuildInsert(net_instaweb::HtmlParse* parser,
                             net_instaweb::HtmlElement* parent,
                             const PlanOp& op,
                             std::vector<net_instaweb::HtmlNode*>* out) const {
  net_instaweb::HtmlElement* script =
      parser->NewElement(parent, net_instaweb::HtmlName::kScript);
  parser->AddEscapedAttribute(script, net_instaweb::HtmlName::kType,
                              "application/ld+json");
  out->push_back(script);
  out->push_back(parser->NewCharactersNode(script, op.value));
}

}  // namespace pagespeed::packs
