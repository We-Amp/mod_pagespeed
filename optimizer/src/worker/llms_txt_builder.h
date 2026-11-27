// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef SRC_WORKER_LLMS_TXT_BUILDER_H_
#define SRC_WORKER_LLMS_TXT_BUILDER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pagespeed {

// Synthesizes a customer's /llms.txt site-index. PURE of process
// effects: the two blocking effects (own-origin HTTP GET, agent-variant cache
// read) are INJECTED, so the whole orchestration is hermetically unit-testable.
// The worker wires the real effects (FetchSubresource behind the G1 own-origin
// IP-pin; ReadAlternate of an existing kAgentMarkdown variant) — see
// Worker::HandleLlmsTxtBuild. No Chrome render is ever triggered here.

// Result of an own-origin GET. `ok` == (the fetch fulfilled with HTTP 200).
struct LlmsTxtFetchResult {
  bool ok = false;
  int status = 0;
  std::string body;
  std::string x_robots_tag;     // X-Robots-Tag response header (for §2.6)
  std::string google_extended;  // Google-Extended response header (for §2.6)
};

// Fetch an ABSOLUTE own-origin URL ("scheme://host/path"). The injected impl is
// responsible for the G1 own-origin pin + SSRF guard; it must NEVER follow a
// URL the builder did not derive from the configured origin.
using LlmsTxtFetchFn =
    std::function<LlmsTxtFetchResult(const std::string& url)>;

// Return rendered-DOM markdown for `url` if an agent_optimize variant already
// exists in cache (a free summary source, D-OQ4), else nullopt.
using LlmsTxtVariantReaderFn =
    std::function<std::optional<std::string>(const std::string& url)>;

struct LlmsTxtBuildOptions {
  std::string scheme = "https";  // own origin scheme
  std::string host;              // own origin authority (no scheme)
  std::string sitemap_path = "/sitemap.xml";
  std::vector<std::string>
      allow_paths;  // agent_optimize_paths; empty / "/" = all
  bool respect_ai_directives = true;
  std::size_t summary_fetch_cap = 200;  // max cheap (non-Chrome) HTML fetches
  bool stub_only =
      false;  // cold-miss fast path: sitemap-only, zero per-page fetch
};

enum class LlmsTxtBuildStatus : std::uint8_t {
  kOk,         // llms_txt is populated — write it
  kAiBlocked,  // robots.txt blocks AI site-wide (§2.6) — do NOT synthesize
  kNoSource,   // no reachable sitemap and no fallback pages — do NOT write
};

struct LlmsTxtBuildResult {
  LlmsTxtBuildStatus status = LlmsTxtBuildStatus::kNoSource;
  std::string llms_txt;
  std::array<std::byte, 32>
      sitemap_hash{};  // SHA-256 of the fetched sitemap bytes
};

// Self-bounds (independent of the formatter's per-field caps).
inline constexpr std::size_t kLlmsTxtMaxNestedSitemaps =
    10;                                                  // one-level fan-out
inline constexpr std::size_t kLlmsTxtMaxEntries = 5000;  // index entry ceiling

class LlmsTxtBuilder {
 public:
  LlmsTxtBuilder(LlmsTxtFetchFn fetch, LlmsTxtVariantReaderFn variant_reader);

  LlmsTxtBuildResult Build(const LlmsTxtBuildOptions& opts) const;

 private:
  LlmsTxtFetchFn fetch_;
  LlmsTxtVariantReaderFn variant_reader_;
};

}  // namespace pagespeed

#endif  // SRC_WORKER_LLMS_TXT_BUILDER_H_
