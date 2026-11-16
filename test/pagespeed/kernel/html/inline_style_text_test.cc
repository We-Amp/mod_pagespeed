// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

#include "pagespeed/kernel/html/inline_style_text.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/empty_html_filter.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/html/html_name.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/html/html_parse.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kEsc[] = "\\3c ";

// Returns the prepared text, or "REFUSED".
GoogleString PrepareOrRefused(StringPiece css,
                              InlineStyleMode mode = InlineStyleMode::kHtml) {
  InlineStyleText out;
  const char* reason = nullptr;
  if (!InlineStyleText::Prepare(css, mode, &out, &reason)) {
    EXPECT_NE(nullptr, reason);
    EXPECT_TRUE(out.text().empty());
    return "REFUSED";
  }
  EXPECT_TRUE(IsSafeInlineStyleText(out.text(), mode)) << out.text();
  return out.text();
}

// --- The postcondition, on hand-made text (independent of Prepare). ---

TEST(IsSafeInlineStyleTextTest, Html) {
  const InlineStyleMode kHtml = InlineStyleMode::kHtml;
  EXPECT_TRUE(IsSafeInlineStyleText("", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("a{b:c}", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("(width< 1px)", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("(width<1px)", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("(width<=1px)", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("(width<.5px)", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("a<\tb<\nc<\rd<\fe", kHtml));
  EXPECT_TRUE(IsSafeInlineStyleText("a{content:\"\\3c b\"}", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("<", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<b", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a</b", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<!b", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<?b", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<<1", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a< 1<x", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText(StringPiece("a<\0", 3), kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<\x1b", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<\x80", kHtml));
  EXPECT_FALSE(IsSafeInlineStyleText("a<-1px", kHtml));
}

TEST(IsSafeInlineStyleTextTest, Strict) {
  const InlineStyleMode kStrict = InlineStyleMode::kStrict;
  EXPECT_TRUE(IsSafeInlineStyleText("", kStrict));
  EXPECT_TRUE(IsSafeInlineStyleText("a{content:\"\\3c b\"}", kStrict));
  EXPECT_FALSE(IsSafeInlineStyleText("(width< 1px)", kStrict));
  EXPECT_FALSE(IsSafeInlineStyleText("<", kStrict));
}

// Every byte after a '<' outside strings and comments: exactly ASCII
// whitespace, '=', a digit and '.' keep it; any other byte refuses.
TEST(InlineStyleTextTest, ByteAfterLessThan) {
  for (int b = 0; b < 256; ++b) {
    const char c = static_cast<char>(b);
    GoogleString css = "x<";
    css.push_back(c);
    css.append("y");
    const bool keep = c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                      c == '\f' || c == '=' || c == '.' ||
                      (c >= '0' && c <= '9');
    EXPECT_EQ(keep ? css : GoogleString("REFUSED"), PrepareOrRefused(css))
        << "byte " << b;
    EXPECT_EQ(keep, IsSafeInlineStyleText(css, InlineStyleMode::kHtml))
        << "byte " << b;
  }
}

TEST(InlineStyleTextTest, LessThanAtEndIsRefused) {
  EXPECT_EQ("REFUSED", PrepareOrRefused("<"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:c}<"));
}

TEST(InlineStyleTextTest, NoLessThanIsCopied) {
  for (InlineStyleMode mode :
       {InlineStyleMode::kHtml, InlineStyleMode::kStrict}) {
    InlineStyleText out;
    ASSERT_TRUE(
        InlineStyleText::Prepare("a{b:\"c\"}/*d*/", mode, &out, nullptr));
    EXPECT_EQ("a{b:\"c\"}/*d*/", out.text());
    EXPECT_FALSE(out.changed());
  }
}

TEST(InlineStyleTextTest, RangeQueriesAreKept) {
  const char* kCss[] = {
      "@media (width<48rem){a{b:c}}",
      "@media (width < 600px){a{b:c}}",
      "@media (400px<=width<=700px){a{b:c}}",
      "@media not (width<48rem){a{b:c}}",
      "@container (width<24rem){a{b:c}}",
      "@media (width<.5rem){a{b:c}}",
      "@media (width< calc(1px + 1em)){a{b:c}}",
  };
  for (const char* css : kCss) {
    InlineStyleText out;
    ASSERT_TRUE(
        InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &out, nullptr))
        << css;
    EXPECT_EQ(css, out.text());
    EXPECT_FALSE(out.changed());
  }
}

TEST(InlineStyleTextTest, LessThanBeforeOtherBytesIsRefused) {
  EXPECT_EQ("REFUSED", PrepareOrRefused("@media (width<calc(1px + 1em)){}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("@media (width<var(--x)){}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("@media (width<-1px){}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("<!--a{b:c}-->"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:c}</style>"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("[x=<y]{b:c}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{--x:<svg>}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused(StringPiece("a<\0b", 4)));
}

TEST(InlineStyleTextTest, StringsAreEscaped) {
  EXPECT_EQ(StrCat("@property --x{syntax:\"", kEsc, "length>\"}"),
            PrepareOrRefused("@property --x{syntax:\"<length>\"}"));
  EXPECT_EQ(StrCat("a{content:'", kEsc, "b'}"),
            PrepareOrRefused("a{content:'<b'}"));
  // The escape's own space ends it; a space after the '<' stays.
  EXPECT_EQ(StrCat("a{content:\"", kEsc, " b\"}"),
            PrepareOrRefused("a{content:\"< b\"}"));
  // An escape right before the '<' is ended by the new backslash.
  EXPECT_EQ(StrCat("a{content:\"\\26", kEsc, "\"}"),
            PrepareOrRefused("a{content:\"\\26<\"}"));
  EXPECT_EQ(StrCat("a{content:\"", kEsc, "/style>\"}"),
            PrepareOrRefused("a{content:\"</style>\"}"));
  EXPECT_EQ(StrCat("[x=\"", kEsc, "y\"]{b:c}"),
            PrepareOrRefused("[x=\"<y\"]{b:c}"));
  EXPECT_EQ(StrCat("@import \"", kEsc, "a.css\";"),
            PrepareOrRefused("@import \"<a.css\";"));
}

TEST(InlineStyleTextTest, StringEndsAsInCss) {
  // An escaped quote does not end the string.
  EXPECT_EQ(StrCat("a{b:\"c\\\"", kEsc, "d\"}"),
            PrepareOrRefused("a{b:\"c\\\"<d\"}"));
  // The other quote does not end it either.
  EXPECT_EQ(StrCat("a{b:\"'", kEsc, "d\"}"), PrepareOrRefused("a{b:\"'<d\"}"));
  // A newline ends it (a bad string); what follows is outside.
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"c\n<d}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"c\r<d}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"c\f<d}"));
  // A backslash before a newline continues it; CR LF is one newline.
  EXPECT_EQ(StrCat("a{b:\"c\\\n", kEsc, "d\"}"),
            PrepareOrRefused("a{b:\"c\\\n<d\"}"));
  EXPECT_EQ(StrCat("a{b:\"c\\\r\n", kEsc, "d\"}"),
            PrepareOrRefused("a{b:\"c\\\r\n<d\"}"));
  // Outside a string a backslash before a newline escapes nothing.
  EXPECT_EQ(StrCat("a\\\n\"", kEsc, "b\""), PrepareOrRefused("a\\\n\"<b\""));
  // Unterminated at the end of the text.
  EXPECT_EQ(StrCat("a{b:\"", kEsc, "c"), PrepareOrRefused("a{b:\"<c"));
}

TEST(InlineStyleTextTest, CommentsAreEscaped) {
  EXPECT_EQ(StrCat("/*", kEsc, "b>*/a{}"), PrepareOrRefused("/*<b>*/a{}"));
  EXPECT_EQ(StrCat("/*! licence ", kEsc, "me@example.com> */a{}"),
            PrepareOrRefused("/*! licence <me@example.com> */a{}"));
  // A quote in a comment opens nothing.
  EXPECT_EQ("REFUSED", PrepareOrRefused("/*\"*/<b"));
  // A backslash in a comment escapes nothing.
  EXPECT_EQ("REFUSED", PrepareOrRefused("/*\\*/<b"));
  EXPECT_EQ(StrCat("/*\\", kEsc, "*/"), PrepareOrRefused("/*\\<*/"));
  // Unterminated at the end of the text.
  EXPECT_EQ(StrCat("a{}/*", kEsc, "b"), PrepareOrRefused("a{}/*<b"));
  // "/*" in a string opens no comment.
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"/*\"}<c*/"));
}

TEST(InlineStyleTextTest, EscapedLessThanIsRewritten) {
  EXPECT_EQ(StrCat(".a", kEsc, "b{c:d}"), PrepareOrRefused(".a\\<b{c:d}"));
  EXPECT_EQ(StrCat(".content-\\[\\'", kEsc, "\\'\\]{c:d}"),
            PrepareOrRefused(".content-\\[\\'\\<\\'\\]{c:d}"));
  EXPECT_EQ(StrCat("a{b:\"", kEsc, "c\"}"), PrepareOrRefused("a{b:\"\\<c\"}"));
  // Text that already holds the escape is left as it is.
  EXPECT_EQ(".a\\3c b{c:\"\\3C d\"}",
            PrepareOrRefused(".a\\3c b{c:\"\\3C d\"}"));
}

TEST(InlineStyleTextTest, UnquotedUrl) {
  EXPECT_EQ(StrCat("a{b:url(x", kEsc, "y)}"),
            PrepareOrRefused("a{b:url(x<y)}"));
  EXPECT_EQ(StrCat("a{b:URL(x", kEsc, "y)}"),
            PrepareOrRefused("a{b:URL(x<y)}"));
  EXPECT_EQ(StrCat("a{b:url(  x", kEsc, "y)}"),
            PrepareOrRefused("a{b:url(  x<y)}"));
  // A quote inside an unquoted url opens no string.
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:url(x\"y)}c<d"));
  EXPECT_EQ(StrCat("a{b:url(x\"y", kEsc, ")}c"),
            PrepareOrRefused("a{b:url(x\"y<)}c"));
  // An escaped parenthesis does not end it.
  EXPECT_EQ(StrCat("a{b:url(x\\)", kEsc, "y)}"),
            PrepareOrRefused("a{b:url(x\\)<y)}"));
  // A quoted url is a string.
  EXPECT_EQ(StrCat("a{b:url( \"x", kEsc, "y\")}"),
            PrepareOrRefused("a{b:url( \"x<y\")}"));
  // Not a url: another function whose name ends in "url".
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:my-url(x<y)}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:xurl(x<y)}"));
}

TEST(InlineStyleTextTest, StrictRefusesAnyLessThan) {
  const InlineStyleMode kStrict = InlineStyleMode::kStrict;
  EXPECT_EQ("REFUSED", PrepareOrRefused("@media (width < 600px){}", kStrict));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"<c\"}", kStrict));
  EXPECT_EQ("REFUSED", PrepareOrRefused("/*<b>*/", kStrict));
  EXPECT_EQ("REFUSED", PrepareOrRefused(".a\\<b{}", kStrict));
  EXPECT_EQ("a{b:\"\\3c c\"}", PrepareOrRefused("a{b:\"\\3c c\"}", kStrict));
}

TEST(InlineStyleTextTest, NonUtf8WithLessThanIsRefused) {
  // Valid UTF-8 is fine.
  EXPECT_EQ(StrCat("a{b:\"\xc3\xa9", kEsc, "\"}"),
            PrepareOrRefused("a{b:\"\xc3\xa9<\"}"));
  EXPECT_EQ(StrCat("a{b:\"\xf0\x9f\x98\x80", kEsc, "\"}"),
            PrepareOrRefused("a{b:\"\xf0\x9f\x98\x80<\"}"));
  // Other high bytes, together with a '<', are not.
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"\xe9<\"}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"<\"}\x81"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"\xc0\xaf<\"}"));  // overlong
  EXPECT_EQ("REFUSED",
            PrepareOrRefused("a{b:\"\xed\xa0\x80<\"}"));  // surrogate
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"\xf4\x90\x80\x80<\"}"));
  EXPECT_EQ("REFUSED", PrepareOrRefused("a{b:\"<\"}\xe2\x82"));  // truncated
  // Without a '<' there is nothing to decide.
  EXPECT_EQ("a{b:\"\xe9\"}", PrepareOrRefused("a{b:\"\xe9\"}"));
}

TEST(InlineStyleTextTest, ChangedFlag) {
  InlineStyleText out;
  ASSERT_TRUE(InlineStyleText::Prepare("a{b:\"<\"}", InlineStyleMode::kHtml,
                                       &out, nullptr));
  EXPECT_TRUE(out.changed());
  ASSERT_TRUE(InlineStyleText::Prepare("@media (a<1px){}",
                                       InlineStyleMode::kHtml, &out, nullptr));
  EXPECT_FALSE(out.changed());
  // A refusal leaves an empty text.
  EXPECT_FALSE(
      InlineStyleText::Prepare("<b", InlineStyleMode::kHtml, &out, nullptr));
  EXPECT_TRUE(out.text().empty());
  EXPECT_FALSE(out.changed());
}

// The lookup needs the labels sorted; each one gets kHtml.
TEST(InlineStyleTextTest, CharsetLabelsAreSorted) {
  const StringPieceVector labels = InlineStyleCharsetLabelsForTesting();
  EXPECT_EQ(151u, labels.size());
  EXPECT_TRUE(std::is_sorted(labels.begin(), labels.end()));
  EXPECT_TRUE(std::adjacent_find(labels.begin(), labels.end()) == labels.end());
  for (StringPiece label : labels) {
    EXPECT_EQ(InlineStyleMode::kHtml, InlineStyleModeForCharset(label))
        << label;
  }
}

TEST(InlineStyleTextTest, ModeForCharset) {
  const InlineStyleMode kHtml = InlineStyleMode::kHtml;
  const InlineStyleMode kStrict = InlineStyleMode::kStrict;
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("utf-8"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("UTF-8"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset(" utf-8 "));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("utf8"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("windows-1250"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("Windows-1252"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("windows-1258"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("iso-8859-1"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("ISO-8859-15"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("iso-8859-16"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset(""));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("windows-1259"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("windows-125"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("windows-12520"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("windows-874"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-0"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-11"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-12"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("iso-8859-9"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("iso-8859-8-i"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("latin1"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("us-ascii"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("unicode-1-1-utf-8"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("\tUTF8\n"));
  EXPECT_EQ(kHtml, InlineStyleModeForCharset("x-cp1250"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("utf-7"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("cp1259"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("utf-8;"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("utf\xc2\xa0-8"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-01"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-17"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-1x"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-8859-"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("windows-874"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("iso-2022-jp"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("shift_jis"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("gbk"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("big5"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("utf-16"));
  EXPECT_EQ(kStrict, InlineStyleModeForCharset("\"utf-8\""));
  EXPECT_STREQ("h", InlineStyleModeKey(kHtml));
  EXPECT_STREQ("x", InlineStyleModeKey(kStrict));
}

// --- A second opinion from a real tokenizer: this tree's HTML parser. ---

// Records what the parser makes of a page.
class EventRecorder : public EmptyHtmlFilter {
 public:
  const char* Name() const override { return "EventRecorder"; }
  void StartElement(HtmlElement* element) override {
    events_.push_back(StrCat("<", element->name_str(), ">"));
    if (element->keyword() == HtmlName::kI) {
      const char* id = element->AttributeValue(HtmlName::kId);
      events_.back().append(id == nullptr ? "" : id);
    }
  }
  void EndElement(HtmlElement* element) override {
    events_.push_back(StrCat("</", element->name_str(), ">"));
  }
  void Characters(HtmlCharactersNode* characters) override {
    // The parser may split text; join what it splits.
    if (!events_.empty() && events_.back().compare(0, 5, "text:") == 0) {
      events_.back().append(characters->contents());
    } else {
      events_.push_back(StrCat("text:", characters->contents()));
    }
  }
  void Comment(HtmlCommentNode* comment) override {
    events_.push_back("comment");
  }
  void Cdata(HtmlCdataNode* cdata) override { events_.push_back("cdata"); }
  void IEDirective(HtmlIEDirectiveNode* directive) override {
    events_.push_back("iedirective");
  }
  void Directive(HtmlDirectiveNode* directive) override {
    events_.push_back("directive");
  }
  std::vector<GoogleString> events_;
};

std::vector<GoogleString> ParseEvents(StringPiece html) {
  HtmlKeywords::Init();
  NullMessageHandler handler;
  HtmlParse parse(&handler);
  EventRecorder recorder;
  parse.AddFilter(&recorder);
  parse.StartParse("http://example.com/");
  parse.ParseText(html);
  parse.FinishParse();
  return recorder.events_;
}

// Accepted text, between a start and an end tag, is one run of text to the
// parser: as the contents of a <style> element, and also as plain text in an
// element, where a markup opener would be read as one.
void ExpectOneRunOfText(StringPiece text) {
  const GoogleString t(text.data(), text.size());
  const std::vector<GoogleString> in_style =
      ParseEvents(StrCat("<style>", t, "</style><i id=end></i>"));
  const std::vector<GoogleString> expected_style = {
      "<style>", StrCat("text:", t), "</style>", "<i>end", "</i>"};
  std::vector<GoogleString> expected_div = {"<div>", StrCat("text:", t),
                                            "</div>", "<i>end", "</i>"};
  if (t.empty()) {
    expected_div.erase(expected_div.begin() + 1);
  }
  std::vector<GoogleString> expected_empty_style = expected_style;
  if (t.empty()) {
    expected_empty_style.erase(expected_empty_style.begin() + 1);
  }
  EXPECT_EQ(expected_empty_style, in_style) << t;
  EXPECT_EQ(expected_div,
            ParseEvents(StrCat("<div>", t, "</div><i id=end></i>")))
      << t;
}

TEST(InlineStyleTextTest, ParserReadsAcceptedTextAsText) {
  const char* kCss[] = {
      "@property --x{syntax:\"<length>\"}",
      "@media (width < 600px){a{b:c}}@media (width<48rem){a{b:c}}",
      "@media (400px<=width<=700px){a{b:c}}",
      "a{content:\"</style><i id=end>\"}/*<!--*/",
      ".content-\\[\\'\\<\\'\\]{content:'<'}",
      "a{b:url(x</style>)}",
      "a<.5em{}a< 1{}a<\t1{}",
  };
  for (const char* css : kCss) {
    InlineStyleText out;
    ASSERT_TRUE(
        InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &out, nullptr))
        << css;
    ExpectOneRunOfText(out.text());
  }
}

// --- Randomized: the output properties on generated text. ---

// A second statement of the postcondition, written apart from
// IsSafeInlineStyleText().
bool IndependentlySafe(const GoogleString& text, InlineStyleMode mode) {
  static const GoogleString kAllowed("\t\n\f\r =.0123456789");
  size_t pos = 0;
  while ((pos = text.find('<', pos)) != GoogleString::npos) {
    if (mode == InlineStyleMode::kStrict) return false;
    if (pos + 1 == text.size()) return false;
    if (kAllowed.find(text[pos + 1]) == GoogleString::npos) return false;
    ++pos;
  }
  return true;
}

// Every input byte is copied, or a '<' or a backslash and '<' became the
// escape.
bool InLockstep(const GoogleString& in, const GoogleString& out) {
  const StringPiece esc(kEsc);
  size_t i = 0, j = 0;
  while (i < in.size()) {
    const bool out_has_esc = StringPiece(out).substr(j).starts_with(esc);
    if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == '<' && out_has_esc) {
      i += 2;
      j += esc.size();
    } else if (in[i] == '<' && out_has_esc) {
      i += 1;
      j += esc.size();
    } else if (j < out.size() && in[i] == out[j]) {
      ++i;
      ++j;
    } else {
      return false;
    }
  }
  return j == out.size();
}

GoogleString Generate(std::mt19937* rng) {
  static const char* const kTokens[] = {
      "\"",    "'",        "\\",       "/*",   "*/",
      "url(",  "URL( ",    "url(\"",   "(",    ")",
      "\n",    "\r\n",     "\f",       "<",    "<",
      "<",     "</style>", "</STYLE ", "<!--", "-->",
      "<?x",   "<a",       "<1",       "< ",   "<=",
      "<.",    "\x1b(B",   "\x1b$B",   " ",    "a",
      "{",     "}",        ":",        ";",    "@media (width",
      "\\3c ", "\\<",      "\xc3\xa9", "\xe9", "\xff",
      "0",     "=",        "."};
  const size_t kNumTokens = sizeof(kTokens) / sizeof(kTokens[0]);
  std::uniform_int_distribution<int> len_dist(0, 24);
  std::uniform_int_distribution<size_t> token_dist(0, kNumTokens - 1);
  std::uniform_int_distribution<int> kind_dist(0, 9);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  GoogleString css;
  const int len = len_dist(*rng);
  for (int k = 0; k < len; ++k) {
    if (kind_dist(*rng) == 0) {
      css.push_back(static_cast<char>(byte_dist(*rng)));
    } else if (kind_dist(*rng) == 0) {
      css.push_back('\0');
    } else {
      css.append(kTokens[token_dist(*rng)]);
    }
  }
  return css;
}

TEST(InlineStyleTextTest, RandomizedOutputProperties) {
  const uint32_t kSeeds[] = {1, 20261114, 0x5eed};
  const int kCasesPerSeed = 70000;
  int html_accepted = 0, html_changed = 0;
  for (uint32_t seed : kSeeds) {
    std::mt19937 rng(seed);
    for (int n = 0; n < kCasesPerSeed; ++n) {
      const GoogleString css = Generate(&rng);
      InlineStyleText html, strict;
      const bool html_ok =
          InlineStyleText::Prepare(css, InlineStyleMode::kHtml, &html, nullptr);
      const bool strict_ok = InlineStyleText::Prepare(
          css, InlineStyleMode::kStrict, &strict, nullptr);
      // Safe, by the second statement of the property.
      if (html_ok) {
        ASSERT_TRUE(IndependentlySafe(html.text(), InlineStyleMode::kHtml))
            << "seed " << seed << " case " << n;
        ++html_accepted;
        if (html.changed()) ++html_changed;
      }
      if (strict_ok) {
        ASSERT_TRUE(IndependentlySafe(strict.text(), InlineStyleMode::kStrict))
            << "seed " << seed << " case " << n;
        // Strict is a copy, and whatever it accepts HTML accepts unchanged.
        ASSERT_EQ(css, strict.text());
        ASSERT_TRUE(html_ok);
        ASSERT_EQ(css, html.text());
      }
      if (!html_ok) {
        ASSERT_FALSE(strict_ok);
        continue;
      }
      // Lockstep with the input.
      ASSERT_TRUE(InLockstep(css, html.text()))
          << "seed " << seed << " case " << n;
      // Idempotent.
      InlineStyleText again;
      ASSERT_TRUE(InlineStyleText::Prepare(html.text(), InlineStyleMode::kHtml,
                                           &again, nullptr))
          << "seed " << seed << " case " << n;
      ASSERT_EQ(html.text(), again.text()) << "seed " << seed << " case " << n;
      // And a second opinion from the parser, on some of them.
      if (n % 16 == 0 && html.text().find('\0') == GoogleString::npos) {
        ExpectOneRunOfText(html.text());
        if (HasFailure()) {
          FAIL() << "seed " << seed << " case " << n;
        }
      }
    }
  }
  // The generator reaches both outcomes and the rewrite.
  EXPECT_LT(1000, html_accepted);
  EXPECT_LT(1000, html_changed);
}

}  // namespace

}  // namespace net_instaweb
