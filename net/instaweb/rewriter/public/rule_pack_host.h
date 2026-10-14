// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_HOST_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_HOST_H_

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/instaweb/rewriter/public/rule_pack_provider.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/pack_host.h"

namespace pagespeed::packs {
class RulePackFilter;
}  // namespace pagespeed::packs

namespace net_instaweb {

class HtmlCdataNode;
class HtmlCharactersNode;
class HtmlCommentNode;
class HtmlDirectiveNode;
class HtmlElement;
class HtmlIEDirectiveNode;
class ResponseHeaders;
class RewriteDriver;
class Statistics;
class Variable;

// The PackHost of the rule_packs filter on a RewriteDriver: the driver's
// response and request headers, its message handler, the server's
// statistics and clock, the decision log of the server's RulePackProvider,
// and debug comments when the debug filter is on.
//
// It carries the pack (and the hash of the file it came from) that it handed
// to the filter for the current document, so every decision it logs names
// that pack, even when the provider has moved on to a newer version. There is
// one per configured pack file; the hosts of one driver share the claim set
// of the document (`claims`, owned by the RulePackDriverFilter).
class RewriteDriverPackHost : public pagespeed::packs::PackHost {
 public:
  RewriteDriverPackHost(RewriteDriver* driver,
                        pagespeed::packs::ClaimSet* claims);
  ~RewriteDriverPackHost() override;

  // Registers the rule_pack_* statistics (CounterName() of every counter).
  static void InitStats(Statistics* statistics);

  // Starts a document run with `pack` from `provider`.
  void BeginDocument(RulePackProvider* provider, const LoadedRulePack& pack);
  // The first flush window of the document has gone out.
  void set_headers_flushed() { headers_flushed_ = true; }
  const LoadedRulePack& pack() const { return pack_; }

  // PackHost.
  Headers* response_headers() override;
  std::optional<std::string> RequestHeader(
      std::string_view name) const override;
  void Log(pagespeed::packs::LogLevel level, std::string_view message) override;
  void Count(pagespeed::packs::Counter counter, int64_t delta = 1) override;
  void Decision(const pagespeed::packs::PageUrl& url,
                const pagespeed::packs::PackDecision& decision) override;
  int64_t NowMs() const override;
  bool debug_comments() const override;
  const std::string* Claim(std::string_view target,
                           std::string_view pack_id) override;

 private:
  class DriverHeaders : public Headers {
   public:
    void set(ResponseHeaders* headers) { headers_ = headers; }
    std::vector<std::string> Values(std::string_view name) const override;
    void Add(std::string_view name, std::string_view value) override;
    void Replace(std::string_view name,
                 const std::vector<std::string>& values) override;

   private:
    ResponseHeaders* headers_ = nullptr;
  };

  RewriteDriver* driver_;
  pagespeed::packs::ClaimSet* claims_;
  RulePackProvider* provider_ = nullptr;
  LoadedRulePack pack_;
  bool headers_flushed_ = false;
  DriverHeaders headers_;
  std::array<Variable*, pagespeed::packs::kCounterCount> counters_{};
};

// The rule_packs filter as the driver runs it. A driver (and its filter
// chain) is reused for many documents, while the packs may change between
// them, so this filter fetches every configured pack (RulePackFile, in
// configuration order) from the server's RulePackProvider at the start of
// every document, and builds a RulePackFilter for a pack when the pack (or
// the directive mode) differs from the one the previous document used.
//
// The packs run in load order, each event going to the first pack's filter,
// then the second's, and so on; the first pack that selects a rule of a kind
// on the page owns that kind for the document (the claim set, cleared per
// document). A file whose pack has the id of a pack loaded earlier on the
// host is refused for as long as that is so (logged and counted once per
// process and file version). When two packs carry rules of one kind and the
// modes they can run it in differ, a warning says which pack owns it (once
// per process, host, pack pair and kind).
//
// Every HtmlFilter event goes to the filters, including the ones
// RulePackFilter does not override today (Cdata, Comment, IEDirective,
// Directive, RenderDone), so an override added there later is not silently
// dropped here.
class RulePackDriverFilter : public EmptyHtmlFilter {
 public:
  explicit RulePackDriverFilter(RewriteDriver* driver);
  ~RulePackDriverFilter() override;

  void DetermineEnabled(GoogleString* disabled_reason) override;
  void StartDocument() override;
  void StartElement(HtmlElement* element) override;
  void EndElement(HtmlElement* element) override;
  void Characters(HtmlCharactersNode* characters) override;
  void Cdata(HtmlCdataNode* cdata) override;
  void Comment(HtmlCommentNode* comment) override;
  void IEDirective(HtmlIEDirectiveNode* directive) override;
  void Directive(HtmlDirectiveNode* directive) override;
  void Flush() override;
  void RenderDone() override;
  void EndDocument() override;
  bool CanModifyUrls() override;
  ScriptUsage GetScriptUsage() const override;
  const char* Name() const override { return "RulePackFilter"; }

  // The filter of the `i`th pack file for the current (or last) document;
  // null before that file had a usable pack.
  const pagespeed::packs::RulePackFilter* filter(size_t i = 0) const;
  // The host of the `i`th pack file; the file must have been looked at.
  const RewriteDriverPackHost& host(size_t i = 0) const;

 private:
  // One configured pack file: its host, and the filter of its pack.
  struct PackRun;

  // Logs the overlap warnings of the packs that run on the host (in load
  // order), when that list changed since the last check.
  void CheckOverlaps(RulePackProvider* provider,
                     const std::vector<PackRun*>& loaded,
                     pagespeed::packs::Mode mode);

  RewriteDriver* driver_;
  pagespeed::packs::ClaimSet claims_;
  std::vector<std::unique_ptr<PackRun>> runs_;
  // What CheckOverlaps() last looked at.
  std::vector<const pagespeed::packs::Pack*> overlap_checked_;
  pagespeed::packs::Mode overlap_mode_ = pagespeed::packs::Mode::kReport;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_HOST_H_
