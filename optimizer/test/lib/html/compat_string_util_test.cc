// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Behavioral coverage for the lib/html/compat/ dialect headers added for the
// #1130 HTML kernel vendoring: string_util (SplitStringPieceToVector,
// TrimWhitespace, LowerString/UpperString, StringSetInsensitive, the absl
// StrCat/StrAppend using-decls), string_hash (canonical HashString
// recurrence + case folding), sparse_hash_map (unordered_map subclass with
// no-op gperftools marker APIs), stl_util (canonical STLDeleteElements:
// delete every element, then clear; nullptr no-op), and timer (minimal
// abstract Timer). The expectations pin CANONICAL semantics — these headers
// exist so the vendored kernel compiles unchanged, so their behavior must
// match what the canonical kernel was written against.

#include <string>
#include <vector>

#include "absl/hash/hash.h"
#include "gtest/gtest.h"
#include "lib/html/compat/sparse_hash_map.h"
#include "lib/html/compat/stl_util.h"
#include "lib/html/compat/string_hash.h"
#include "lib/html/compat/string_util.h"
#include "lib/html/compat/timer.h"

namespace {

TEST(HtmlCompatStringUtilTest, StrCatAndStrAppendAreVisibleUnqualified) {
  // Canonical string_util.h declares `using absl::StrCat/StrAppend` at
  // global scope; the vendored kernel calls them unqualified.
  GoogleString s = StrCat("a", 1, "b");
  EXPECT_EQ(s, "a1b");
  StrAppend(&s, "c", 2);
  EXPECT_EQ(s, "a1bc2");
}

TEST(HtmlCompatStringUtilTest, SplitStringPieceToVectorCanonicalSemantics) {
  net_instaweb::StringPieceVector out;

  out.clear();
  net_instaweb::SplitStringPieceToVector("a b  c", " ", &out, true);
  EXPECT_EQ(out, (std::vector<StringPiece>{"a", "b", "c"}));

  // omit_empty_strings=false keeps the empty middle piece AND the empty
  // tail after a trailing separator (canonical statement-for-statement).
  out.clear();
  net_instaweb::SplitStringPieceToVector("a b ", " ", &out, false);
  EXPECT_EQ(out, (std::vector<StringPiece>{"a", "b", ""}));

  out.clear();
  net_instaweb::SplitStringPieceToVector("a b ", " ", &out, true);
  EXPECT_EQ(out, (std::vector<StringPiece>{"a", "b"}));

  // Multiple separator characters; content_type.cc relies on this for
  // ";"-then-"=" splitting.
  out.clear();
  net_instaweb::SplitStringPieceToVector("text/html; charset=utf-8", "; ", &out,
                                         true);
  EXPECT_EQ(out, (std::vector<StringPiece>{"text/html", "charset=utf-8"}));

  out.clear();
  net_instaweb::SplitStringPieceToVector("", ";", &out, false);
  EXPECT_EQ(out, (std::vector<StringPiece>{""}));
}

TEST(HtmlCompatStringUtilTest, TrimWhitespaceIsHtmlWhitespace) {
  StringPiece s(" \t\r\nhello \n");
  EXPECT_TRUE(net_instaweb::TrimWhitespace(&s));
  EXPECT_EQ(s, "hello");
  EXPECT_FALSE(net_instaweb::TrimWhitespace(&s));
  // IsHtmlSpace includes \f (HTML5 space chars), unlike isspace-only
  // trimming.
  StringPiece f("\fhello\f");
  EXPECT_TRUE(net_instaweb::TrimWhitespace(&f));
  EXPECT_EQ(f, "hello");
}

TEST(HtmlCompatStringUtilTest, LowerUpperString) {
  GoogleString s("AbC/");
  net_instaweb::LowerString(&s);
  EXPECT_EQ(s, "abc/");
  net_instaweb::UpperString(&s);
  EXPECT_EQ(s, "ABC/");
}

TEST(HtmlCompatStringUtilTest, StringSetInsensitiveIsOrderedAndCaseFolding) {
  net_instaweb::StringSetInsensitive set;
  set.insert("Beta");
  set.insert("alpha");
  EXPECT_EQ(set.count("ALPHA"), size_t{1});
  EXPECT_EQ(set.count("beta"), size_t{1});
  EXPECT_EQ(set.count("gamma"), size_t{0});
  // std::set ordering (canonical type), not hash order.
  EXPECT_EQ(*set.begin(), "alpha");
}

TEST(HtmlCompatStringHashTest, CanonicalRecurrenceAndFolding) {
  // Pin the canonical HashString recurrence: h = h * 131 + c.
  using CasePreserve = net_instaweb::CasePreserve;
  EXPECT_EQ((net_instaweb::HashString<CasePreserve, size_t>("a", 1)), 97u);
  const size_t ab = net_instaweb::HashString<CasePreserve, size_t>("ab", 2);
  EXPECT_EQ(ab, 97u * 131u + 98u);
  // The functors mix that result for absl's Swiss tables, as canonical does.
  EXPECT_EQ(net_instaweb::CasePreserveStringHash()(GoogleString("ab")),
            absl::HashOf(ab));
  // Case folding makes hashes case-insensitive; equality functor agrees.
  EXPECT_EQ(net_instaweb::CaseFoldStringHash()(GoogleString("aBc")),
            net_instaweb::CaseFoldStringHash()(GoogleString("AbC")));
  EXPECT_TRUE(net_instaweb::CaseFoldStringEqual()(GoogleString("aBc"),
                                                  GoogleString("AbC")));
  EXPECT_FALSE(net_instaweb::CaseFoldStringEqual()(GoogleString("abc"),
                                                   GoogleString("abcd")));
}

TEST(HtmlCompatSparseHashMapTest, UnorderedMapSubclassWithMarkerNoops) {
  sparse_hash_map<GoogleString, const char*, net_instaweb::CaseFoldStringHash,
                  net_instaweb::CaseFoldStringEqual>
      map;
  // gperftools marker APIs exist and are no-ops.
  map.set_deleted_key("");
  map.set_empty_key("");
  map[GoogleString("AMP")] = "&";
  auto it = map.find(GoogleString("amp"));
  ASSERT_NE(it, map.end());
  EXPECT_STREQ(it->second, "&");
  map.erase(it);
  EXPECT_EQ(map.find(GoogleString("amp")), map.end());
}

class FixedTimer : public net_instaweb::Timer {
 public:
  int64_t NowUs() const override { return 1234567; }
};

TEST(HtmlCompatTimerTest, MinimalInterface) {
  FixedTimer timer;
  EXPECT_EQ(timer.NowUs(), int64_t{1234567});
}

namespace {

struct DtorCounter {
  static int destroyed;
  ~DtorCounter() { ++destroyed; }
};
int DtorCounter::destroyed = 0;

}  // namespace

TEST(HtmlCompatStlUtilTest, DeleteElementsDeletesAndClears) {
  // Canonical semantics (pagespeed/kernel/base/stl_util.h): delete every
  // element, then clear the container; nullptr is a no-op.
  DtorCounter::destroyed = 0;
  std::vector<DtorCounter*> v;
  v.push_back(new DtorCounter());
  v.push_back(new DtorCounter());
  v.push_back(new DtorCounter());
  STLDeleteElements(&v);
  EXPECT_EQ(DtorCounter::destroyed, 3);
  EXPECT_TRUE(v.empty());

  std::vector<DtorCounter*>* null_container = nullptr;
  STLDeleteElements(null_container);  // must not crash
  EXPECT_EQ(DtorCounter::destroyed, 3);

  // Empty container: nothing deleted, still cleared (trivially).
  STLDeleteElements(&v);
  EXPECT_EQ(DtorCounter::destroyed, 3);
}

}  // namespace
