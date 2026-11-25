// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Replays the HTML cache policy fuzz harness's embedded seeds, so the seeds
// and the harness's oracles are exercised on every CI run without libFuzzer.
// The test dumps the corpus to a temporary directory exactly as
// `html_cache_policy_fuzz --dump_corpus=DIR` does, reads every file back, and
// runs it through the harness entry point (which aborts the process on an
// oracle violation).  It also checks what the seeds cover, so a seed that
// stops reaching its reason is a red test rather than a silent loss.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/cache/headers_sidecar.h"
#include "lib/cache/html_cache_policy.h"
#include "lib/cache/html_cache_policy_fuzz.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/cache/vary_storability.h"
#include "lib/classify/alternate_metadata.h"
#include "test/test_util/temp_dir.h"

namespace pagespeed {
namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

std::string LowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
  }
  return out;
}

// A one-line view of a seed for a failure message: the header block only.
std::string Describe(const std::string& seed) {
  std::string out = seed.substr(0, seed.find("\n\n"));
  for (char& c : out) {
    if (c == '\n') c = '|';
  }
  return out.substr(0, 200);
}

TEST(HtmlCachePolicyFuzzSeedsTest, DumpedCorpusRoundTripsAndReplaysClean) {
  test::TempDir temp;
  const std::string dir = temp.path() + "/corpus";
  ASSERT_EQ(fuzz::DumpHtmlCachePolicyFuzzCorpus(dir), 0);

  const std::vector<std::string>& seeds = fuzz::HtmlCachePolicyFuzzSeeds();
  ASSERT_GE(seeds.size(), 12u);

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  ASSERT_EQ(files.size(), seeds.size());

  size_t replayed = 0;
  for (size_t i = 0; i < files.size(); ++i) {
    const std::string bytes = ReadFile(files[i]);
    EXPECT_EQ(bytes, seeds[i]) << files[i];
    fuzz::HtmlCachePolicyFuzzOne(bytes);
    ++replayed;
  }
  EXPECT_EQ(replayed, seeds.size());
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsCoverEveryStoreReasonAndSignal) {
  std::set<HtmlStoreReason> reasons;
  std::set<std::string> signals;
  size_t storable = 0;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    const HtmlStoreVerdict v = fuzz::ClassifyHtmlCachePolicyFuzzBlob(seed);
    reasons.insert(v.reason);
    if (v.stores()) ++storable;
    if (v.reason == HtmlStoreReason::kPersonalisationSignal) {
      signals.insert(v.detail);
    }
  }
  for (uint8_t i = 0; i < kHtmlStoreReasonCount; ++i) {
    // The harness's main verdict runs in a mode that acts, so never off.
    if (static_cast<HtmlStoreReason>(i) == HtmlStoreReason::kMode) continue;
    EXPECT_TRUE(reasons.count(static_cast<HtmlStoreReason>(i)) != 0)
        << "no seed reaches "
        << HtmlStoreReasonName(static_cast<HtmlStoreReason>(i));
  }
  EXPECT_GE(storable, 2u);  // a plain store and a no-transform store
  ASSERT_EQ(HtmlPersonalisationSignalNames().size(), 11u);
  for (std::string_view signal : HtmlPersonalisationSignalNames()) {
    EXPECT_TRUE(signals.count(std::string(signal)) != 0)
        << "no seed reaches " << signal;
  }
}

// The differential property, stated independently of the harness's own
// oracle: whatever the policy stores, the gates that already guard the
// shared cache accept too.  The policy may be stricter than them, never
// looser.  A seed that stops holding this is a red test with its header
// block in the message, not an abort.
TEST(HtmlCachePolicyFuzzSeedsTest, WhatThePolicyStoresTheExistingGatesAccept) {
  size_t stored = 0;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    const HtmlStoreVerdict v = fuzz::ClassifyHtmlCachePolicyFuzzBlob(seed);
    if (!v.stores()) continue;
    ++stored;
    const auto headers = fuzz::HtmlCachePolicyFuzzBlobHeaders(seed);
    std::vector<HeaderField> fields;
    std::string combined_vary;
    bool sets_cookie = false;
    OriginCacheControl cc;
    for (const auto& [name, value] : headers) {
      fields.push_back({name, value});
      const std::string lower = LowerAscii(name);
      if (lower == "vary") {
        if (!combined_vary.empty()) combined_vary.push_back(',');
        combined_vary.append(value);
      } else if (lower == "set-cookie" || lower == "set-cookie2") {
        sets_cookie = true;
      } else if (lower == "cache-control") {
        AccumulateOriginCacheControl(value, &cc);
      }
    }
    EXPECT_FALSE(VaryUncacheable(combined_vary)) << Describe(seed);
    EXPECT_TRUE(ClassifyForHeadersSidecar(fields).swap_eligible())
        << Describe(seed);
    EXPECT_FALSE(sets_cookie) << Describe(seed);
    EXPECT_EQ(cc.cc_flags & (AlternateMetadata::kCCOriginPrivate |
                             AlternateMetadata::kCCOriginNoStore),
              0)
        << Describe(seed);
    // And the policy's own statement of the same facts agrees.
    EXPECT_EQ(v.cc_flags & (AlternateMetadata::kCCOriginPrivate |
                            AlternateMetadata::kCCOriginNoStore),
              0)
        << Describe(seed);
    EXPECT_TRUE(v.sidecar_evaluated && v.sidecar.swap_eligible())
        << Describe(seed);
  }
  EXPECT_GE(stored, 4u);
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsReachTheRewrittenShapeAndDateForms) {
  bool rewritten = false;
  bool obsolete_dates = false;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    if (seed.find(".pagespeed.") != std::string::npos) {
      rewritten = true;
      EXPECT_EQ(fuzz::ClassifyHtmlCachePolicyFuzzBlob(seed).reason,
                HtmlStoreReason::kUrl);
    }
    if (seed.find("06-Nov-94") != std::string::npos) {
      obsolete_dates = true;
      const HtmlStoreVerdict v = fuzz::ClassifyHtmlCachePolicyFuzzBlob(seed);
      EXPECT_TRUE(v.stores()) << v.reason_code();
      EXPECT_EQ(v.lifetime_s, 60u);
    }
  }
  EXPECT_TRUE(rewritten);
  EXPECT_TRUE(obsolete_dates);
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsIncludeAnOversizedHeaderValue) {
  size_t longest = 0;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    size_t run = 0;
    for (char c : seed) {
      run = (c == '\n') ? 0 : run + 1;
      if (run > longest) longest = run;
    }
  }
  EXPECT_GE(longest, 70000u);
}

TEST(HtmlCachePolicyFuzzSeedsTest, MalformedAndEmptyInputsAreHandled) {
  for (std::string_view blob :
       {std::string_view(), std::string_view("\n\n"), std::string_view(":"),
        std::string_view("::\n::"), std::string_view("a"),
        std::string_view("\0\0\0", 3), std::string_view("x: y\n\n\n\n"),
        std::string_view("\r\n\r\n"),
        std::string_view(":status: 99999999999999999999\n\n")}) {
    fuzz::HtmlCachePolicyFuzzOne(blob);
  }
}

TEST(HtmlCachePolicyFuzzSeedsTest, SeedsReachTheServeRequestReasons) {
  std::set<HtmlServeReason> reasons;
  bool markdown = false;
  for (const std::string& seed : fuzz::HtmlCachePolicyFuzzSeeds()) {
    for (const HtmlServeVerdict& v : fuzz::ServeHtmlCachePolicyFuzzBlob(seed)) {
      reasons.insert(v.reason);
      markdown |= v.action == HtmlServeAction::kServeMarkdown;
    }
  }
  EXPECT_TRUE(markdown);
  for (HtmlServeReason r :
       {HtmlServeReason::kMethod, HtmlServeReason::kRange,
        HtmlServeReason::kCors, HtmlServeReason::kNoTransform,
        HtmlServeReason::kWithheld, HtmlServeReason::kClientNoCache,
        HtmlServeReason::kParentStale, HtmlServeReason::kFresh}) {
    EXPECT_TRUE(reasons.count(r) != 0)
        << "no seed reaches " << HtmlServeReasonName(r);
  }
}

}  // namespace
}  // namespace pagespeed
