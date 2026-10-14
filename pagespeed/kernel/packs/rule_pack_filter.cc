// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/rule_pack_filter.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/ascii.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "base/logging.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/packs/entity_decode.h"
#include "pagespeed/kernel/packs/link_header.h"
#include "pagespeed/kernel/packs/planner_common.h"

namespace pagespeed::packs {

using net_instaweb::HtmlCharactersNode;
using net_instaweb::HtmlElement;
using net_instaweb::HtmlName;
using net_instaweb::HtmlNode;

namespace {

// Elements that may appear in <head>. The first other element (usually
// <body>) ends the head even when the document never writes </head>.
// `lower_name` matters only for elements that are not keywords.
bool IsHeadContent(HtmlName::Keyword kw, std::string_view lower_name) {
  switch (kw) {
    case HtmlName::kTitle:
    case HtmlName::kMeta:
    case HtmlName::kLink:
    case HtmlName::kBase:
    case HtmlName::kScript:
    case HtmlName::kStyle:
    case HtmlName::kNoscript:
    case HtmlName::kTemplate:
    case HtmlName::kNoembed:
    case HtmlName::kNoframes:
      return true;
    case HtmlName::kNotAKeyword:
      return lower_name == "svg" || lower_name == "math";
    default:
      return false;
  }
}

// Subtrees whose elements are not part of the main document: what a kind
// would find there is not what a search engine reads.
bool IsInertTag(HtmlName::Keyword kw, std::string_view lower_name) {
  switch (kw) {
    case HtmlName::kTemplate:
    case HtmlName::kNoscript:
    case HtmlName::kNoembed:
    case HtmlName::kNoframes:
      return true;
    case HtmlName::kNotAKeyword:
      return lower_name == "svg" || lower_name == "math";
    default:
      return false;
  }
}

// A decision whose change could not be made: nothing happened, so it carries
// no action and no values, only the reason.
PackDecision NotRewritable(PackDecision d) {
  d.action = Action::kNone;
  d.reason = Reason::kNotRewritable;
  d.before_hash.clear();
  d.after.clear();
  d.removed.clear();
  return d;
}

// Room for the debug comment in front of every change.
constexpr size_t kDebugCommentBytes = 256;

}  // namespace

RulePackFilter::RulePackFilter(net_instaweb::HtmlParse* parser,
                               std::shared_ptr<const Pack> pack,
                               Mode directive_mode, PackHost* host,
                               RulePackFilterOptions options)
    : parser_(parser),
      pack_(std::move(pack)),
      directive_mode_(directive_mode),
      host_(host),
      options_(options) {
  CHECK(pack_ != nullptr) << "RulePackFilter needs a pack";
  if (directive_mode_ == Mode::kEnforce) {
    for (const Rule& r : pack_->rules) {
      if (!r.enabled || !r.enforce || r.kind == nullptr) continue;
      if (r.kind->ModifiesUrls(r.on_present)) can_modify_urls_ = true;
      if (r.kind->MayInjectScripts()) may_inject_scripts_ = true;
    }
  }
}

RulePackFilter::~RulePackFilter() = default;

void RulePackFilter::BeginPage() {
  active_ = false;
  url_ = PageUrl();
  selection_ = Selection();
  needs_head_values_ = false;
  facts_ = PageFacts();
  base_.clear();
  inert_.clear();
  open_head_ = nullptr;
  text_element_ = nullptr;
  text_kind_ = nullptr;
  text_fact_ = 0;
  text_raw_.clear();
  window_ = 0;
  insert_before_window_ = -1;
  headers_flushed_ = false;
  head_decided_ = false;
  head_dropped_ = false;
  head_plan_ = PagePlan();
  document_decided_ = false;
  document_dropped_ = false;
  document_plan_ = PagePlan();
  document_anchor_ = nullptr;
  commented_.clear();
  plan_for_.fill(nullptr);
  inserted_.fill(false);
  late_.fill(LateState());
  norms_.fill(NormCache());
  header_decision_.fill(std::nullopt);
  claimed_.clear();
  for (std::string& holder : header_claimed_by_) holder.clear();
  claimed_only_ = false;
  modified_ = false;
  headers_modified_ = false;
  would_modify_ = false;
  skip_reason_ = SkipReason::kNone;
  decisions_.clear();
  // (The headers are still writable here; a host without any says so now.)
  has_headers_ = host_->response_headers() != nullptr;
}

void RulePackFilter::DetermineEnabled(GoogleString* disabled_reason) {
  BeginPage();
  set_is_enabled(false);
  std::optional<PageUrl> url = ParsePageUrl(parser_->url());
  if (!url.has_value()) {
    *disabled_reason = "rule_packs: not an http(s) URL";
    return;
  }
  url_ = *std::move(url);
  selection_ = SelectRules(*pack_, url_.host, url_.path);
  if (selection_.site == nullptr) {
    skip_reason_ = SkipReason::kHostNotListed;
    host_->Count(Counter::kPagesSkippedHostNotListed);
    *disabled_reason = "rule_packs: host not listed";
    return;
  }
  bool any = false;
  for (const auto& hit : selection_.by_kind) any = any || hit.has_value();
  if (!any) {
    *disabled_reason = "rule_packs: no rule for this path";
    return;
  }
  TakeClaims();
  if (!claimed_.empty()) {
    any = false;
    for (const auto& hit : selection_.by_kind) any = any || hit.has_value();
    if (!any) {
      // Only the claimed decisions to log, at the end of the document.
      claimed_only_ = true;
      set_is_enabled(true);
      return;
    }
  }
  // A kind that decides at the end of the document may quote the head values
  // the document ends with, even those no rule of the pack selects for this
  // path: they are read off the page.
  for (const RuleKind* kind : AllKinds()) {
    if (kind->decision_point() == DecisionPoint::kDocumentEnd &&
        selection_.by_kind[KindIndex(kind)].has_value()) {
      needs_head_values_ = true;
    }
  }
  active_ = true;
  set_is_enabled(true);
}

void RulePackFilter::TakeClaims() {
  for (const RuleKind* kind : AllKinds()) {
    const size_t i = KindIndex(kind);
    std::optional<RuleHit>& hit = selection_.by_kind[i];
    if (!hit.has_value()) continue;
    const Rule& rule = *hit->rule;
    const std::string* holder =
        host_->Claim(absl::StrCat("kind:", kind->name()), pack_->info.id);
    if (holder != nullptr) {
      PackDecision d;
      d.rule_id = rule.id;
      d.kind = kind;
      d.mode =
          EffectiveMode(directive_mode_, selection_.site->mode, rule.enforce);
      d.action = Action::kNone;
      d.reason = Reason::kClaimed;
      d.claimed_by = *holder;
      claimed_.push_back(std::move(d));
      hit.reset();
      continue;
    }
    if (rule.link_header == LinkHeaderMode::kMatch) {
      holder = host_->Claim("header:link", pack_->info.id);
      if (holder != nullptr) header_claimed_by_[i] = *holder;
    }
  }
}

void RulePackFilter::StartDocument() {
  // The per-document state was reset in DetermineEnabled(), which the parser
  // calls before the first event of every document.
}

void RulePackFilter::Flush() {
  ++window_;
  headers_flushed_ = true;
}

void RulePackFilter::EndLogicalHead(HtmlElement* ender) {
  // An inert element (svg, noscript, ...) that is still open here swallowed
  // the end of the head; where the head really ends is not knowable.
  if (!inert_.empty()) facts_.malformed_head = true;
  facts_.insert_before = ender;
  insert_before_window_ = window_;
  open_head_ = nullptr;
}

void RulePackFilter::StartElement(HtmlElement* element) {
  if (!active_) return;
  const HtmlName::Keyword kw = element->keyword();
  // The lowercase tag name: the keyword's own spelling, or a folded copy for
  // the (rare) elements that are not keywords.
  std::string folded;
  std::string_view name;
  if (kw == HtmlName::kNotAKeyword) {
    folded = absl::AsciiStrToLower(element->name_str());
    name = folded;
  } else {
    name = *net_instaweb::HtmlKeywords::KeywordToString(kw);
  }

  if (!head_decided_) {
    // The head ends at </head>, at <body>, or at the first element that
    // cannot be head content, whichever comes first.
    if (open_head_ != nullptr && element != open_head_) {
      if (kw == HtmlName::kBody ||
          (inert_.empty() && !IsHeadContent(kw, name))) {
        EndLogicalHead(element);
      }
    }
    // <body> is a decision point of its own: the head is over.
    if (kw == HtmlName::kBody) DecideHead();
  }

  if (IsInertTag(kw, name)) {
    inert_.push_back(element);
    return;
  }
  if (!inert_.empty()) return;

  if (kw == HtmlName::kHtml && facts_.html == nullptr) facts_.html = element;
  if (kw == HtmlName::kBody && facts_.body == nullptr) facts_.body = element;

  const bool in_head = open_head_ != nullptr;
  if (kw == HtmlName::kHead) {
    if (facts_.head == nullptr && !head_decided_) {
      facts_.head = element;
      open_head_ = element;
    }
    return;
  }
  if (kw == HtmlName::kBase) {
    if (facts_.base_href.empty() && !head_decided_) {
      facts_.base_href = AttrValue(element, HtmlName::kHref);
    }
    return;
  }

  ElementView view;
  view.element = element;
  view.keyword = kw;
  view.lower_name = name;
  view.in_head = in_head;
  for (const RuleKind* kind : AllKinds()) {
    if (!selection_.by_kind[KindIndex(kind)].has_value() &&
        !(needs_head_values_ && kind->head_slot() != HeadSlot::kNone)) {
      continue;
    }
    ElementFact fact;
    if (!kind->Observe(view, &fact)) continue;
    const bool late =
        head_decided_ && kind->decision_point() == DecisionPoint::kHeadEnd;
    fact.late = late;
    if (late) fact.in_head = false;
    std::vector<ElementFact>& facts = facts_.For(kind);
    facts.push_back(std::move(fact));
    if (facts.back().text_mode != TextMode::kNone) {
      // Literal-text elements (title, script) cannot nest, so one at a time.
      text_element_ = element;
      text_kind_ = kind;
      text_fact_ = facts.size() - 1;
      text_raw_.clear();
    } else if (late) {
      HandleLate(kind, facts.size() - 1);
    }
  }
}

void RulePackFilter::EndElement(HtmlElement* element) {
  if (!active_) return;
  if (!head_decided_ && element == facts_.head) {
    // </head>, written or implied by the parser.
    if (element == open_head_ && !inert_.empty()) {
      facts_.malformed_head = true;
    }
    open_head_ = nullptr;
    DecideHead();
    return;
  }
  if (!inert_.empty()) {
    if (inert_.back() == element) {
      inert_.pop_back();
      // An inert element in <head> that the source never closed was closed by
      // the parser at the end of the head; where the head really ends is not
      // knowable.
      const auto style = element->style();
      if (open_head_ != nullptr && (style == HtmlElement::AUTO_CLOSE ||
                                    style == HtmlElement::UNCLOSED)) {
        facts_.malformed_head = true;
      }
    }
    return;
  }
  if (text_element_ != nullptr && element == text_element_) {
    FinishTextElement();
  }
}

void RulePackFilter::Characters(HtmlCharactersNode* characters) {
  if (!active_ || text_element_ == nullptr || !inert_.empty() ||
      characters->parent() != text_element_) {
    return;
  }
  ElementFact& f = facts_.For(text_kind_)[text_fact_];
  f.text_nodes.push_back(characters);
  if (f.text_mode == TextMode::kRaw) {
    if (f.unreadable ||
        f.value.size() + characters->contents().size() > kMaxJsonLdScanBytes) {
      f.unreadable = true;
      f.value.clear();
    } else {
      f.value.append(characters->contents());
    }
  } else {
    text_raw_.append(characters->contents());
  }
}

void RulePackFilter::FinishTextElement() {
  const RuleKind* kind = text_kind_;
  const size_t index = text_fact_;
  ElementFact& f = facts_.For(kind)[index];
  if (f.text_mode == TextMode::kDecoded) {
    f.value = SafeDecodeHtmlEntities(text_raw_);
  }
  text_element_ = nullptr;
  text_kind_ = nullptr;
  text_raw_.clear();
  if (f.late) HandleLate(kind, index);
}

void RulePackFilter::DecideHead() {
  if (head_decided_) return;
  head_decided_ = true;
  PageContext ctx;
  ctx.url = url_;
  ctx.global_mode = directive_mode_;
  base_ = BaseUrl(url_, facts_);
  const size_t extra = host_->debug_comments() ? kDebugCommentBytes : 0;
  head_plan_ =
      BuildPlan(*pack_, ctx, selection_, facts_, DecisionPoint::kHeadEnd,
                options_.max_added_bytes, extra);
  if (head_plan_.skip != SkipReason::kNone) {
    skip_reason_ = head_plan_.skip;
    return;
  }
  for (const RulePlan& rp : head_plan_.rules) {
    const size_t i = KindIndex(rp.decision.kind);
    plan_for_[i] = &rp;
    for (const PlanOp& op : rp.ops) {
      if (op.type == OpType::kInsert) inserted_[i] = true;
    }
  }
  if (!Rewritable(head_plan_, DecisionPoint::kHeadEnd)) {
    head_dropped_ = true;
    skip_reason_ = SkipReason::kNotRewritable;
    return;
  }
  Apply(head_plan_, DecisionPoint::kHeadEnd);
  AnnotateReport(head_plan_, DecisionPoint::kHeadEnd);
  ApplyHeaders(head_plan_);
}

void RulePackFilter::DecideDocumentEnd() {
  if (document_decided_) return;
  document_decided_ = true;
  PageContext ctx;
  ctx.url = url_;
  ctx.global_mode = directive_mode_;
  ctx.head_plan = &head_plan_;
  ctx.head_applied = !head_dropped_;
  // The head plan's bytes count against the same page budget. (A head plan
  // that was dropped still uses its share: this overcounts, on the safe side.)
  size_t budget = options_.max_added_bytes;
  if (head_plan_.skip == SkipReason::kNone) {
    budget -= std::min(budget, head_plan_.added_bytes);
  }
  const size_t extra = host_->debug_comments() ? kDebugCommentBytes : 0;
  document_plan_ = BuildPlan(*pack_, ctx, selection_, facts_,
                             DecisionPoint::kDocumentEnd, budget, extra);
  if (document_plan_.skip != SkipReason::kNone) {
    if (skip_reason_ == SkipReason::kNone) skip_reason_ = document_plan_.skip;
    return;
  }
  if (document_plan_.rules.empty()) return;
  document_anchor_ = DocumentAnchor();
  if (!Rewritable(document_plan_, DecisionPoint::kDocumentEnd)) {
    document_dropped_ = true;
    if (skip_reason_ == SkipReason::kNone) {
      skip_reason_ = SkipReason::kNotRewritable;
    }
    return;
  }
  Apply(document_plan_, DecisionPoint::kDocumentEnd);
  AnnotateReport(document_plan_, DecisionPoint::kDocumentEnd);
}

HtmlElement* RulePackFilter::DocumentAnchor() const {
  if (facts_.body != nullptr && parser_->CanAppendChild(facts_.body)) {
    return facts_.body;
  }
  if (facts_.html != nullptr && parser_->CanAppendChild(facts_.html)) {
    return facts_.html;
  }
  return nullptr;
}

void RulePackFilter::ApplyHeaders(const PagePlan& plan) {
  for (const RulePlan& rp : plan.rules) {
    // A host that never had response headers has nothing to match, whether
    // or not a flush came.
    if (!has_headers_) continue;
    if (rp.rule == nullptr || rp.rule->link_header != LinkHeaderMode::kMatch ||
        !rp.planned || !rp.has_target || rp.final_norm.empty()) {
      continue;
    }
    PackDecision d;
    d.rule_id = rp.decision.rule_id;
    d.kind = rp.decision.kind;
    d.mode = rp.decision.mode;
    d.header = true;
    const size_t i = KindIndex(rp.decision.kind);
    if (!header_claimed_by_[i].empty()) {
      // An earlier pack keeps the header in line with its own rule.
      d.action = Action::kNone;
      d.reason = Reason::kClaimed;
      d.claimed_by = header_claimed_by_[i];
      header_decision_[i] = std::move(d);
      continue;
    }
    if (headers_flushed_) {
      d.action = Action::kNone;
      d.reason = Reason::kHeadersFlushed;
      host_->Count(Counter::kHeadersFlushed);
      header_decision_[i] = std::move(d);
      continue;
    }
    PackHost::Headers* headers = host_->response_headers();
    // A host without response headers has nothing to match.
    if (headers == nullptr) continue;
    CanonicalLinkEdit edit = PlanCanonicalLinkHeader(
        headers->Values("Link"), PageUrlString(url_), rp.final_norm,
        rp.final_url, rp.target_norm, rp.rule->on_present);
    if (!edit.applicable) continue;
    d.action = edit.action;
    d.reason = edit.reason;
    d.defect = std::move(edit.defect);
    d.before_hash = std::move(edit.before_hash);
    if (edit.mutate) d.after = TruncateUtf8(rp.final_url, kMaxAfterBytes);
    if (edit.mutate && d.mode == Mode::kEnforce) {
      if (edit.append_only) {
        headers->Add("Link", edit.lines.back());
      } else {
        headers->Replace("Link", edit.lines);
      }
      headers_modified_ = true;
      host_->Count(Counter::kHeadersSet);
    }
    header_decision_[i] = std::move(d);
  }
}

bool RulePackFilter::InsertAnchorAvailable() const {
  if (facts_.insert_before != nullptr) {
    // Inserting before an element needs only its start in the current
    // window.
    return insert_before_window_ == window_ && facts_.insert_before->live();
  }
  return facts_.head != nullptr && parser_->CanAppendChild(facts_.head);
}

bool RulePackFilter::Rewritable(const PagePlan& plan,
                                DecisionPoint point) const {
  for (const RulePlan& rp : plan.rules) {
    if (rp.decision.mode != Mode::kEnforce) continue;
    for (const PlanOp& op : rp.ops) {
      if (op.type == OpType::kInsert) {
        const bool anchored = point == DecisionPoint::kHeadEnd
                                  ? InsertAnchorAvailable()
                                  : document_anchor_ != nullptr;
        if (!anchored) return false;
        continue;
      }
      if (!parser_->IsRewritable(op.target.element)) return false;
      for (const HtmlCharactersNode* t : op.target.text_nodes) {
        if (!parser_->IsRewritable(t)) return false;
      }
    }
  }
  return true;
}

void RulePackFilter::Apply(const PagePlan& plan, DecisionPoint point) {
  for (const RulePlan& rp : plan.rules) {
    if (rp.decision.mode != Mode::kEnforce) continue;
    for (const PlanOp& op : rp.ops) ApplyOp(rp, op, point);
  }
}

void RulePackFilter::InsertAt(DecisionPoint point, HtmlNode* node) {
  if (point == DecisionPoint::kHeadEnd) {
    InsertAtHeadEnd(node);
  } else {
    parser_->AppendChild(document_anchor_, node);
  }
}

void RulePackFilter::InsertAtHeadEnd(HtmlNode* node) {
  if (facts_.insert_before != nullptr) {
    parser_->InsertNodeBeforeNode(facts_.insert_before, node);
  } else {
    parser_->AppendChild(facts_.head, node);
  }
}

void RulePackFilter::SetEscaped(HtmlElement* element, HtmlName::Keyword keyword,
                                const std::string& escaped) {
  HtmlElement::Attribute* attr = element->FindAttribute(keyword);
  if (attr != nullptr) {
    attr->SetEscapedValue(escaped);
    // An attribute that was unquoted would otherwise stay unquoted around a
    // value that may now hold spaces or quotes.
    attr->set_quote_style(HtmlElement::DOUBLE_QUOTE);
  } else {
    parser_->AddEscapedAttribute(element, keyword, escaped);
  }
}

std::string RulePackFilter::DebugText(const RulePlan& rp, bool would,
                                      Action action) const {
  return absl::StrCat(" pagespeed-pack ", pack_->info.id, "@",
                      pack_->info.version, " rule=", rp.rule->id,
                      would ? " would=" : " action=", ActionName(action),
                      " mode=", ModeName(rp.decision.mode), " ");
}

void RulePackFilter::AddDebugComment(const RulePlan& rp, HtmlElement* before,
                                     DecisionPoint point) {
  if (!host_->debug_comments()) return;
  // One comment per rule, however many elements it writes.
  if (!commented_.insert(&rp).second) return;
  HtmlElement* parent =
      before != nullptr
          ? before->parent()
          : (point == DecisionPoint::kHeadEnd ? facts_.head : document_anchor_);
  HtmlNode* comment =
      parser_->NewCommentNode(parent, DebugText(rp, false, rp.decision.action));
  if (before != nullptr) {
    parser_->InsertNodeBeforeNode(before, comment);
  } else {
    InsertAt(point, comment);
  }
}

void RulePackFilter::AnnotateReport(const PagePlan& plan, DecisionPoint point) {
  if (!host_->debug_comments()) return;
  // The comment after which the next one goes, for an anchor the page wrote
  // briefly closed (<head/>): a child would make the writer spell it out as
  // <head>...</head>, which changes the page beyond the comments, so the
  // comments go right after it instead.
  HtmlNode* after = nullptr;
  for (const RulePlan& rp : plan.rules) {
    if (rp.decision.mode == Mode::kEnforce || rp.ops.empty() ||
        rp.decision.action == Action::kNone) {
      continue;
    }
    // One comment per rule, where its first change would have gone.
    const PlanOp& op = rp.ops.front();
    HtmlElement* before = nullptr;
    HtmlElement* anchor =
        point == DecisionPoint::kHeadEnd ? facts_.head : document_anchor_;
    bool brief = false;
    if (op.type == OpType::kInsert) {
      if (point == DecisionPoint::kHeadEnd ? !InsertAnchorAvailable()
                                           : anchor == nullptr) {
        continue;
      }
      brief = (point != DecisionPoint::kHeadEnd ||
               facts_.insert_before == nullptr) &&
              anchor->style() == HtmlElement::BRIEF_CLOSE;
      if (brief && after == nullptr) {
        if (!parser_->IsRewritable(anchor)) continue;
        after = anchor;
      }
    } else {
      before = op.target.element;
      if (before == nullptr || !parser_->IsRewritable(before)) continue;
    }
    HtmlElement* parent = before != nullptr ? before->parent()
                          : brief           ? anchor->parent()
                                            : anchor;
    HtmlNode* comment = parser_->NewCommentNode(
        parent, DebugText(rp, true, rp.decision.action));
    if (before != nullptr) {
      parser_->InsertNodeBeforeNode(before, comment);
    } else if (brief) {
      parser_->InsertNodeAfterNode(after, comment);
      after = comment;
    } else {
      InsertAt(point, comment);
    }
  }
}

void RulePackFilter::ApplyOp(const RulePlan& rp, const PlanOp& op,
                             DecisionPoint point) {
  HtmlElement* el = op.target.element;
  switch (op.type) {
    case OpType::kRemove:
      if (parser_->DeleteNode(el)) modified_ = true;
      return;
    case OpType::kRemoveRelToken:
      if (el->FindAttribute(HtmlName::kRel) != nullptr) {
        SetEscaped(el, HtmlName::kRel, EscapeAttributeValue(op.value));
        modified_ = true;
      }
      return;
    case OpType::kSetValue:
      AddDebugComment(rp, el, point);
      if (op.attr == HtmlName::kNotAKeyword) {
        for (HtmlCharactersNode* t : op.target.text_nodes) {
          parser_->DeleteNode(t);
        }
        parser_->AppendChild(el, parser_->NewCharactersNode(el, op.value));
      } else {
        SetEscaped(el, op.attr, EscapeAttributeValue(op.value));
      }
      modified_ = true;
      return;
    case OpType::kInsert: {
      std::vector<HtmlNode*> nodes;
      op.kind->BuildInsert(
          parser_,
          point == DecisionPoint::kHeadEnd ? facts_.head : document_anchor_, op,
          &nodes);
      AddDebugComment(rp, nullptr, point);
      for (size_t i = 0; i < nodes.size(); ++i) {
        HtmlNode* node = nodes[i];
        HtmlNode* parent = nullptr;
        for (size_t j = 0; j < i; ++j) {
          if (node->parent() == nodes[j]) parent = nodes[j];
        }
        if (parent != nullptr) {
          parser_->AppendChild(static_cast<HtmlElement*>(parent), node);
        } else {
          InsertAt(point, node);
        }
      }
      modified_ = true;
      return;
    }
  }
}

void RulePackFilter::HandleLate(const RuleKind* kind, size_t fact_index) {
  const size_t i = KindIndex(kind);
  const RulePlan* rp = plan_for_[i];
  LateState& late = late_[i];
  late.seen = true;
  if (rp == nullptr) return;  // no decision for the kind on this page
  const std::vector<ElementFact>& facts = facts_.For(kind);
  const ElementFact& fact = facts[fact_index];
  PlanState state;
  state.plan = rp;
  state.inserted = inserted_[i];
  state.base = base_;
  state.earlier = absl::MakeConstSpan(facts.data(), fact_index);
  state.norms = &norms_[i];
  const LateAction action = kind->OnLateElement(state, fact);
  if (action.type == LateAction::kLeave) return;
  const bool first = !late.acted;
  if (first) late.before = action.norm;
  late.acted = true;
  if (action.conflict) late.conflict = true;
  // Report mode, or a head whose plan could not be applied: record only.
  if (head_dropped_ || rp->decision.mode != Mode::kEnforce) {
    // With debug comments on, report mode marks the first late element of
    // the kind it would have removed.
    if (!head_dropped_ && first && host_->debug_comments() &&
        parser_->IsRewritable(fact.element)) {
      parser_->InsertNodeBeforeNode(
          fact.element,
          parser_->NewCommentNode(fact.element->parent(),
                                  DebugText(*rp, true, Action::kRemove)));
    }
    return;
  }
  HtmlElement* el = fact.element;
  bool done = false;
  if (parser_->IsRewritable(el)) {
    if (action.type == LateAction::kRemove) {
      done = parser_->DeleteNode(el);
    } else if (el->FindAttribute(HtmlName::kRel) != nullptr) {
      SetEscaped(
          el, HtmlName::kRel,
          EscapeAttributeValue(absl::StrJoin(fact.other_rel_tokens, " ")));
      done = true;
    }
  }
  if (done) {
    modified_ = true;
    host_->Count(Counter::kLateRemoved);
  } else {
    late.unremovable = true;
    host_->Count(Counter::kLateUnremovable);
  }
}

void RulePackFilter::FinalizeHeadDecisions() {
  if (head_plan_.skip != SkipReason::kNone) return;
  // The ladder over the whole document, for rules whose late elements the
  // head-close plan could not see. Built only when a late element came.
  std::optional<PagePlan> whole;
  for (const RulePlan& rp : head_plan_.rules) {
    const size_t i = KindIndex(rp.decision.kind);
    const LateState& late = late_[i];
    // The rule's response header decision, if any, follows its own.
    auto append_header = [&] {
      if (header_decision_[i].has_value()) {
        decisions_.push_back(*std::move(header_decision_[i]));
        header_decision_[i].reset();
      }
    };
    // A dropped head plan changed nothing, late elements included: every
    // enforcing rule that would have acted, at head close or on a late
    // element, records not_rewritable.
    if (head_dropped_ && rp.decision.mode == Mode::kEnforce &&
        (!rp.ops.empty() || late.acted)) {
      PackDecision d = rp.decision;
      decisions_.push_back(NotRewritable(std::move(d)));
      append_header();
      continue;
    }
    if (!late.seen) {
      decisions_.push_back(rp.decision);
      append_header();
      continue;
    }
    if (rp.rule->on_present == OnPresent::kKeep && inserted_[i]) {
      // The head decision stands; the late elements it made redundant get a
      // decision of their own.
      decisions_.push_back(rp.decision);
      if (late.acted) {
        PackDecision d;
        d.rule_id = rp.decision.rule_id;
        d.kind = rp.decision.kind;
        d.mode = rp.decision.mode;
        d.action = late.conflict ? Action::kRemove : Action::kDedupe;
        d.reason = late.conflict ? Reason::kLateConflict : Reason::kDuplicate;
        if (!late.before.empty()) d.before_hash = HashValue(late.before);
        if (late.unremovable) d = NotRewritable(std::move(d));
        decisions_.push_back(std::move(d));
      }
      append_header();
      continue;
    }
    // Otherwise the late elements were handled exactly as the ladder handles
    // them when it sees the whole document, and so is the decision.
    if (!whole.has_value()) {
      PageContext ctx;
      ctx.url = url_;
      ctx.global_mode = directive_mode_;
      const size_t extra = host_->debug_comments() ? kDebugCommentBytes : 0;
      whole =
          BuildPlan(*pack_, ctx, selection_, facts_, DecisionPoint::kHeadEnd,
                    options_.max_added_bytes, extra);
    }
    PackDecision d = rp.decision;
    for (const RulePlan& w : whole->rules) {
      if (w.decision.kind == rp.decision.kind) d = w.decision;
    }
    if (late.unremovable) d = NotRewritable(std::move(d));
    decisions_.push_back(std::move(d));
    append_header();
  }
}

void RulePackFilter::FinalizeDocumentDecisions() {
  // (A skipped plan has no rules.)
  for (const RulePlan& rp : document_plan_.rules) {
    // A dropped plan changed nothing: an enforcing rule that would have acted
    // records not_rewritable.
    if (document_dropped_ && rp.decision.mode == Mode::kEnforce &&
        !rp.ops.empty()) {
      PackDecision d = rp.decision;
      decisions_.push_back(NotRewritable(std::move(d)));
    } else {
      decisions_.push_back(rp.decision);
    }
  }
}

void RulePackFilter::Report() {
  host_->Count(Counter::kPagesEvaluated);
  // A page counts as skipped only when nothing was applied to it: the head
  // plan and the document-end plan are independent, and a page where one
  // applied and the other dropped is a modified page (its dropped decisions
  // say not_rewritable).
  switch (modified_ ? SkipReason::kNone : skip_reason_) {
    case SkipReason::kNone:
    case SkipReason::kHostNotListed:  // counted when the filter disabled
      break;
    case SkipReason::kNoHead:
      host_->Count(Counter::kPagesSkippedNoHead);
      break;
    case SkipReason::kMalformedHead:
      host_->Count(Counter::kPagesSkippedMalformedHead);
      break;
    case SkipReason::kNotRewritable:
      host_->Count(Counter::kPagesSkippedNotRewritable);
      break;
    case SkipReason::kSizeLimit:
      host_->Count(Counter::kPagesSkippedSizeLimit);
      break;
  }
  for (const PackDecision& d : decisions_) {
    if (d.reason == Reason::kClaimed) host_->Count(Counter::kDecisionsClaimed);
    if (d.action == Action::kNone || d.reason == Reason::kNotRewritable) {
      continue;
    }
    if (d.mode == Mode::kEnforce) {
      host_->Count(Counter::kDecisionsApplied);
    } else {
      would_modify_ = true;
      host_->Count(Counter::kDecisionsWouldApply);
    }
  }
  if (modified_ || headers_modified_) host_->Count(Counter::kPagesModified);
  if (would_modify_) host_->Count(Counter::kPagesWouldModify);
  for (const PackDecision& d : decisions_) host_->Decision(url_, d);
}

void RulePackFilter::EndDocument() {
  if (claimed_only_) {
    // The page was not evaluated by this pack: no page counters, only the
    // rules that another pack kept off it.
    claimed_only_ = false;
    decisions_ = std::move(claimed_);
    claimed_.clear();
    for (const PackDecision& d : decisions_) {
      host_->Count(Counter::kDecisionsClaimed);
      host_->Decision(url_, d);
    }
    return;
  }
  if (!active_) return;
  if (text_element_ != nullptr) FinishTextElement();
  if (!head_decided_) DecideHead();
  DecideDocumentEnd();
  FinalizeHeadDecisions();
  FinalizeDocumentDecisions();
  for (PackDecision& d : claimed_) decisions_.push_back(std::move(d));
  claimed_.clear();
  Report();
  active_ = false;
}

}  // namespace pagespeed::packs
