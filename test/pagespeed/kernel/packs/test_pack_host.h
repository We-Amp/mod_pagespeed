// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_PAGESPEED_KERNEL_PACKS_TEST_PACK_HOST_H_
#define TEST_PAGESPEED_KERNEL_PACKS_TEST_PACK_HOST_H_

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/strings/match.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/pack_host.h"
#include "pagespeed/kernel/packs/template.h"

namespace pagespeed::packs {

// Response headers as an ordered list of (name, value) lines. Names compare
// ASCII case-insensitively; Replace() drops every line of the name and
// appends the new ones at the end, as the rewriter's header class does.
class TestHeaders : public PackHost::Headers {
 public:
  std::vector<std::string> Values(std::string_view name) const override {
    std::vector<std::string> out;
    for (const auto& [n, v] : lines_) {
      if (absl::EqualsIgnoreCase(n, name)) out.push_back(v);
    }
    return out;
  }
  void Add(std::string_view name, std::string_view value) override {
    lines_.emplace_back(std::string(name), std::string(value));
  }
  void Replace(std::string_view name,
               const std::vector<std::string>& values) override {
    std::erase_if(lines_, [&](const auto& line) {
      return absl::EqualsIgnoreCase(line.first, name);
    });
    for (const std::string& v : values) Add(name, v);
  }
  const std::vector<std::pair<std::string, std::string>>& lines() const {
    return lines_;
  }

 private:
  std::vector<std::pair<std::string, std::string>> lines_;
};

// A host that records what the filter reports. By default it has no response
// headers (as on a server that has none to offer); give it some with
// SetResponseHeaders(). FlushHeaders() takes them away again, as the first
// flush does on a server.
class TestPackHost : public PackHost {
 public:
  explicit TestPackHost(bool debug_comments = false)
      : debug_comments_(debug_comments) {}

  Headers* response_headers() override {
    return has_headers_ && !headers_flushed_ ? &headers_ : nullptr;
  }
  void SetResponseHeaders(
      const std::vector<std::pair<std::string, std::string>>& lines) {
    has_headers_ = true;
    for (const auto& [name, value] : lines) headers_.Add(name, value);
  }
  void FlushHeaders() { headers_flushed_ = true; }
  // The response headers as they stand (also after a flush).
  const TestHeaders& headers() const { return headers_; }
  std::optional<std::string> RequestHeader(
      std::string_view name) const override {
    return std::nullopt;
  }
  void Log(LogLevel level, std::string_view message) override {
    log_.emplace_back(message);
  }
  void Count(Counter counter, int64_t delta) override {
    counts_[static_cast<int>(counter)] += delta;
  }
  void Decision(const PageUrl& url, const PackDecision& decision) override {
    decisions_.push_back(decision);
  }
  int64_t NowMs() const override { return 0; }
  bool debug_comments() const override { return debug_comments_; }
  // Claims go to `claims` (shared with the hosts of the other packs on the
  // same document); without one the host runs a single pack.
  void set_claims(ClaimSet* claims) { claims_ = claims; }
  const std::string* Claim(std::string_view target,
                           std::string_view pack_id) override {
    return claims_ == nullptr ? nullptr : claims_->Claim(target, pack_id);
  }

  int64_t count(Counter counter) const {
    return counts_[static_cast<int>(counter)];
  }
  const std::vector<PackDecision>& decisions() const { return decisions_; }
  const std::vector<std::string>& log() const { return log_; }

 private:
  bool debug_comments_;
  ClaimSet* claims_ = nullptr;
  bool has_headers_ = false;
  bool headers_flushed_ = false;
  TestHeaders headers_;
  std::array<int64_t, kCounterCount> counts_{};
  std::vector<PackDecision> decisions_;
  std::vector<std::string> log_;
};

}  // namespace pagespeed::packs

#endif  // TEST_PAGESPEED_KERNEL_PACKS_TEST_PACK_HOST_H_
