// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The Markdown-versus-HTML choice on Accept q-values, asserted against the
// shared vector file. The same file, byte for byte, is asserted by the
// optimizer's test suite; kVectorFileHash pins it in both repositories so that
// one copy cannot change without the other.

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "net/instaweb/rewriter/public/agent_optimize_vary_filter.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kVectorPath[] =
    "/test/net/instaweb/rewriter/testdata/accept_markdown_vectors.txt";

// FNV-1a 64 of the vector file with carriage returns removed. The same
// constant is in the optimizer's
// test/lib/classify/accept_markdown_preference_test.cc. When the vectors
// change, change both copies of the file and both pins together.
const uint64_t kVectorFileHash = 0xffced6346df2fffaULL;
const int kVectorCount = 35;

struct Vector {
  int id = 0;
  bool markdown = false;
  std::vector<GoogleString> lines;  // empty = no Accept header
};

GoogleString ReadVectorFile() {
  std::ifstream in(StrCat(GTestSrcDir(), kVectorPath).c_str(),
                   std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  GoogleString contents = buffer.str();
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'),
                 contents.end());
  return contents;
}

uint64_t Fnv1a64(StringPiece data) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : data) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// "<id> <MD|HTML> [<line>]..." per line; '#' lines and blank lines skipped.
bool ParseVectors(StringPiece contents, std::vector<Vector>* out) {
  size_t start = 0;
  while (start < contents.size()) {
    size_t nl = contents.find('\n', start);
    if (nl == StringPiece::npos) {
      nl = contents.size();
    }
    StringPiece row(contents.substr(start, nl - start));
    start = nl + 1;
    if (row.empty() || row[0] == '#') {
      continue;
    }
    Vector v;
    size_t sp = row.find(' ');
    if (sp == StringPiece::npos) {
      return false;
    }
    v.id = std::stoi(GoogleString(row.substr(0, sp)));
    size_t verdict_end = row.find(' ', sp + 1);
    StringPiece verdict(row.substr(
        sp + 1,
        verdict_end == StringPiece::npos ? StringPiece::npos
                                         : verdict_end - sp - 1));
    if (verdict == "MD") {
      v.markdown = true;
    } else if (verdict != "HTML") {
      return false;
    }
    size_t pos = verdict_end;
    while (pos != StringPiece::npos && pos < row.size()) {
      size_t open = row.find('<', pos);
      if (open == StringPiece::npos) {
        break;
      }
      size_t close = row.find('>', open + 1);
      if (close == StringPiece::npos) {
        return false;
      }
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

bool Decide(const std::vector<GoogleString>& lines) {
  AcceptMarkdownPreference preference;
  for (const GoogleString& line : lines) {
    preference.AddLine(line);
  }
  return preference.PrefersMarkdown();
}

bool DecideOne(StringPiece accept) {
  AcceptMarkdownPreference preference;
  preference.AddLine(accept);
  return preference.PrefersMarkdown();
}

TEST(AcceptMarkdownPreferenceTest, VectorFileIsTheSharedCopy) {
  const GoogleString contents = ReadVectorFile();
  ASSERT_FALSE(contents.empty()) << "cannot read " << kVectorPath;
  EXPECT_EQ(kVectorFileHash, Fnv1a64(contents))
      << "the vector file changed: change the optimizer's copy to match, "
         "then update kVectorFileHash in both test suites to 0x"
      << std::hex << Fnv1a64(contents);
}

TEST(AcceptMarkdownPreferenceTest, AllVectors) {
  const std::vector<Vector> vectors = LoadVectors();
  ASSERT_EQ(kVectorCount, static_cast<int>(vectors.size()));
  for (int i = 0; i < kVectorCount; ++i) {
    const Vector& v = vectors[i];
    EXPECT_EQ(i + 1, v.id);
    EXPECT_EQ(v.markdown, Decide(v.lines)) << "vector " << v.id;
  }
}

// The same vectors through RequestHeaders, one Accept header per line.
TEST(AcceptMarkdownPreferenceTest, AllVectorsThroughRequestHeaders) {
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestPrefersMarkdown(nullptr));
  for (const Vector& v : LoadVectors()) {
    RequestHeaders headers;
    for (const GoogleString& line : v.lines) {
      headers.Add(HttpAttributes::kAccept, line);
    }
    EXPECT_EQ(v.markdown, AgentOptimizeVaryFilter::RequestPrefersMarkdown(
                              &headers))
        << "vector " << v.id;
  }
}

// Joining the lines with a comma (RFC 9110 section 5.3) gives the same verdict
// as feeding them one by one.
TEST(AcceptMarkdownPreferenceTest, CombinedValueMatchesLines) {
  for (const Vector& v : LoadVectors()) {
    GoogleString combined;
    for (size_t i = 0; i < v.lines.size(); ++i) {
      if (i > 0) {
        combined += ", ";
      }
      combined += v.lines[i];
    }
    EXPECT_EQ(v.markdown, DecideOne(combined)) << "vector " << v.id;
  }
}

TEST(AcceptMarkdownPreferenceTest, QuotedParametersAndEmptyElements) {
  EXPECT_TRUE(DecideOne(
      "text/markdown;variant=\"GFM, strict\";q=0.9, text/html;q=0.5"));
  EXPECT_TRUE(DecideOne("text/markdown;x=\"a\\\"b\";q=0.9, text/html;q=0.5"));
  EXPECT_TRUE(DecideOne(", ,text/markdown,, text/html;q=0.5,"));
  // Unterminated quote, quoted weight, a range without a subtype.
  EXPECT_FALSE(DecideOne("text/markdown;x=\"open"));
  EXPECT_FALSE(DecideOne("text/markdown;q=\"0.9\""));
  EXPECT_FALSE(DecideOne("text/markdown, text/"));
  EXPECT_FALSE(DecideOne("text/markdown, */html;q=0.1"));
  EXPECT_FALSE(DecideOne("text/markdown;q=0.9;q=0.9"));
  // A malformed range anywhere makes the whole header malformed.
  EXPECT_FALSE(DecideOne("text/markdown, image/webp;q=2"));
  // Several text/html ranges resolve to the highest q.
  EXPECT_FALSE(DecideOne("text/markdown;q=0.8, text/html;q=0.1, text/html"));
}

// The Vary signal keeps the presence test: a request that lists text/markdown
// without preferring it still gets Vary: Accept.
TEST(AcceptMarkdownPreferenceTest, VarySignalStaysOnPresence) {
  RequestHeaders headers;
  headers.Add(HttpAttributes::kAccept, "text/markdown;q=0");
  EXPECT_TRUE(AgentOptimizeVaryFilter::RequestAcceptsMarkdown(&headers));
  EXPECT_FALSE(AgentOptimizeVaryFilter::RequestPrefersMarkdown(&headers));
}

// Every truncation of every vector line and every single-byte overwrite of it
// parses without fault; the result is only required to be deterministic.
TEST(AcceptMarkdownPreferenceTest, MutatedVectorsParse) {
  const char kBytes[] = {',', ';', '=', '"',  '\\',   ' ',   '\t', '/',
                         '*', 'q', '0', '1',  '.',    '\0',  '\x7f', '\xff'};
  for (const Vector& v : LoadVectors()) {
    for (const GoogleString& line : v.lines) {
      for (size_t n = 0; n <= line.size(); ++n) {
        StringPiece prefix(line.data(), n);
        EXPECT_EQ(DecideOne(prefix), DecideOne(prefix));
      }
      for (size_t i = 0; i < line.size(); ++i) {
        for (char b : kBytes) {
          GoogleString mutated = line;
          mutated[i] = b;
          EXPECT_EQ(DecideOne(mutated), DecideOne(mutated));
        }
      }
    }
  }
}

}  // namespace
}  // namespace net_instaweb
