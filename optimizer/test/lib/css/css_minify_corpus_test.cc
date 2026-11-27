// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Replay the mod_pagespeed 1.15 CSS fuzz corpus through the 2.0
// streaming minifier (lib/css/css_minify.cc) with two oracles:
//   1. Replay: every corpus entry minifies without crash/exception, and the
//      output never grows beyond the input.
//   2. Idempotence: minify(minify(x)) == minify(x) for every corpus entry.
// The corpus entries live in css_fuzz_corpus.h (extracted verbatim from the
// 1.15 parser fuzz harness; provenance in that file's banner). DeepNesters()
// below is a verbatim port of the same harness's runtime-generated inputs.

#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "lib/css/css_minify.h"
#include "test/lib/css/css_fuzz_corpus.h"

namespace pagespeed::css {
namespace {

// Ported verbatim from DeepNesters() in mod_pagespeed 1.15's
// third_party/css_parser/src/webutil/css/parser_fuzz.cc: progressively deeper
// nested inputs probing recursion / depth guards without baking giant
// literals into the binary.
std::vector<std::string> DeepNesters() {
  std::vector<std::string> out;
  for (int depth : {16, 64, 256, 1024, 4096}) {
    out.emplace_back("a { width: " + std::string(depth, '(') + "1" +
                     std::string(depth, ')') + " }");
    out.emplace_back(std::string(depth, '{'));
    out.emplace_back("a { content: " + std::string(depth, '[') + "x" + " }");
  }
  // Nested group rules hammer the statement-recursion cap: beyond it the rest
  // must be consumed iteratively, whatever the depth. Both unterminated and
  // terminated forms.
  for (int depth : {16, 64, 256, 1024}) {
    std::string open;
    for (int i = 0; i < depth; ++i) {
      open += "@supports (a:b){";
    }
    out.emplace_back(open);
    out.emplace_back(open + "e{f:g}" + std::string(depth, '}'));
  }
  return out;
}

std::vector<std::string> FullCorpus() {
  std::vector<std::string> all;
  for (const char* s : kCssFuzzCorpus) {
    all.emplace_back(s, std::strlen(s));
  }
  for (const char* s : kCssFuzzCorpusLocal) {
    all.emplace_back(s, std::strlen(s));
  }
  for (std::string& s : DeepNesters()) {
    all.push_back(std::move(s));
  }
  return all;
}

TEST(CssMinifyCorpusTest, ReplaysWithoutCrashAndNeverGrows) {
  const std::vector<std::string> corpus = FullCorpus();
  for (size_t i = 0; i < corpus.size(); ++i) {
    std::string output;
    ASSERT_TRUE(MinifyCss(corpus[i], &output)) << "corpus index " << i;
    EXPECT_LE(output.size(), corpus[i].size())
        << "minifier grew corpus index " << i;
  }
}

TEST(CssMinifyCorpusTest, Idempotent) {
  const std::vector<std::string> corpus = FullCorpus();
  for (size_t i = 0; i < corpus.size(); ++i) {
    std::string once;
    ASSERT_TRUE(MinifyCss(corpus[i], &once)) << "corpus index " << i;
    std::string twice;
    ASSERT_TRUE(MinifyCss(once, &twice)) << "corpus index " << i;
    EXPECT_EQ(once, twice) << "MinifyCss is not idempotent on corpus index "
                           << i;
  }
}

}  // namespace
}  // namespace pagespeed::css
