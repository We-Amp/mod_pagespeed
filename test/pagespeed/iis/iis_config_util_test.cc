// Unit tests for the cross-platform IIS config tokenizer.
// These tests are platform-independent and can run on any OS.

#include "pagespeed/iis/iis_config_util.h"

#include <cctype>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

using iis_config_util::tokenize;

TEST(IisConfigUtilTokenizeTest, SimpleSpaceSeparated) {
  std::vector<std::string> result = tokenize("hello world", isspace);
  ASSERT_EQ(2, result.size());
  EXPECT_EQ("hello", result[0]);
  EXPECT_EQ("world", result[1]);
}

TEST(IisConfigUtilTokenizeTest, MultipleSpaces) {
  std::vector<std::string> result = tokenize("hello   world", isspace);
  ASSERT_EQ(2, result.size());
  EXPECT_EQ("hello", result[0]);
  EXPECT_EQ("world", result[1]);
}

TEST(IisConfigUtilTokenizeTest, LeadingTrailingSpaces) {
  std::vector<std::string> result = tokenize("  hello world  ", isspace);
  ASSERT_EQ(2, result.size());
  EXPECT_EQ("hello", result[0]);
  EXPECT_EQ("world", result[1]);
}

TEST(IisConfigUtilTokenizeTest, QuotedString) {
  std::vector<std::string> result =
      tokenize("hello \"big world\" end", isspace);
  ASSERT_EQ(3, result.size());
  EXPECT_EQ("hello", result[0]);
  EXPECT_EQ("big world", result[1]);
  EXPECT_EQ("end", result[2]);
}

TEST(IisConfigUtilTokenizeTest, EscapedQuoteInString) {
  // Doubled quotes ("") inside a quoted region produce a single literal quote.
  // Input bytes: "he said ""hi"" ok"
  // The tokenizer sees the opening ", then accumulates text. When it hits "",
  // it emits one literal " and stays in quoted mode. The final " closes the
  // quoted region.
  // Result: a single token "he said "hi" ok" (with literal quotes inside).
  std::vector<std::string> result =
      tokenize("\"he said \"\"hi\"\" ok\"", isspace);
  ASSERT_EQ(1, result.size());
  EXPECT_EQ("he said \"hi\" ok", result[0]);
}

TEST(IisConfigUtilTokenizeTest, EmptyInput) {
  std::vector<std::string> result = tokenize("", isspace);
  EXPECT_TRUE(result.empty());
}

TEST(IisConfigUtilTokenizeTest, OnlySpaces) {
  std::vector<std::string> result = tokenize("   ", isspace);
  EXPECT_TRUE(result.empty());
}

TEST(IisConfigUtilTokenizeTest, TabsAsSeparator) {
  std::vector<std::string> result = tokenize("hello\tworld", isspace);
  ASSERT_EQ(2, result.size());
  EXPECT_EQ("hello", result[0]);
  EXPECT_EQ("world", result[1]);
}

TEST(IisConfigUtilTokenizeTest, UnclosedQuote) {
  // An unclosed quote: the quoted region extends to end of string.
  // The accumulated token is pushed at the end because tmp is non-empty.
  std::vector<std::string> result = tokenize("\"hello world", isspace);
  ASSERT_EQ(1, result.size());
  EXPECT_EQ("hello world", result[0]);
}

TEST(IisConfigUtilTokenizeTest, EmptyQuotedString) {
  // "" produces an empty tmp between the open and close quotes.
  // Because tmp.size() == 0, nothing is pushed.
  std::vector<std::string> result = tokenize("\"\"", isspace);
  EXPECT_TRUE(result.empty());
}

TEST(IisConfigUtilTokenizeTest, QuotedWithSpacesOnly) {
  // Spaces inside quotes are preserved as a single token.
  std::vector<std::string> result = tokenize("\"   \"", isspace);
  ASSERT_EQ(1, result.size());
  EXPECT_EQ("   ", result[0]);
}

TEST(IisConfigUtilTokenizeTest, MixedQuotedUnquoted) {
  // Typical IIS config line: directive followed by a quoted Windows path.
  std::vector<std::string> result =
      tokenize("FileCachePath \"C:\\temp\\cache\"", isspace);
  ASSERT_EQ(2, result.size());
  EXPECT_EQ("FileCachePath", result[0]);
  EXPECT_EQ("C:\\temp\\cache", result[1]);
}

}  // namespace
