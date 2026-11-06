// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <cmath>
#include <cstdlib>
#include <string>

#include "gtest/gtest.h"
#include "pagespeed/kernel/packs/json_util.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {
namespace {

// The value the C library itself gives a spelling; nothing in this test
// changes the thread's locale, so this is the number that must come back.
double Decoded(const char* spelling) { return std::strtod(spelling, nullptr); }

bool Parses(const std::string& text) {
  Json::Value value;
  return ParseStrictJson(text, kMaxJsonLdDepth, false, &value);
}

bool ParsesInto(const std::string& text, Json::Value* v,
                bool reject_duplicate_keys = false) {
  return ParseStrictJson(text, kMaxJsonLdDepth, reject_duplicate_keys, v);
}

TEST(ParseStrictJsonSubnormalTest, SubnormalValuesAreAccepted) {
  Json::Value v;
  ASSERT_TRUE(ParsesInto(R"({"a":1e-320,"b":-1e-320,"c":5e-324})", &v));
  EXPECT_EQ(Decoded("1e-320"), v["a"].asDouble());
  EXPECT_EQ(-Decoded("1e-320"), v["b"].asDouble());
  EXPECT_EQ(Decoded("5e-324"), v["c"].asDouble());
  // Several in one document, each with its own value.
  ASSERT_TRUE(ParsesInto(R"({"x":[1e-320,-2.5e-320],"y":{"z":1e-321}})", &v));
  EXPECT_EQ(Decoded("1e-320"), v["x"][Json::ArrayIndex(0)].asDouble());
  EXPECT_EQ(-Decoded("2.5e-320"), v["x"][Json::ArrayIndex(1)].asDouble());
  EXPECT_EQ(Decoded("1e-321"), v["y"]["z"].asDouble());
}

TEST(ParseStrictJsonSubnormalTest, NormalRangeIsAccepted) {
  Json::Value v;
  // The smallest normal double.
  ASSERT_TRUE(ParsesInto(R"({"a":2.2250738585072014e-308})", &v));
  EXPECT_EQ(2.2250738585072014e-308, v["a"].asDouble());
  // The largest one.
  ASSERT_TRUE(ParsesInto("[1.7976931348623157e308]", &v));
  EXPECT_EQ(1.7976931348623157e308, v[Json::ArrayIndex(0)].asDouble());
}

TEST(ParseStrictJsonSubnormalTest, ZeroWithExponentIsAccepted) {
  Json::Value v;
  ASSERT_TRUE(
      ParsesInto(R"({"a":0e0,"b":0e320,"c":-0e-320,"d":0.000e-999})", &v));
  EXPECT_EQ(0.0, v["a"].asDouble());
  EXPECT_EQ(0.0, v["b"].asDouble());
  EXPECT_EQ(0.0, v["c"].asDouble());
  EXPECT_EQ(0.0, v["d"].asDouble());
}

TEST(ParseStrictJsonSubnormalTest, OverflowBecomesInfinity) {
  Json::Value v;
  ASSERT_TRUE(ParsesInto(R"({"a":1e999,"b":-1e999,"c":1.8e308})", &v));
  EXPECT_TRUE(std::isinf(v["a"].asDouble()));
  EXPECT_GT(v["a"].asDouble(), 0.0);
  EXPECT_TRUE(std::isinf(v["b"].asDouble()));
  EXPECT_LT(v["b"].asDouble(), 0.0);
  EXPECT_TRUE(std::isinf(v["c"].asDouble()));
  // An overflowing number beside a subnormal one: only the subnormal is held
  // aside, the overflow still comes back as infinity.
  ASSERT_TRUE(ParsesInto(R"({"a":1e999,"b":1e-320})", &v));
  EXPECT_TRUE(std::isinf(v["a"].asDouble()));
  EXPECT_EQ(Decoded("1e-320"), v["b"].asDouble());
}

TEST(ParseStrictJsonSubnormalTest, MalformedNumbersAreRefused) {
  EXPECT_FALSE(Parses(R"([.1e-320])"));       // no digit before the point
  EXPECT_FALSE(Parses(R"({"a":1e-320 1})"));  // junk after the value
  EXPECT_FALSE(Parses(R"({1e-320:2})"));      // a number as an object key
  EXPECT_FALSE(Parses(R"([1e-320e])"));       // a second exponent
  EXPECT_FALSE(Parses("[1e]"));
  EXPECT_FALSE(Parses("[1e+]"));
  EXPECT_FALSE(Parses("[.]"));
  EXPECT_FALSE(Parses("1e-320"));  // a bare number is not a document
  Json::Value v;
  // jsoncpp's leniencies are kept for everything the rescue does not touch.
  ASSERT_TRUE(ParsesInto("[1.]", &v));
  EXPECT_EQ(1.0, v[Json::ArrayIndex(0)].asDouble());
  ASSERT_TRUE(ParsesInto("[01]", &v));
  EXPECT_EQ(1, v[Json::ArrayIndex(0)].asInt());
  ASSERT_TRUE(ParsesInto("[+1]", &v));
  EXPECT_EQ(1.0, v[Json::ArrayIndex(0)].asDouble());
  // A number written inside a string stays a string.
  ASSERT_TRUE(ParsesInto(R"({"a":"1e-320"})", &v));
  EXPECT_TRUE(v["a"].isString());
}

// A value that underflows past the subnormal range is not held aside: the
// ordinary decoding decides, and its result is the one it always gave.
TEST(ParseStrictJsonSubnormalTest, UnderflowPastSubnormalKeepsItsResult) {
#if defined(_LIBCPP_VERSION)
  EXPECT_FALSE(Parses(R"({"a":1e-999})"));
#elif defined(__GLIBCXX__)
  Json::Value v;
  ASSERT_TRUE(ParsesInto(R"({"a":1e-999})", &v));
  EXPECT_EQ(0.0, v["a"].asDouble());
#else
  Json::Value v;
  if (ParsesInto(R"({"a":1e-999})", &v)) {
    EXPECT_EQ(0.0, v["a"].asDouble());
  }
#endif
}

// A subnormal behind a leading '+' is not held aside either: strict JSON
// spells a number without one, and the rewrite leaves the sign in place.
TEST(ParseStrictJsonSubnormalTest, PlusSignedSubnormalKeepsItsResult) {
#if defined(_LIBCPP_VERSION)
  EXPECT_FALSE(Parses("[+1e-320]"));
#elif defined(__GLIBCXX__)
  Json::Value v;
  ASSERT_TRUE(ParsesInto("[+1e-320]", &v));
  EXPECT_EQ(Decoded("1e-320"), v[Json::ArrayIndex(0)].asDouble());
#else
  Json::Value v;
  if (ParsesInto("[+1e-320]", &v)) {
    EXPECT_EQ(Decoded("1e-320"), v[Json::ArrayIndex(0)].asDouble());
  }
#endif
}

// A document that fails to parse and carries the escape \u0000 is never
// rewritten, so no string of it can pose as a placeholder.
TEST(ParseStrictJsonSubnormalTest, DocumentSpellingAPlaceholderRefusesIt) {
  EXPECT_FALSE(Parses(R"({"a":"\u0000#0",})"));
  EXPECT_FALSE(Parses(R"(["\u0000#0",1e-320 x])"));
  EXPECT_FALSE(Parses(R"({"a":1e-320,"a":"\u0000#00",})"));
#if defined(_LIBCPP_VERSION)
  EXPECT_FALSE(Parses(R"({"a":1e-320,"a":"\u0000#0"})"));
#elif defined(__GLIBCXX__)
  Json::Value v;
  ASSERT_TRUE(ParsesInto(R"({"a":1e-320,"a":"\u0000#0"})", &v));
  EXPECT_TRUE(v["a"].isString());
#endif
}

// A repeated key, kept the way a browser keeps it, collapses the member that
// holds a placeholder, so the value cannot be put back; refused on request it
// is an error either way.
TEST(ParseStrictJsonSubnormalTest, DuplicateKeyOverASubnormalKeepsItsResult) {
  Json::Value v;
  EXPECT_FALSE(ParsesInto(R"({"a":1e-320,"a":2})", &v, true));
#if defined(_LIBCPP_VERSION)
  EXPECT_FALSE(Parses(R"({"a":1e-320,"a":2})"));
#elif defined(__GLIBCXX__)
  ASSERT_TRUE(ParsesInto(R"({"a":1e-320,"a":2})", &v));
  EXPECT_EQ(2.0, v["a"].asDouble());
#endif
}

// A JSON-LD block can be as large as kMaxJsonLdScanBytes. The rescue works in
// one pass over the text, so a block of subnormals, valid or not, takes time
// in proportion to its size; this target runs under a short timeout, and a
// rescue that walks the text once per number runs out of it.
TEST(ParseStrictJsonSubnormalTest, BlockOfSubnormalsAtTheScanLimit) {
  std::string valid = "[";
  size_t count = 0;
  while (valid.size() + 16 < kMaxJsonLdScanBytes) {
    valid += "1e-320,";
    ++count;
  }
  valid += "1e-320]";
  ++count;
  Json::Value v;
  ASSERT_TRUE(ParsesInto(valid, &v));
  ASSERT_TRUE(v.isArray());
  ASSERT_EQ(count, v.size());
  EXPECT_EQ(Decoded("1e-320"), v[Json::ArrayIndex(0)].asDouble());
  EXPECT_EQ(Decoded("1e-320"), v[Json::ArrayIndex(count - 1)].asDouble());

  std::string invalid = "[";
  while (invalid.size() + 16 < kMaxJsonLdScanBytes) invalid += "1e-320,";
  invalid += "x]";
  EXPECT_FALSE(Parses(invalid));
}

}  // namespace
}  // namespace pagespeed::packs
