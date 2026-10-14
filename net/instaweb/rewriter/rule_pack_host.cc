// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "net/instaweb/rewriter/public/rule_pack_host.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
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
#include "pagespeed/kernel/packs/pack_overlap.h"
#include "pagespeed/kernel/packs/rule_kind.h"
#include "pagespeed/kernel/packs/rule_pack_filter.h"

namespace net_instaweb {

using pagespeed::packs::ClaimSet;
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

bool RulePackProvider::FirstTime(const std::string& key) {
  std::lock_guard<std::mutex> lock(once_mutex_);
  return once_keys_.insert(key).second;
}

RewriteDriverPackHost::RewriteDriverPackHost(RewriteDriver* driver,
                                             ClaimSet* claims)
    : driver_(driver), claims_(claims) {
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

const std::string* RewriteDriverPackHost::Claim(std::string_view target,
                                                std::string_view pack_id) {
  return claims_ == nullptr ? nullptr : claims_->Claim(target, pack_id);
}

// ---------------------------------------------------------------------------
// RulePackDriverFilter
// ---------------------------------------------------------------------------

struct RulePackDriverFilter::PackRun {
  PackRun(RewriteDriver* driver, ClaimSet* claims) : host(driver, claims) {}
  RewriteDriverPackHost host;
  std::unique_ptr<pagespeed::packs::RulePackFilter> filter;
  std::shared_ptr<const pagespeed::packs::Pack> pack;
  pagespeed::packs::Mode mode = pagespeed::packs::Mode::kReport;
  // The filter runs on the current document.
  bool enabled = false;
};

RulePackDriverFilter::RulePackDriverFilter(RewriteDriver* driver)
    : driver_(driver) {}

RulePackDriverFilter::~RulePackDriverFilter() = default;

const pagespeed::packs::RulePackFilter* RulePackDriverFilter::filter(
    size_t i) const {
  return i < runs_.size() ? runs_[i]->filter.get() : nullptr;
}

const RewriteDriverPackHost& RulePackDriverFilter::host(size_t i) const {
  return runs_[i]->host;
}

namespace {

void CountGet(RewriteDriverPackHost* host, RulePackProvider::LoadEvent event,
              RulePackProvider::SignatureEvent signature_event) {
  if (event != RulePackProvider::LoadEvent::kNone) {
    host->Count(event == RulePackProvider::LoadEvent::kLoaded
                    ? Counter::kLoads
                    : Counter::kLoadErrors);
  }
  switch (signature_event) {
    case RulePackProvider::SignatureEvent::kNone:
      break;
    case RulePackProvider::SignatureEvent::kVerified:
      host->Count(Counter::kLoadsSigned);
      break;
    case RulePackProvider::SignatureEvent::kRefusedUnsigned:
      host->Count(Counter::kRefusedUnsigned);
      break;
    case RulePackProvider::SignatureEvent::kRefusedSignatureUnreadable:
      host->Count(Counter::kRefusedSignatureUnreadable);
      break;
    case RulePackProvider::SignatureEvent::kRefusedPublisherMismatch:
      host->Count(Counter::kRefusedPublisherMismatch);
      break;
    case RulePackProvider::SignatureEvent::kRefusedUnknownKey:
      host->Count(Counter::kRefusedUnknownKey);
      break;
    case RulePackProvider::SignatureEvent::kRefusedBadSignature:
      host->Count(Counter::kRefusedBadSignature);
      break;
  }
}

// Identifies the virtual host in once-per-process keys: each has its own
// server context for the life of the process.
std::string VhostKey(const RewriteDriver* driver) {
  return absl::StrCat(reinterpret_cast<uintptr_t>(driver->server_context()));
}

}  // namespace

void RulePackDriverFilter::DetermineEnabled(GoogleString* disabled_reason) {
  set_is_enabled(false);
  claims_.Clear();
  for (auto& run : runs_) run->enabled = false;
  const RewriteOptions* options = driver_->options();
  const std::vector<GoogleString>& paths = options->rule_pack_files();
  RulePackProvider* provider = driver_->server_context()->rule_pack_provider();
  if (paths.empty() || provider == nullptr) {
    *disabled_reason = "rule_packs: no rule pack file";
    return;
  }
  while (runs_.size() < paths.size()) {
    runs_.push_back(std::make_unique<PackRun>(driver_, &claims_));
  }
  pagespeed::packs::PackTrust trust;
  trust.require_signed = options->rule_pack_require_signed();
  // Built once per document; every pack file is checked against it, so the
  // built-in key is in each pack's trust list exactly once.
  trust.keys =
      pagespeed::packs::WithBuiltInTrustKey(options->rule_pack_trust_keys());
  const pagespeed::packs::Mode mode =
      options->rule_pack_mode() == RewriteOptions::kRulePackModeEnforce
          ? pagespeed::packs::Mode::kEnforce
          : pagespeed::packs::Mode::kReport;
  // The runs whose pack is in use on this host, in load order.
  std::vector<PackRun*> loaded_runs;
  std::vector<const GoogleString*> loaded_paths;
  loaded_runs.reserve(paths.size());
  loaded_paths.reserve(paths.size());
  bool any = false;
  for (size_t i = 0; i < paths.size(); ++i) {
    PackRun& run = *runs_[i];
    RulePackProvider::LoadEvent event = RulePackProvider::LoadEvent::kNone;
    RulePackProvider::SignatureEvent signature_event =
        RulePackProvider::SignatureEvent::kNone;
    LoadedRulePack loaded = provider->Get(
        paths[i], trust, driver_->message_handler(), &event, &signature_event);
    CountGet(&run.host, event, signature_event);
    GoogleString reason;
    if (loaded.pack == nullptr) {
      reason = "rule_packs: no usable rule pack";
    } else {
      const GoogleString* first_path = nullptr;
      for (size_t j = 0; j < loaded_runs.size(); ++j) {
        if (loaded_runs[j]->pack->info.id == loaded.pack->info.id) {
          first_path = loaded_paths[j];
        }
      }
      if (first_path != nullptr) {
        reason = "rule_packs: duplicate pack id";
        if (provider->FirstTime(absl::StrCat("duplicate-id\n",
                                             VhostKey(driver_), "\n", paths[i],
                                             "\n", loaded.sha256))) {
          run.host.Count(Counter::kRefusedDuplicateId);
          driver_->message_handler()->Message(
              kWarning,
              "rule pack refused (duplicate id): %s@%s sha256=%s path=%s; "
              "the pack in %s has the same id",
              loaded.pack->info.id.c_str(), loaded.pack->info.version.c_str(),
              loaded.sha256.c_str(), paths[i].c_str(), first_path->c_str());
        }
      } else {
        // The filter of the previous document holds its pack for as long as
        // it lives; a new pack (or mode) gets a new filter.
        if (run.filter == nullptr || run.pack != loaded.pack ||
            run.mode != mode) {
          // The new filter is built before the old one goes, so the two are
          // never the same object.
          auto next = std::make_unique<pagespeed::packs::RulePackFilter>(
              driver_, loaded.pack, mode, &run.host);
          run.filter = std::move(next);
          run.pack = loaded.pack;
          run.mode = mode;
        }
        loaded_runs.push_back(&run);
        loaded_paths.push_back(&paths[i]);
        run.host.BeginDocument(provider, loaded);
        run.filter->DetermineEnabled(&reason);
        run.enabled = run.filter->is_enabled();
        any = any || run.enabled;
      }
    }
    // The first file's reason is the one a disabled filter reports.
    if (i == 0) *disabled_reason = reason;
  }
  if (loaded_runs.size() > 1) CheckOverlaps(provider, loaded_runs, mode);
  set_is_enabled(any);
}

void RulePackDriverFilter::CheckOverlaps(RulePackProvider* provider,
                                         const std::vector<PackRun*>& loaded,
                                         pagespeed::packs::Mode mode) {
  std::vector<const pagespeed::packs::Pack*> packs;
  packs.reserve(loaded.size());
  for (const PackRun* run : loaded) packs.push_back(run->pack.get());
  if (packs == overlap_checked_ && mode == overlap_mode_) return;
  overlap_checked_ = packs;
  overlap_mode_ = mode;
  for (const pagespeed::packs::PackOverlap& overlap :
       pagespeed::packs::FindOverlaps(packs, mode)) {
    const pagespeed::packs::Pack& first = *packs[overlap.first];
    const pagespeed::packs::Pack& later = *packs[overlap.later];
    if (!provider->FirstTime(absl::StrCat("overlap\n", VhostKey(driver_), "\n",
                                          first.info.id, "\n", later.info.id,
                                          "\n", overlap.kind->name()))) {
      continue;
    }
    loaded[overlap.later]->host.Log(
        pagespeed::packs::LogLevel::kWarning,
        pagespeed::packs::OverlapMessage(first, later, overlap));
  }
}

void RulePackDriverFilter::StartDocument() {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->StartDocument();
  }
}

void RulePackDriverFilter::StartElement(HtmlElement* element) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->StartElement(element);
  }
}

void RulePackDriverFilter::EndElement(HtmlElement* element) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->EndElement(element);
  }
}

void RulePackDriverFilter::Characters(HtmlCharactersNode* characters) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->Characters(characters);
  }
}

void RulePackDriverFilter::Cdata(HtmlCdataNode* cdata) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->Cdata(cdata);
  }
}

void RulePackDriverFilter::Comment(HtmlCommentNode* comment) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->Comment(comment);
  }
}

void RulePackDriverFilter::IEDirective(HtmlIEDirectiveNode* directive) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->IEDirective(directive);
  }
}

void RulePackDriverFilter::Directive(HtmlDirectiveNode* directive) {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->Directive(directive);
  }
}

void RulePackDriverFilter::RenderDone() {
  for (auto& run : runs_) {
    if (run->filter != nullptr) run->filter->RenderDone();
  }
}

void RulePackDriverFilter::Flush() {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->Flush();
    run->host.set_headers_flushed();
  }
}

void RulePackDriverFilter::EndDocument() {
  for (auto& run : runs_) {
    if (run->enabled) run->filter->EndDocument();
  }
}

bool RulePackDriverFilter::CanModifyUrls() {
  for (auto& run : runs_) {
    if (run->filter != nullptr && run->filter->CanModifyUrls()) return true;
  }
  return false;
}

HtmlFilter::ScriptUsage RulePackDriverFilter::GetScriptUsage() const {
  for (const auto& run : runs_) {
    if (run->filter != nullptr &&
        run->filter->GetScriptUsage() == kMayInjectScripts) {
      return kMayInjectScripts;
    }
  }
  return kNeverInjectsScripts;
}

}  // namespace net_instaweb
