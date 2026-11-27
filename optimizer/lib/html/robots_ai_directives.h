// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_HTML_ROBOTS_AI_DIRECTIVES_H_
#define PAGESPEED_LIB_HTML_ROBOTS_AI_DIRECTIVES_H_

#include <string>
#include <string_view>
#include <vector>

namespace net_instaweb {

// AI-crawler directive analysis for the /llms.txt builder.
// PURE (no I/O): the builder fetches robots.txt / response headers / page meta
// and feeds the bytes here.

struct RobotsAiPolicy {
  // True iff the site's robots.txt broadly disallows AI crawling at the root:
  // the wildcard `User-agent: *` group disallows "/", OR any recognized AI
  // crawler's own group disallows "/". When true the builder must NOT synthesize
  // /llms.txt at all (respect-leaning default — the file is itself an
  // AI-consumption artifact). This is a documented, conservative v1 policy.
  bool ai_blocked_site_wide = false;
};

// The recognized AI-crawler user-agent tokens (case-insensitive). Exposed for
// tests and so the policy is auditable in one place.
const std::vector<std::string>& KnownAiUserAgents();

// Analyze a robots.txt body for the site-wide AI block signal.
RobotsAiPolicy AnalyzeRobotsForAi(std::string_view robots_txt);

// Per-page exclusion: returns true if a page opts out of AI use via any of
//   - an `X-Robots-Tag` response header carrying `noai` / `noimageai` / `none`
//   - a `<meta name="robots">` (or `name="ai"`) content carrying `noai` / `none`
//   - a `Google-Extended` response header set to `none`
// Each argument is the corresponding raw value (empty if absent). Token match
// is case-insensitive and comma/space delimited so `noindex, noai` is caught.
bool PageExcludedByAiDirectives(std::string_view x_robots_tag,
                                std::string_view meta_robots,
                                std::string_view google_extended);

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_HTML_ROBOTS_AI_DIRECTIVES_H_
