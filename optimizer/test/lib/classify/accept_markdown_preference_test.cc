// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Markdown-versus-HTML choice on Accept q-values, asserted against the
// shared vector file. The same file, byte for byte, is asserted by
// mod_pagespeed's test suite; kVectorFileHash pins it in both repositories so
// that one copy cannot change without the other.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

constexpr std::string_view kVectorPath =
    "test/lib/classify/testdata/accept_markdown_vectors.txt";

// FNV-1a 64 of the vector file with carriage returns removed. The same
// constant is in mod_pagespeed's
// test/net/instaweb/rewriter/accept_markdown_preference_test.cc. When the
// vectors change, change both copies of the file and both pins together.
constexpr uint64_t kVectorFileHash = 0x7d06687d90300a08ULL;
constexpr int kVectorCount = 42;

struct Vector {
  int id = 0;
  bool markdown = false;
  std::vector<std::string> lines;  // empty = no Accept header
};

std::string ReadVectorFile() {
  std::ifstream in(std::string(kVectorPath), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string contents = buffer.str();
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'),
                 contents.end());
  return contents;
}

uint64_t Fnv1a64(std::string_view data) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// "<id> <MD|HTML> [<line>]..." per line; '#' lines and blank lines skipped.
bool ParseVectors(std::string_view contents, std::vector<Vector>* out) {
  size_t start = 0;
  while (start < contents.size()) {
    size_t nl = contents.find('\n', start);
    if (nl == std::string_view::npos) nl = contents.size();
    std::string_view row = contents.substr(start, nl - start);
    start = nl + 1;
    if (row.empty() || row[0] == '#') continue;
    Vector v;
    size_t sp = row.find(' ');
    if (sp == std::string_view::npos) return false;
    v.id = std::stoi(std::string(row.substr(0, sp)));
    size_t verdict_end = row.find(' ', sp + 1);
    std::string_view verdict = row.substr(
        sp + 1, verdict_end == std::string_view::npos ? std::string_view::npos
                                                      : verdict_end - sp - 1);
    if (verdict == "MD") {
      v.markdown = true;
    } else if (verdict != "HTML") {
      return false;
    }
    size_t pos = verdict_end;
    while (pos != std::string_view::npos && pos < row.size()) {
      size_t open = row.find('<', pos);
      if (open == std::string_view::npos) break;
      size_t close = row.find('>', open + 1);
      if (close == std::string_view::npos) return false;
      v.lines.emplace_back(row.substr(open + 1, close - open - 1));
      pos = close + 1;
    }
    out->push_back(std::move(v));
  }
  return true;
}

std::vector<Vector> LoadVectors() {
  std::vector<Vector> vectors;
  EXPECT_TRUE(ParseVectors(ReadVectorFile(), &vectors));
  return vectors;
}

bool Decide(const std::vector<std::string>& lines) {
  std::vector<std::string_view> views(lines.begin(), lines.end());
  return AcceptPrefersMarkdown(views);
}

TEST(AcceptMarkdownPreferenceTest, VectorFileIsTheSharedCopy) {
  const std::string contents = ReadVectorFile();
  ASSERT_FALSE(contents.empty()) << "cannot read " << kVectorPath;
  EXPECT_EQ(Fnv1a64(contents), kVectorFileHash)
      << "the vector file changed: change mod_pagespeed's copy to match, then "
         "update kVectorFileHash in both test suites to 0x"
      << std::hex << Fnv1a64(contents);
}

TEST(AcceptMarkdownPreferenceTest, AllVectors) {
  const std::vector<Vector> vectors = LoadVectors();
  ASSERT_EQ(static_cast<int>(vectors.size()), kVectorCount);
  for (int i = 0; i < kVectorCount; ++i) {
    const Vector& v = vectors[i];
    EXPECT_EQ(v.id, i + 1);
    EXPECT_EQ(Decide(v.lines), v.markdown) << "vector " << v.id;
  }
}

// WantsAgentMarkdown takes one combined value; joining the lines with a comma
// (RFC 9110 section 5.3) gives the same verdict as feeding them one by one.
TEST(AcceptMarkdownPreferenceTest, CombinedValueMatchesLines) {
  for (const Vector& v : LoadVectors()) {
    std::string combined;
    for (size_t i = 0; i < v.lines.size(); ++i) {
      if (i > 0) combined += ", ";
      combined += v.lines[i];
    }
    EXPECT_EQ(WantsAgentMarkdown(combined), v.markdown) << "vector " << v.id;
  }
}

TEST(AcceptMarkdownPreferenceTest, QuotedParametersAndEmptyElements) {
  EXPECT_TRUE(WantsAgentMarkdown(
      "text/markdown;variant=\"GFM, strict\";q=0.9, text/html;q=0.5"));
  EXPECT_TRUE(
      WantsAgentMarkdown("text/markdown;x=\"a\\\"b\";q=0.9, text/html;q=0.5"));
  EXPECT_TRUE(WantsAgentMarkdown(", ,text/markdown,, text/html;q=0.5,"));
  // Unterminated quote, quoted weight, a range without a subtype.
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown;x=\"open"));
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown;q=\"0.9\""));
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown, text/"));
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown, */html;q=0.1"));
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown;q=0.9;q=0.9"));
  // A malformed range anywhere makes the whole header malformed.
  EXPECT_FALSE(WantsAgentMarkdown("text/markdown, image/webp;q=2"));
  // Several text/html ranges resolve to the highest q.
  EXPECT_FALSE(
      WantsAgentMarkdown("text/markdown;q=0.8, text/html;q=0.1, text/html"));
}

// Every truncation of every vector line and every single-byte overwrite of it
// parses without fault; the result is only required to be deterministic.
TEST(AcceptMarkdownPreferenceTest, MutatedVectorsParse) {
  const char kBytes[] = {',', ';', '=', '"', '\\', ' ',  '\t',   '/',
                         '*', 'q', '0', '1', '.',  '\0', '\x7f', '\xff'};
  for (const Vector& v : LoadVectors()) {
    for (const std::string& line : v.lines) {
      for (size_t n = 0; n <= line.size(); ++n) {
        std::string_view prefix(line.data(), n);
        EXPECT_EQ(WantsAgentMarkdown(prefix), WantsAgentMarkdown(prefix));
      }
      for (size_t i = 0; i < line.size(); ++i) {
        for (char b : kBytes) {
          std::string mutated = line;
          mutated[i] = b;
          EXPECT_EQ(WantsAgentMarkdown(mutated), WantsAgentMarkdown(mutated));
        }
      }
    }
  }
}

}  // namespace
}  // namespace pagespeed
