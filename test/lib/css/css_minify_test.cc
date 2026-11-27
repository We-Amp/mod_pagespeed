// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/css/css_minify.h"

#include <string>

#include "gtest/gtest.h"

namespace pagespeed::css {
namespace {

void ExpectMinify(std::string_view input, std::string_view expected) {
  std::string output;
  ASSERT_TRUE(MinifyCss(input, &output));
  EXPECT_EQ(expected, output) << "Input: " << input;
  // Idempotence oracle: minify(minify(x)) == minify(x) for every
  // minify input exercised here.
  std::string remified;
  ASSERT_TRUE(MinifyCss(output, &remified));
  EXPECT_EQ(output, remified)
      << "MinifyCss is not idempotent; input: " << input;
}

TEST(CssMinifyTest, BasicWhitespaceRemoval) {
  ExpectMinify("  h1  {  color : red  }  ", "h1{color :red}");
}

TEST(CssMinifyTest, CommentRemoval) {
  ExpectMinify("/* comment */ h1 { color: red; }", "h1{color:red}");
}

TEST(CssMinifyTest, MultipleRules) {
  ExpectMinify("h1 { color: red; } p { font-size: 12px; }",
               "h1{color:red}p{font-size:12px}");
}

TEST(CssMinifyTest, TrailingSemicolonRemoval) {
  ExpectMinify("h1 { color: red; }", "h1{color:red}");
}

TEST(CssMinifyTest, PreserveSingleQuotedString) {
  ExpectMinify("h1::after { content: '  spaces  '; }",
               "h1::after{content:'  spaces  '}");
}

TEST(CssMinifyTest, PreserveDoubleQuotedString) {
  ExpectMinify("h1 { background: url(\"path/to/img.png\"); }",
               "h1{background:url(\"path/to/img.png\")}");
}

TEST(CssMinifyTest, MediaRule) {
  ExpectMinify("@media screen and (max-width: 600px) { h1 { color: red; } }",
               "@media screen and (max-width:600px){h1{color:red}}");
}

TEST(CssMinifyTest, ImportRule) {
  ExpectMinify("@import url('style.css');", "@import url('style.css');");
}

TEST(CssMinifyTest, MultipleComments) {
  ExpectMinify("/* c1 */ h1 /* c2 */ { /* c3 */ color: red; /* c4 */ }",
               "h1{color:red}");
}

TEST(CssMinifyTest, DecimalOptimization) {
  ExpectMinify("h1 { margin: 0.5em 0.25px; }", "h1{margin:.5em .25px}");
}

TEST(CssMinifyTest, EmptyInput) { ExpectMinify("", ""); }

TEST(CssMinifyTest, AlreadyMinified) {
  ExpectMinify("h1{color:red}", "h1{color:red}");
}

TEST(CssMinifyTest, Newlines) {
  ExpectMinify("h1\n{\n  color:\n    red;\n}", "h1{color:red}");
}

TEST(CssMinifyTest, Combinators) {
  ExpectMinify("div > p + span ~ em { color: red; }",
               "div>p+span~em{color:red}");
}

TEST(CssMinifyTest, MultipleSelectors) {
  ExpectMinify("h1, h2, h3 { color: red; }", "h1,h2,h3{color:red}");
}

TEST(CssMinifyTest, PseudoElements) {
  ExpectMinify("h1::before { content: ''; }", "h1::before{content:''}");
}

TEST(CssMinifyTest, EscapeInStrings) {
  ExpectMinify(R"(h1 { content: "quote\"here"; })",
               R"(h1{content:"quote\"here"})");
}

TEST(CssMinifyTest, UrlWithoutQuotes) {
  ExpectMinify("h1 { background: url(img.png); }",
               "h1{background:url(img.png)}");
}

TEST(CssMinifyTest, Charset) {
  ExpectMinify("@charset \"UTF-8\";\nh1 { color: red; }",
               "@charset \"UTF-8\";h1{color:red}");
}

TEST(CssMinifyTest, CalcPreservesSpaces) {
  ExpectMinify("h1 { width: calc(100% - 20px); }",
               "h1{width:calc(100% - 20px)}");
}

TEST(CssMinifyTest, Important) {
  ExpectMinify("h1 { color: red !important; }", "h1{color:red!important}");
}

TEST(CssMinifyTest, NegativeDecimal) {
  ExpectMinify("h1 { margin: -0.5em; }", "h1{margin:-.5em}");
}

TEST(CssMinifyTest, NullOutput) { EXPECT_FALSE(MinifyCss("h1 { }", nullptr)); }

TEST(CssMinifyTest, OnlyWhitespace) { ExpectMinify("   \t\n  ", ""); }

TEST(CssMinifyTest, OnlyComments) { ExpectMinify("/* comment only */", ""); }

// ========== Preserved comments (#582) ==========
// A comment starting with /*! or containing @license or @preserve
// survives byte for byte, in its position (whitespace around it still
// collapses by the normal rules); every other comment is still
// dropped.  ExpectMinify re-runs the idempotence oracle on each.

TEST(CssMinifyTest, PreservedCommentAtTop) {
  ExpectMinify("/*! banner v2 */ h1 { color: red; }",
               "/*! banner v2 */ h1{color:red}");
}

TEST(CssMinifyTest, PreservedCommentBetweenRules) {
  ExpectMinify("h1 { color: red; } /*! keep me */ p { color: blue; }",
               "h1{color:red}/*! keep me */ p{color:blue}");
}

TEST(CssMinifyTest, PreservedCommentInsideRule) {
  ExpectMinify("h1 { /*! keep */ color: red; }", "h1{/*! keep */ color:red}");
}

TEST(CssMinifyTest, PreservedCommentStillMinifiesAroundIt) {
  ExpectMinify("/*! v */ h1 { margin: 0.5em; }", "/*! v */ h1{margin:.5em}");
}

TEST(CssMinifyTest, OrdinaryCommentStillDroppedNextToPreserved) {
  ExpectMinify("/* drop */ h1 { /* drop too */ color: red; /* drop */ }",
               "h1{color:red}");
  ExpectMinify("h1{color:red}/* ordinary *//*! kept */p{color:blue}",
               "h1{color:red}/*! kept */p{color:blue}");
}

TEST(CssMinifyTest, LicenseAndPreserveCommentsKept) {
  ExpectMinify("/* @license MIT */ h1{color:red}",
               "/* @license MIT */ h1{color:red}");
  ExpectMinify("/*\n@preserve\n*/ h1{color:red}",
               "/*\n@preserve\n*/ h1{color:red}");
}

TEST(CssMinifyTest, PreservedCommentInCustomPropertyValue) {
  // Custom-property values are opaque; the comment's bytes are value
  // content and stay verbatim.
  ExpectMinify(":root{--x:/*! keep */ v}", ":root{--x:/*! keep */ v}");
}

TEST(CssMinifyTest, PreservedCommentMarkerInsideStringOrUrlIsNotAComment) {
  // A "/*!" inside a string or url() is content, not a comment: it is
  // neither parsed as a preserved comment nor altered.
  ExpectMinify("h1::after { content: \"/*! x */\"; }",
               "h1::after{content:\"/*! x */\"}");
  ExpectMinify("h1 { background: url(\"a/*!b*/c.png\"); }",
               "h1{background:url(\"a/*!b*/c.png\")}");
  ExpectMinify("h1 { background: url(a/*!b*/c.png); }",
               "h1{background:url(a/*!b*/c.png)}");
}

TEST(CssMinifyTest, PreservedCommentUnterminated) {
  // No "*/" and no crash: the comment runs to end of input and is
  // closed on output, so the result stays well-formed when it is
  // concatenated with other CSS.
  ExpectMinify("h1{color:red}/*! never closed",
               "h1{color:red}/*! never closed*/");
  ExpectMinify("/*!", "/*!*/");
  // "/*!/" is not a closed comment either.
  ExpectMinify("/*!/", "/*!/*/");
  // Concatenated output keeps the following rule.
  std::string out;
  ASSERT_TRUE(pagespeed::css::MinifyCss("a{b:c}/*! open", &out));
  std::string both;
  ASSERT_TRUE(pagespeed::css::MinifyCss(out + "p{q:r}", &both));
  EXPECT_EQ(both, "a{b:c}/*! open*/p{q:r}");
}

TEST(CssMinifyTest, NestedMediaRule) {
  ExpectMinify(
      "@media screen { @media (min-width: 768px) { h1 { color: red; } } }",
      "@media screen{@media (min-width:768px){h1{color:red}}}");
}

TEST(CssMinifyTest, MultipleSemicolons) {
  ExpectMinify("h1 { color: red;; }", "h1{color:red}");
}

TEST(CssMinifyTest, CalcWithAddition) {
  ExpectMinify("h1 { width: calc(50% + 10px); }", "h1{width:calc(50% + 10px)}");
}

TEST(CssMinifyTest, CalcWithMultiplication) {
  // Spaces around * and / are NOT required in calc, so they should be removed.
  ExpectMinify("h1 { width: calc(100% * 0.5); }", "h1{width:calc(100%*.5)}");
}

// ========== Phase 1.3: calc() whitespace preservation ==========

TEST(CssMinifyTest, CalcWithDivision) {
  ExpectMinify("h1 { width: calc(100% / 3); }", "h1{width:calc(100%/3)}");
}

TEST(CssMinifyTest, CalcNestedParens) {
  // Nested calc() with mixed operators.
  ExpectMinify("h1 { width: calc((100% - 40px) / 2); }",
               "h1{width:calc((100% - 40px)/2)}");
}

TEST(CssMinifyTest, CalcWithMultipleSubtractions) {
  ExpectMinify("h1 { margin: calc(100vh - 80px - 20px); }",
               "h1{margin:calc(100vh - 80px - 20px)}");
}

TEST(CssMinifyTest, CalcInShorthand) {
  // calc() inside a shorthand property with multiple values.
  ExpectMinify("h1 { padding: calc(10px + 2%) 0 calc(20px - 1%); }",
               "h1{padding:calc(10px + 2%) 0 calc(20px - 1%)}");
}

// ========== Phase 1.4: @font-face preservation ==========

TEST(CssMinifyTest, FontFaceBasic) {
  ExpectMinify(
      "@font-face {\n"
      "  font-family: 'CustomFont';\n"
      "  src: url('font.woff2') format('woff2');\n"
      "}",
      "@font-face{font-family:'CustomFont';src:url('font.woff2') "
      "format('woff2')}");
}

TEST(CssMinifyTest, FontFaceMultipleSources) {
  ExpectMinify(
      "@font-face {\n"
      "  font-family: 'CustomFont';\n"
      "  src: local('Custom Font'),\n"
      "       url('font.woff2') format('woff2'),\n"
      "       url('font.woff') format('woff');\n"
      "  font-weight: 400;\n"
      "  font-style: normal;\n"
      "  font-display: swap;\n"
      "}",
      "@font-face{font-family:'CustomFont';"
      "src:local('Custom Font'),"
      "url('font.woff2') format('woff2'),"
      "url('font.woff') format('woff');"
      "font-weight:400;font-style:normal;font-display:swap}");
}

TEST(CssMinifyTest, FontFaceUnicodeRange) {
  ExpectMinify(
      "@font-face {\n"
      "  font-family: 'Icons';\n"
      "  src: url('icons.woff2') format('woff2');\n"
      "  unicode-range: U+E000-E0FF, U+F000-F0FF;\n"
      "}",
      "@font-face{font-family:'Icons';"
      "src:url('icons.woff2') format('woff2');"
      "unicode-range:U+E000-E0FF,U+F000-F0FF}");
}

TEST(CssMinifyTest, FontFaceWithFontDisplay) {
  ExpectMinify(
      "@font-face {\n"
      "  font-family: 'MyFont';\n"
      "  src: url('/fonts/myfont.woff2') format('woff2');\n"
      "  font-display: swap;\n"
      "}",
      "@font-face{font-family:'MyFont';"
      "src:url('/fonts/myfont.woff2') format('woff2');"
      "font-display:swap}");
}

// ========== Phase 3.4: prefers-color-scheme preservation ==========

TEST(CssMinifyTest, PrefersColorSchemeDark) {
  ExpectMinify(
      "@media (prefers-color-scheme: dark) {\n"
      "  body { background: #111; color: #eee; }\n"
      "}",
      "@media (prefers-color-scheme:dark){body{background:#111;color:#eee}}");
}

TEST(CssMinifyTest, PrefersColorSchemeLight) {
  ExpectMinify(
      "@media (prefers-color-scheme: light) {\n"
      "  body { background: #fff; color: #333; }\n"
      "}",
      "@media (prefers-color-scheme:light){body{background:#fff;color:#333}}");
}

// ========== Phase 3.5: prefers-reduced-motion preservation ==========

TEST(CssMinifyTest, PrefersReducedMotion) {
  ExpectMinify(
      "@media (prefers-reduced-motion: reduce) {\n"
      "  *, *::before, *::after {\n"
      "    animation-duration: 0.01ms !important;\n"
      "    transition-duration: 0.01ms !important;\n"
      "  }\n"
      "}",
      "@media (prefers-reduced-motion:reduce){"
      "*,*::before,*::after{"
      "animation-duration:.01ms!important;"
      "transition-duration:.01ms!important}}");
}

// ========== Phase 3.8: CSS data: URI preservation ==========

TEST(CssMinifyTest, DataUriInUrlQuoted) {
  ExpectMinify("h1 { background: url(\"data:image/png;base64,iVBOR+w==\"); }",
               "h1{background:url(\"data:image/png;base64,iVBOR+w==\")}");
}

TEST(CssMinifyTest, DataUriInUrlUnquoted) {
  ExpectMinify(
      "h1 { background: url(data:image/svg+xml,%3Csvg%3E%3C/svg%3E); }",
      "h1{background:url(data:image/svg+xml,%3Csvg%3E%3C/svg%3E)}");
}

TEST(CssMinifyTest, DataUriWithSpacesInQuotes) {
  // Quoted data URI with base64 that may have internal + or / characters.
  ExpectMinify("h1 { background: url('data:image/gif;base64,R0lGODlh/w=='); }",
               "h1{background:url('data:image/gif;base64,R0lGODlh/w==')}");
}

// ========== Phase 4.3: CSS custom properties / var() ==========

TEST(CssMinifyTest, CssCustomProperties) {
  ExpectMinify(":root { --my-color: #ff0; }", ":root{--my-color:#ff0}");
}

TEST(CssMinifyTest, CssVarUsage) {
  ExpectMinify("h1 { color: var(--my-color); }", "h1{color:var(--my-color)}");
}

TEST(CssMinifyTest, CssVarWithFallback) {
  ExpectMinify("h1 { color: var(--my-color, #ff0); }",
               "h1{color:var(--my-color,#ff0)}");
}

TEST(CssMinifyTest, CssVarInCalc) {
  ExpectMinify("h1 { width: calc(var(--sidebar) + 20px); }",
               "h1{width:calc(var(--sidebar) + 20px)}");
}

// ========== Phase 4.4: @media print preservation ==========

TEST(CssMinifyTest, MediaPrint) {
  ExpectMinify("@media print { body { font-size: 12pt; } }",
               "@media print{body{font-size:12pt}}");
}

TEST(CssMinifyTest, MediaPrintWithScreenRules) {
  ExpectMinify(
      "body { color: #333; }\n"
      "@media print {\n"
      "  body { color: #000; font-size: 12pt; }\n"
      "  .no-print { display: none; }\n"
      "}",
      "body{color:#333}"
      "@media print{body{color:#000;font-size:12pt}.no-print{display:none}}");
}

// ========== Phase 4.5: decimal optimization edge cases ==========

TEST(CssMinifyTest, DecimalWithPrecedingDigit) {
  // "10.5" must NOT strip the zero — it's preceded by another digit.
  ExpectMinify("h1 { width: 10.5em; }", "h1{width:10.5em}");
}

TEST(CssMinifyTest, UnquotedUrlDecimalNotStripped) {
  // Phase 4 must skip decimal optimization inside unquoted url() content.
  ExpectMinify("h1{background:url(path/v0.5/img.png)}",
               "h1{background:url(path/v0.5/img.png)}");
}

// ========== #1163: decimal rule respects identifier boundaries ==========

TEST(CssMinifyTest, DecimalInClassSelectorNotStripped) {
  // "0.5" inside an identifier is ident content, not a number: Phase 4
  // must not rename the class (".a0.5" -> ".a.5" was valid-input
  // corruption, #1163).
  ExpectMinify(".a0.5{c:d}", ".a0.5{c:d}");
  ExpectMinify("div.a0.5{c:d}", "div.a0.5{c:d}");
  ExpectMinify(".a0.5 .b0.5{c:d}", ".a0.5 .b0.5{c:d}");
}

TEST(CssMinifyTest, DecimalInIdSelectorNotStripped) {
  ExpectMinify("#id0.5{c:d}", "#id0.5{c:d}");
}

TEST(CssMinifyTest, DecimalInDashedIdentNotStripped) {
  // A '-' following an ident char continues the identifier, so the '0'
  // after it is ident content too ("a-0.5" is a valid custom-element
  // name selector).
  ExpectMinify("a-0.5{x:y}", "a-0.5{x:y}");
  ExpectMinify(".a--0.5{c:d}", ".a--0.5{c:d}");
  ExpectMinify(".x0.5y{c:d}", ".x0.5y{c:d}");
}

TEST(CssMinifyTest, DecimalAtNumberStartStillStripped) {
  // The rule's legitimate function is unchanged: at a number-token
  // start "0.5" -> ".5", including after a bare '-' number sign, and
  // "10.5" (digit-preceded) stays as-is.
  ExpectMinify("a{width:0.5px}", "a{width:.5px}");
  ExpectMinify("a{width:-0.5px}", "a{width:-.5px}");
  ExpectMinify("a{margin:0.5}", "a{margin:.5}");
  ExpectMinify("a{margin:0 -0.5px}", "a{margin:0 -.5px}");
  ExpectMinify("a{width:10.5em}", "a{width:10.5em}");
}

TEST(CssMinifyTest, DecimalAfterStringIsNumberStart) {
  // A '0' after a closed string is a number token (adjacent <string>
  // and <number> values), so the strip still fires; the boundary guard
  // only blocks ident continuation and escaped chars.
  ExpectMinify("a{content:\"x\"0.5}", "a{content:\"x\".5}");
  // Attribute-selector strings are strings: untouched.
  ExpectMinify("[data-x=\"0.5\"]{c:d}", "[data-x=\"0.5\"]{c:d}");
}

TEST(CssMinifyTest, DecimalAfterEscapeNotStripped) {
  // Escaped chars are ident content: ".a\'0.5" (P4-a escape guard) and
  // ".a\ 0.5" both keep their '0' via the boundary guard's backslash
  // parity check.
  ExpectMinify(".a\\'0.5{c:d}", ".a\\'0.5{c:d}");
  ExpectMinify(".a\\ 0.5{c:d}", ".a\\ 0.5{c:d}");
}

// ========== #1238: decimal rule respects dot boundaries ==========

TEST(CssMinifyTest, DecimalBetweenDotsNotStripped) {
  // A '0' between two dots never starts a valid number token, so the
  // strip must not fire there — ".0." became ".." (run 30913256122, 15
  // artifacts).  Invalid-input-only, but byte-stability on garbage is
  // what keeps the token oracle's number channel intact.
  ExpectMinify("h:5-.0.", "h:5-.0.");  // minimal repro (crash-87654c53)
  ExpectMinify("a{b:.0.}", "a{b:.0.}");
  ExpectMinify("a{b:-.0.}", "a{b:-.0.}");
  // Only the dot-adjacent '0' is protected: a number-start '0' earlier
  // in the same token still strips ("0.0." -> ".0.").
  ExpectMinify("a{b:0.0.}", "a{b:.0.}");
}

TEST(CssMinifyTest, DecimalBetweenDotsArtifactSkeletons) {
  // Minimized skeletons of the run-30913256122 artifacts: the ".0"
  // token survives everywhere (whitespace phases still apply).
  ExpectMinify("t)}\t);5-.0.t", "t)});5-.0.t");          // crash-28ef2f47
  ExpectMinify(").0-.0-5-.0.at05", ").0-.0-5-.0.at05");  // crash-3ba64d9f
  ExpectMinify("-0.--.0-.0..4", "-.--.0-.0..4");         // crash-3c5e5c79
  ExpectMinify("A{Wa-:\tp}.0-5-.0.;pLA\ta",
               "A{Wa-:p}.0-5-.0.;pLA a");  // crash-b98a61a0
  ExpectMinify("A{Wa--:a))\t-.0-5-.0.;p\t\ta",
               "A{Wa--:a)) -.0-5-.0.;p a");  // crash-32becff8
  ExpectMinify("a{bax{.0-0-.0-.0-5-.0.t0505-0-0c-0-.0-.0.05-0-0cu}",
               "a{bax{.0-0-.0-.0-5-.0.t0505-0-0c-0-.0-.0.05-0-0cu}");
  // crash-decdafad (two dot-adjacent sites)
}

TEST(CssMinifyTest, DecimalValidStripsUnaffected) {
  // The hard constraint: every legitimate strip still fires (the
  // byte-identical gate on the valid corpus backs this).
  ExpectMinify("a{width:0.5px}", "a{width:.5px}");
  ExpectMinify("a{width:-0.5px}", "a{width:-.5px}");
  ExpectMinify("a{b:0.0%}", "a{b:.0%}");
  ExpectMinify("a{margin:0 -0.5px}", "a{margin:0 -.5px}");
}

// ========== #1175: decimal rule decodes hex-escape structure ==========

TEST(CssMinifyTest, DecimalAfterHexEscapeTerminatorNotStripped) {
  // A `\<hex>{1,6}<ws>?` escape consumes its single whitespace
  // terminator — that space is not a token separator, so the '0'
  // after it is ident content (".a\35 0.5" decodes to class "a50";
  // stripping the '0' renamed it to "a5", #1175).  1-6 hex digits,
  // space and tab terminators, class and element position.
  ExpectMinify(".a\\35 0.5{c:d}", ".a\\35 0.5{c:d}");
  ExpectMinify("a\\5c 0.5{x:y}", "a\\5c 0.5{x:y}");
  ExpectMinify(".a\\3 0.5{c:d}", ".a\\3 0.5{c:d}");
  ExpectMinify(".a\\353 0.5{c:d}", ".a\\353 0.5{c:d}");
  ExpectMinify(".a\\3535 0.5{c:d}", ".a\\3535 0.5{c:d}");
  ExpectMinify(".a\\35353 0.5{c:d}", ".a\\35353 0.5{c:d}");
  ExpectMinify(".a\\353535 0.5{c:d}", ".a\\353535 0.5{c:d}");
  ExpectMinify(".a\\000035 0.5{c:d}", ".a\\000035 0.5{c:d}");
  // A tab terminator works the same (Phase 2 collapses it to a space).
  ExpectMinify(".a\\35\t0.5{c:d}", ".a\\35 0.5{c:d}");
}

TEST(CssMinifyTest, DecimalAfterEscapeContentBeforeDashNotStripped) {
  // Escape content before a '-': the escaped char is ident content, so
  // the '-' continues the identifier and the '0' after it is ident
  // content too — the two-char lookback must see through the escape,
  // not just the raw byte ('\\', ' ', '\'', '&').
  ExpectMinify(".a\\\\-0.5{c:d}", ".a\\\\-0.5{c:d}");
  ExpectMinify(".a\\ -0.5{c:d}", ".a\\ -0.5{c:d}");
  ExpectMinify(".a\\'-0.5{c:d}", ".a\\'-0.5{c:d}");
  ExpectMinify(".a\\26 -0.5{c:d}", ".a\\26 -0.5{c:d}");
}

TEST(CssMinifyTest, DecimalHexEscapeBoundaryCases) {
  // Seven hex digits: the escape closes after 6, the 7th digit is a
  // literal ident char, and the space after it IS a real separator —
  // the strip still fires.
  ExpectMinify(".a\\3535353 0.5{c:d}", ".a\\3535353 .5{c:d}");
  // Escaped backslash + literal "35": the space is a real separator
  // (the second backslash is escape content, not an introducer).
  ExpectMinify(".a\\\\35 0.5{c:d}", ".a\\\\35 .5{c:d}");
  // No terminator: hex digits adjacent to the '0' are ident chars, so
  // the plain ident-char guard already blocks the strip (unchanged).
  ExpectMinify(".a\\350.5{c:d}", ".a\\350.5{c:d}");
}

// ========== Coverage: escape and whitespace edge cases ==========

TEST(CssMinifyTest, EscapeInSingleQuotedString) {
  // Exercises escape handling in kInSingleStr state across all phases.
  ExpectMinify("h1::after { content: 'it\\'s here'; }",
               "h1::after{content:'it\\'s here'}");
}

TEST(CssMinifyTest, EscapeInUnquotedUrl) {
  // Exercises escape handling in kInUrl state (Phase 1 and Phase 4).
  ExpectMinify("h1 { background: url(path/img\\)name.png); }",
               "h1{background:url(path/img\\)name.png)}");
}

TEST(CssMinifyTest, UrlTrailingWhitespaceUnquoted) {
  // Exercises trailing whitespace removal inside unquoted url() (Phase 1).
  ExpectMinify("h1 { background: url(  img.png  ); }",
               "h1{background:url(img.png)}");
}

TEST(CssMinifyTest, UrlLeadingWhitespaceQuoted) {
  // Exercises whitespace skip before quoted string inside url() (Phase 1).
  // Leading whitespace inside url() before the quote is removed.
  // Trailing whitespace after the quote is collapsed to a single space
  // by the normal whitespace handler (not url-internal trimming).
  ExpectMinify("h1 { background: url(  \"img.png\"  ); }",
               "h1{background:url(\"img.png\" )}");
}

// ========== Coverage: Phase 2 trailing space at end of input ==========
// css_minify.cc lines 186-187: trailing space at end of input in Phase2.
// When the last character is a space and we're at EOF, it should be skipped.

TEST(CssMinifyTest, TrailingSpaceAtEndOfInput) {
  ExpectMinify("h1{color:red} ", "h1{color:red}");
}

TEST(CssMinifyTest, TrailingMultipleSpacesAtEof) {
  ExpectMinify("h1{color:red}   ", "h1{color:red}");
}

TEST(CssMinifyTest, OnlySpaceInput) { ExpectMinify(" ", ""); }

// ========== Coverage: calc() edge cases in Phase2 ==========
// css_minify.cc lines 200, 208, 218: calc() spacing edges.

TEST(CssMinifyTest, CalcSpaceBeforePlusPreserved) {
  // Space before '+' inside calc, preceded by a value (not '(' or ','):
  // should preserve space before '+'.
  ExpectMinify("h1{width:calc(10px +5px)}", "h1{width:calc(10px +5px)}");
}

TEST(CssMinifyTest, CalcSpaceAfterMinusPreserved) {
  // Space after '-' inside calc, preceded by value (not '(' or ','):
  // should preserve space after '-'.
  ExpectMinify("h1{width:calc(10px- 5px)}", "h1{width:calc(10px- 5px)}");
}

TEST(CssMinifyTest, CalcSpaceAroundStarRemoved) {
  // Spaces around '*' inside calc: should be removed (lines 210-217).
  ExpectMinify("h1{width:calc(10px * 2)}", "h1{width:calc(10px*2)}");
}

TEST(CssMinifyTest, CalcSpaceAroundSlashRemoved) {
  // Spaces around '/' inside calc: should be removed.
  ExpectMinify("h1{width:calc(100% / 3)}", "h1{width:calc(100%/3)}");
}

// Line 200: Space before '+' right after '(' — should be removed (unary +).
// This is the fall-through at line 200 where pc=='(' so space is dropped.
TEST(CssMinifyTest, CalcSpaceBeforePlusAfterOpenParen) {
  ExpectMinify("h1{width:calc( +5px)}", "h1{width:calc(+5px)}");
}

// Line 200: Space before '-' right after ',' — should be removed.
TEST(CssMinifyTest, CalcSpaceBeforeMinusAfterComma) {
  ExpectMinify("h1{width:clamp(0px, -5px,10px)}",
               "h1{width:clamp(0px,-5px,10px)}");
}

// Line 208: Space after '+' right after '(' — e.g. calc((+ 5px).
// When pc=='+' and r[r.size()-2]=='(', space is dropped.
TEST(CssMinifyTest, CalcSpaceAfterPlusAfterOpenParen) {
  ExpectMinify("h1{width:calc((+ 5px))}", "h1{width:calc((+5px))}");
}

// Line 208: Space after '-' right after '(' — e.g. calc((- 5px).
// When pc=='-' and r[r.size()-2]=='(', the calc-specific space preservation
// is skipped, but since CanRemoveSpaceAfter('-') is false, the general
// logic preserves the space anyway.
TEST(CssMinifyTest, CalcSpaceAfterMinusAfterOpenParen) {
  ExpectMinify("h1{width:calc((- 5px))}", "h1{width:calc((- 5px))}");
}

// Line 218: Space inside calc that is NOT around +, -, *, /.
// Falls through to general space removal logic.
TEST(CssMinifyTest, CalcSpaceNotAroundOperator) {
  // Space between two values inside calc — not adjacent to any operator.
  ExpectMinify("h1{width:calc(10px 20px)}", "h1{width:calc(10px 20px)}");
}

// ========== Coverage: calc() space removal around * and / ==========

TEST(CssMinifyTest, CalcMultiplyAndDivide) {
  // calc() with both * and / operators: spaces around them should be removed.
  ExpectMinify("div { width: calc(100% * 2 / 3); }",
               "div{width:calc(100%*2/3)}");
}

TEST(CssMinifyTest, CalcSpaceOnlyAfterStar) {
  // Space only after '*' (no space before): exercises pc=='*' path (line 214).
  ExpectMinify("h1{width:calc(100%* 2)}", "h1{width:calc(100%*2)}");
}

TEST(CssMinifyTest, CalcSpaceOnlyAfterSlash) {
  // Space only after '/' (no space before): exercises pc=='/' path (line 214).
  ExpectMinify("h1{width:calc(100%/ 3)}", "h1{width:calc(100%/3)}");
}

TEST(CssMinifyTest, CalcSpaceOnlyBeforeStar) {
  // Space only before '*' (no space after): exercises nc=='*' path (line 210).
  ExpectMinify("h1{width:calc(100% *2)}", "h1{width:calc(100%*2)}");
}

TEST(CssMinifyTest, CalcSpaceOnlyBeforeSlash) {
  // Space only before '/' (no space after): exercises nc=='/' path (line 210).
  ExpectMinify("h1{width:calc(100% /3)}", "h1{width:calc(100%/3)}");
}

TEST(CssMinifyTest, CalcMixedOperators) {
  // Mix of all four calc() operators in one expression.
  ExpectMinify("div { width: calc(100% - 20px + 10px * 2 / 3); }",
               "div{width:calc(100% - 20px + 10px*2/3)}");
}

TEST(CssMinifyTest, CalcNestedCalcInsideCalc) {
  // Nested calc with multiply and division. The outer calc context persists
  // through nested math functions, so '/' spaces are correctly stripped.
  ExpectMinify("div { width: calc(calc(100% * 2) / 3); }",
               "div{width:calc(calc(100%*2)/3)}");
}

// ========== Coverage: escape handling in string states ==========

TEST(CssMinifyTest, EscapeNonQuoteInSingleQuotedString) {
  // Backslash followed by a non-quote char in a single-quoted string.
  // This exercises kInSingleStr escape handling across all phases.
  ExpectMinify("h1::before { content: 'line\\nbreak'; }",
               "h1::before{content:'line\\nbreak'}");
}

TEST(CssMinifyTest, EscapeNonQuoteInDoubleQuotedString) {
  // Backslash followed by a non-quote char in a double-quoted string.
  // Exercises kInDoubleStr escape handling across all phases.
  ExpectMinify(R"(h1::after { content: "path\\to\\file"; })",
               R"(h1::after{content:"path\\to\\file"})");
}

TEST(CssMinifyTest, EscapeInSingleQuotedStringSemicolon) {
  // Single-quoted string with escape, followed by semicolon before '}'.
  // Exercises Phase 3 kInSingleStr escape when trailing-semicolon removal
  // is active around the string.
  ExpectMinify("h1 { content: 'foo\\'bar'; }", "h1{content:'foo\\'bar'}");
}

TEST(CssMinifyTest, EscapeInDoubleQuotedStringSemicolon) {
  // Double-quoted string with escape in Phase 3 (trailing semicolon context).
  ExpectMinify(R"(h1 { content: "foo\"bar"; })", R"(h1{content:"foo\"bar"})");
}

TEST(CssMinifyTest, EscapeInSingleQuotedStringDecimal) {
  // Single-quoted string containing '0.' to verify Phase 4 does not
  // optimize decimals inside strings.
  ExpectMinify("h1 { content: 'version 0.5'; }", "h1{content:'version 0.5'}");
}

TEST(CssMinifyTest, EscapeInDoubleQuotedStringDecimal) {
  // Double-quoted string with escape followed by decimal.
  ExpectMinify(R"(h1 { content: "v\\0.5"; })", R"(h1{content:"v\\0.5"})");
}

// ========== Coverage: escape handling in unquoted url() ==========

TEST(CssMinifyTest, EscapeBackslashInUnquotedUrl) {
  // Backslash escape inside unquoted url(), exercises Phase 1 url escape
  // path (line 129-133) and Phase 4 url escape path (line 411-414).
  ExpectMinify(R"(div { background: url(path\\to\\file.png); })",
               R"(div{background:url(path\\to\\file.png)})");
}

TEST(CssMinifyTest, EscapeInUnquotedUrlWithDecimal) {
  // Unquoted url() with backslash escape AND a '0.' pattern.
  // Phase 4 should NOT strip the leading zero inside url().
  ExpectMinify("h1{background:url(v0.5/img\\)x.png)}",
               "h1{background:url(v0.5/img\\)x.png)}");
}

TEST(CssMinifyTest, EscapeInUnquotedUrlCloseParenEscape) {
  // url() with escaped close paren: the \) should not end the url.
  ExpectMinify("div { background: url(a\\)b.png); }",
               "div{background:url(a\\)b.png)}");
}

// ========== Coverage: Phase 4 url() with quoted string bypass ==========

TEST(CssMinifyTest, Phase4UrlWithQuotedContentDecimal) {
  // url() with quoted string in Phase 4: decimal inside the quoted URL
  // should NOT be optimized (Phase 4 enters kInDoubleStr, not kInUrl).
  ExpectMinify("h1 { background: url(\"img0.5.png\"); }",
               "h1{background:url(\"img0.5.png\")}");
}

TEST(CssMinifyTest, Phase4UrlWithSingleQuotedContentDecimal) {
  // url() with single-quoted string: decimal not optimized.
  ExpectMinify("h1 { background: url('v0.5/img.png'); }",
               "h1{background:url('v0.5/img.png')}");
}

TEST(CssMinifyTest, Phase4UnquotedUrlSkipsDecimalOptimization) {
  // Unquoted url() in Phase 4: decimal 0.5 in path must not be stripped.
  ExpectMinify("h1 { background: url(v0.5.png); }",
               "h1{background:url(v0.5.png)}");
}

// ========== Coverage: Phase 1 comment interaction with psp ==========

TEST(CssMinifyTest, CommentBetweenPropertyAndValue) {
  // Comment between tokens: the comment should be removed and whitespace
  // collapsed. This exercises the psp=true path after comment end (line 96).
  ExpectMinify("h1 { color: /* mid */ red; }", "h1{color:red}");
}

TEST(CssMinifyTest, CommentAtEndOfInput) {
  // Comment as last token: should be stripped completely.
  ExpectMinify("h1 { color: red; } /* trailing */", "h1{color:red}");
}

// ========== Coverage: Phase 1 string immediately after whitespace ==========

TEST(CssMinifyTest, SingleQuotedStringAfterWhitespace) {
  // Whitespace before a single-quoted string: the psp flag is flushed
  // as a space before entering the string (lines 50-53).
  ExpectMinify("h1 { font-family: 'Arial'; }", "h1{font-family:'Arial'}");
}

TEST(CssMinifyTest, DoubleQuotedStringAfterWhitespace) {
  // Whitespace before a double-quoted string: the psp flag flushed (lines 59-63).
  ExpectMinify("h1 { font-family: \"Helvetica\"; }",
               "h1{font-family:\"Helvetica\"}");
}

// ========== Coverage: Phase 2 string states with calc context ==========

TEST(CssMinifyTest, Phase2SingleQuotedStringInCalcExpression) {
  // A single-quoted string inside a calc context (unusual but valid CSS):
  // Phase 2 processes strings without calc-specific logic.
  ExpectMinify("h1 { content: 'calc(100% + 20px)'; }",
               "h1{content:'calc(100% + 20px)'}");
}

TEST(CssMinifyTest, Phase2DoubleQuotedStringEscapeInProperty) {
  // A double-quoted string with multiple escapes: backslash-quote and
  // backslash-backslash, exercising the Phase 2 kInDoubleStr escape path.
  ExpectMinify(R"(h1 { content: "a\"b\\c"; })", R"(h1{content:"a\"b\\c"})");
}

// ========== Coverage: Phase 1 trailing space in specific states ==========

TEST(CssMinifyTest, Phase1TrailingSpaceAfterSingleQuotedString) {
  // Trailing space after a string literal at EOF exercises Phase 1's psp flag
  // being set but never flushed (line 80-83 set psp=true, then loop ends).
  ExpectMinify("h1{content:'hello' }", "h1{content:'hello'}");
}

TEST(CssMinifyTest, Phase1TrailingSpaceAfterUrlAtEof) {
  // Trailing space after url() at end of input: exercises Phase 1 trailing
  // whitespace handling after returning from kInUrl state to kNormal.
  ExpectMinify("h1{background:url(img.png)} ", "h1{background:url(img.png)}");
}

// ========== Coverage: Phase 1 trailing space at EOF (line 85-87) ==========
// When the input ends with whitespace in kNormal state, psp is set to true
// but never flushed because no non-whitespace character follows.  The
// pending space is simply discarded.

TEST(CssMinifyTest, Phase1TrailingSpaceOnly) {
  // Input that is just a single space — Phase 1 sets psp=true, loop ends.
  ExpectMinify(" ", "");
}

TEST(CssMinifyTest, Phase1TrailingTabAtEof) {
  // Tab at end of input exercises the IsCssWhitespace branch for '\t'.
  ExpectMinify("h1{color:red}\t", "h1{color:red}");
}

// ========== CSS math functions: case-insensitive + min/max/clamp ==========

TEST(CssMinifyTest, CalcUpperCasePreservesSpaces) {
  ExpectMinify("h1 { width: CALC(100% - 20px); }",
               "h1{width:CALC(100% - 20px)}");
}

TEST(CssMinifyTest, CalcMixedCasePreservesSpaces) {
  ExpectMinify("h1 { width: Calc(50% + 10px); }", "h1{width:Calc(50% + 10px)}");
}

TEST(CssMinifyTest, MinPreservesSpaces) {
  ExpectMinify("h1 { width: min(100% - 20px, 500px); }",
               "h1{width:min(100% - 20px,500px)}");
}

TEST(CssMinifyTest, MaxPreservesSpaces) {
  ExpectMinify("h1 { width: max(50vw + 10px, 300px); }",
               "h1{width:max(50vw + 10px,300px)}");
}

TEST(CssMinifyTest, ClampPreservesSpaces) {
  ExpectMinify("h1 { width: clamp(200px, 50% + 20px, 800px); }",
               "h1{width:clamp(200px,50% + 20px,800px)}");
}

TEST(CssMinifyTest, MinUpperCasePreservesSpaces) {
  ExpectMinify("h1 { width: MIN(100% - 20px, 500px); }",
               "h1{width:MIN(100% - 20px,500px)}");
}

TEST(CssMinifyTest, ClampMixedCasePreservesSpaces) {
  ExpectMinify("h1 { font-size: Clamp(1rem, 2vw + 0.5rem, 3rem); }",
               "h1{font-size:Clamp(1rem,2vw + .5rem,3rem)}");
}

TEST(CssMinifyTest, NestedMinInsideCalc) {
  // calc(min(...) + ...) — both calc and min should preserve spaces.
  ExpectMinify("h1 { width: calc(min(100%, 500px) + 20px); }",
               "h1{width:calc(min(100%,500px) + 20px)}");
}

// ========== CSS Values 4 trig/exp math functions (calc-sum arguments) ======
// sin/cos/tan/asin/acos/atan/atan2/pow/sqrt/hypot/log/exp accept calc-sums
// directly, so spaces around binary + and - inside them are required.
// "sin(1rad+45deg)" lexes "+45deg" as a signed dimension and the declaration
// is dropped by browsers.

TEST(CssMinifyTest, SinPreservesSpaces) {
  ExpectMinify("a { rotate: sin(1rad + 45deg); }",
               "a{rotate:sin(1rad + 45deg)}");
}

TEST(CssMinifyTest, CosPreservesSpaces) {
  ExpectMinify("a { rotate: cos(1rad - 45deg); }",
               "a{rotate:cos(1rad - 45deg)}");
}

TEST(CssMinifyTest, TanPreservesSpaces) {
  ExpectMinify("a { width: tan(0.5rad + 0.1rad); }",
               "a{width:tan(.5rad + .1rad)}");
}

TEST(CssMinifyTest, AsinAcosAtanPreserveSpaces) {
  ExpectMinify("a { rotate: asin(0.5 + 0.25); }", "a{rotate:asin(.5 + .25)}");
  ExpectMinify("a { rotate: acos(0.5 - 0.25); }", "a{rotate:acos(.5 - .25)}");
  ExpectMinify("a { rotate: atan(1 + 2); }", "a{rotate:atan(1 + 2)}");
}

TEST(CssMinifyTest, Atan2PreservesSpaces) {
  ExpectMinify("a { rotate: atan2(1 + 2, 3); }", "a{rotate:atan2(1 + 2,3)}");
}

TEST(CssMinifyTest, PowPreservesSpaces) {
  ExpectMinify("a { width: pow(2, 1 + 1); }", "a{width:pow(2,1 + 1)}");
}

TEST(CssMinifyTest, SqrtPreservesSpaces) {
  ExpectMinify("a { width: sqrt(4 + 5); }", "a{width:sqrt(4 + 5)}");
}

TEST(CssMinifyTest, HypotPreservesSpaces) {
  ExpectMinify("a { width: hypot(1px + 2px, 3px); }",
               "a{width:hypot(1px + 2px,3px)}");
}

TEST(CssMinifyTest, LogExpPreserveSpaces) {
  ExpectMinify("a { width: log(2 + 6, 2); }", "a{width:log(2 + 6,2)}");
  ExpectMinify("a { width: exp(1 + 1); }", "a{width:exp(1 + 1)}");
}

TEST(CssMinifyTest, TrigUpperCasePreservesSpaces) {
  // Function-name matching is ASCII case-insensitive.
  ExpectMinify("a { rotate: SIN(1rad + 45deg); }",
               "a{rotate:SIN(1rad + 45deg)}");
  ExpectMinify("a { width: Hypot(1px + 2px, 3px); }",
               "a{width:Hypot(1px + 2px,3px)}");
}

TEST(CssMinifyTest, SinNestedInsideCalcPreservesSpaces) {
  // Spaces around * are still removed; the + inside sin() is preserved.
  ExpectMinify("a { rotate: calc(sin(1rad + 2rad) * 2); }",
               "a{rotate:calc(sin(1rad + 2rad)*2)}");
}

TEST(CssMinifyTest, CalcNestedInsideSinPreservesSpaces) {
  ExpectMinify("a { rotate: sin(calc(1rad + 2rad)); }",
               "a{rotate:sin(calc(1rad + 2rad))}");
}

// ========== CSS Values 5 functions with calc-sum arguments ==========

TEST(CssMinifyTest, CalcSizePreservesSpaces) {
  ExpectMinify("a { width: calc-size(auto, size + 20px); }",
               "a{width:calc-size(auto,size + 20px)}");
}

TEST(CssMinifyTest, ProgressPreservesSpaces) {
  ExpectMinify("a { opacity: progress(50px + 10px from 0px to 100px); }",
               "a{opacity:progress(50px + 10px from 0px to 100px)}");
}

TEST(CssMinifyTest, RandomPreservesSpaces) {
  ExpectMinify("a { width: random(10px + 2px, 100px); }",
               "a{width:random(10px + 2px,100px)}");
}

// --- Non-regression: non-math functions still tighten around + and - ---

TEST(CssMinifyTest, CustomFunctionEndingWithSinNotTreatedAsMath) {
  // "mysin" ends with "sin" but is NOT a CSS math function; spaces around
  // + are removed as usual.
  ExpectMinify("h1 { width: mysin(100px + 2px); }",
               "h1{width:mysin(100px+2px)}");
}

TEST(CssMinifyTest, CustomFunctionEndingWithExpNotTreatedAsMath) {
  ExpectMinify("h1 { width: myexp(100px + 2px); }",
               "h1{width:myexp(100px+2px)}");
}

TEST(CssMinifyTest, TranslateNegativeArgumentUnchanged) {
  // Non-math function: existing tightening behavior is unchanged.
  ExpectMinify("h1 { transform: translate(10px, -5px); }",
               "h1{transform:translate(10px,-5px)}");
}

TEST(CssMinifyTest, SelectorPlusCombinatorStillTightened) {
  // Function-name matching only triggers at '('; a bare "sin" ident in a
  // selector never sets math context, so selector '+' still tightens.
  ExpectMinify("p + sin { color: red; }", "p+sin{color:red}");
}

// --- calc() false positive: custom functions ending with math names (#163) ---

TEST(CssMinifyTest, CustomFunctionEndingWithCalcNotTreatedAsMath) {
  // "localcalc" ends with "calc" but is NOT a CSS math function.
  // In buggy code, calc mode removes spaces around *. After fix, normal
  // mode preserves them (since * is not in CanRemoveSpaceBefore/After).
  ExpectMinify("h1 { width: localcalc(100% * 2); }",
               "h1{width:localcalc(100% * 2)}");
}

TEST(CssMinifyTest, CustomFunctionEndingWithMinNotTreatedAsMath) {
  // "mymin" ends with "min" but is NOT a CSS math function.
  // In buggy code, spaces around + are preserved (calc mode). After fix,
  // normal mode removes them (+ is in CanRemoveSpaceBefore/After).
  ExpectMinify("h1 { width: mymin(100px + 2px); }",
               "h1{width:mymin(100px+2px)}");
}

TEST(CssMinifyTest, RealCalcStillPreservesSpaces) {
  // Verify actual calc() still works after the fix.
  ExpectMinify("h1 { width: calc(100% - 20px); }",
               "h1{width:calc(100% - 20px)}");
}

TEST(CssMinifyTest, VendorPrefixedCalcPreservesSpaces) {
  // -webkit-calc() should still be treated as math (- is not alphanumeric).
  ExpectMinify("h1 { width: -webkit-calc(100% - 20px); }",
               "h1{width:-webkit-calc(100% - 20px)}");
}

TEST(CssMinifyTest, Phase1TrailingNewlineAtEof) {
  ExpectMinify("h1{color:red}\n", "h1{color:red}");
}

TEST(CssMinifyTest, Phase1MultipleWhitespaceTypesAtEof) {
  // Mix of whitespace types at EOF.
  ExpectMinify("h1{color:red} \t\n\r\f", "h1{color:red}");
}

// ========== Coverage: Phase 2 default case (lines 259-262) ==========
// Phase 2's switch handles kNormal, kInSingleStr, kInDoubleStr.
// The default case (lines 259-262) handles kInUrl and kInComment, but
// Phase 2 never transitions to those states — Phase 1 strips comments
// and Phase 2 has no URL detection.  These are defensive fallback paths.
//
// The closest reachable code: verify that Phase 2 correctly processes
// single-quoted strings with escape sequences (kInSingleStr state).

TEST(CssMinifyTest, Phase2SingleQuotedStringWithBackslashEscape) {
  // Exercises kInSingleStr state in Phase 2 with backslash + non-quote.
  ExpectMinify("h1 { content: 'a\\nb'; }", "h1{content:'a\\nb'}");
}

TEST(CssMinifyTest, Phase2SingleQuotedStringWithHexEscape) {
  // Single-quoted string with CSS hex escape (backslash + hex digits).
  ExpectMinify("h1 { content: '\\0041'; }", "h1{content:'\\0041'}");
}

TEST(CssMinifyTest, Phase2SingleQuotedStringWithBackslashAtEnd) {
  // Backslash as the last character of input inside a single-quoted string.
  // This hits the `i + 1 < input.size()` false branch in kInSingleStr.
  std::string input = "h1{content:'\\";
  std::string output;
  ASSERT_TRUE(MinifyCss(input, &output));
  // The backslash at EOF is preserved as-is.
  EXPECT_EQ(output, "h1{content:'\\");
  // Idempotence oracle.
  std::string remified;
  ASSERT_TRUE(MinifyCss(output, &remified));
  EXPECT_EQ(output, remified)
      << "MinifyCss is not idempotent; input: " << input;
}

// ========== Coverage: Phase 3 default case (lines 326-329) ==========
// Phase 3's switch handles kNormal, kInSingleStr, kInDoubleStr.
// The default case handles kInUrl and kInComment, neither of which
// Phase 3 ever enters.
//
// Test the reachable paths in Phase 3 string states.

TEST(CssMinifyTest, Phase3SingleQuotedStringSemicolonInside) {
  // A semicolon inside a single-quoted string must not be removed even
  // when it appears before '}'. Exercises kInSingleStr in Phase 3.
  ExpectMinify("h1 { content: 'a;b'; }", "h1{content:'a;b'}");
}

TEST(CssMinifyTest, Phase3DoubleQuotedStringWithEscapeBeforeBrace) {
  // Double-quoted string with escape, followed by '}'. Exercises
  // kInDoubleStr escape path in Phase 3.
  ExpectMinify(R"(h1 { content: "x\"}"; })", R"(h1{content:"x\"}"})");
}

TEST(CssMinifyTest, Phase3ConsecutiveSemicolonsAndWhitespaceBeforeBrace) {
  // Multiple semicolons and whitespace before '}' should all be removed.
  ExpectMinify("h1 { color: red; ; ; }", "h1{color:red}");
}

// ========== Coverage: Phase 4 URL state (lines 404-417) ==========
// Phase 4 enters kInUrl for unquoted url() content to skip decimal
// optimization inside URLs.

TEST(CssMinifyTest, Phase4UnquotedUrlWithEscapeAndDecimal) {
  // Unquoted url() with both a backslash escape and a 0. pattern.
  // Phase 4 should NOT strip the leading zero inside url().
  ExpectMinify("h1{background:url(v0.5/a\\)b.png)}",
               "h1{background:url(v0.5/a\\)b.png)}");
}

TEST(CssMinifyTest, Phase4UnquotedUrlWithMultipleDecimals) {
  // Multiple 0.x patterns inside unquoted url — none should be stripped.
  ExpectMinify("h1{background:url(v0.1/0.2/0.3.png)}",
               "h1{background:url(v0.1/0.2/0.3.png)}");
}

TEST(CssMinifyTest, Phase4UnquotedUrlEscapeAtEnd) {
  // Backslash as the last character of url content (before ')').
  // The escape check `i + 1 < input.size()` evaluates to true because
  // ')' follows, so it consumes the backslash + ')' as an escape pair.
  ExpectMinify("h1{background:url(a\\)b)}", "h1{background:url(a\\)b)}");
}

TEST(CssMinifyTest, Phase4QuotedUrlBypassesUrlState) {
  // Quoted url() in Phase 4: the quote detection at line 364-365
  // causes Phase 4 to NOT enter kInUrl, instead relying on string states.
  // Decimal inside the quoted URL is not optimized (handled by string state).
  ExpectMinify("h1{background:url('v0.5/img.png')}",
               "h1{background:url('v0.5/img.png')}");
}

// ========== Coverage: Phase 4 default case (lines 418-421) ==========
// Phase 4's switch handles kNormal, kInSingleStr, kInDoubleStr, kInUrl.
// The default case handles kInComment, which Phase 4 never enters
// (comments are stripped in Phase 1).
//
// Test the reachable transitions in Phase 4 string states with decimals.

TEST(CssMinifyTest, Phase4SingleQuotedStringPreservesDecimal) {
  // Decimal inside single-quoted string must not be optimized.
  ExpectMinify("h1{content:'price is 0.99'}", "h1{content:'price is 0.99'}");
}

TEST(CssMinifyTest, Phase4DoubleQuotedStringPreservesDecimal) {
  // Decimal inside double-quoted string must not be optimized.
  ExpectMinify("h1{content:\"v0.5\"}", "h1{content:\"v0.5\"}");
}

TEST(CssMinifyTest, Phase4SingleQuotedStringEscapePreservesDecimal) {
  // Escape + decimal inside single-quoted string.
  ExpectMinify("h1{content:'\\0.5'}", "h1{content:'\\0.5'}");
}

TEST(CssMinifyTest, Phase4DoubleQuotedStringEscapePreservesDecimal) {
  // Escape + decimal inside double-quoted string.
  ExpectMinify(R"(h1{content:"\0.5"})", R"(h1{content:"\0.5"})");
}

// ========== Bug fix: preserve descendant combinator before pseudo-classes ==========

TEST(CssMinifyTest, SelectorSpaceBeforeWhere) {
  // Space before :where() is a descendant combinator — must be preserved.
  ExpectMinify(".prose :where(h2){color:red}", ".prose :where(h2){color:red}");
}

TEST(CssMinifyTest, SelectorSpaceBeforeIs) {
  ExpectMinify(".div :is(.a,.b){color:red}", ".div :is(.a,.b){color:red}");
}

TEST(CssMinifyTest, SelectorSpaceBeforeNot) {
  ExpectMinify(".x :not(.y){color:red}", ".x :not(.y){color:red}");
}

TEST(CssMinifyTest, SelectorSpaceBeforeHas) {
  ExpectMinify(".a :has(.b){color:red}", ".a :has(.b){color:red}");
}

TEST(CssMinifyTest, SelectorSpaceBeforeHover) {
  // Space before :hover is a descendant combinator.
  ExpectMinify(".nav :hover{color:blue}", ".nav :hover{color:blue}");
}

TEST(CssMinifyTest, SelectorSpaceBeforePseudoElement) {
  // Space before ::before is a descendant combinator.
  ExpectMinify(".card ::before{content:''}", ".card ::before{content:''}");
}

TEST(CssMinifyTest, SelectorSpaceBeforeFirstChild) {
  ExpectMinify("ul :first-child{color:red}", "ul :first-child{color:red}");
}

TEST(CssMinifyTest, CompoundSelectorNoSpace) {
  // NO space before :where — compound selector, NOT descendant.
  // This should still work (no space to remove).
  ExpectMinify(".prose:where(h2){color:red}", ".prose:where(h2){color:red}");
}

TEST(CssMinifyTest, TailwindTypographyPattern) {
  // Real-world Tailwind CSS v4 pattern that triggered this bug.
  ExpectMinify(
      ".prose :where(h2):not(:where([class~=not-prose],[class~=not-prose] "
      "*)){color:red}",
      ".prose :where(h2):not(:where([class~=not-prose],[class~=not-prose] "
      "*)){color:red}");
}

TEST(CssMinifyTest, DescendantCombinatorInsideAtLayer) {
  // Selectors inside @layer/@supports blocks must preserve descendant
  // combinator spaces — the previous brace-depth heuristic broke this.
  ExpectMinify("@layer base{.prose :where(h2){font-weight:700}}",
               "@layer base{.prose :where(h2){font-weight:700}}");
}

TEST(CssMinifyTest, DescendantCombinatorInsideNestedAtRules) {
  // Double-nested at-rules: @layer > @supports > selector
  ExpectMinify(
      "@layer base{@supports (display:grid){.prose :where(h2){color:red}}}",
      "@layer base{@supports (display:grid){.prose :where(h2){color:red}}}");
}

TEST(CssMinifyTest, PropertyColonSpacePreserved) {
  // Space before ':' is always preserved — safe because build tools never
  // emit 'property : value' with a leading space.
  ExpectMinify("h1 { color : red }", "h1{color :red}");
}

TEST(CssMinifyTest, MediaFeatureColonSpacePreserved) {
  // Space before ':' in media features is also preserved (same reasoning).
  ExpectMinify("@media (max-width : 600px){h1{color:red}}",
               "@media (max-width :600px){h1{color:red}}");
}

// =============================================================================
// Phase 5: Shorthand collapsing tests
// =============================================================================

// --- padding ---

TEST(CssMinifyTest, Phase5PaddingAllEqual) {
  ExpectMinify(
      ".a { padding-top: 10px; padding-right: 10px; "
      "padding-bottom: 10px; padding-left: 10px }",
      ".a{padding:10px}");
}

TEST(CssMinifyTest, Phase5PaddingTwoValue) {
  ExpectMinify(
      ".a { padding-top: 5px; padding-right: 10px; "
      "padding-bottom: 5px; padding-left: 10px }",
      ".a{padding:5px 10px}");
}

TEST(CssMinifyTest, Phase5PaddingThreeValue) {
  ExpectMinify(
      ".a { padding-top: 5px; padding-right: 10px; "
      "padding-bottom: 15px; padding-left: 10px }",
      ".a{padding:5px 10px 15px}");
}

TEST(CssMinifyTest, Phase5PaddingFourValue) {
  ExpectMinify(
      ".a { padding-top: 1px; padding-right: 2px; "
      "padding-bottom: 3px; padding-left: 4px }",
      ".a{padding:1px 2px 3px 4px}");
}

// --- margin ---

TEST(CssMinifyTest, Phase5MarginAllEqual) {
  ExpectMinify(
      ".a { margin-top: 0; margin-right: 0; "
      "margin-bottom: 0; margin-left: 0 }",
      ".a{margin:0}");
}

TEST(CssMinifyTest, Phase5MarginTwoValue) {
  ExpectMinify(
      ".a { margin-top: 10px; margin-right: auto; "
      "margin-bottom: 10px; margin-left: auto }",
      ".a{margin:10px auto}");
}

// --- border-side ---

TEST(CssMinifyTest, Phase5BorderTop) {
  ExpectMinify(
      ".a { border-top-width: 1px; border-top-style: solid; "
      "border-top-color: red }",
      ".a{border-top:1px solid red}");
}

TEST(CssMinifyTest, Phase5BorderBottom) {
  ExpectMinify(
      ".a { border-bottom-width: 2px; border-bottom-style: dashed; "
      "border-bottom-color: #333 }",
      ".a{border-bottom:2px dashed #333}");
}

TEST(CssMinifyTest, Phase5BorderRight) {
  ExpectMinify(
      ".a { border-right-width: 1px; border-right-style: solid; "
      "border-right-color: blue }",
      ".a{border-right:1px solid blue}");
}

TEST(CssMinifyTest, Phase5BorderLeft) {
  ExpectMinify(
      ".a { border-left-width: 3px; border-left-style: dotted; "
      "border-left-color: green }",
      ".a{border-left:3px dotted green}");
}

// --- overflow ---

TEST(CssMinifyTest, Phase5OverflowSameValues) {
  ExpectMinify(".a { overflow-x: hidden; overflow-y: hidden }",
               ".a{overflow:hidden}");
}

TEST(CssMinifyTest, Phase5OverflowDifferentValues) {
  ExpectMinify(".a { overflow-x: auto; overflow-y: scroll }",
               ".a{overflow:auto scroll}");
}

// --- Safety: values that block collapsing ---

TEST(CssMinifyTest, Phase5VarBlocksCollapse) {
  ExpectMinify(
      ".a { padding-top: var(--x); padding-right: 10px; "
      "padding-bottom: 10px; padding-left: 10px }",
      ".a{padding-top:var(--x);padding-right:10px;"
      "padding-bottom:10px;padding-left:10px}");
}

TEST(CssMinifyTest, Phase5ImportantBlocksCollapse) {
  ExpectMinify(
      ".a { padding-top: 10px!important; padding-right: 10px; "
      "padding-bottom: 10px; padding-left: 10px }",
      ".a{padding-top:10px!important;padding-right:10px;"
      "padding-bottom:10px;padding-left:10px}");
}

TEST(CssMinifyTest, Phase5InheritBlocksCollapse) {
  ExpectMinify(
      ".a { margin-top: inherit; margin-right: 10px; "
      "margin-bottom: 10px; margin-left: 10px }",
      ".a{margin-top:inherit;margin-right:10px;"
      "margin-bottom:10px;margin-left:10px}");
}

TEST(CssMinifyTest, Phase5InitialBlocksCollapse) {
  ExpectMinify(
      ".a { padding-top: initial; padding-right: 0; "
      "padding-bottom: 0; padding-left: 0 }",
      ".a{padding-top:initial;padding-right:0;"
      "padding-bottom:0;padding-left:0}");
}

// --- Non-consecutive: other property interleaves ---

TEST(CssMinifyTest, Phase5NonConsecutiveNotCollapsed) {
  ExpectMinify(
      ".a { padding-top: 10px; color: red; "
      "padding-right: 10px; padding-bottom: 10px; padding-left: 10px }",
      ".a{padding-top:10px;color:red;"
      "padding-right:10px;padding-bottom:10px;padding-left:10px}");
}

// --- Incomplete set ---

TEST(CssMinifyTest, Phase5IncompleteSetNotCollapsed) {
  ExpectMinify(
      ".a { padding-top: 10px; padding-right: 10px; padding-bottom: 10px }",
      ".a{padding-top:10px;padding-right:10px;padding-bottom:10px}");
}

// --- Out-of-order longhands still collapse ---

TEST(CssMinifyTest, Phase5OutOfOrderCollapses) {
  ExpectMinify(
      ".a { padding-left: 4px; padding-bottom: 3px; "
      "padding-right: 2px; padding-top: 1px }",
      ".a{padding:1px 2px 3px 4px}");
}

// --- Mixed with other properties (before/after preserved) ---

TEST(CssMinifyTest, Phase5PreserveSurroundingDeclarations) {
  ExpectMinify(
      ".a { color: red; padding-top: 5px; padding-right: 5px; "
      "padding-bottom: 5px; padding-left: 5px; display: block }",
      ".a{color:red;padding:5px;display:block}");
}

// --- Nested @media blocks ---

TEST(CssMinifyTest, Phase5NestedMediaBlock) {
  ExpectMinify(
      "@media(max-width:600px){ .a { padding-top: 10px; padding-right: 10px; "
      "padding-bottom: 10px; padding-left: 10px } }",
      "@media(max-width:600px){.a{padding:10px}}");
}

// --- Multiple families in one block ---

TEST(CssMinifyTest, Phase5MultipleFamilies) {
  ExpectMinify(
      ".a { padding-top: 1px; padding-right: 2px; padding-bottom: 3px; "
      "padding-left: 4px; margin-top: 0; margin-right: auto; "
      "margin-bottom: 0; margin-left: auto }",
      ".a{padding:1px 2px 3px 4px;margin:0 auto}");
}

// --- Border side out of order ---

TEST(CssMinifyTest, Phase5BorderSideOutOfOrder) {
  ExpectMinify(
      ".a { border-top-color: red; border-top-width: 1px; "
      "border-top-style: solid }",
      ".a{border-top:1px solid red}");
}

// --- unset/revert block ---

TEST(CssMinifyTest, Phase5UnsetBlocksCollapse) {
  ExpectMinify(".a { overflow-x: unset; overflow-y: hidden }",
               ".a{overflow-x:unset;overflow-y:hidden}");
}

TEST(CssMinifyTest, Phase5RevertBlocksCollapse) {
  ExpectMinify(".a { overflow-x: revert; overflow-y: auto }",
               ".a{overflow-x:revert;overflow-y:auto}");
}

TEST(CssMinifyTest, Phase5RevertLayerBlocksCollapse) {
  ExpectMinify(".a { overflow-x: revert-layer; overflow-y: auto }",
               ".a{overflow-x:revert-layer;overflow-y:auto}");
}

// --- Unterminated block passthrough ---

TEST(CssMinifyTest, Phase5UnterminatedBlock) {
  // Missing closing brace: Phase5 should emit remainder as-is.
  ExpectMinify(".a{color:red", ".a{color:red");
}

// --- Deep nesting cap ---

// ========== Custom-property values are opaque token streams ==========
// Whitespace inside a custom-property value is significant: operand
// fragments consumed via calc(var(--x)) become invalid if spaces around
// binary +/- are stripped, and getPropertyValue() must see the stored
// token stream.  Phases 1 and 2 treat everything after "--ident:" up to
// the terminating ';' or '}' as opaque.

TEST(CssMinifyTest, CustomPropertyCalcOperandsPreserved) {
  // calc(var(--x)) substitution requires the spaces around '+'.
  ExpectMinify(":root { --x: 1px + 2px; }", ":root{--x:1px + 2px}");
}

TEST(CssMinifyTest, CustomPropertySimpleValueMinified) {
  // Leading/trailing whitespace of the value is still trimmed.
  ExpectMinify(":root { --gap: 4px; }", ":root{--gap:4px}");
}

TEST(CssMinifyTest, CustomPropertySelectorFragmentPreserved) {
  // A selector fragment stored in a custom property keeps its spaces.
  ExpectMinify(":root { --sel: .a > .b; }", ":root{--sel:.a > .b}");
}

TEST(CssMinifyTest, CustomPropertyInternalSpaceRunsPreserved) {
  // Interior whitespace runs are significant for getPropertyValue().
  ExpectMinify(":root { --msg: a   b; }", ":root{--msg:a   b}");
}

// ========== #1238: escaped chars in custom-property names ==========
// AtCustomPropertyColon's backward name-scan broke on stop-set chars
// that are ESCAPE CONTENT (odd backslash run): an escaped char is a
// valid custom-property name char per CSS Syntax 3, so the scan must
// treat it as name content, not a boundary.  Before the fix the value
// got ordinary-CSS processing (Phase 4's decimal rule rewrote it).

TEST(CssMinifyTest, CustomPropertyEscapedSpaceInNamePreserved) {
  // The triage repro (nightly 30849604191, issue #1238): `--\ \>` is
  // the name `--`, space, `>` — valid per CSS Syntax 3.  The value
  // must stay byte-verbatim.
  ExpectMinify("a{--\\ \\>:0.}", "a{--\\ \\>:0.}");
  ExpectMinify("a{--\\ \\>:5.0.100}", "a{--\\ \\>:5.0.100}");
  ExpectMinify("a{--\\ \\>: 0.5;}", "a{--\\ \\>:0.5}");
  ExpectMinify("a{--\\ \\>:x}", "a{--\\ \\>:x}");
  // Top-level form of the minimized crash repro.
  ExpectMinify("--\\ \\>:0.", "--\\ \\>:0.");
}

TEST(CssMinifyTest, CustomPropertyEscapePositionsAndParity) {
  // Escaped char at name start.
  ExpectMinify("a{--\\>:0.}", "a{--\\>:0.}");
  // Multiple escapes inside the name.
  ExpectMinify("a{--a\\ b\\ c:0.}", "a{--a\\ b\\ c:0.}");
  // Backslash-run parity: an EVEN run means the following char is NOT
  // escaped — `\\` is an escaped backslash (name content) and the
  // plain space after it is still just spacing.
  ExpectMinify("a{--x\\\\ :0.}", "a{--x\\\\ :0.}");
  // Escape immediately before the colon: `\:` is name content, the
  // second ':' is the declaration colon.
  ExpectMinify("a{--xy\\::0.}", "a{--xy\\::0.}");
  // Name ending in an escaped space.
  ExpectMinify("a{--x\\ :0.}", "a{--x\\ :0.}");
  // Ordinary custom properties are unchanged.
  ExpectMinify("a{--x:0.}", "a{--x:0.}");
}

TEST(CssMinifyTest, PhantomCustomNameStillRejected) {
  // The #1156 leak stays closed: "--" mid-value never triggers the
  // custom-property path, escaped or not.
  ExpectMinify("a{b:calc(1*2; --* y: 2)}", "a{b:calc(1*2; --*y:2)}");
}

// ========== #1238 pass-ordering: glue-aware custom detection ==========
// AtCustomPropertyColon ran in Phase 1 BEFORE Phase 2's escape-glue
// whitespace removal, so pass 1 saw the name unglued ("--\> p" -> name
// "p" -> not custom -> value processed as ordinary CSS, keeping one
// post-colon space), Phase 2 glued the name, and pass 2 saw a custom
// property from the start and trimmed the value-leading space — a
// kept-then-dropped space broke minify∘minify = minify (nightly
// 30875672853, #1238 triage).  The scan now skips a whitespace run
// exactly when Phase 2 would remove it, so the name is evaluated on
// its post-glue form on every pass.  ExpectMinify's built-in
// idempotence check pins the fixpoint.

TEST(CssMinifyTest, CustomDetectionGlueAwareFixpoints) {
  // The three minimized nightly skeletons: escape-terminated name
  // prefix + ws + name char, then ws after the colon (tab, CR, and
  // tab-before-';' variants).  Pass 1 must now recognize the custom
  // property immediately (value-leading ws trimmed, name glued by
  // Phase 2), making the output a fixpoint.
  ExpectMinify("--\\>\tp: x", "--\\>p:x");
  ExpectMinify("--\\>\tp:\rr", "--\\>p:r");
  ExpectMinify("--\\>\tp:\t;", "--\\>p:;");
  // Controls: no escape, no glue — the name stays two tokens and the
  // declaration stays non-custom on every pass.
  ExpectMinify("--x\tp: x", "--x p:x");
  ExpectMinify("--x: x", "--x:x");
}

TEST(CssMinifyTest, GlueAwareScanKeepsBoundaryAndCombinatorRules) {
  // A boundary-preceded whitespace run ('{' before it) is a separator,
  // not name content: the custom property is still recognized.
  ExpectMinify("a{ --x: 0.5; }", "a{--x:0.5}");
  // Whitespace before the colon is preserved (never a glue target).
  ExpectMinify("a{--x : 0.5}", "a{--x :0.5}");
  // Combinators still break the name (#1156 phantom-glue protection):
  // these are selector-ish/non-custom on both passes.
  ExpectMinify("a{--x > y: 1px}", "a{--x>y:1px}");
  ExpectMinify("a{--x+y: 1px}", "a{--x+y:1px}");
  // #1239's escaped-name cases stay preserved.
  ExpectMinify("a{--\\ \\>:0.}", "a{--\\ \\>:0.}");
  ExpectMinify("a{--\\ \\>:5.0.100}", "a{--\\ \\>:5.0.100}");
}

TEST(CssMinifyTest, NormalDeclarationsAroundCustomPropertyStillMinified) {
  ExpectMinify(".a { --x: 1px + 2px; margin: 0 ; color: red; }",
               ".a{--x:1px + 2px;margin:0;color:red}");
}

TEST(CssMinifyTest, SelectorAroundCustomPropertyStillMinified) {
  // The selector combinators outside the value still collapse; the
  // value ends at the '}' when no ';' is present.
  ExpectMinify("div > p { --x: 1px + 2px }", "div>p{--x:1px + 2px}");
}

TEST(CssMinifyTest, RuleFollowingCustomPropertyStillMinified) {
  // Opaque mode must end at the block's closing brace.
  ExpectMinify(".a{--x: 1px + 2px}.b > .c { color: red; }",
               ".a{--x:1px + 2px}.b>.c{color:red}");
}

TEST(CssMinifyTest, CustomPropertyValueWithBalancedBraces) {
  // A balanced {}-block inside the value does not end the declaration.
  ExpectMinify(".a{--x: {a:b};color: red}", ".a{--x:{a:b};color:red}");
}

TEST(CssMinifyTest, CustomPropertyUnterminatedValueDoesNotCrash) {
  ExpectMinify(".a{--x: 1px + 2px", ".a{--x:1px + 2px");
}

TEST(CssMinifyTest, CustomPropertyCommentBecomesTokenSeparator) {
  // A comment inside the value is stripped but must keep tokens apart.
  ExpectMinify(":root { --x: a/*c*/b; }", ":root{--x:a b}");
}

TEST(CssMinifyTest, CustomPropertyInsideParensNotOpaque) {
  // "--x: y" inside parens (@supports query) is not a declaration;
  // normal minification applies there.
  ExpectMinify("@supports (--x: y) { h1 { color: red; } }",
               "@supports (--x:y){h1{color:red}}");
}

TEST(CssMinifyTest, CustomPropertyLookalikeInStringNotOpaque) {
  // "--x: y" inside a string literal is content, not a declaration;
  // the declarations around it still minify normally.
  ExpectMinify(".a { content: \"--x: y\"; color: red }",
               ".a{content:\"--x: y\";color:red}");
}

TEST(CssMinifyTest, DoubleHyphenClassSelectorNotOpaque) {
  // A double hyphen inside a selector ident is not a custom property.
  ExpectMinify(".foo--bar:hover { color: red }", ".foo--bar:hover{color:red}");
}

TEST(CssMinifyTest, CalcDoubleHyphenInNormalValueUnaffected) {
  // "- -" inside a normal calc() value must not trip opaque mode and
  // must keep its operator spaces.
  ExpectMinify(".a { width: calc(a - -b); }", ".a{width:calc(a - -b)}");
}

// ========== Unquoted url() inside custom-property values ==========
// Per CSS syntax, "/*" inside an unquoted url-token is URL content,
// not a comment start.  The non-custom Phase 1 path already shields it
// via the url() state; the opaque custom-value path must do the same.

TEST(CssMinifyTest, CustomPropertyUnquotedUrlCommentStartNotAComment) {
  ExpectMinify(":root { --u: url(http://e.com/*path); color: red }",
               ":root{--u:url(http://e.com/*path);color:red}");
}

TEST(CssMinifyTest, CustomPropertyUnquotedUrlCommentPairNotAComment) {
  ExpectMinify(":root { --u: url(http://e.com/*path*/x); color: red }",
               ":root{--u:url(http://e.com/*path*/x);color:red}");
}

TEST(CssMinifyTest, CustomPropertyUppercaseUrlAlsoShielded) {
  // CSS function names are case-insensitive.
  ExpectMinify(":root { --u: URL(http://e.com/*path); color: red }",
               ":root{--u:URL(http://e.com/*path);color:red}");
}

TEST(CssMinifyTest, CustomPropertyUrlLeadingWhitespaceAlsoShielded) {
  // Whitespace after "url(" is still an unquoted url-token; the value
  // is opaque so the interior space is preserved verbatim.
  ExpectMinify(":root { --u: url( http://e.com/*path); color: red }",
               ":root{--u:url( http://e.com/*path);color:red}");
}

TEST(CssMinifyTest, CustomPropertyQuotedUrlStillPreserved) {
  // Quoted url() content is covered by the string states, not the
  // verbatim url mode.
  ExpectMinify(":root { --u: url(\"http://e.com//x\"); color: red }",
               ":root{--u:url(\"http://e.com//x\");color:red}");
}

TEST(CssMinifyTest, CustomPropertyQuotedUrlInsideFunctionKeepsDepth) {
  // The quoted-url paren must still participate in depth tracking so
  // the ';' inside foo() does not terminate the value early.
  ExpectMinify(".a{--x: foo(url(\"a\");b); color: red}",
               ".a{--x:foo(url(\"a\");b);color:red}");
}

TEST(CssMinifyTest, NormalUnquotedUrlCommentStartStillShielded) {
  // The non-custom path's existing url() shielding is unaffected.
  ExpectMinify("h1 { background: url(http://e.com/*p); color: red }",
               "h1{background:url(http://e.com/*p);color:red}");
}

// ========== Escaped whitespace at end of custom-property values ==========
// "a\ " ends in an escaped space — a token character, not trimmable
// whitespace.  Trimming it glues the backslash to the terminator
// ("\;" absorbs the next declaration; "\}" deletes the closing brace).

TEST(CssMinifyTest, CustomPropertyTrailingEscapedSpaceBeforeSemicolon) {
  ExpectMinify(".a{--x: a\\ ; color: red}", ".a{--x:a\\ ;color:red}");
}

TEST(CssMinifyTest, CustomPropertyTrailingEscapedSpaceBeforeBrace) {
  ExpectMinify(".a{--x: a\\ }", ".a{--x:a\\ }");
}

TEST(CssMinifyTest, CustomPropertyTrailingSpaceAfterEscapedBackslashTrimmed) {
  // "\\" is an escaped backslash: the space after it is genuine
  // whitespace and is still trimmed.
  ExpectMinify(".a{--x: a\\\\ ; color: red}", ".a{--x:a\\\\;color:red}");
}

// ========== Escaped space in identifiers ==========
// ".a\ " is a class named "a " — the escaped space is part of the
// identifier, not removable whitespace.  Stripping it turned the
// selector into ".a\{", killing the whole rule.

TEST(CssMinifyTest, EscapedSpaceInSelectorPreserved) {
  ExpectMinify(".a\\ { color: red; }", ".a\\ {color:red}");
}

TEST(CssMinifyTest, EscapedSpaceBeforeCombinatorPreserved) {
  // The escaped space stays; the real space before '>' still collapses.
  ExpectMinify(".a\\  > b { color: red; }", ".a\\ >b{color:red}");
}

// ========== Backslash escapes in normal context (#1133) ==========
// Phase 1's kNormal consumed no escape pairs, so it tokenized
// differently from Phase 2 (which got an escape guard in 01d14490a):
// an escaped quote opened a phantom string in Phase 1 only, and an
// escaped '/' opened a phantom comment.  The former made the
// end-of-input space drop collapse one trailing space per minify pass;
// the latter corrupted valid CSS containing "\/" escapes.

TEST(CssMinifyTest, Issue1133CounterexampleIdempotent) {
  // The original 4-byte fuzz counterexample: backslash, quote, two
  // spaces used to minify to "\' " and then "\'" — one trailing space
  // collapsed per pass.  Now collapses in a single pass.
  ExpectMinify("\\'  ", "\\'");
}

TEST(CssMinifyTest, EscapedDoubleQuoteTrailingSpaces) {
  ExpectMinify("\\\"  ", "\\\"");
}

TEST(CssMinifyTest, EscapedQuoteTrailingTabsRemoved) {
  ExpectMinify("\\'\t\t", "\\'");
}

TEST(CssMinifyTest, EvenBackslashRunBeforeQuoteOpensString) {
  // "\\\\" is an escaped backslash, so the quote genuinely opens an
  // (unterminated) string whose trailing spaces are significant content
  // and survive verbatim.  Both phases agree; stable.
  ExpectMinify("\\\\'  ", "\\\\'  ");
}

TEST(CssMinifyTest, OddBackslashRunBeforeQuoteIsEscaped) {
  // "\\\\\\'" is an escaped backslash followed by an escaped quote: no
  // string opens, so the trailing spaces are removable.
  ExpectMinify("\\\\\\'  ", "\\\\\\'");
}

TEST(CssMinifyTest, EscapedQuoteInClassSelector) {
  // Valid CSS: a class named "a'b".  The escape must not open a string
  // spanning the rest of the stylesheet.
  ExpectMinify(".a\\'b { c: d }", ".a\\'b{c:d}");
}

TEST(CssMinifyTest, EscapedSlashDoesNotStartComment) {
  // "\/" is an escaped slash, not a comment start: the declaration
  // block must survive.  (Previously Phase 1 ate "/* { color: red }"
  // as an unterminated comment.)
  ExpectMinify(".a\\/* { color: red }", ".a\\/*{color:red}");
  ExpectMinify(".a\\/*x*/b { c: d }", ".a\\/*x*/b{c:d}");
}

TEST(CssMinifyTest, EscapedSlashInCustomPropertyValuePreserved) {
  // Valid CSS and the sharpest #1133 corruption: custom-property
  // values are opaque token streams observed verbatim via var() /
  // getPropertyValue(); Phase 1 used to eat "\/*y*/" as a comment and
  // reduce the value to "\".
  ExpectMinify("a{--x:\\/*y*/;b:c}", "a{--x:\\/*y*/;b:c}");
}

TEST(CssMinifyTest, EscapedSlashInDeclarationValuePreserved) {
  ExpectMinify("a{width:\\/*y*/10px}", "a{width:\\/*y*/10px}");
}

TEST(CssMinifyTest, UnterminatedUrlTrailingSpacesTrimmed) {
  // Phase 1's kInUrl preserved trailing spaces verbatim at end of
  // input while Phase 2 (no url state) dropped one per pass — the same
  // #1133 divergence shape without any escape involved.
  ExpectMinify("a{background:url(x  ", "a{background:url(x");
  ExpectMinify("a{background:url(\\'  ", "a{background:url(\\'");
}

TEST(CssMinifyTest, UnterminatedUrlEscapedTrailingSpacePreserved) {
  // Review find on #1133: the end-of-input trim must not pop an
  // escaped space ("\ " is a legal unquoted-url character) and leave
  // the backslash dangling at end of input.  Same parity rule as the
  // custom-property terminator trim.
  ExpectMinify("url(x\\ ", "url(x\\ ");
  ExpectMinify("a{background:url(x\\ ", "a{background:url(x\\ ");
  // An escaped backslash makes the trailing space genuine whitespace:
  // trimmed.
  ExpectMinify("url(x\\\\ ", "url(x\\\\");
  // A genuine trailing space after the escaped one is trimmed; the
  // escaped space stays.
  ExpectMinify("url(x\\  ", "url(x\\ ");
}

TEST(CssMinifyTest, UrlEscapedSpaceBeforeCloseParenPreserved) {
  // Issue #1154: the ')'-handler trailing-space trims (Phase 1 and the
  // Phase 2 mirror) popped an escaped space, gluing the backslash to
  // the paren — url(x\ ) became url(x\), deleting a legal URL
  // character and rebinding the paren as an escape on the next pass.
  ExpectMinify("a{background:url(x\\ )}", "a{background:url(x\\ )}");
  ExpectMinify("a{background:url(x\\\\ )}", "a{background:url(x\\\\)}");
  ExpectMinify("a{background:url(x\\\\\\ )}", "a{background:url(x\\\\\\ )}");
  // Genuine trailing spaces before ')' still trim.
  ExpectMinify("a{background:url(x  )}", "a{background:url(x)}");
  ExpectMinify("a{background:url(x\\  )}", "a{background:url(x\\ )}");
  // The minimized libFuzzer find (#1154): the paren rebind made the
  // url close at the second ')' on the next pass, dropping the
  // trailing newline one pass late.  Now idempotent in one pass.
  ExpectMinify("url(\\ )}--:)\n", "url(\\ )}--:)\n");
}

TEST(CssMinifyTest, Phase3SemicolonTrimOpaqueInsideUrl) {
  // Issue #1158 (audit P3-c): ';' and '}' are legal unquoted-url code
  // points, but Phase 3's trailing-semicolon trim had no url state and
  // fired inside them — valid-input corruption.  Phase 3 now mirrors
  // Phase 2's url state.
  ExpectMinify("a{background:url(x;}y)}", "a{background:url(x;}y)}");
  ExpectMinify("a{background:url(x};y)}", "a{background:url(x};y)}");
  ExpectMinify("a{background:url(a;b;c)}", "a{background:url(a;b;c)}");
  ExpectMinify("a{background:URL(X;}Y)}", "a{background:URL(X;}Y)}");
  // ';}' OUTSIDE the url still trims.
  ExpectMinify("a{b:c;}", "a{b:c}");
  ExpectMinify("a{b:c;; ;}", "a{b:c}");
  // Quoted urls were already shielded by the string states.
  ExpectMinify("a{background:url('x;}y')}", "a{background:url('x;}y')}");
  // Issue #1160: same root, idempotence-shaped — converges in one pass.
  ExpectMinify("url(; }", "url(; }");
  // Custom-property url(): Phase 1's custom_url keeps the whitespace
  // after '(' verbatim, and Phase 3 (no custom awareness) must too.
  ExpectMinify(":root{--y:url(  spaced.png  );b:c}",
               ":root{--y:url(  spaced.png  );b:c}");
  // Custom-property variant of the #1158 corruption (review on #1161):
  // the ';' inside a custom-property url was trimmed on main
  // (url(x}y)); now preserved.
  ExpectMinify(":root{--y:url(x;}y);b:c}", ":root{--y:url(x;}y);b:c}");
}

TEST(CssMinifyTest, Phase5ScannersOpaqueInsideUrl) {
  // Audit P5-c: Phase 5's declaration splitter saw the ';' inside the
  // url as a declaration boundary and "found" four padding longhands
  // to collapse — rewriting the declaration into garbage.
  ExpectMinify(
      "a{padding-top:url(x;padding-right:1px);padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:url(x;padding-right:1px);padding-bottom:1px;"
      "padding-left:1px}");
  // '{' and '}' inside an unquoted url are url code points, not block
  // boundaries (brace matcher + top-level scanner).
  ExpectMinify("a{background:url(x{y})}", "a{background:url(x{y})}");
  ExpectMinify("a{background:url(x}y)}", "a{background:url(x}y)}");
  // An escaped paren must not close the url early.
  ExpectMinify("a{background:url(x\\)y;z)}", "a{background:url(x\\)y;z)}");
  // Over-suppression guard: a real four-longhand block still
  // collapses.
  ExpectMinify(
      "a{padding-top:1px;padding-right:2px;padding-bottom:3px;padding-left:"
      "4px}",
      "a{padding:1px 2px 3px 4px}");
  // These three collapsed when #1161 landed, on the reasoning that this
  // "matches the minifier's pre-existing treatment of other value-invalid
  // longhands".  That treatment is the defect #1378 names: a url() is not a
  // valid margin, overflow or border-width value, and collapsing a family
  // around it converts one ignored declaration into the loss of all of them.
  // The value gate withdraws them; the ';'-inside-url property they were
  // written for is pinned by the first case in this test, where the splitter
  // bug produced visible garbage rather than a mere missed collapse.
  // One pin per family.
  ExpectMinify(
      "a{margin-top:url(x;y);margin-right:1px;margin-bottom:1px;margin-left:"
      "1px}",
      "a{margin-top:url(x;y);margin-right:1px;margin-bottom:1px;margin-left:"
      "1px}");
  ExpectMinify("a{overflow-x:url(a;b);overflow-y:hidden}",
               "a{overflow-x:url(a;b);overflow-y:hidden}");
  ExpectMinify(
      "a{border-top-width:url(x;y);border-top-style:solid;border-top-color:"
      "red}",
      "a{border-top-width:url(x;y);border-top-style:solid;border-top-color:"
      "red}");
  // A '{' inside a url no longer marks its block nested, so the
  // longhands in the same block collapse too.
  ExpectMinify(
      "a{background:url(x{y});padding-top:1px;padding-right:1px;padding-bottom:"
      "1px;padding-left:1px}",
      "a{background:url(x{y});padding:1px}");
}

TEST(CssMinifyTest, UnterminatedUrlRunBeforeBraceStable) {
  // Minimized libFuzzer find: Phase 1's kInUrl swallows the '}' and
  // preserves the whitespace run verbatim, while a url-less Phase 2
  // peeled one space off the run per pass.  Phase 2 now mirrors Phase
  // 1's url state, so the (invalid) input round-trips unchanged.
  ExpectMinify("url(g  }", "url(g  }");
  ExpectMinify("a{background:url(a  b}", "a{background:url(a  b}");
}

TEST(CssMinifyTest, OperatorCharsInsideUrlPreserved) {
  // ';' is legal inside an unquoted url(): Phase 2 scanned url content
  // as ordinary CSS and stripped the space before it, corrupting the
  // URL.
  ExpectMinify("a{background:url(a ;b)}", "a{background:url(a ;b)}");
}

TEST(CssMinifyTest, UrlInsideCalcKeepsMathSpacing) {
  // Paren-depth accounting must stay balanced across both quoted and
  // unquoted url() so calc's +/- spaces survive.
  ExpectMinify("a{width:calc(url(\"x.png\") + 10px)}",
               "a{width:calc(url(\"x.png\") + 10px)}");
  ExpectMinify("a{width:calc(url(x.png) + 10px)}",
               "a{width:calc(url(x.png) + 10px)}");
}

TEST(CssMinifyTest, CustomPropertyUrlBracesShieldedFromDepthTracking) {
  // Minimized libFuzzer find ("--:url({); }"): Phase 2 tracked
  // brace depth through unquoted url() content inside custom-property
  // values while Phase 1 shields it (custom_url), so the phases
  // disagreed on where the value ends.  Phase 2 now mirrors Phase 1's
  // shielding.
  ExpectMinify("--:url({); }", "--:url({)}");
  ExpectMinify(":root{--y:url(a{b});color:red}",
               ":root{--y:url(a{b});color:red}");
  ExpectMinify(":root{--y:url(  spaced.png  );b:c}",
               ":root{--y:url(  spaced.png  );b:c}");
}

TEST(CssMinifyTest, CustomPropertyCommentBeforeFirstValueToken) {
  // Minimized libFuzzer find ("--:/**/\n"), reachable with valid CSS:
  // a comment immediately after the colon was replaced by a separator
  // space AFTER the value's leading-whitespace trim had already run,
  // re-introducing leading whitespace that the next pass (comment
  // gone) trimmed — one pass late.  Comments and whitespace before the
  // first value token now contribute nothing.
  ExpectMinify("a{--x:/*c*/v;b:c}", "a{--x:v;b:c}");
  ExpectMinify("a{--x:/*c*/\nv;b:c}", "a{--x:v;b:c}");
  ExpectMinify("a{--x:/*a*//*b*/v;b:c}", "a{--x:v;b:c}");
  ExpectMinify("a{--x:/*c*/\tv;b:c}", "a{--x:v;b:c}");
  ExpectMinify("--:/**/\n", "--:");
  // Round-4 libFuzzer find ("--:/**/\\'? \n"): same class via the
  // escape guard — an escape pair as the first value token must not
  // resurrect the comment's separator space either.
  ExpectMinify("--:/**/\\'? \n", "--:\\'? \n");
  ExpectMinify("a{--x:/*c*/\\ v;b:c}", "a{--x:\\ v;b:c}");
  // Mid-value comment separators and comment-only values are
  // unaffected.
  ExpectMinify("a{--x:v /*c*/ w;b:c}", "a{--x:v  w;b:c}");
  ExpectMinify("a{--x:/*c*/;b:c}", "a{--x:;b:c}");
}

TEST(CssMinifyTest, Phase3CustomValueOpacity) {
  // Audit P3-d: Phase 3's trailing-';' trim had no custom-value or
  // nesting awareness and fired inside opaque values — valid-input
  // corruption.  Phase 3 now mirrors Phase 2's in_custom (including
  // custom_url shielding) and guards the trim with paren depth.
  ExpectMinify("a{--x:{;}}", "a{--x:{;}}");
  // Also needs Phase 5's brace matcher to respect paren depth (the
  // '}' inside foo(...) closed the block early on main).
  ExpectMinify("a{--x:foo(a;});b:c}", "a{--x:foo(a;});b:c}");
  ExpectMinify("a{--x:[;];b:c}", "a{--x:[;];b:c}");
  ExpectMinify("a{--x:url(a;b);b:c}", "a{--x:url(a;b);b:c}");
  ExpectMinify("a{--x:';';b:c}", "a{--x:';';b:c}");
  // The terminating ';' still trims — the value ends, the normal rule
  // applies.
  ExpectMinify("a{--x:v;}", "a{--x:v}");
  ExpectMinify("a{--x:v;;}", "a{--x:v}");
  ExpectMinify("a{color:red;;}", "a{color:red}");
  // IE progid filters: colons and parens, no custom-property entry,
  // trim still fires after the closing paren.
  ExpectMinify("a{filter:progid:DXImageTransform.Microsoft.Alpha(Opacity=50);}",
               "a{filter:progid:DXImageTransform.Microsoft.Alpha(Opacity=50)}");
  ExpectMinify("a{filter:alpha(opacity=50);color:red;}",
               "a{filter:alpha(opacity=50);color:red}");
}

TEST(CssMinifyTest, Phase4CustomValueOpacity) {
  // Audit P4-d (owner-approved behavior change): custom-property
  // values are observed verbatim, so decimal optimization must not
  // fire inside them.  "--x:0.5" now keeps its 0.
  ExpectMinify(":root{--x:0.5}", ":root{--x:0.5}");
  ExpectMinify(":root{--x:0.5;--y:1px}", ":root{--x:0.5;--y:1px}");
  ExpectMinify(":root{--x:url(0.5.png)}", ":root{--x:url(0.5.png)}");
  // Non-custom decimals still optimize.
  ExpectMinify("a{width:0.5px}", "a{width:.5px}");
  ExpectMinify("a{margin:-0.5px}", "a{margin:-.5px}");
  // Audit P4-c: whitespace-skip before the quote test at the url entry
  // — a quoted url with spaces must not open a phantom unquoted url
  // whose ')' could close inside the string.
  ExpectMinify(":root{--y:url( \"v0.5.png\" )}",
               ":root{--y:url( \"v0.5.png\" )}");
  ExpectMinify(":root{--y:url( \"v0.5).png\" )}",
               ":root{--y:url( \"v0.5).png\" )}");
  // Review on #1165: the custom-url quote test must be && (Phases 1/2
  // use &&; the mirrors briefly had ||, so quoted urls entered the
  // shield, exited at a ')' inside the quoted text, and a phantom
  // string suppressed the decimal rule for subsequent REGULAR
  // declarations — these pins fail under the || bug.
  ExpectMinify("a{--x:url(\"a)b\");width:0.5px}",
               "a{--x:url(\"a)b\");width:.5px}");
  ExpectMinify("a{--x:url('a)b');width:0.5px}", "a{--x:url('a)b');width:.5px}");
  ExpectMinify("a{--x:URL(\"a)b\");width:0.5px}",
               "a{--x:URL(\"a)b\");width:.5px}");
  // The whitespace variant additionally delivers the P4-c intent the
  // || bug (and main) suppressed.
  ExpectMinify("a{--x:Url( \"a)b\" );width:0.5px}",
               "a{--x:Url( \"a)b\" );width:.5px}");
}

TEST(CssMinifyTest, EscapeGuardsInPhases3And4) {
  // Audit P4-a: without an escape guard, "\0" in an identifier let the
  // decimal rule treat the escaped 0 as a number start.
  ExpectMinify(".a\\0.5{c:d}", ".a\\0.5{c:d}");
  // An escaped quote is not a string opener in Phase 4 either (the
  // decimal rule must also not fire right after the pair).
  ExpectMinify(".a\\'0.5{c:d}", ".a\\'0.5{c:d}");
  // Audit P3-a: an escaped quote/semicolon no longer opens a phantom
  // string that suppressed the trailing-';' trim.
  ExpectMinify("a{b:\\'c;}", "a{b:\\'c}");
}

TEST(CssMinifyTest, CustomPropertyColonCombinatorStopSet) {
  // Issue #1156: combinators can never appear in a property name, and
  // Phase 2 removes whitespace after them before AtCustomPropertyColon
  // runs — without the stop, the scan glued a phantom "--" name across
  // the removed space and the phases disagreed on custom-ness.
  ExpectMinify("--+ y: }", "--+y:}");
  ExpectMinify("--> y: }", "-->y:}");
  ExpectMinify("--~ y: }", "--~y:}");
  // Same leak via Phase 2's space-removal before '!' (found by the
  // post-review fuzz round; pre-existing on main, invalid-input-only).
  ExpectMinify("-- !: 3", "--!:3");
  ExpectMinify("--x !: 3", "--x!:3");
  // And via calc-mode's space stripping around '*' and '/' (review on
  // #1165; needs a ';' inside calc — invalid-input-only).  Calc mode
  // keeps the space after ';' (potential '-' operand), so the stable
  // form retains it; the ':' space is stripped because the name is
  // correctly NOT a custom property.
  ExpectMinify("a{b:calc(1*2; --* y: 2)}", "a{b:calc(1*2; --*y:2)}");
  ExpectMinify("a{b:calc(1*2; --/ y: 2)}", "a{b:calc(1*2; --/y:2)}");
  // Genuine custom-property detection is unaffected.
  ExpectMinify("a{--x:  calc(1px +  2px) ;b:c}", "a{--x:calc(1px +  2px);b:c}");
  ExpectMinify("a{--z:b}", "a{--z:b}");
}

TEST(CssMinifyTest, Phase5SplitterEscapePairs) {
  // Issue #1164: an escaped ';' is value content, never a declaration
  // terminator — ParseBlockDecls now consumes escape pairs outside
  // strings, mirroring every other scanner in the file.
  ExpectMinify("a{b:c\\;}", "a{b:c\\;}");
  // The escaped ';' no longer terminates; the REAL ';' still splits.
  ExpectMinify("a{b:c\\;;d:e}", "a{b:c\\;;d:e}");
  // Custom-value faces (token-stream oracle red-liners): the escape
  // must not eat the terminator ...
  ExpectMinify("a{--x:\\;}", "a{--x:\\;}");
  // ... nor glue into the next declaration name, silently swallowing
  // an entire custom property (reviewer find on #1169).
  ExpectMinify("a{m:\\;;--z:url(x)}", "a{m:\\;;--z:url(x)}");
  // An escaped ':' is not a name/value split either.
  ExpectMinify("a{b\\:c:d}", "a{b\\:c:d}");
  // Escaped quotes pass through (this alignment errs symmetrically in
  // every scanner, so bytes round-trip — the phantom-string face two
  // pins below needed a second quote to misalign block boundaries).
  ExpectMinify("a{b:c\\';d:e}", "a{b:c\\';d:e}");
  // Phantom-string face (found by the post-fix fuzz sweep, after an
  // earlier "not reproducible" mis-triage): an escaped quote outside a
  // string opened a phantom string in the top-level scan and the brace
  // matcher, misaligning block boundaries so CollapseBlock rewrote
  // string content ("e{x;}" lost its ';').  Both scanners now consume
  // escape pairs outside strings.
  ExpectMinify("a{b:c\\'d'e{x;}}", "a{b:c\\'d'e{x;}}");
  ExpectMinify("a{b:c\\'d'{x;}}", "a{b:c\\'d'{x;}}");
}

TEST(CssMinifyTest, CalcMathModeNeverManufacturesCommentToken) {
  // Issue #1159: tightening the space between '/' and '*' inside calc
  // manufactures a "/*" comment token pass 2's comment stripping honors
  // (truncating the sheet); the mirror ('*' + '/' -> "*/") could end a
  // real enclosing comment early.  The space between them is kept.
  ExpectMinify("a{b:calc(1 / *2)}", "a{b:calc(1/ *2)}");
  ExpectMinify("a{b:calc(1 * /2)}", "a{b:calc(1* /2)}");
  ExpectMinify("a{b:round( / *2)}", "a{b:round(/ *2)}");
  ExpectMinify("a{b:calc(1px/ * 2px)}", "a{b:calc(1px/ *2px)}");
  // Legitimate tightening around * and / is unaffected.
  ExpectMinify("a{b:calc(1px / 2)}", "a{b:calc(1px/2)}");
  ExpectMinify("a{b:calc(1px * 2)}", "a{b:calc(1px*2)}");
}

TEST(CssMinifyTest, EmptyLonghandValueBlocksCollapse) {
  // Issue #1162: TryCollapseFamily accepted empty longhand values and
  // FormatOverflowValues emitted the separator unconditionally, leaving
  // a trailing space ("{overflow:: }") no pass trims — one-pass-late
  // convergence.  Empty values now refuse the collapse.
  ExpectMinify("{overflow-y:;overflow-x::}", "{overflow-y:;overflow-x::}");
  ExpectMinify("a{overflow-y:;overflow-x::}", "a{overflow-y:;overflow-x::}");
  // Broader face (post-fix fuzz sweep): a value whose edge char makes
  // the emitted separator space Phase-2-trimmable also refuses the
  // collapse — ':'/',' (CanRemoveSpaceAfter) and '!' (space-before-'!'
  // strip) variants.
  ExpectMinify("a{overflow-x:v:;overflow-y:w}",
               "a{overflow-x:v:;overflow-y:w}");
  ExpectMinify("a{overflow-x:v,;overflow-y:w}",
               "a{overflow-x:v,;overflow-y:w}");
  ExpectMinify("a{overflow-x:v;overflow-y:!w}",
               "a{overflow-x:v;overflow-y:!w}");
  // A real empty-value declaration elsewhere passes through unchanged.
  ExpectMinify("a{b:}", "a{b:}");
  // Genuine overflow collapse is unaffected.
  ExpectMinify("a{overflow-x:hidden;overflow-y:auto}",
               "a{overflow:hidden auto}");
}

TEST(CssMinifyTest, UnbalancedNestingLonghandValueBlocksCollapse) {
  // Issue #1170: Phase 5's shorthand collapse reorders longhand values;
  // a value with unbalanced parens/brackets (invalid input — valid
  // values balance, brackets never occur in these families, and Phases
  // 1-2 never unbalance either) relocates a bracket across what the
  // next pass sees as a block edge, shifting the depth tracking that
  // delimits blocks (Phase 5's matcher, Phase 3's custom-value extent)
  // and with it Phase 3's trim context and Phase 5's reparse extent
  // between passes, so minify(minify(x)) != minify(x).  The collapse
  // now refuses unbalanced values.  Minimized from the nightly
  // strict-oracle artifacts plus the issue's repro; ExpectMinify's
  // second pass is the oracle that red-lined.
  ExpectMinify(
      "{padding-top:);padding-left:(;padding-right:p{Y;padding-bottom:)};}",
      "{padding-top:);padding-left:(;padding-right:p{Y;padding-bottom:)};}");
  ExpectMinify(
      "{padding-top:;padding-left:(;padding-right:@;padding-bottom::{)};}",
      "{padding-top:;padding-left:(;padding-right:@;padding-bottom::{)};}");
  ExpectMinify(
      "a {overflow-y:-!(U -&y4;overflow-x:):w)}4;ov;rfov;;x-rlow-x:):] }",
      "a{overflow-y:-!(U -&y4;overflow-x:):w)}4;ov;rfov;;x-rlow-x:):]}");
  // Bracket face (post-fix sweep): the reordered ']'/'[' shifts Phase
  // 3's custom-value nesting depth, so the trailing ';' of the custom
  // value becomes trimmable one pass late ("--?:{padding:x ] [ q};}" ->
  // "--?:{padding:x ] [ q}}").
  ExpectMinify(
      "--?:{padding-left:q;padding-top:x;padding-right:];padding-bottom:[};}",
      "--?:{padding-left:q;padding-top:x;padding-right:];padding-bottom:[};}");
  ExpectMinify(
      "--@:{padding-left:q;padding-top:x;padding-right:];padding-bottom:[};}",
      "--@:{padding-left:q;padding-top:x;padding-right:];padding-bottom:[};}");
  // Brace-run face (third sweep): the top value "(}}})" is
  // paren-balanced but its '}' run shifts Phase 1's custom-value
  // nesting depth once reordered first, ending the opaque region one
  // spot earlier next pass — the left value's tab, preserved inside
  // the custom value in pass 1, gets whitespace-normalized in pass 2.
  ExpectMinify(
      "--$:{padding-right:d;padding-bottom:x;padding-left:a\tb;"
      "padding-top:(}}})}",
      "--$:{padding-right:d;padding-bottom:x;padding-left:a\tb;"
      "padding-top:(}}})}");
  // Paren-shielded-brace face (fourth sweep): "w{(])" is paren- and
  // bracket-balanced, but its '{' is invisible to Phase 5's block
  // matcher only while a preceding value's unbalanced '(' holds the
  // depth open; the reordered arrangement exposes it, flipping the
  // has_nested/reparse path and dropping the ';' after "({".
  ExpectMinify(
      "{({;padding-left:w{(];padding-top:v;padding-right:x;"
      "padding-bottom:[}{)}}",
      "{({;padding-left:w{(];padding-top:v;padding-right:x;"
      "padding-bottom:[}{)}}");
  // These two used to collapse ("balanced-paren values still collapse:
  // calc/min/max/env functions are legitimate longhand values") and no longer
  // do -- withdrawn deliberately by #1378's value gate, which whitelists plain
  // numbers/dimensions/percentages and family keywords and cannot validate a
  // function's contents ("calc(1px + )" is balanced and invalid, and
  // collapsing it costs the whole family).  They are kept, flipped, because
  // what they still pin is that this refusal comes from the VALUE gate and not
  // from the nesting guard #1170 added: the nesting guard's own green side is
  // the case below, which is balanced and whitelisted and does collapse.
  ExpectMinify(("a{padding-top:calc(1px + 2px);padding-right:calc(2px - 1px);"
                "padding-bottom:min(1px,2px);padding-left:max(3px,4px)}"),
               ("a{padding-top:calc(1px + 2px);padding-right:calc(2px - 1px);"
                "padding-bottom:min(1px,2px);padding-left:max(3px,4px)}"));
  ExpectMinify(("a{margin-top:env(safe-area-inset-top);"
                "margin-right:env(safe-area-inset-right);"
                "margin-bottom:env(safe-area-inset-bottom);"
                "margin-left:env(safe-area-inset-left)}"),
               ("a{margin-top:env(safe-area-inset-top);"
                "margin-right:env(safe-area-inset-right);"
                "margin-bottom:env(safe-area-inset-bottom);"
                "margin-left:env(safe-area-inset-left)}"));
  // The nesting guard does not over-refuse: whitelisted values collapse
  // whether or not other declarations in the sheet carry parens.
  ExpectMinify(
      "a{padding-top:1px;padding-right:2%;padding-bottom:0;"
      "padding-left:4rem}",
      "a{padding:1px 2% 0 4rem}");
}

TEST(CssMinifyTest, InvalidLonghandValueBlocksCollapse) {
  // Issue #1378: Phase 5 collapsed a family without ever asking whether the
  // member values are legal for the property.  Per CSS Cascading an invalid
  // LONGHAND drops one declaration while an invalid SHORTHAND drops the whole
  // family, so the collapse converted one ignored typo into the loss of every
  // sibling.  The five cases below are the browser-confirmed ones from the
  // issue (Chromium, the minifier's own output); each must now be emitted
  // unchanged.

  // padding-right takes a length/percentage, not a colour keyword.
  // 3 of 4 declarations applied -> 0 once collapsed.
  ExpectMinify(
      "a{padding-top:1px;padding-right:red;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:1px;padding-right:red;padding-bottom:1px;"
      "padding-left:1px}");
  // Typo'd unit: "4pxx" is not a dimension.
  ExpectMinify(
      "a{padding-top:10px;padding-right:20px;padding-bottom:30px;"
      "padding-left:4pxx}",
      "a{padding-top:10px;padding-right:20px;padding-bottom:30px;"
      "padding-left:4pxx}");
  // Malformed math function: balanced, and invalid.
  ExpectMinify(
      "a{margin-top:1px;margin-right:2px;margin-bottom:3px;"
      "margin-left:calc(1px + )}",
      "a{margin-top:1px;margin-right:2px;margin-bottom:3px;"
      "margin-left:calc(1px + )}");
  // Not a colour.  2 of 3 applied -> 0 once collapsed.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:notacolor}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:notacolor}");
  // Not an overflow keyword.  The collapse also silently changed overflow-x
  // from hidden to visible.
  ExpectMinify("a{overflow-x:hidden;overflow-y:bogus}",
               "a{overflow-x:hidden;overflow-y:bogus}");
}

TEST(CssMinifyTest, InvalidLonghandValueGateIsPerLonghand) {
  // The gate asks what THIS longhand accepts, not what the family accepts
  // somewhere.  A value that is legal for a sibling is still invalid here, and
  // a union-shaped gate would wave both of these through.
  ExpectMinify(
      "a{border-top-width:solid;border-top-style:solid;"
      "border-top-color:red}",
      "a{border-top-width:solid;border-top-style:solid;"
      "border-top-color:red}");
  ExpectMinify(
      "a{border-left-width:1px;border-left-style:1px;"
      "border-left-color:red}",
      "a{border-left-width:1px;border-left-style:1px;"
      "border-left-color:red}");
  ExpectMinify(
      "a{border-right-width:1px;border-right-style:solid;"
      "border-right-color:solid}",
      "a{border-right-width:1px;border-right-style:solid;"
      "border-right-color:solid}");
  // "auto" is a margin keyword; padding has no keywords at all.
  ExpectMinify(
      "a{padding-top:auto;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:auto;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
  // A unitless non-zero length is invalid; unitless zero is not.
  ExpectMinify(
      "a{padding-top:5;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:5;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
  ExpectMinify(
      "a{padding-top:0;padding-right:0;padding-bottom:0;"
      "padding-left:0}",
      "a{padding:0}");
  // Percentages are legal for padding/margin and not for border-width.
  ExpectMinify(
      "a{margin-top:10%;margin-right:10%;margin-bottom:10%;"
      "margin-left:10%}",
      "a{margin:10%}");
  ExpectMinify(
      "a{border-top-width:10%;border-top-style:solid;"
      "border-top-color:red}",
      "a{border-top-width:10%;border-top-style:solid;"
      "border-top-color:red}");
}

TEST(CssMinifyTest, ValidLonghandFamiliesStillCollapse) {
  // The green side: the gate must not be a blanket refusal.  Every value class
  // it whitelists, and the case-insensitivity keywords and units have in CSS.
  ExpectMinify(
      "a{padding-top:1px;padding-right:2em;padding-bottom:3.5rem;"
      "padding-left:0}",
      "a{padding:1px 2em 3.5rem 0}");
  ExpectMinify(
      "a{padding-top:1vmin;padding-right:2dvh;padding-bottom:3q;"
      "padding-left:4pc}",
      "a{padding:1vmin 2dvh 3q 4pc}");
  ExpectMinify(
      "a{margin-top:0;margin-right:auto;margin-bottom:0;"
      "margin-left:auto}",
      "a{margin:0 auto}");
  ExpectMinify(
      "a{margin-top:-5px;margin-right:-5px;margin-bottom:-5px;"
      "margin-left:-5px}",
      "a{margin:-5px}");
  ExpectMinify(
      "a{border-top-width:thin;border-top-style:double;"
      "border-top-color:rebeccapurple}",
      "a{border-top:thin double rebeccapurple}");
  ExpectMinify(
      "a{border-bottom-width:medium;border-bottom-style:groove;"
      "border-bottom-color:#abcd}",
      "a{border-bottom:medium groove #abcd}");
  ExpectMinify(
      "a{border-left-width:0;border-left-style:none;"
      "border-left-color:currentcolor}",
      "a{border-left:0 none currentcolor}");
  ExpectMinify(
      "a{border-right-width:2px;border-right-style:hidden;"
      "border-right-color:transparent}",
      "a{border-right:2px hidden transparent}");
  ExpectMinify("a{overflow-x:clip;overflow-y:visible}",
               "a{overflow:clip visible}");
  // Keywords and units are ASCII case-insensitive in CSS.
  ExpectMinify(
      "a{padding-top:1PX;padding-right:1Px;padding-bottom:1pX;"
      "padding-left:1px}",
      "a{padding:1PX 1Px 1pX 1px}");
  ExpectMinify(
      "a{border-top-width:THIN;border-top-style:Solid;"
      "border-top-color:RED}",
      "a{border-top:THIN Solid RED}");
}

TEST(CssMinifyTest, FunctionalColoursStillCollapse) {
  // Functional colours in a border-*-color longhand are the one shape real
  // stylesheets put in these families often enough for a refusal to cost
  // measurable bytes, so the gate admits them -- by a bounded check: the
  // function name, one paren pair closing at the end, a body of nothing but
  // digits, commas, dots, percent signs and spaces, and a well-formed
  // comma-separated argument list.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgba(255,255,255,.1)}",
      "a{border-top:1px solid rgba(255,255,255,.1)}");
  ExpectMinify(
      "a{border-left-width:2px;border-left-style:dashed;"
      "border-left-color:rgb(0,0,0)}",
      "a{border-left:2px dashed rgb(0,0,0)}");
  ExpectMinify(
      "a{border-right-width:1px;border-right-style:solid;"
      "border-right-color:hsl(120,50%,50%)}",
      "a{border-right:1px solid hsl(120,50%,50%)}");
  ExpectMinify(
      "a{border-bottom-width:1px;border-bottom-style:solid;"
      "border-bottom-color:hsla(120,50%,50%,.5)}",
      "a{border-bottom:1px solid hsla(120,50%,50%,.5)}");
  // All-percentage rgb(), and an alpha component written both ways.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(100%,0%,0%)}",
      "a{border-top:1px solid rgb(100%,0%,0%)}");
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgba(1,2,3,50%)}",
      "a{border-top:1px solid rgba(1,2,3,50%)}");
  // Case-insensitive function names.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:RGBA(1,2,3,.4)}",
      "a{border-top:1px solid RGBA(1,2,3,.4)}");
}

TEST(CssMinifyTest, FunctionalColourCheckStaysBounded) {
  // The red side of F1's check.  It must not become a generic
  // "known-prefix + balanced parens" rule: that is exactly what would re-admit
  // `calc(1px + )`, this issue's own reported case.  Everything whose body the
  // check cannot account for refuses the collapse.
  //
  // A colour function is not a licence for arbitrary contents.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(var(--r),0,0)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(var(--r),0,0)}");
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(calc(1px + ),0,0)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(calc(1px + ),0,0)}");
  // Colour functions this gate does not read are refused, not guessed at.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:color-mix(in srgb,red,blue)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:color-mix(in srgb,red,blue)}");
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:oklch(.7 .1 200)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:oklch(.7 .1 200)}");
  // Slash alpha syntax and <angle> hues carry bytes the body rule rejects.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(255 0 0/10%)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(255 0 0/10%)}");
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:hsl(120deg 50% 50%)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:hsl(120deg 50% 50%)}");
  // No digits at all, and an unterminated form.
  ExpectMinify(
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(,,)}",
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:rgb(,,)}");
  // Reading the bytes is not enough: an argument list made only of accountable
  // bytes can still be invalid CSS, and an invalid value inside a collapsed
  // shorthand is the blast shape this whole gate exists to prevent.  It must
  // not be re-opened inside the class the gate re-admits.
  const char* kMalformedArgumentLists[] = {
      "rgb(0)",            // too few components
      "rgb(0,0)",          // still too few
      "rgb(0,0,0,0,0)",    // too many
      "rgb(0,0,0,)",       // trailing comma -> empty component
      "rgb(,0,0)",         // leading comma -> empty component
      "rgb(0,,0)",         // empty interior component
      "rgb(1.2.3,0,0)",    // two dots in one component
      "rgb(1%0,0,0)",      // '%' is a unit, not an interior byte
      "rgb(100%,0,0)",     // legacy rgb() is all-number or all-percentage
      "hsl(50,50,50)",     // saturation/lightness need '%'
      "hsl(50%,50%,50%)",  // ... and the hue must not have one
      "hsla(50,50%,50)",   // lightness still needs '%'
  };
  for (const char* colour : kMalformedArgumentLists) {
    std::string css = std::string(
                          "a{border-top-width:1px;border-top-style:solid;"
                          "border-top-color:") +
                      colour + "}";
    ExpectMinify(css, css);
  }
  // A colour function is a colour, not a length or a style.
  ExpectMinify(
      "a{border-top-width:rgb(1,2,3);border-top-style:solid;"
      "border-top-color:red}",
      "a{border-top-width:rgb(1,2,3);border-top-style:solid;"
      "border-top-color:red}");
  ExpectMinify(
      "a{padding-top:rgb(1,2,3);padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:rgb(1,2,3);padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
}

TEST(CssMinifyTest, NegativeLengthBlocksCollapseWhereTheGrammarForbidsIt) {
  // padding-* is <length-percentage [0,inf]> and
  // border-*-width is [0,inf], so a negative there is an invalid longhand --
  // one dropped declaration if left alone, the whole family if collapsed.
  ExpectMinify(
      "a{padding-top:-1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:-1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
  ExpectMinify(
      "a{padding-top:1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:-2%}",
      "a{padding-top:1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:-2%}");
  ExpectMinify(
      "a{border-top-width:-1px;border-top-style:solid;border-top-color:red}",
      "a{border-top-width:-1px;border-top-style:solid;border-top-color:red}");
  // margin-* genuinely takes negatives and shares the same helper -- the flag
  // is per value class, not global.
  ExpectMinify(
      "a{margin-top:-1px;margin-right:-2%;margin-bottom:0;margin-left:auto}",
      "a{margin:-1px -2% 0 auto}");
  // A '+' sign is not a negative, but the separator-edge guard refuses signed
  // lengths ahead of this gate for its own reason; pinned so a change to
  // either guard has to face the other.
  ExpectMinify(
      "a{padding-top:+1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:+1px;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
}

TEST(CssMinifyTest, ContainerQueryUnitsStillCollapse) {
  // Container-query units are ordinary lengths and must be in the unit table:
  // a unit missing from it withdraws every family that uses it, silently.
  ExpectMinify(
      "a{padding-top:1cqw;padding-right:2cqh;padding-bottom:3cqi;"
      "padding-left:4cqb}",
      "a{padding:1cqw 2cqh 3cqi 4cqb}");
  ExpectMinify(
      "a{margin-top:1cqmin;margin-right:2cqmax;margin-bottom:1cqmin;"
      "margin-left:2cqmax}",
      "a{margin:1cqmin 2cqmax}");
  // Still not a licence for any two letters after a number.
  ExpectMinify(
      "a{padding-top:1cqz;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}",
      "a{padding-top:1cqz;padding-right:1px;padding-bottom:1px;"
      "padding-left:1px}");
}

TEST(CssMinifyTest, InvalidLonghandValueGateAppliesInsideNestedBlocks) {
  // Phase 5 recurses into @media/@layer/@supports blocks and collapses there
  // through the same code path, so the gate has to hold there too -- a nested
  // block is exactly where a hand-written override typo lives.
  ExpectMinify(
      "@media(max-width:600px){a{padding-top:1px;padding-right:red;"
      "padding-bottom:1px;padding-left:1px}}",
      "@media(max-width:600px){a{padding-top:1px;padding-right:red;"
      "padding-bottom:1px;padding-left:1px}}");
  ExpectMinify("@layer base{a{overflow-x:hidden;overflow-y:bogus}}",
               "@layer base{a{overflow-x:hidden;overflow-y:bogus}}");
  ExpectMinify(
      "@supports(display:grid){@media screen{"
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:notacolor}}}",
      "@supports(display:grid){@media screen{"
      "a{border-top-width:1px;border-top-style:solid;"
      "border-top-color:notacolor}}}");
  // ... and does not block sound collapses there either.
  ExpectMinify(
      "@media(max-width:600px){a{padding-top:1px;padding-right:1px;"
      "padding-bottom:1px;padding-left:1px}}",
      "@media(max-width:600px){a{padding:1px}}");
  ExpectMinify("@layer base{a{overflow-x:hidden;overflow-y:auto}}",
               "@layer base{a{overflow:hidden auto}}");
}

TEST(CssMinifyTest, OperatorEdgeLonghandValueBlocksCollapse) {
  // Issue #1170's operator-space face: Phase 2 strips whitespace
  // adjacent to '*'/'/' inside calc math mode, and an unterminated
  // "calc(" leaves math mode on for the rest of the sheet — so a
  // longhand value whose edge char is '*'/'/' makes the collapse's
  // emitted separator space Phase-2-trimmable next pass (one-pass-late
  // convergence; the reorder also relocates which value carries the
  // edge).  Valid longhand values never begin or end with an operator.
  // Minimized from the post-fix fork-mode sweep's residual tripper.
  ExpectMinify(
      "calc({padding-top:x;padding-left:p;padding-right:x;"
      "padding-bottom:*}",
      "calc({padding-top:x;padding-left:p;padding-right:x;"
      "padding-bottom:*}");
  ExpectMinify(
      "calc({padding-top:x;padding-left:*y;padding-right:z;"
      "padding-bottom:w}",
      "calc({padding-top:x;padding-left:*y;padding-right:z;"
      "padding-bottom:w}");
}

TEST(CssMinifyTest, BraceGroupsInDeclarationValuesAreOpaque) {
  // Issue #1167: Phase 5's brace matcher recursed into {} groups
  // nested in declaration values and CollapseBlock re-emitted their
  // content; Phase 3's ';'-trim fired inside them too.  Both phases
  // now treat the group as opaque value content (IsValueGroupBrace).
  // Leading-';' face (#1168's repro, dup'd into #1167):
  ExpectMinify("a{--z:{;x}}", "a{--z:{;x}}");
  ExpectMinify("a{--z:{;;x}}", "a{--z:{;;x}}");
  // Trailing-';' face (custom and ordinary):
  ExpectMinify("a{--z:{L;}}", "a{--z:{L;}}");
  ExpectMinify("a{b:{L;}}", "a{b:{L;}}");
  // False shorthand collapse inside a custom value:
  ExpectMinify(("a{--z:{padding-top:1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}}"),
               ("a{--z:{padding-top:1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}}"));
  // Ordinary-value faces (invisible to both oracles, pinned here):
  ExpectMinify("a{b:c{;x}}", "a{b:c{;x}}");
  ExpectMinify("a{b:c{;x}d;e:f}", "a{b:c{;x}d;e:f}");
  // Value continues after the group; paren'd ';' does not end it:
  ExpectMinify("a{--z:foo(a;c){;x}}", "a{--z:foo(a;c){;x}}");
  // Empty group behavior unchanged (CollapseBlock no-op before, too):
  ExpectMinify("a{--z:{;}}", "a{--z:{;}}");
  ExpectMinify("a{--z:{L}}", "a{--z:{L}}");
  // Top-level custom-property declaration (depth-0 '--' rule):
  ExpectMinify("--x:{;x}", "--x:{;x}");
  // Guards: real blocks, the real trim, pseudo-class selectors, and
  // genuine collapse are all unaffected.
  ExpectMinify("a{b:c;}", "a{b:c}");
  ExpectMinify("a:hover{b:c;}", "a:hover{b:c}");
  // Nested pseudo selectors (empty pre-colon run): unambiguous CSS
  // Nesting — trim and collapse still fire (review MUST-FIX).
  ExpectMinify("a{:hover{b:c;}}", "a{:hover{b:c}}");
  ExpectMinify("a{::before{content:\"x\";color:red;}}",
               "a{::before{content:\"x\";color:red}}");
  ExpectMinify(("a{:hover{padding-top:1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}}"),
               "a{:hover{padding:1px}}");
  // The disclosed trade-off: ident-starting pseudo preludes are treated
  // as value groups (genuinely declaration-ambiguous per CSS Nesting's
  // relaxed parsing, which tries declarations first).
  ExpectMinify("a{a:hover{color:red}}", "a{a:hover{color:red}}");
  ExpectMinify(":root{--x:0.5}", ":root{--x:0.5}");
  ExpectMinify("@media screen and (max-width:600px){a{color:blue}}",
               "@media screen and (max-width:600px){a{color:blue}}");
  ExpectMinify(("a{padding-top:1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}"),
               "a{padding:1px}");
}

TEST(CssMinifyTest, SemicolonsInsideParensAreValueContent) {
  // Phase 5's declaration splitter (ParseBlockDecls) cut
  // at every ';' outside strings/url()/escapes with no paren or bracket
  // depth, so the empty segment between the two ';' of a parenthesized
  // group was an empty declaration and the rejoin dropped it.  A custom
  // property reads its value back verbatim (Chromium applies
  // --x:"(a;;b)" for the input and "(a;b)" for the old output), so this
  // was served-bytes corruption of valid CSS.  CSS Syntax 3 ends a
  // declaration only at a TOP-LEVEL ';'; the splitter now tracks the
  // depth, as Phase 3's ';'-trim already did.
  ExpectMinify("a{--x:(a;;b)}", "a{--x:(a;;b)}");
  ExpectMinify("a{--x:[a;;b]}", "a{--x:[a;;b]}");
  ExpectMinify("a{--x:f(a;;b)}", "a{--x:f(a;;b)}");
  ExpectMinify("a{--x:(a{b;;c}d)}", "a{--x:(a{b;;c}d)}");
  ExpectMinify("a{--x:(a;;b);c:d}", "a{--x:(a;;b);c:d}");
  // Ordinary values go through the same splitter.
  ExpectMinify("a{b:(a;;b)}", "a{b:(a;;b)}");
  // The declaration after the group is still its own declaration:
  // shorthand collapse keeps firing there.
  ExpectMinify(("a{--x:(a;;b);padding-top:1px;padding-right:1px;"
                "padding-bottom:1px;padding-left:1px}"),
               "a{--x:(a;;b);padding:1px}");
  // The fuzz artifact (crash-5e066d75, run 37097654289): an unclosed
  // TOP-LEVEL '(' is a simple block that CSS Syntax 3 runs to EOF, so
  // no browser applies anything after it (Chromium: 0 rules for input
  // and old output alike) — but the Phase 5 top-level scan still opened
  // a declaration block on the '{' inside it and dropped the empty
  // declarations, while its own brace matcher treats a '{' inside
  // parens as content.  Byte-stable now, both at top level and in a
  // prelude function such as a container style query.
  ExpectMinify("3-0:/(;--:{!aar;;;;;;;;;;;;;;y}",
               "3-0:/(;--:{!aar;;;;;;;;;;;;;;y}");
  ExpectMinify("(;--:{a;;b}", "(;--:{a;;b}");
  ExpectMinify("@container style(--c:{a;;b}){x{y:z}}",
               "@container style(--c:{a;;b}){x{y:z}}");
  // Bracket twins (review of the fix): the brace matcher and the
  // top-level scan count '[' like '('.  An unclosed '[' swallows the
  // '}' that used to close a's block (the matcher then collapsed
  // "c:d;;" as e's declarations), and a ')' inside a brace group inside
  // parens must not close the paren.
  ExpectMinify("a{--x:[a}e{c:d;;}", "a{--x:[a}e{c:d;;}");
  ExpectMinify("a{--x:(a{b)c;;d}e)}", "a{--x:(a{b)c;;d}e)}");
  ExpectMinify("[;--:{a;;b}", "[;--:{a;;b}");
  // Guards: top-level ';' runs still trim, a stray closer at depth 0 is
  // not an opener, and parens in a prelude close before the block.
  ExpectMinify("a{b:c;;;d:e}", "a{b:c;d:e}");
  ExpectMinify("a{;;b:c}", "a{b:c}");
  ExpectMinify("a{b:c];;d:e}", "a{b:c];d:e}");
  ExpectMinify("a){color:red;;}", "a){color:red}");
  ExpectMinify(":is(a,b){color:red;;}", ":is(a,b){color:red}");
  ExpectMinify("@media (min-width:1px){a{color:blue;;}}",
               "@media (min-width:1px){a{color:blue}}");
  // An unclosed paren inside a block makes one declaration to the
  // block end: emitted verbatim, no collapse (same bytes as before —
  // the paren-unbalanced refusal already produced them).
  ExpectMinify(("a{padding-top:(1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}"),
               ("a{padding-top:(1px;padding-right:1px;padding-bottom:1px;"
                "padding-left:1px}"));
}

TEST(CssMinifyTest, EscapedCustomPropertyNameKeepsValueBraceGroup) {
  // #954 (earlier faces: #848, #882, #907): an escaped char is part of a
  // custom-property name, so a brace group in the value is opaque value
  // content.  Phase 5 classified such a name as not-a-declaration and
  // collapsed the group as a nested block (empty declarations dropped,
  // longhands merged), rewriting the value.
  ExpectMinify("a{--\\>p:{;b;}}", "a{--\\>p:{;b;}}");
  ExpectMinify("a{--a\\:b:{;b;}}", "a{--a\\:b:{;b;}}");
  ExpectMinify("a{--a\\{b:{;b;}}", "a{--a\\{b:{;b;}}");
  ExpectMinify("a{--a\\}b:{;b;}}", "a{--a\\}b:{;b;}}");
  ExpectMinify("a{--\\.p:x {;b;} y}", "a{--\\.p:x {;b;} y}");
  ExpectMinify(("a{--\\>p:{padding-top:1px;padding-right:1px;"
                "padding-bottom:1px;padding-left:1px}}"),
               ("a{--\\>p:{padding-top:1px;padding-right:1px;"
                "padding-bottom:1px;padding-left:1px}}"));
  // Hex escapes and unescaped names were already handled; unchanged.
  ExpectMinify("a{--a\\3e b:{;b;}}", "a{--a\\3e b:{;b;}}");
  ExpectMinify("a{--p:{;b;}}", "a{--p:{;b;}}");
  // A nested-selector-shaped prelude with an escaped char before a ':'
  // reads as a declaration name too, so its brace group is kept as
  // written (the conservative reading: never rewrite what might be a
  // value).
  ExpectMinify("a{b\\>c:hover{;d:e;}}", "a{b\\>c:hover{;d:e;}}");
  // An escaped backslash does not escape the char after it: a real
  // nested rule block still collapses.
  ExpectMinify("a{b\\\\>c{;d:e;}}", "a{b\\\\>c{d:e}}");
}

TEST(CssMinifyTest, TrailingBackslashAtEofDoesNotCrash) {
  ExpectMinify("h1{color:red}\\", "h1{color:red}\\");
}

TEST(CssMinifyTest, Phase5DeepNestingDoesNotCrash) {
  // 200 nested blocks exceeds the 128-depth cap.
  // Phase5 should complete without crashing; content is passed through.
  std::string input;
  for (int i = 0; i < 200; ++i) input += "a{";
  input += "color:red";
  for (int i = 0; i < 200; ++i) input += "}";
  std::string output;
  ASSERT_TRUE(MinifyCss(input, &output));
  EXPECT_FALSE(output.empty());
  // The innermost declaration should survive.
  EXPECT_NE(output.find("color:red"), std::string::npos);
}

}  // namespace
}  // namespace pagespeed::css
