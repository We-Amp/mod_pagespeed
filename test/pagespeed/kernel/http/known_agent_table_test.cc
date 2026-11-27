// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/kernel/http/known_agent_table.h"

#include <set>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/http/agent_vectors.h"

namespace net_instaweb {
namespace {

bool Known(StringPiece user_agent) {
  return KnownAgentMatcher::Default().Match(user_agent) != nullptr;
}

// The rule as the vector file states it, written the slow way: every
// occurrence of every token, both ends checked.
bool ReferenceKnown(StringPiece user_agent) {
  const GoogleString ua = [&] {
    GoogleString out = user_agent.as_string();
    LowerString(&out);
    return out;
  }();
  size_t size = 0;
  const KnownAgent* table = KnownAgentTable(&size);
  for (size_t t = 0; t < size; ++t) {
    GoogleString token = table[t].token;
    LowerString(&token);
    for (size_t i = ua.find(token); i != GoogleString::npos;
         i = ua.find(token, i + 1)) {
      const size_t j = i + token.size();
      if ((i == 0 || IsKnownAgentTokenBoundary(ua[i - 1])) &&
          (j == ua.size() || IsKnownAgentTokenBoundary(ua[j]))) {
        return true;
      }
    }
  }
  return false;
}

GoogleString Lower(StringPiece s) {
  GoogleString out = s.as_string();
  LowerString(&out);
  return out;
}

// The compiled-in table holds exactly the tokens the shared vectors are
// written against, so a vector file and a snapshot cannot drift apart.
TEST(KnownAgentTableTest, TableIsTheVectorFilesTokenList) {
  const AgentVectors vectors = LoadAgentVectors();
  std::set<GoogleString> from_file(vectors.tokens.begin(),
                                   vectors.tokens.end());
  ASSERT_EQ(vectors.tokens.size(), from_file.size());
  size_t size = 0;
  const KnownAgent* table = KnownAgentTable(&size);
  std::set<GoogleString> compiled;
  for (size_t i = 0; i < size; ++i) {
    EXPECT_EQ(strlen(table[i].token), table[i].length) << table[i].token;
    compiled.insert(table[i].token);
  }
  EXPECT_EQ(from_file, compiled);
  EXPECT_EQ(size, KnownAgentMatcher::Default().size());
}

// Every shared vector, through the compiled-in table.
TEST(KnownAgentTableTest, EverySharedVector) {
  const AgentVectors vectors = LoadAgentVectors();
  ASSERT_GT(vectors.vectors.size(), 100u);
  for (const AgentVector& v : vectors.vectors) {
    ASSERT_TRUE(v.expected_class == 0 || v.expected_class == 3) << v.line;
    EXPECT_EQ(v.expected_class == 3, Known(v.user_agent))
        << "agent_vectors.txt:" << v.line << ": " << v.user_agent;
  }
}

// Every table token on its own, in any ASCII case, padded by each boundary.
TEST(KnownAgentTableTest, EveryTokenOnEveryBoundary) {
  size_t size = 0;
  const KnownAgent* table = KnownAgentTable(&size);
  for (size_t i = 0; i < size; ++i) {
    const GoogleString token = table[i].token;
    EXPECT_TRUE(Known(token)) << token;
    EXPECT_TRUE(Known(Lower(token))) << token;
    GoogleString upper = token;
    UpperString(&upper);
    EXPECT_TRUE(Known(upper)) << token;
    for (const char* b : {" ", "(", ")", ";", "/"}) {
      EXPECT_TRUE(Known(StrCat("x", b, token, b, "y"))) << token << b;
    }
    // A letter, digit or other non-boundary byte on either side: never
    // this token, though a shorter token may still end on a boundary inside
    // it ("iaskspider" in "iaskspider/2.0x"); the slow reference decides.
    for (const char* n : {"x", "0", "-", "_", ".", ",", ":", "+", "\t"}) {
      for (const GoogleString& ua : {StrCat(n, token), StrCat(token, n)}) {
        const KnownAgent* m = KnownAgentMatcher::Default().Match(ua);
        EXPECT_EQ(ReferenceKnown(ua), m != nullptr) << ua;
        if (m != nullptr) {
          EXPECT_NE(token, m->token) << ua;
        }
      }
    }
  }
}

TEST(KnownAgentTableTest, NeverASubstring) {
  EXPECT_TRUE(Known("Mozilla/5.0 (compatible; GPTBot/1.2)"));
  EXPECT_FALSE(Known("Mozilla/5.0 (compatible; NotGPTBot/1.2)"));
  EXPECT_FALSE(Known("GPTBotX/1.0"));
  EXPECT_FALSE(Known("GPTBo"));
  EXPECT_FALSE(Known("PTBot"));
  EXPECT_FALSE(Known("GPT Bot"));
  // A table token inside a URL path segment still sits on '/' boundaries:
  // the rule is lexical, and the User-Agent is the client's claim anyway.
  EXPECT_TRUE(Known("curl/8 (+https://example.test/gptbot)"));
  // Not a glob or a pattern.
  EXPECT_FALSE(Known("GPT*"));
  EXPECT_FALSE(Known(".*"));
}

TEST(KnownAgentTableTest, PhrasesMatchOnlyWhole) {
  EXPECT_TRUE(Known("ChatGPT Agent"));
  EXPECT_TRUE(Known("x;ChatGPT Agent/2"));
  EXPECT_TRUE(Known("chatgpt agent"));
  EXPECT_FALSE(Known("ChatGPT  Agent"));
  EXPECT_FALSE(Known("ChatGPT-Agent"));
  EXPECT_FALSE(Known("ChatGPTAgent"));
  EXPECT_FALSE(Known("ChatGPT Agents"));
  EXPECT_FALSE(Known("MyChatGPT Agent"));
  EXPECT_FALSE(Known("ChatGPT\tAgent"));
  // A token with an inner '/' is literal there.
  const KnownAgent table[] = {{"spider/2.0", 10, KnownAgentKind::kOther}};
  KnownAgentMatcher matcher(table, 1);
  EXPECT_NE(nullptr, matcher.Match("spider/2.0"));
  EXPECT_NE(nullptr, matcher.Match("Mozilla (spider/2.0; x)"));
  EXPECT_EQ(nullptr, matcher.Match("spider/2.01"));
  EXPECT_EQ(nullptr, matcher.Match("spider"));
  EXPECT_EQ(nullptr, matcher.Match("spider/2"));
}

// The fast scan and the slow reference agree on a corpus built from every
// token: prefixes, suffixes, joins and boundary swaps.
TEST(KnownAgentTableTest, AgreesWithTheSlowReference) {
  size_t size = 0;
  const KnownAgent* table = KnownAgentTable(&size);
  int checked = 0;
  for (size_t i = 0; i < size; ++i) {
    const GoogleString token = table[i].token;
    std::vector<GoogleString> corpus = {
        token.substr(1), token.substr(0, token.size() - 1),
        StrCat(token, token), StrCat(token, " ", token),
        StrCat(token.substr(0, token.size() / 2), " ",
               token.substr(token.size() / 2))};
    GoogleString swapped = token;
    for (char& c : swapped) {
      if (c == ' ') c = '/';
    }
    corpus.push_back(swapped);
    corpus.push_back(StrCat(table[(i + 1) % size].token, token));
    for (const GoogleString& ua : corpus) {
      EXPECT_EQ(ReferenceKnown(ua), Known(ua)) << ua;
      ++checked;
    }
  }
  EXPECT_GT(checked, 1000);
}

// Case folding is ASCII only: no other byte folds into an ASCII letter.
TEST(KnownAgentTableTest, CaseFoldingIsAsciiOnly) {
  // U+212A KELVIN SIGN, which some Unicode lower-casing maps to 'k'.
  EXPECT_FALSE(Known("\xE2\x84\xAA" "angaroo Bot"));
  EXPECT_TRUE(Known("kANGAROO bOT"));
  // A Latin-1 or UTF-8 letter next to a token is not a boundary.
  EXPECT_FALSE(Known("\xC3\xA9" "GPTBot"));
  EXPECT_FALSE(Known("GPTBot\xC3\xA9"));
  EXPECT_FALSE(Known("GPTBot\x80"));
}

TEST(KnownAgentTableTest, HostileBytesAreHarmless) {
  EXPECT_FALSE(Known(""));
  EXPECT_FALSE(Known(" "));
  EXPECT_FALSE(Known("/;()"));
  const GoogleString nul_inside("GPT\0Bot", 7);
  EXPECT_FALSE(Known(nul_inside));
  // A NUL is not a boundary, and does not end the scan.
  const GoogleString nul_before("\0GPTBot", 7);
  EXPECT_FALSE(Known(nul_before));
  const GoogleString nul_then_token("x\0 GPTBot", 9);
  EXPECT_TRUE(Known(nul_then_token));
  // Every byte value, alone and around a token.
  for (int c = 0; c < 256; ++c) {
    const GoogleString byte(1, static_cast<char>(c));
    EXPECT_FALSE(Known(byte)) << c;
    const bool boundary = IsKnownAgentTokenBoundary(static_cast<char>(c));
    EXPECT_EQ(boundary, Known(StrCat(byte, "GPTBot"))) << c;
    EXPECT_EQ(boundary, Known(StrCat("GPTBot", byte))) << c;
  }
}

// A long, boundary-dense User-Agent is one linear pass.
TEST(KnownAgentTableTest, LongUserAgents) {
  GoogleString many;
  for (int i = 0; i < 4096; ++i) many += "a ";
  EXPECT_FALSE(Known(many));
  EXPECT_TRUE(Known(StrCat(many, "GPTBot")));
  GoogleString near;
  for (int i = 0; i < 1000; ++i) near += "GPTBo/";
  EXPECT_FALSE(Known(near));
  EXPECT_TRUE(Known(StrCat(near, "GPTBot")));
  const GoogleString one_word(64 * 1024, 'G');
  EXPECT_FALSE(Known(one_word));
}

// Where tokens share a start, the longest one that ends on a boundary wins.
TEST(KnownAgentTableTest, LongestMatchAtTheFirstPosition) {
  const KnownAgent table[] = {
      {"AI2Bot", 6, KnownAgentKind::kTraining},
      {"AI2Bot-DeepResearchEval", 23, KnownAgentKind::kFetcher},
      {"Zed", 3, KnownAgentKind::kOther},
  };
  KnownAgentMatcher matcher(table, 3);
  const KnownAgent* m = matcher.Match("AI2Bot-DeepResearchEval/1");
  ASSERT_NE(nullptr, m);
  EXPECT_STREQ("AI2Bot-DeepResearchEval", m->token);
  m = matcher.Match("AI2Bot/1 Zed");
  ASSERT_NE(nullptr, m);
  EXPECT_STREQ("AI2Bot", m->token);
  m = matcher.Match("zed ai2bot");
  ASSERT_NE(nullptr, m);
  EXPECT_STREQ("Zed", m->token);
  EXPECT_EQ(nullptr, matcher.Match("AI2Bot-Deep"));
}

TEST(KnownAgentTableTest, AnEmptyTableMatchesNothing) {
  const KnownAgent unused[] = {{"x", 1, KnownAgentKind::kOther}};
  KnownAgentMatcher matcher(unused, 0);
  EXPECT_EQ(0u, matcher.size());
  EXPECT_EQ(nullptr, matcher.Match("GPTBot"));
  EXPECT_EQ(nullptr, matcher.Match(""));
}

TEST(KnownAgentTableTest, KindsComeFromTheTable) {
  const KnownAgent* m = KnownAgentMatcher::Default().Match("GPTBot/1.2");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(KnownAgentKind::kTraining, m->kind);
  m = KnownAgentMatcher::Default().Match("ChatGPT-User/1.0");
  ASSERT_NE(nullptr, m);
  EXPECT_EQ(KnownAgentKind::kFetcher, m->kind);
}

}  // namespace
}  // namespace net_instaweb
