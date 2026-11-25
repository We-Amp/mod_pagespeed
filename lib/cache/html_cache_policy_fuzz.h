// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_FUZZ_H_
#define PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_FUZZ_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lib/cache/html_cache_policy.h"

namespace pagespeed::fuzz {

// The pieces of the HTML cache policy fuzz harness that the seed-replay test
// reuses.  The harness itself is html_cache_policy_fuzz.cc; this header
// exists so a test can drive the same seeds and the same oracle without
// libFuzzer.

// Run every parser the harness covers over one input blob and check the
// oracles, aborting the process on a violation.  See the harness for the blob
// format.
void HtmlCachePolicyFuzzOne(std::string_view blob);

// What the store policy decides for a blob, for tests that check what the
// seeds cover.
HtmlStoreVerdict ClassifyHtmlCachePolicyFuzzBlob(std::string_view blob);

// The serve verdicts the harness computes for a blob's serve request (both
// serve modes; a live copy, a no-transform parent, a withheld copy), for
// tests that check what the seeds cover.
std::vector<HtmlServeVerdict> ServeHtmlCachePolicyFuzzBlob(
    std::string_view blob);

// The response header lines a blob carries, as the harness parses them, for a
// test that asks the existing gates about the same headers.
std::vector<std::pair<std::string, std::string>> HtmlCachePolicyFuzzBlobHeaders(
    std::string_view blob);

// The embedded seed set: valid inputs that between them reach every store
// refusal reason, a storable response, every personalisation signal, and an
// oversized header value.
const std::vector<std::string>& HtmlCachePolicyFuzzSeeds();

// Write the seeds as seed-NN files into `dir` (created if missing).  Returns
// 0 on success, 1 on failure with the cause on stderr.
int DumpHtmlCachePolicyFuzzCorpus(const std::string& dir);

}  // namespace pagespeed::fuzz

#endif  // PAGESPEED_LIB_CACHE_HTML_CACHE_POLICY_FUZZ_H_
