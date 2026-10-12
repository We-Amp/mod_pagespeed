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
// that pack, even when the provider has moved on to a newer version.
class RewriteDriverPackHost : public pagespeed::packs::PackHost {
 public:
  explicit RewriteDriverPackHost(RewriteDriver* driver);
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
  RulePackProvider* provider_ = nullptr;
  LoadedRulePack pack_;
  bool headers_flushed_ = false;
  DriverHeaders headers_;
  std::array<Variable*, pagespeed::packs::kCounterCount> counters_{};
};

// The rule_packs filter as the driver runs it. A driver (and its filter
// chain) is reused for many documents, while the pack may change between
// them, so this filter fetches the pack from the server's RulePackProvider at
// the start of every document and builds a RulePackFilter for it when the
// pack (or the directive mode) differs from the one the previous document
// used.
//
// Every HtmlFilter event goes to that filter, including the ones
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

  // The filter of the current (or last) document; null before the first.
  const pagespeed::packs::RulePackFilter* filter() const {
    return filter_.get();
  }
  const RewriteDriverPackHost& host() const { return host_; }

 private:
  RewriteDriver* driver_;
  RewriteDriverPackHost host_;
  std::unique_ptr<pagespeed::packs::RulePackFilter> filter_;
  std::shared_ptr<const pagespeed::packs::Pack> filter_pack_;
  pagespeed::packs::Mode filter_mode_ = pagespeed::packs::Mode::kReport;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_RULE_PACK_HOST_H_
