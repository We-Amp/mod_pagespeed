// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/rule_pack_host.h"

#include <memory>
#include <string>
#include <utility>

#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"

namespace net_instaweb {

using pagespeed::packs::Counter;
using pagespeed::packs::CounterName;
using pagespeed::packs::kCounterCount;

namespace {

StringPiece Piece(std::string_view s) {
  return StringPiece(s.data(), s.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// RewriteDriverPackHost
// ---------------------------------------------------------------------------

RewriteDriverPackHost::RewriteDriverPackHost(RewriteDriver* driver)
    : driver_(driver) {
  Statistics* statistics = driver_->statistics();
  for (int i = 0; i < kCounterCount; ++i) {
    counters_[i] = statistics == nullptr
                       ? nullptr
                       : statistics->FindVariable(
                             Piece(CounterName(static_cast<Counter>(i))));
  }
}

RewriteDriverPackHost::~RewriteDriverPackHost() = default;

void RewriteDriverPackHost::InitStats(Statistics* statistics) {
  for (int i = 0; i < kCounterCount; ++i) {
    statistics->AddVariable(Piece(CounterName(static_cast<Counter>(i))));
  }
}

void RewriteDriverPackHost::BeginDocument(RulePackProvider* provider,
                                          const LoadedRulePack& pack) {
  provider_ = provider;
  pack_ = pack;
  headers_flushed_ = false;
}

RewriteDriverPackHost::Headers* RewriteDriverPackHost::response_headers() {
  // Writable until the first flush window has gone out, as for
  // MetaTagFilter.
  if (headers_flushed_) return nullptr;
  ResponseHeaders* headers = driver_->mutable_response_headers();
  if (headers == nullptr) return nullptr;
  headers_.set(headers);
  return &headers_;
}

std::vector<std::string> RewriteDriverPackHost::DriverHeaders::Values(
    std::string_view name) const {
  std::vector<std::string> out;
  ConstStringStarVector values;
  if (headers_ != nullptr && headers_->Lookup(Piece(name), &values)) {
    for (const GoogleString* v : values) {
      out.push_back(v == nullptr ? std::string() : *v);
    }
  }
  return out;
}

void RewriteDriverPackHost::DriverHeaders::Add(std::string_view name,
                                               std::string_view value) {
  if (headers_ != nullptr) headers_->Add(Piece(name), Piece(value));
}

void RewriteDriverPackHost::DriverHeaders::Replace(
    std::string_view name, const std::vector<std::string>& values) {
  if (headers_ == nullptr) return;
  headers_->RemoveAll(Piece(name));
  for (const std::string& v : values) headers_->Add(Piece(name), v);
}

std::optional<std::string> RewriteDriverPackHost::RequestHeader(
    std::string_view name) const {
  const RequestHeaders* headers = driver_->request_headers();
  ConstStringStarVector values;
  if (headers == nullptr || !headers->Lookup(Piece(name), &values)) {
    return std::nullopt;
  }
  std::string joined;
  for (const GoogleString* v : values) {
    if (!joined.empty()) joined.append(", ");
    if (v != nullptr) joined.append(*v);
  }
  return joined;
}

void RewriteDriverPackHost::Log(pagespeed::packs::LogLevel level,
                                std::string_view message) {
  MessageType type = kInfo;
  switch (level) {
    case pagespeed::packs::LogLevel::kInfo:
      type = kInfo;
      break;
    case pagespeed::packs::LogLevel::kWarning:
      type = kWarning;
      break;
    case pagespeed::packs::LogLevel::kError:
      type = kError;
      break;
  }
  driver_->message_handler()->Message(
      type, "%.*s", static_cast<int>(message.size()), message.data());
}

void RewriteDriverPackHost::Count(Counter counter, int64_t delta) {
  const int i = static_cast<int>(counter);
  if (i < 0 || i >= kCounterCount || counters_[i] == nullptr) return;
  counters_[i]->Add(delta);
}

void RewriteDriverPackHost::Decision(
    const pagespeed::packs::PageUrl& url,
    const pagespeed::packs::PackDecision& decision) {
  if (provider_ == nullptr || pack_.pack == nullptr) return;
  switch (provider_->LogDecision(*driver_->options(), pack_, url, decision,
                                 NowMs(), driver_->message_handler())) {
    case RulePackProvider::LogResult::kWritten:
      Count(Counter::kDecisionLogLines);
      break;
    case RulePackProvider::LogResult::kDropped:
      Count(Counter::kDecisionLogDropped);
      break;
    case RulePackProvider::LogResult::kSuppressed:
    case RulePackProvider::LogResult::kDisabled:
      break;
  }
}

int64_t RewriteDriverPackHost::NowMs() const {
  return driver_->server_context()->timer()->NowMs();
}

bool RewriteDriverPackHost::debug_comments() const {
  return driver_->options()->Enabled(RewriteOptions::kDebug);
}

// ---------------------------------------------------------------------------
// RulePackDriverFilter
// ---------------------------------------------------------------------------

RulePackDriverFilter::RulePackDriverFilter(RewriteDriver* driver)
    : driver_(driver), host_(driver) {}

RulePackDriverFilter::~RulePackDriverFilter() = default;

void RulePackDriverFilter::DetermineEnabled(GoogleString* disabled_reason) {
  set_is_enabled(false);
  const RewriteOptions* options = driver_->options();
  const GoogleString& path = options->rule_pack_file();
  RulePackProvider* provider = driver_->server_context()->rule_pack_provider();
  if (path.empty() || provider == nullptr) {
    *disabled_reason = "rule_packs: no rule pack file";
    return;
  }
  RulePackProvider::LoadEvent event = RulePackProvider::LoadEvent::kNone;
  RulePackProvider::SignatureEvent signature_event =
      RulePackProvider::SignatureEvent::kNone;
  pagespeed::packs::PackTrust trust;
  trust.require_signed = options->rule_pack_require_signed();
  trust.keys = options->rule_pack_trust_keys();
  LoadedRulePack loaded = provider->Get(path, trust, driver_->message_handler(),
                                        &event, &signature_event);
  if (event != RulePackProvider::LoadEvent::kNone) {
    host_.Count(event == RulePackProvider::LoadEvent::kLoaded
                    ? Counter::kLoads
                    : Counter::kLoadErrors);
  }
  switch (signature_event) {
    case RulePackProvider::SignatureEvent::kNone:
      break;
    case RulePackProvider::SignatureEvent::kVerified:
      host_.Count(Counter::kLoadsSigned);
      break;
    case RulePackProvider::SignatureEvent::kRefusedUnsigned:
      host_.Count(Counter::kRefusedUnsigned);
      break;
    case RulePackProvider::SignatureEvent::kRefusedSignatureUnreadable:
      host_.Count(Counter::kRefusedSignatureUnreadable);
      break;
    case RulePackProvider::SignatureEvent::kRefusedPublisherMismatch:
      host_.Count(Counter::kRefusedPublisherMismatch);
      break;
    case RulePackProvider::SignatureEvent::kRefusedUnknownKey:
      host_.Count(Counter::kRefusedUnknownKey);
      break;
    case RulePackProvider::SignatureEvent::kRefusedBadSignature:
      host_.Count(Counter::kRefusedBadSignature);
      break;
  }
  if (loaded.pack == nullptr) {
    *disabled_reason = "rule_packs: no usable rule pack";
    return;
  }
  const pagespeed::packs::Mode mode =
      options->rule_pack_mode() == RewriteOptions::kRulePackModeEnforce
          ? pagespeed::packs::Mode::kEnforce
          : pagespeed::packs::Mode::kReport;
  // The filter of the previous document holds its pack for as long as it
  // lives; a new pack (or mode) gets a new filter.
  if (filter_ == nullptr || filter_pack_ != loaded.pack ||
      filter_mode_ != mode) {
    // The new filter is built before the old one goes, so the two are
    // never the same object.
    auto next = std::make_unique<pagespeed::packs::RulePackFilter>(
        driver_, loaded.pack, mode, &host_);
    filter_ = std::move(next);
    filter_pack_ = loaded.pack;
    filter_mode_ = mode;
  }
  host_.BeginDocument(provider, loaded);
  filter_->DetermineEnabled(disabled_reason);
  set_is_enabled(filter_->is_enabled());
}

void RulePackDriverFilter::StartDocument() { filter_->StartDocument(); }

void RulePackDriverFilter::StartElement(HtmlElement* element) {
  filter_->StartElement(element);
}

void RulePackDriverFilter::EndElement(HtmlElement* element) {
  filter_->EndElement(element);
}

void RulePackDriverFilter::Characters(HtmlCharactersNode* characters) {
  filter_->Characters(characters);
}

void RulePackDriverFilter::Cdata(HtmlCdataNode* cdata) {
  filter_->Cdata(cdata);
}

void RulePackDriverFilter::Comment(HtmlCommentNode* comment) {
  filter_->Comment(comment);
}

void RulePackDriverFilter::IEDirective(HtmlIEDirectiveNode* directive) {
  filter_->IEDirective(directive);
}

void RulePackDriverFilter::Directive(HtmlDirectiveNode* directive) {
  filter_->Directive(directive);
}

void RulePackDriverFilter::RenderDone() {
  if (filter_ != nullptr) filter_->RenderDone();
}

void RulePackDriverFilter::Flush() {
  filter_->Flush();
  host_.set_headers_flushed();
}

void RulePackDriverFilter::EndDocument() { filter_->EndDocument(); }

bool RulePackDriverFilter::CanModifyUrls() {
  return filter_ != nullptr && filter_->CanModifyUrls();
}

HtmlFilter::ScriptUsage RulePackDriverFilter::GetScriptUsage() const {
  return filter_ == nullptr ? kNeverInjectsScripts : filter_->GetScriptUsage();
}

}  // namespace net_instaweb
