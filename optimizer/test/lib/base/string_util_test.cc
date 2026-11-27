// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for string utilities beyond C++23 and abseil.

#include "lib/base/string_util.h"

#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace net_instaweb {
namespace {

// =============================================================================
// HTML whitespace tests
// =============================================================================

TEST(StringUtilTest, IsHtmlSpace) {
  EXPECT_TRUE(IsHtmlSpace(' '));
  EXPECT_TRUE(IsHtmlSpace('\t'));
  EXPECT_TRUE(IsHtmlSpace('\n'));
  EXPECT_TRUE(IsHtmlSpace('\f'));
  EXPECT_TRUE(IsHtmlSpace('\r'));
  EXPECT_FALSE(IsHtmlSpace('a'));
  EXPECT_FALSE(IsHtmlSpace('\v'));  // HTML doesn't include vertical tab
  EXPECT_FALSE(IsHtmlSpace('\0'));
}

TEST(StringUtilTest, TrimHtmlWhitespace) {
  std::string_view str = "  hello  ";
  EXPECT_TRUE(TrimHtmlWhitespace(&str));
  EXPECT_EQ("hello", str);

  str = "hello";
  EXPECT_FALSE(TrimHtmlWhitespace(&str));
  EXPECT_EQ("hello", str);
}

TEST(StringUtilTest, TrimLeadingHtmlWhitespace) {
  std::string_view str = "  hello  ";
  EXPECT_TRUE(TrimLeadingHtmlWhitespace(&str));
  EXPECT_EQ("hello  ", str);
}

TEST(StringUtilTest, TrimTrailingHtmlWhitespace) {
  std::string_view str = "  hello  ";
  EXPECT_TRUE(TrimTrailingHtmlWhitespace(&str));
  EXPECT_EQ("  hello", str);
}

// =============================================================================
// Case-insensitive operation tests
// =============================================================================

TEST(StringUtilTest, UpperLowerChar) {
  EXPECT_EQ('A', UpperChar('a'));
  EXPECT_EQ('Z', UpperChar('z'));
  EXPECT_EQ('A', UpperChar('A'));
  EXPECT_EQ('1', UpperChar('1'));

  EXPECT_EQ('a', LowerChar('A'));
  EXPECT_EQ('z', LowerChar('Z'));
  EXPECT_EQ('a', LowerChar('a'));
  EXPECT_EQ('1', LowerChar('1'));
}

TEST(StringUtilTest, StringCaseCompare) {
  EXPECT_EQ(0, StringCaseCompare("hello", "HELLO"));
  EXPECT_EQ(0, StringCaseCompare("Hello", "hElLo"));
  EXPECT_LT(StringCaseCompare("apple", "Banana"), 0);
  EXPECT_GT(StringCaseCompare("banana", "Apple"), 0);
  EXPECT_LT(StringCaseCompare("abc", "abcd"), 0);
}

TEST(StringUtilTest, StringCaseEqual) {
  EXPECT_TRUE(StringCaseEqual("Hello", "hello"));
  EXPECT_TRUE(StringCaseEqual("HELLO", "hello"));
  EXPECT_FALSE(StringCaseEqual("Hello", "World"));
  EXPECT_FALSE(StringCaseEqual("Hello", "HelloWorld"));
}

TEST(StringUtilTest, StringCaseStartsWith) {
  EXPECT_TRUE(StringCaseStartsWith("Hello World", "hello"));
  EXPECT_TRUE(StringCaseStartsWith("hello", "HELLO"));
  EXPECT_TRUE(StringCaseStartsWith("hello", ""));
  EXPECT_FALSE(StringCaseStartsWith("hello", "world"));
}

TEST(StringUtilTest, StringCaseEndsWith) {
  EXPECT_TRUE(StringCaseEndsWith("Hello World", "WORLD"));
  EXPECT_TRUE(StringCaseEndsWith("hello", "HELLO"));
  EXPECT_TRUE(StringCaseEndsWith("hello", ""));
  EXPECT_FALSE(StringCaseEndsWith("hello", "world"));
}

TEST(StringUtilTest, FindIgnoreCase) {
  EXPECT_EQ(0u, FindIgnoreCase("Hello World", "hello"));
  EXPECT_EQ(6u, FindIgnoreCase("Hello World", "WORLD"));
  EXPECT_EQ(std::string_view::npos, FindIgnoreCase("Hello", "World"));
  EXPECT_EQ(0u, FindIgnoreCase("hello", ""));  // Empty needle
}

// =============================================================================
// String escaping tests
// =============================================================================

TEST(StringUtilTest, CEscape) {
  EXPECT_EQ("hello", CEscape("hello"));
  EXPECT_EQ("hello\\nworld", CEscape("hello\nworld"));
  EXPECT_EQ("hello\\tworld", CEscape("hello\tworld"));
  EXPECT_EQ("hello\\rworld", CEscape("hello\rworld"));
  EXPECT_EQ("a\\\"b", CEscape("a\"b"));
  EXPECT_EQ("a\\'b", CEscape("a'b"));
  EXPECT_EQ("a\\\\b", CEscape("a\\b"));
  // Non-printable character (ASCII 1) -> \001
  EXPECT_EQ("a\\001b", CEscape(std::string("a\001b", 3)));
}

TEST(StringUtilTest, BackslashEscape) {
  std::string result;
  BackslashEscape("a\"b'c", "\"'", &result);
  EXPECT_EQ("a\\\"b\\'c", result);

  result.clear();
  BackslashEscape("hello", "", &result);
  EXPECT_EQ("hello", result);
}

// =============================================================================
// String manipulation tests
// =============================================================================

TEST(StringUtilTest, GlobalReplaceSubstring) {
  std::string s = "hello world hello";
  EXPECT_EQ(2, GlobalReplaceSubstring("hello", "hi", &s));
  EXPECT_EQ("hi world hi", s);

  s = "hello world";
  EXPECT_EQ(0, GlobalReplaceSubstring("foo", "bar", &s));
  EXPECT_EQ("hello world", s);

  s = "aaa";
  EXPECT_EQ(3, GlobalReplaceSubstring("a", "bb", &s));
  EXPECT_EQ("bbbbbb", s);
}

TEST(StringUtilTest, GlobalEraseBracketedSubstring) {
  std::string s = "abc[def]ghi[jkl]mno";
  EXPECT_EQ(2, GlobalEraseBracketedSubstring("[", "]", &s));
  EXPECT_EQ("abcghimno", s);

  s = "no brackets here";
  EXPECT_EQ(0, GlobalEraseBracketedSubstring("[", "]", &s));
  EXPECT_EQ("no brackets here", s);
}

TEST(StringUtilTest, CountSubstring) {
  EXPECT_EQ(2, CountSubstring("hello hello world", "hello"));
  EXPECT_EQ(3, CountSubstring("aaaaa", "aaa"));  // Overlapping matches
  EXPECT_EQ(0, CountSubstring("hello", "world"));
  EXPECT_EQ(0, CountSubstring("hello", ""));
}

// =============================================================================
// Security utility tests
// =============================================================================

TEST(StringUtilTest, ConstantTimeCompare) {
  EXPECT_TRUE(ConstantTimeCompare("hello", "hello"));
  EXPECT_TRUE(ConstantTimeCompare("", ""));
  EXPECT_FALSE(ConstantTimeCompare("hello", "world"));
  EXPECT_FALSE(ConstantTimeCompare("hello", "hell"));
  EXPECT_FALSE(ConstantTimeCompare("", "x"));
}

TEST(StringUtilTest, CountCharacterMismatches) {
  EXPECT_EQ(0, CountCharacterMismatches("hello", "hello"));
  EXPECT_EQ(1, CountCharacterMismatches("hello", "hallo"));
  EXPECT_EQ(1, CountCharacterMismatches("hello", "hell"));  // Length diff
  // "hello" vs "world": h!=w, e!=o, l!=r, l==l, o!=d = 4 mismatches
  EXPECT_EQ(4, CountCharacterMismatches("hello", "world"));
}

// =============================================================================
// Parsing utility tests
// =============================================================================

TEST(StringUtilTest, ParseShellLikeString) {
  std::vector<std::string> parts;

  ParseShellLikeString("a b c", &parts);
  ASSERT_EQ(3u, parts.size());
  EXPECT_EQ("a", parts[0]);
  EXPECT_EQ("b", parts[1]);
  EXPECT_EQ("c", parts[2]);

  ParseShellLikeString("a b \"c d\" e 'f g'", &parts);
  ASSERT_EQ(5u, parts.size());
  EXPECT_EQ("a", parts[0]);
  EXPECT_EQ("b", parts[1]);
  EXPECT_EQ("c d", parts[2]);
  EXPECT_EQ("e", parts[3]);
  EXPECT_EQ("f g", parts[4]);

  ParseShellLikeString("", &parts);
  EXPECT_TRUE(parts.empty());
}

TEST(StringUtilTest, PieceAfterEquals) {
  EXPECT_EQ("value", PieceAfterEquals("key=value"));
  EXPECT_EQ("value", PieceAfterEquals("key = value"));
  EXPECT_EQ("", PieceAfterEquals("key="));
  EXPECT_EQ("", PieceAfterEquals("noequals"));
}

// =============================================================================
// Numeric parsing tests
// =============================================================================

TEST(StringUtilTest, AccumulateDecimalValue) {
  uint32_t value = 0;
  EXPECT_TRUE(AccumulateDecimalValue('1', &value));
  EXPECT_EQ(1u, value);
  EXPECT_TRUE(AccumulateDecimalValue('2', &value));
  EXPECT_EQ(12u, value);
  EXPECT_TRUE(AccumulateDecimalValue('3', &value));
  EXPECT_EQ(123u, value);
  EXPECT_FALSE(AccumulateDecimalValue('a', &value));
  EXPECT_EQ(123u, value);  // Unchanged on failure
}

TEST(StringUtilTest, AccumulateHexValue) {
  uint32_t value = 0;
  EXPECT_TRUE(AccumulateHexValue('a', &value));
  EXPECT_EQ(10u, value);
  EXPECT_TRUE(AccumulateHexValue('F', &value));
  EXPECT_EQ(0xAFu, value);
  EXPECT_TRUE(AccumulateHexValue('0', &value));
  EXPECT_EQ(0xAF0u, value);
  EXPECT_FALSE(AccumulateHexValue('g', &value));
  EXPECT_EQ(0xAF0u, value);  // Unchanged on failure
}

TEST(StringUtilTest, CharacterClassification) {
  // IsHexDigit
  EXPECT_TRUE(IsHexDigit('0'));
  EXPECT_TRUE(IsHexDigit('9'));
  EXPECT_TRUE(IsHexDigit('a'));
  EXPECT_TRUE(IsHexDigit('f'));
  EXPECT_TRUE(IsHexDigit('A'));
  EXPECT_TRUE(IsHexDigit('F'));
  EXPECT_FALSE(IsHexDigit('g'));
  EXPECT_FALSE(IsHexDigit('G'));

  // IsDecimalDigit
  EXPECT_TRUE(IsDecimalDigit('0'));
  EXPECT_TRUE(IsDecimalDigit('9'));
  EXPECT_FALSE(IsDecimalDigit('a'));

  // IsAsciiAlphaNumeric
  EXPECT_TRUE(IsAsciiAlphaNumeric('a'));
  EXPECT_TRUE(IsAsciiAlphaNumeric('Z'));
  EXPECT_TRUE(IsAsciiAlphaNumeric('5'));
  EXPECT_FALSE(IsAsciiAlphaNumeric('!'));
  EXPECT_FALSE(IsAsciiAlphaNumeric(' '));
}

// =============================================================================
// Additional coverage tests
// =============================================================================

TEST(StringUtilTest, CEscapeExtendedAscii) {
  // Characters >= 127 should be escaped as octal
  EXPECT_EQ("\\177", CEscape(std::string(1, '\x7F')));
  EXPECT_EQ("\\377", CEscape(std::string(1, '\xFF')));
}

TEST(StringUtilTest, CEscapeControlChars) {
  // Non-printable characters < 32 (other than \n, \r, \t)
  EXPECT_EQ("\\005", CEscape(std::string(1, '\x05')));
  EXPECT_EQ("\\037", CEscape(std::string(1, '\x1F')));
  EXPECT_EQ("\\000", CEscape(std::string(1, '\x00')));
}

TEST(StringUtilTest, GlobalEraseBracketedSubstringMissingRightBracket) {
  // Left bracket found but no closing right bracket
  std::string s = "abc[def";
  EXPECT_EQ(0, GlobalEraseBracketedSubstring("[", "]", &s));
  // Text from left bracket to end is kept (no removal)
  EXPECT_EQ("abc[def", s);
}

TEST(StringUtilTest, GlobalEraseBracketedSubstringEmptyBrackets) {
  std::string s = "abc[]def[]ghi";
  EXPECT_EQ(2, GlobalEraseBracketedSubstring("[", "]", &s));
  EXPECT_EQ("abcdefghi", s);
}

TEST(StringUtilTest, GlobalReplaceSubstringEmptyInput) {
  std::string s;
  EXPECT_EQ(0, GlobalReplaceSubstring("a", "b", &s));
  EXPECT_EQ("", s);
}

TEST(StringUtilTest, GlobalReplaceSubstringEmptySubstring) {
  std::string s = "hello";
  EXPECT_EQ(0, GlobalReplaceSubstring("", "b", &s));
  EXPECT_EQ("hello", s);
}

TEST(StringUtilTest, StringCaseCompareLongerSecond) {
  EXPECT_GT(StringCaseCompare("abcd", "abc"), 0);
}

TEST(StringUtilTest, FindIgnoreCaseNeedleLargerThanHaystack) {
  EXPECT_EQ(std::string_view::npos, FindIgnoreCase("hi", "hello"));
}

TEST(StringUtilTest, ParseShellLikeStringEscapedQuote) {
  std::vector<std::string> parts;
  ParseShellLikeString(R"("a\"b")", &parts);
  ASSERT_EQ(1u, parts.size());
  EXPECT_EQ("a\"b", parts[0]);
}

TEST(StringUtilTest, CountSubstringExactMatch) {
  EXPECT_EQ(1, CountSubstring("abc", "abc"));
}

TEST(StringUtilTest, PieceAfterEqualsWhitespace) {
  EXPECT_EQ("", PieceAfterEquals("key"));
}

// =============================================================================
// Round 12: ConstantTimeCompare length-truncation regression
// =============================================================================

TEST(StringUtilTest, ConstantTimeCompareLengthDiffMultipleOf256) {
  // The old implementation used an unsigned char accumulator for the size XOR.
  // When lengths differ by exactly 256 (a multiple of 256), the XOR of the
  // sizes truncates to 0 in an 8-bit accumulator, producing a false match
  // if the overlapping bytes happen to be equal.
  std::string short_str(1, 'A');
  std::string long_str(257, 'A');  // Same first byte, length differs by 256.
  EXPECT_FALSE(ConstantTimeCompare(short_str, long_str));
  EXPECT_FALSE(ConstantTimeCompare(long_str, short_str));

  // Also verify length difference of 512.
  std::string longer_str(513, 'A');
  EXPECT_FALSE(ConstantTimeCompare(short_str, longer_str));
}

// =============================================================================
// JsonEscapeMinimal / JsonEscapeHtmlSafe (2.S11)
// =============================================================================

// Independently computed RFC 8259 expectation for a single byte. Deliberately
// does NOT share code with the implementation (no snprintf, hex table built
// here) so that a regression in string_util.h cannot be mirrored by the oracle.
std::string ExpectedMinimalEscape(unsigned char b) {
  switch (b) {
    case '"':
      return "\\\"";
    case '\\':
      return "\\\\";
    case '\b':
      return "\\b";
    case '\f':
      return "\\f";
    case '\n':
      return "\\n";
    case '\r':
      return "\\r";
    case '\t':
      return "\\t";
    default:
      break;
  }
  if (b < 0x20) {
    static const char kHex[] = "0123456789abcdef";
    std::string out = "\\u00";
    out += kHex[b >> 4];
    out += kHex[b & 0x0F];
    return out;
  }
  return std::string(1, static_cast<char>(b));
}

// The exact, and only, delta between the two escapers: '<' -> \u003c and
// '>' -> \u003e.
std::string SubstituteAngleBrackets(std::string_view minimal) {
  std::string out;
  for (char c : minimal) {
    if (c == '<') {
      out += "\\u003c";
    } else if (c == '>') {
      out += "\\u003e";
    } else {
      out += c;
    }
  }
  return out;
}

// Pins the RFC 8259-minimal escaping of JsonEscapeMinimal and, critically,
// that it passes '<'/'>' through raw (it is NOT HTML-<script>-safe).
TEST(StringUtilTest, JsonEscapeMinimalBasics) {
  EXPECT_EQ(JsonEscapeMinimal("a\"b\\c\n"), "a\\\"b\\\\c\\n");
  // Every named short escape, pinned by absolute value.
  EXPECT_EQ(JsonEscapeMinimal("\b\f\n\r\t"), "\\b\\f\\n\\r\\t");
  // Control char below 0x20 with no short form -> \u00XX.
  EXPECT_EQ(JsonEscapeMinimal(std::string("\x01")), "\\u0001");
  // Angle brackets pass through unescaped -- the footgun this rename guards.
  EXPECT_EQ(JsonEscapeMinimal("<b>"), "<b>");
}

// JsonEscapeHtmlSafe escapes the angle brackets so a "</script>" payload cannot
// break out of an HTML <script> block.
TEST(StringUtilTest, JsonEscapeHtmlSafeIsScriptSafe) {
  const std::string out = JsonEscapeHtmlSafe("</script>");
  EXPECT_EQ(out, "\\u003c/script\\u003e");
  EXPECT_EQ(out.find('<'), std::string::npos);
}

// Exhaustive sweep over the whole single-byte alphabet. For every byte this
// asserts BOTH an absolute RFC 8259 expectation (so a regression shared by both
// escapers -- e.g. dropping `case '\t':` from both bodies -- fails loudly) and
// the delta relation (so the two can only ever differ by the '<'/'>'
// substitution). Together these pin the delta as exactly {'<', '>'} across all
// 256 byte values.
TEST(StringUtilTest, JsonEscapeVariantsDifferOnlyOnAngleBracketsAllBytes) {
  for (int i = 0; i < 256; ++i) {
    const unsigned char b = static_cast<unsigned char>(i);
    const std::string in(1, static_cast<char>(b));
    const std::string minimal = JsonEscapeMinimal(in);
    const std::string safe = JsonEscapeHtmlSafe(in);

    // Absolute: minimal matches the independent RFC 8259 oracle.
    EXPECT_EQ(minimal, ExpectedMinimalEscape(b)) << "byte " << i;
    // Relational: the HTML-safe form is exactly minimal + the substitution.
    EXPECT_EQ(safe, SubstituteAngleBrackets(minimal)) << "byte " << i;
    // The two agree on every byte except '<' and '>'.
    if (b == '<' || b == '>') {
      EXPECT_NE(minimal, safe) << "byte " << i;
    } else {
      EXPECT_EQ(minimal, safe) << "byte " << i;
    }
  }
}

// Neither escaper touches '&' or the U+2028 / U+2029 line and paragraph
// separators; both pass their raw UTF-8 bytes through. This is intentional and
// pinned here so a change to that posture is a deliberate, visible edit.
// (Adjacent string literals keep the \x escapes from greedily absorbing the
// following character under C++ maximal munch.)
TEST(StringUtilTest, JsonEscapeVariantsLeaveAmpersandAndSeparatorsRaw) {
  EXPECT_EQ(JsonEscapeMinimal("&"), "&");
  EXPECT_EQ(JsonEscapeHtmlSafe("&"), "&");

  // U+2028 LINE SEPARATOR (e2 80 a8) -- passed through verbatim.
  const std::string u2028 =
      "x"
      "\xe2\x80\xa8"
      "y";
  EXPECT_EQ(JsonEscapeMinimal(u2028), u2028);
  EXPECT_EQ(JsonEscapeHtmlSafe(u2028), u2028);

  // U+2029 PARAGRAPH SEPARATOR (e2 80 a9) -- passed through verbatim.
  const std::string u2029 =
      "x"
      "\xe2\x80\xa9"
      "y";
  EXPECT_EQ(JsonEscapeMinimal(u2029), u2029);
  EXPECT_EQ(JsonEscapeHtmlSafe(u2029), u2029);

  // Other characters an HTML-context escaper might have handled, but neither
  // does: single quote and backtick.
  EXPECT_EQ(JsonEscapeMinimal("'`"), "'`");
  EXPECT_EQ(JsonEscapeHtmlSafe("'`"), "'`");
}

}  // namespace
}  // namespace net_instaweb
