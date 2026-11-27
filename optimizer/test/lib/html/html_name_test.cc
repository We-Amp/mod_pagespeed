// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Ported from mod_pagespeed for PageSpeed 2.0

// Unit-test the html name class, make sure we can do case
// insensitive matching.

#include "lib/html/html_name.h"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/base/string_util.h"

namespace net_instaweb {

namespace {

// Helper to convert string to uppercase. (Not named UpperString: the
// vendored kernel's compat string_util.h declares that in net_instaweb.)
void ToUpperString(std::string* s) {
  std::transform(s->begin(), s->end(), s->begin(), [](unsigned char c) {
    return UpperChar(static_cast<char>(c));
  });
}

}  // namespace

class HtmlNameTest : public testing::Test {
 protected:
};

TEST_F(HtmlNameTest, OneKeyword) {
  EXPECT_EQ(HtmlName::kStyle, HtmlName::Lookup("style"));
}

TEST_F(HtmlNameTest, ConvergedKeywordUnion) {
  // Keywords converged with mod_pagespeed 1.15's html_name table:
  // living-standard attributes/tags that were
  // 1.15-only, now part of the optimizer's union table.
  EXPECT_EQ(HtmlName::kAllowfullscreen, HtmlName::Lookup("allowfullscreen"));
  EXPECT_EQ(HtmlName::kDecoding, HtmlName::Lookup("decoding"));
  EXPECT_EQ(HtmlName::kDialog, HtmlName::Lookup("dialog"));
  EXPECT_EQ(HtmlName::kFetchpriority, HtmlName::Lookup("fetchpriority"));
  EXPECT_EQ(HtmlName::kLoading, HtmlName::Lookup("loading"));
  EXPECT_EQ(HtmlName::kPicture, HtmlName::Lookup("picture"));
  EXPECT_EQ(HtmlName::kPlaysinline, HtmlName::Lookup("playsinline"));
  // optimizer-only keywords kept (SRI / template handling).
  EXPECT_EQ(HtmlName::kCrossorigin, HtmlName::Lookup("crossorigin"));
  EXPECT_EQ(HtmlName::kIntegrity, HtmlName::Lookup("integrity"));
  EXPECT_EQ(HtmlName::kTemplate, HtmlName::Lookup("template"));
  // 1.15-internal keyword adopted for canonical-enum parity (#1140): with
  // this entry the Keyword enum matches 1.15's html_name.h exactly, so
  // vendoring a single canonical html_name (#1130) is ordinal-preserving.
  EXPECT_EQ(HtmlName::kDataPagespeedSrcsetUrlHashes,
            HtmlName::Lookup("data-pagespeed-srcset-url-hashes"));
  EXPECT_EQ(HtmlName::kDataPagespeedSrcsetUrlHashes,
            HtmlName::Lookup("Data-Pagespeed-Srcset-Url-Hashes"));
}

TEST_F(HtmlNameTest, AllKeywordsDefaultCase) {
  for (HtmlName::Iterator iter; !iter.AtEnd(); iter.Next()) {
    EXPECT_EQ(iter.keyword(), HtmlName::Lookup(iter.name()));
  }
}

TEST_F(HtmlNameTest, AllKeywordsUpperCase) {
  for (HtmlName::Iterator iter; !iter.AtEnd(); iter.Next()) {
    std::string upper(iter.name());
    ToUpperString(&upper);
    EXPECT_EQ(iter.keyword(), HtmlName::Lookup(upper));
  }
}

TEST_F(HtmlNameTest, AllKeywordsMixedCase) {
  for (HtmlName::Iterator iter; !iter.AtEnd(); iter.Next()) {
    std::string mixed(iter.name());
    bool upper = false;
    for (size_t i = 0, n = mixed.size(); i < n; ++i) {
      char c = mixed[i];
      upper = !upper;
      if (upper) {
        c = UpperChar(c);
      } else {
        c = LowerChar(c);
      }
      mixed[i] = c;
    }
    EXPECT_EQ(iter.keyword(), HtmlName::Lookup(mixed));
  }
}

TEST_F(HtmlNameTest, Bogus) {
  EXPECT_EQ(HtmlName::kNotAKeyword, HtmlName::Lookup("hiybbprqag"));
  EXPECT_EQ(HtmlName::kNotAKeyword, HtmlName::Lookup("stylex"));
}

TEST_F(HtmlNameTest, Iterator) {
  int num_iters = 0;
  std::set<std::string> names;
  std::set<HtmlName::Keyword> keywords;
  for (HtmlName::Iterator iter; !iter.AtEnd(); iter.Next()) {
    EXPECT_GT(HtmlName::num_keywords(), static_cast<int>(iter.keyword()));
    keywords.insert(iter.keyword());
    names.insert(iter.name());
    ++num_iters;
  }
  EXPECT_EQ(HtmlName::num_keywords(), num_iters);
  EXPECT_EQ(static_cast<size_t>(HtmlName::num_keywords()), keywords.size());
  EXPECT_EQ(static_cast<size_t>(HtmlName::num_keywords()), names.size());
}

}  // namespace net_instaweb
