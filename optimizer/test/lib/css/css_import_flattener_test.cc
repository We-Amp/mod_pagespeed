// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - CSS @import Flattener Tests

#include "lib/css/css_import_flattener.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace pagespeed::css {
namespace {

// ========== Media wrapping Tests ==========

TEST(StructurallyBalancedTest, BalancedAndNested) {
  EXPECT_TRUE(IsStructurallyBalanced(""));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:c}"));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:c}d{e:f}"));
  EXPECT_TRUE(
      IsStructurallyBalanced("@media (min-width:1px){a{b:c}d{e:f}}x{y:z}"));
  EXPECT_TRUE(IsStructurallyBalanced("@supports (a:b){@media print{a{b:c}}}"));
}

TEST(StructurallyBalancedTest, StrayOrUnclosedBraces) {
  EXPECT_FALSE(IsStructurallyBalanced("a{b:c}}"));
  EXPECT_FALSE(IsStructurallyBalanced("}a{b:c}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:c}}d{e:f}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:c"));
  EXPECT_FALSE(IsStructurallyBalanced("@media x{a{b:c}"));
  EXPECT_FALSE(IsStructurallyBalanced("}{"));
  // A trailing backslash would escape the wrapper's closing brace.
  EXPECT_FALSE(IsStructurallyBalanced("a{b:c}\\"));
}

TEST(StructurallyBalancedTest, BracesInCommentsStringsAndEscapesDoNotCount) {
  EXPECT_TRUE(IsStructurallyBalanced("/* } */a{b:c}"));
  EXPECT_TRUE(IsStructurallyBalanced("a::before{content:\"}\"}"));
  EXPECT_TRUE(IsStructurallyBalanced("a::before{content:'{'}"));
  EXPECT_TRUE(IsStructurallyBalanced("a::before{content:\"\\\"}\"}"));
  EXPECT_TRUE(IsStructurallyBalanced("a\\}b{c:d}"));
  // An unquoted url( ... ) is one token: a brace inside it is not a brace.
  EXPECT_TRUE(IsStructurallyBalanced("a{b:url(a}b)}"));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:URL( x{y )}"));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:url(\"}\")}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:url({)}}c{display:none}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:url(x"));
  // A non-ASCII byte continues an ident, so this is a normal function and
  // its brace counts.
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\xc3\xa9url(})}c{x:y}"));
  EXPECT_FALSE(IsStructurallyBalanced("/* unterminated a{b:c}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{content:\"unterminated}"));
  // A comment cannot hide a real brace.
  EXPECT_FALSE(IsStructurallyBalanced("a{b:c}/* x */}"));
}

TEST(StructurallyBalancedTest, NewlineInAStringAndEscapedUrlName) {
  // A string ends at an unescaped newline, so the quotes below do not pair
  // up the way a scan that ignores newlines would pair them.
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\"x\n}}c{display:none}d{e:\"}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:'x\r}}c{display:none}d{e:'}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\"x\f}}c{display:none}d{e:\"}"));
  // An escaped newline continues a string.
  EXPECT_TRUE(IsStructurallyBalanced("a{b:\"a\\\nb\"}"));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:\"a\\\r\nb\"}"));
  // An escaped spelling of url( is still a url token with a raw body.
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\\url({)}}c{display:none}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\\75rl({)}}c{display:none}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\\000075 rl({)}}c{display:none}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:u\\72l({)}}c{display:none}"));
  EXPECT_FALSE(IsStructurallyBalanced("a{b:\\55RL({)}}c{display:none}"));
  // Other escaped identifiers are ordinary names.
  EXPECT_TRUE(IsStructurallyBalanced("a{b:\\75rlx(c)}"));
  EXPECT_TRUE(IsStructurallyBalanced("a{b:\\66oo(c)}"));
  // An escaped selector is not a url token.
  EXPECT_TRUE(IsStructurallyBalanced(".a\\:b{c:d}"));
  // A name longer than any spelling of "url" cannot be one.
  EXPECT_TRUE(
      IsStructurallyBalanced("a{b:abcdefghijklmnopqrstuvwxyz0123\\75rl({)}}"));
}

TEST(StructurallyBalancedTest, LongRunOfEscapesIsHandled) {
  std::string css = ".x";
  for (int i = 0; i < 200000; ++i) css += "\\a";
  css += "{b:c}";
  EXPECT_TRUE(IsStructurallyBalanced(css));
}

TEST(AppendMediaScopedCssTest, WrapsABalancedSheet) {
  std::string out = "x";
  EXPECT_TRUE(AppendMediaScopedCss("print", "a{b:c}", &out));
  EXPECT_EQ(out, "x@media print{a{b:c}}");
  EXPECT_EQ(out.size(), 1 + MediaScopedCssSize("print", 6));
}

TEST(AppendMediaScopedCssTest, UnscopedSheetIsAppendedAsWritten) {
  std::string out;
  EXPECT_TRUE(AppendMediaScopedCss("", "a{b:c}}d{e:f}", &out));
  EXPECT_EQ(out, "a{b:c}}d{e:f}");
}

TEST(AppendMediaScopedCssTest, SheetThatCannotBeWrappedAddsNothing) {
  std::string out = "x";
  EXPECT_FALSE(AppendMediaScopedCss("print", "a{b:c}}d{e:f}", &out));
  EXPECT_FALSE(AppendMediaScopedCss("print", "a{b:c", &out));
  EXPECT_EQ(out, "x");
}

// ========== ExtractImports Tests ==========

TEST(ExtractImportsTest, NoImports) {
  auto imports = ExtractImports("body { color: red; }");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, SingleImportUrl) {
  auto imports = ExtractImports("@import url(\"style.css\");\nbody { }");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "style.css");
  EXPECT_TRUE(imports[0].media.empty());
}

TEST(ExtractImportsTest, SingleImportQuoted) {
  auto imports = ExtractImports("@import \"theme.css\";\nbody { }");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "theme.css");
}

TEST(ExtractImportsTest, SingleQuoteImport) {
  auto imports = ExtractImports("@import 'theme.css';\nbody { }");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "theme.css");
}

TEST(ExtractImportsTest, UrlWithSingleQuotes) {
  auto imports = ExtractImports("@import url('grid.css');\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "grid.css");
}

TEST(ExtractImportsTest, UrlUnquoted) {
  auto imports = ExtractImports("@import url(base.css);\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "base.css");
}

TEST(ExtractImportsTest, ImportWithMediaQuery) {
  auto imports = ExtractImports("@import url(\"print.css\") print;\nbody { }");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "print.css");
  EXPECT_EQ(imports[0].media, "print");
}

TEST(ExtractImportsTest, ImportWithComplexMedia) {
  auto imports = ExtractImports(
      "@import \"responsive.css\" screen and (max-width: 768px);\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "responsive.css");
  EXPECT_EQ(imports[0].media, "screen and (max-width: 768px)");
}

TEST(ExtractImportsTest, MultipleImports) {
  auto imports = ExtractImports(
      "@import \"reset.css\";\n"
      "@import url(\"theme.css\");\n"
      "@import \"print.css\" print;\n"
      "body { }");
  ASSERT_EQ(imports.size(), 3u);
  EXPECT_EQ(imports[0].url, "reset.css");
  EXPECT_EQ(imports[1].url, "theme.css");
  EXPECT_EQ(imports[2].url, "print.css");
  EXPECT_EQ(imports[2].media, "print");
}

TEST(ExtractImportsTest, CommentsBeforeImport) {
  auto imports = ExtractImports(
      "/* CSS Reset */\n"
      "@import \"reset.css\";\n"
      "body { }");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "reset.css");
}

TEST(ExtractImportsTest, CharsetThenImport) {
  auto imports = ExtractImports(
      "@charset \"UTF-8\";\n"
      "@import \"style.css\";\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "style.css");
}

TEST(ExtractImportsTest, CaseInsensitiveAtKeywords) {
  // Browsers match at-keywords ASCII-case-insensitively.
  auto imports = ExtractImports(
      "@LAYER reset;\n@IMPORT \"a.css\";\n@Import url(\"b.css\");\nbody {}");
  ASSERT_EQ(imports.size(), 2u);
  EXPECT_EQ(imports[0].url, "a.css");
  EXPECT_EQ(imports[1].url, "b.css");
}

TEST(ExtractImportsTest, CharsetIsByteExact) {
  // @charset must be the exact lowercase byte sequence per spec; a case
  // variant is not a charset rule and ends the prelude.
  auto imports = ExtractImports("@CHARSET \"UTF-8\";\n@import \"a.css\";\n");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, StopsAtNonImportRule) {
  auto imports = ExtractImports(
      "@import \"a.css\";\n"
      ".foo { color: red; }\n"
      "@import \"b.css\";\n");
  // Only the first import should be extracted.
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "a.css");
}

// ========== ResolveUrlsInCss Tests ==========

TEST(ResolveUrlsTest, SameDirectory) {
  std::string result = ResolveUrlsInCss("body { background: url(bg.png); }",
                                        "/css/theme.css", "/css/style.css");
  EXPECT_EQ(result, "body { background: url(bg.png); }");
}

TEST(ResolveUrlsTest, SubdirectoryToParent) {
  std::string result =
      ResolveUrlsInCss("body { background: url(images/bg.png); }",
                       "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("theme/images/bg.png"), std::string::npos);
}

TEST(ResolveUrlsTest, AbsoluteUrlUnchanged) {
  std::string result =
      ResolveUrlsInCss("body { background: url(/images/bg.png); }",
                       "/css/theme.css", "/css/style.css");
  EXPECT_NE(result.find("/images/bg.png"), std::string::npos);
}

TEST(ResolveUrlsTest, DataUrlUnchanged) {
  std::string input = "body { background: url(data:image/png;base64,abc); }";
  std::string result =
      ResolveUrlsInCss(input, "/css/theme.css", "/other/style.css");
  EXPECT_NE(result.find("data:image/png;base64,abc"), std::string::npos);
}

TEST(ResolveUrlsTest, ParentDirectoryTraversal) {
  std::string result =
      ResolveUrlsInCss("body { background: url(../images/bg.png); }",
                       "/css/themes/dark.css", "/css/main.css");
  EXPECT_NE(result.find("images/bg.png"), std::string::npos);
}

// ========== FlattenImports Tests ==========

TEST(FlattenImportsTest, NoImports) {
  auto result = FlattenImports("body { color: red; }", "/style.css",
                               [](std::string_view) { return std::nullopt; });
  EXPECT_EQ(result.css, "body { color: red; }");
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.imports_unresolved, 0);
}

TEST(FlattenImportsTest, SingleImportResolved) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/reset.css") return "* { margin: 0; padding: 0; }";
    return std::nullopt;
  };

  auto result = FlattenImports("@import \"reset.css\";\nbody { color: red; }",
                               "/style.css", lookup);

  EXPECT_NE(result.css.find("* { margin: 0; padding: 0; }"), std::string::npos);
  EXPECT_NE(result.css.find("body { color: red; }"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
  EXPECT_EQ(result.imports_unresolved, 0);
}

TEST(FlattenImportsTest, ImportNotInCache) {
  auto result =
      FlattenImports("@import \"missing.css\";\nbody { }", "/style.css",
                     [](std::string_view) { return std::nullopt; });

  EXPECT_NE(result.css.find("@import \"missing.css\""), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.imports_unresolved, 1);
  EXPECT_TRUE(result.skipped_unresolved_import);
}

TEST(FlattenImportsTest, MediaQueryWrapped) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/print.css") return ".header { display: none; }";
    return std::nullopt;
  };

  auto result = FlattenImports("@import \"print.css\" print;\nbody { }",
                               "/style.css", lookup);

  EXPECT_NE(result.css.find("@media print"), std::string::npos);
  EXPECT_NE(result.css.find(".header { display: none; }"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
}

TEST(FlattenImportsTest, CircularImportDetected) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") {
      return "@import \"b.css\";\n.a { color: red; }";
    }
    if (url == "/b.css") {
      // b.css tries to import the parent (style.css).
      return "@import \"style.css\";\n.b { color: blue; }";
    }
    return std::nullopt;
  };

  auto result =
      FlattenImports("@import \"a.css\";\n.main { }", "/style.css", lookup);

  // a.css and b.css are flattened; the circular back-edge to style.css
  // is DROPPED and counted resolved (browsers likewise ignore cyclic
  // imports, and the ancestor's content is the flattened output itself).
  EXPECT_NE(result.css.find(".a { color: red; }"), std::string::npos);
  EXPECT_NE(result.css.find(".b { color: blue; }"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_GE(result.imports_resolved, 1);
}

TEST(FlattenImportsTest, DepthLimitServesOriginal) {
  // Create a chain: each file imports the next.  With max_depth=2 the
  // chain cannot be fully inlined; a partial inline would leave d3's
  // @import mid-sheet (dead to browsers), so the sheet passes through
  // unchanged.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/d1.css") return "@import \"d2.css\";\n.d1 {}";
    if (url == "/d2.css") return "@import \"d3.css\";\n.d2 {}";
    if (url == "/d3.css") return "@import \"d4.css\";\n.d3 {}";
    if (url == "/d4.css") return "@import \"d5.css\";\n.d4 {}";
    if (url == "/d5.css") return "@import \"d6.css\";\n.d5 {}";
    if (url == "/d6.css") return ".d6 {}";
    return std::nullopt;
  };

  std::string css = "@import \"d1.css\";\n.root {}";
  auto result = FlattenImports(css, "/style.css", lookup, 2);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);

  // With enough depth the same chain flattens fully.
  auto deep = FlattenImports(css, "/style.css", lookup, 6);
  EXPECT_FALSE(deep.skipped_unresolved_import);
  EXPECT_EQ(deep.css.find("@import"), std::string::npos);
  EXPECT_NE(deep.css.find(".d6"), std::string::npos);
  EXPECT_EQ(deep.imports_resolved, 6);
}

TEST(FlattenImportsTest, NestedImportFlattened) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") {
      return "@import \"vars.css\";\nbody { font: sans-serif; }";
    }
    if (url == "/vars.css") {
      return ":root { --color: blue; }";
    }
    return std::nullopt;
  };

  auto result =
      FlattenImports("@import \"base.css\";\n.app { color: var(--color); }",
                     "/style.css", lookup);

  EXPECT_NE(result.css.find(":root { --color: blue; }"), std::string::npos);
  EXPECT_NE(result.css.find("body { font: sans-serif; }"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

// ========== Coverage: @layer, escapes, whitespace, edge cases ==========

TEST(ExtractImportsTest, LayerStatementBeforeImport) {
  // @layer statement (no block) should be skipped, import still extracted.
  auto imports = ExtractImports("@layer reset;\n@import \"a.css\";\nbody {}");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "a.css");
}

TEST(ExtractImportsTest, LayerBlockEndsImportPrelude) {
  // An @layer BLOCK ends the import prelude (CSS Cascading L5):
  // browsers ignore any @import after it, so it must not be harvested.
  auto imports = ExtractImports(
      "@layer reset { * { margin: 0; } }\n@import \"a.css\";\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, LayerNoTerminator) {
  // @layer with no semicolon or brace → parsing stops, no imports.
  auto imports = ExtractImports("@layer reset\n@import \"a.css\";");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, EscapedCharInQuotedUrl) {
  // Escaped character in a double-quoted import URL.  "\." denotes '.'
  // (css-syntax-3 §4.3.7), so the URL is path/file.css — decoding it
  // here is what lets the writers re-escape exactly once (#994).
  auto imports = ExtractImports("@import \"path/file\\.css\";\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "path/file.css");
}

TEST(ExtractImportsTest, UrlWithInternalWhitespace) {
  // Whitespace inside url() before and after the quoted string.
  auto imports = ExtractImports("@import url(  \"style.css\"  );\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "style.css");
}

TEST(ExtractImportsTest, InvalidImportSyntax) {
  // @import followed by something that's neither url() nor a string.
  auto imports = ExtractImports("@import 123;\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, MediaQueryTrailingWhitespace) {
  // Trailing whitespace in media query should be trimmed.
  auto imports = ExtractImports("@import \"print.css\" print  ;\n");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].media, "print");
}

TEST(ExtractImportsTest, UnterminatedComment) {
  // Unterminated comment → parsing stops at end, no imports found.
  auto imports = ExtractImports("/* unterminated comment\n@import \"a.css\";");
  EXPECT_TRUE(imports.empty());
}

// ========== ResolveUrlsInCss: string skipping ==========

TEST(ResolveUrlsTest, StringSkipping) {
  // Quoted strings in CSS should be skipped (no false url() match).
  std::string result = ResolveUrlsInCss(
      "h1 { content: \"not a url(test.png)\"; background: url(bg.png); }",
      "/css/theme/dark.css", "/css/style.css");
  // The string content should be preserved as-is.
  EXPECT_NE(result.find("\"not a url(test.png)\""), std::string::npos);
  // The actual url() should be resolved.
  EXPECT_NE(result.find("theme/bg.png"), std::string::npos);
}

TEST(ResolveUrlsTest, StringWithEscape) {
  // Escaped character inside a quoted string should not break string scanning.
  std::string result = ResolveUrlsInCss(
      R"(h1 { content: "has \"escape"; background: url(bg.png); })",
      "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("\\\"escape"), std::string::npos);
  EXPECT_NE(result.find("theme/bg.png"), std::string::npos);
}

TEST(ResolveUrlsTest, SingleQuotedString) {
  // Single-quoted string should also be properly skipped.
  std::string result = ResolveUrlsInCss(
      "h1 { content: 'url(fake.png)'; background: url(real.png); }",
      "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("'url(fake.png)'"), std::string::npos);
  EXPECT_NE(result.find("theme/real.png"), std::string::npos);
}

// ========== Coverage: ExtractUrlFunction fallthrough (line 74) ==========

TEST(ExtractImportsTest, ImportWithNonUrlUPrefix) {
  // @import followed by a word starting with 'u' but not "url(" should
  // cause ExtractUrlFunction to return nullopt (line 74), breaking parse.
  auto imports = ExtractImports("@import universal;\nbody {}");
  EXPECT_TRUE(imports.empty());
}

// ========== Coverage: @layer with no semi and no brace (line 242) ==========

TEST(ExtractImportsTest, LayerNoSemicolonOrBraceAtEndOfInput) {
  // @layer followed by text with neither ';' nor '{' anywhere in the
  // remaining input. This hits the else branch at line 242 (break).
  auto imports = ExtractImports("@layer reset");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, LayerNoSemicolonOrBraceBeforeImport) {
  // @layer with no terminator followed by more content, but still
  // no ';' or '{' in the entire remaining input after "@layer".
  // This should also hit the break at line 242.
  auto imports = ExtractImports("@layer base reset");
  EXPECT_TRUE(imports.empty());
}

// ========== Diamond Import Deduplication ==========

TEST(FlattenImportsTest, DiamondImportDeduplication) {
  // Diamond pattern: style.css imports b.css and c.css, both of which
  // import d.css. D's content should appear exactly once (deduplicated).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/b.css") return "@import \"d.css\";\n.b { color: blue; }";
    if (url == "/c.css") return "@import \"d.css\";\n.c { color: green; }";
    if (url == "/d.css") return ".d { color: red; }";
    return std::nullopt;
  };

  auto result =
      FlattenImports("@import \"b.css\";\n@import \"c.css\";\n.root { }",
                     "/style.css", lookup);

  // D's content should appear exactly once.
  EXPECT_NE(result.css.find(".d { color: red; }"), std::string::npos);
  // Verify it appears only once (second find from after the first match
  // should fail).
  size_t first = result.css.find(".d { color: red; }");
  EXPECT_EQ(result.css.find(".d { color: red; }", first + 1),
            std::string::npos);

  // B and C content should both be present.
  EXPECT_NE(result.css.find(".b { color: blue; }"), std::string::npos);
  EXPECT_NE(result.css.find(".c { color: green; }"), std::string::npos);

  // All imports resolved: b.css, c.css, d.css (imported from b.css),
  // and d.css (deduplicated when imported from c.css).
  EXPECT_GE(result.imports_resolved, 3);

  // No @import statements should remain.
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
}

TEST(FlattenImportsTest, OutputSizeLimitServesOriginal) {
  // Each imported file is ~200KB. With 6 imports, total would be ~1.2MB.
  // Stopping at the 1MB cap partway would leave the remaining @imports
  // mid-sheet (dead to browsers), so the sheet passes through unchanged.
  std::string big_chunk(200ULL * 1024, 'x');  // 200KB of 'x'.

  auto lookup =
      [&big_chunk](std::string_view url) -> std::optional<std::string> {
    // All import URLs resolve to large content.
    if (url == "/a.css" || url == "/b.css" || url == "/c.css" ||
        url == "/d.css" || url == "/e.css" || url == "/f.css") {
      return ".chunk { content: \"" + big_chunk + "\"; }";
    }
    return std::nullopt;
  };

  std::string css =
      "@import \"a.css\";\n"
      "@import \"b.css\";\n"
      "@import \"c.css\";\n"
      "@import \"d.css\";\n"
      "@import \"e.css\";\n"
      "@import \"f.css\";\n"
      ".root { }";
  auto result = FlattenImports(css, "/style.css", lookup, 5);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

// ========== @layer block ends the prelude, regardless of content ==========

TEST(ExtractImportsTest, LayerBlockWithStringContainingBraces) {
  auto imports = ExtractImports(
      "@layer reset { .x { content: \"}\"; } }\n"
      "@import \"a.css\";\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, LayerBlockWithNestedBraces) {
  auto imports = ExtractImports(
      "@layer components {\n"
      "  @media screen {\n"
      "    .btn { color: red; }\n"
      "  }\n"
      "}\n"
      "@import \"d.css\";\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(FlattenImportsTest, ImportAfterLayerBlockNotInlined) {
  // Browsers ignore an @import after an @layer block; inlining it would
  // apply styles the original page never did (inverse of the mid-sheet
  // drop).  The sheet passes through byte-identical.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/b.css") return ".b { color: red; }";
    return std::nullopt;
  };
  std::string css =
      "@layer a { .x { color: blue; } }\n@import \"b.css\";\n.y { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find(".b {"), std::string::npos);
}

// ========== Parenthesized (safe) media values keep flattening ==========

TEST(FlattenImportsTest, ParenthesizedMediaValueStillWrapped) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/safe.css") return ".safe { color: green; }";
    return std::nullopt;
  };
  auto result = FlattenImports(
      "@import \"safe.css\" screen and (max-width: 768px);\nbody { }",
      "/style.css", lookup);
  EXPECT_NE(result.css.find("@media screen and (max-width: 768px)"),
            std::string::npos);
  EXPECT_NE(result.css.find(".safe"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
}

// ========== Non-media @import conditions: layer()/supports() ==========
// Issue #655: these are not media queries — wrapping them as
// "@media <condition> {...}" evaluates to "not all" and silently drops
// the stylesheet.  Flattening must be skipped for the whole sheet.

TEST(FlattenImportsTest, LayerFunctionConditionSkipsFlattening) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") return ".base { color: green; }";
    return std::nullopt;
  };
  std::string css = "@import url(base.css) layer(base);\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css) << "Sheet must pass through unchanged";
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find("@media"), std::string::npos);
}

TEST(FlattenImportsTest, BareLayerConditionSkipsFlattening) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/anon.css") return ".anon { color: red; }";
    return std::nullopt;
  };
  std::string css = "@import \"anon.css\" layer;\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, CommentBeforeLayerConditionSkipsFlattening) {
  // A comment between the URL and the condition must not mask the
  // layer token (the condition capture skips comments).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") return ".base { color: green; }";
    return std::nullopt;
  };
  std::string css = "@import url(base.css) /*c*/ layer(base);\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, CommentAfterLayerKeywordSkipsFlattening) {
  // "layer/*c*/screen": the comment is a token boundary, so this is a
  // layered import with media "screen" — still a non-media condition.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") return ".base { color: green; }";
    return std::nullopt;
  };
  std::string css = "@import url(base.css) layer/*c*/screen;\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, SupportsConditionSkipsFlattening) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/grid.css") return ".grid { display: grid; }";
    return std::nullopt;
  };
  std::string css = "@import url(grid.css) supports(display:grid);\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find("@media"), std::string::npos);
}

TEST(FlattenImportsTest, MixedMediaAndLayerSkipsWholeSheet) {
  // One media import + one layer import: flattening only the media import
  // would leave the passed-through @import after non-import rules (invalid
  // CSS).  The whole sheet passes through unchanged.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/print.css") return ".print { display: none; }";
    if (url == "/base.css") return ".base { color: green; }";
    return std::nullopt;
  };
  std::string css =
      "@import \"print.css\" print;\n"
      "@import url(base.css) layer(base);\n"
      "body { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css) << "No content may be silently dropped";
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find("@media"), std::string::npos);
}

TEST(FlattenImportsTest, NestedLayerConditionSkipsWholeSheet) {
  // The layer import is one level down: inlining the parent would embed
  // an @import mid-sheet.  The skip must propagate to the top level.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") {
      return "@import url(theme.css) layer(theme);\n.base { color: green; }";
    }
    if (url == "/theme.css") return ".theme { color: blue; }";
    return std::nullopt;
  };
  std::string css = "@import \"base.css\";\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_non_media_condition);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

// ========== All-or-nothing flattening on unresolved imports ==========
// A kept-verbatim @import lands mid-sheet in the flattened output, where
// browsers must ignore it (CSS Cascading L4: @import only after @charset
// and @layer statements) — its styles silently vanish even though they
// loaded fine before flattening.  Any unresolvable import must make the
// whole sheet pass through unchanged.

TEST(FlattenImportsTest, MixedResolvedAndMissingServesOriginal) {
  // Regression: resolved first import + missing second import used to
  // inline the first and keep the second's @import AFTER the inlined
  // rules — dead to browsers, silently dropping missing.css's styles.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    return std::nullopt;
  };
  std::string css = "@import \"a.css\";\n@import \"missing.css\";\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css) << "Both imports must stay in valid position";
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_GE(result.imports_unresolved, 1);
}

TEST(FlattenImportsTest, NestedMissingImportServesOriginal) {
  // Regression: a cached child whose own @import is uncached used to be
  // inlined with that @import embedded mid-parent — dead to browsers.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") {
      return "@import \"missing.css\";\n.base { color: green; }";
    }
    return std::nullopt;
  };
  std::string css = "@import \"base.css\";\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, UnsafeMediaValueServesOriginal) {
  // An @import whose media value fails the injection filter can neither
  // be re-emitted as a @media wrapper nor survive mid-sheet.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/evil.css") return ".evil { color: red; }";
    if (url == "/a.css") return ".a { color: blue; }";
    return std::nullopt;
  };
  std::string css =
      "@import \"a.css\";\n"
      "@import \"evil.css\" screen{}*{background:red}etc;\n"
      "body { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find("@media"), std::string::npos);
}

TEST(FlattenImportsTest, UnparseableImportAfterResolvedServesOriginal) {
  // ExtractImports stops on an @import form it cannot parse; the
  // residual @import would sit after the inlined rules.  Serve original.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    return std::nullopt;
  };
  std::string css = "@import \"a.css\";\n@import universal;\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildWithUnparseableImportServesOriginal) {
  // The child's only import is one ExtractImports cannot parse (its
  // import list is empty) — inlining the child would still embed that
  // @import mid-parent.  The skip must propagate from the child.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/child.css") return "@import universal;\n.child { }";
    return std::nullopt;
  };
  std::string css = "@import \"child.css\";\nbody { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, CaseVariantImportFlattensNotStranded) {
  // Regression: exact-case scanning stopped at @IMPORT, inlined the
  // first import, and stranded the @IMPORT mid-sheet (styles dropped).
  // With case-insensitive scanning both flatten; assert no @import form
  // survives mid-sheet.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    if (url == "/b.css") return ".b { color: blue; }";
    return std::nullopt;
  };
  auto result = FlattenImports("@import \"a.css\";\n@IMPORT \"b.css\";\n.x { }",
                               "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find(".a {"), std::string::npos);
  EXPECT_NE(result.css.find(".b {"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.css.find("@IMPORT"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

TEST(FlattenImportsTest, ChildCaseVariantImportFlattensNotStranded) {
  // Regression: a child's @IMPORT was invisible to the exact-case
  // scanner and got inlined verbatim mid-parent (styles dropped).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/base.css") {
      return "@IMPORT url(x.css);\n.base { color: green; }";
    }
    if (url == "/x.css") return ".x { color: red; }";
    return std::nullopt;
  };
  auto result =
      FlattenImports("@import \"base.css\";\nbody { }", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find(".base {"), std::string::npos);
  EXPECT_NE(result.css.find(".x {"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.css.find("@IMPORT"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

TEST(FlattenImportsTest, ChildWithUnterminatedCommentServesOriginal) {
  // A child ending inside an open /* comment would swallow every parent
  // rule appended after it.  Serve the original.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".c { }\n/* unterminated";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\";\n.parent { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css) << "Parent tail must survive";
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_GE(result.imports_unresolved, 1);
}

TEST(FlattenImportsTest, OnlyUnparseableImportFlagsWithCounter) {
  // Contract: skip flag implies imports_unresolved >= 1 (visible via
  // the C API's out_unresolved), including on the residual-guard path
  // where ExtractImports found nothing.
  std::string css = "@import universal;\nbody { }";
  auto result = FlattenImports(css, "/style.css",
                               [](std::string_view) { return std::nullopt; });

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_GE(result.imports_unresolved, 1);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css, css);
}

TEST(FlattenImportsTest, NestedUrlFormImportInSubdirectoryFlattens) {
  // Regression for the rebase-then-recurse double rebase: base.css lives
  // in /css/ and imports url(x.css).  The old order rebased the import
  // to "css/x.css" (parent context) and then resolved it against
  // /css/ — yielding /css/css/x.css, a spurious cache miss.  The chain
  // must fully flatten, with x.css's url() refs rebased for /style.css.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/css/base.css") {
      return "@import url(x.css);\n.base { color: green; }";
    }
    if (url == "/css/x.css") return ".x { background: url(img.png); }";
    return std::nullopt;
  };
  auto result = FlattenImports("@import \"css/base.css\";\nbody { }",
                               "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_NE(result.css.find(".base"), std::string::npos);
  EXPECT_NE(result.css.find(".x"), std::string::npos);
  // x.css's relative image URL must be correct in /style.css's context.
  EXPECT_NE(result.css.find("url(\"css/img.png\")"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

// ========== String-form @import URL rebasing ==========

TEST(ResolveUrlsTest, StringFormImportRebased) {
  std::string result =
      ResolveUrlsInCss("@import \"sub/x.css\";\n.t { }", "/css/theme/dark.css",
                       "/css/style.css");
  EXPECT_NE(result.find("@import \"theme/sub/x.css\""), std::string::npos);
}

TEST(ResolveUrlsTest, StringFormImportSingleQuoteRebased) {
  std::string result =
      ResolveUrlsInCss("@import 'x.css' print;\n.t { }", "/css/theme/dark.css",
                       "/css/style.css");
  // Quote style and media query survive; only the URL is rebased.
  EXPECT_NE(result.find("@import 'theme/x.css' print;"), std::string::npos);
}

TEST(ResolveUrlsTest, StringFormImportCaseVariantRebased) {
  std::string result = ResolveUrlsInCss(
      "@IMPORT \"x.css\";\n.t { }", "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("@IMPORT \"theme/x.css\""), std::string::npos);
}

TEST(ResolveUrlsTest, StringFormImportAbsoluteUnchanged) {
  std::string css =
      "@import \"/abs/x.css\";\n@import \"https://cdn.example.com/y.css\";\n";
  std::string result =
      ResolveUrlsInCss(css, "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("@import \"/abs/x.css\""), std::string::npos);
  EXPECT_NE(result.find("@import \"https://cdn.example.com/y.css\""),
            std::string::npos);
}

TEST(ResolveUrlsTest, UrlFormImportRebased) {
  // The url() form is handled by the general url( branch.
  std::string result = ResolveUrlsInCss(
      "@import url(x.css);\n.t { }", "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("@import url(\"theme/x.css\")"), std::string::npos);
}

TEST(ResolveUrlsTest, ImportTokenInsideStringNotRewritten) {
  std::string css =
      "h1 { content: \"@import \\\"fake.css\\\"\"; background: url(bg.png); }";
  std::string result =
      ResolveUrlsInCss(css, "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find("@import \\\"fake.css\\\""), std::string::npos);
  EXPECT_NE(result.find("theme/bg.png"), std::string::npos);
}

TEST(FlattenImportsTest, MediaConditionStillFlattened) {
  // Plain media-query conditions keep flattening (no regression).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/screen.css") return ".screen { color: green; }";
    return std::nullopt;
  };
  auto result = FlattenImports("@import \"screen.css\" screen;\nbody { }",
                               "/style.css", lookup);

  EXPECT_FALSE(result.skipped_non_media_condition);
  EXPECT_EQ(result.imports_resolved, 1);
  EXPECT_NE(result.css.find("@media screen"), std::string::npos);
  EXPECT_NE(result.css.find(".screen"), std::string::npos);
}

// ========== Comment-aware statement scanning ==========
// A ';' or '{' inside a /*...*/ comment is not a statement terminator
// or a block opener; comment-blind scanning let a comment truncate the
// media value (emitting an open comment that swallows the @media
// wrapper) or misclassify an @layer statement as a block.

TEST(ExtractImportsTest, CommentInMediaIsTokenBoundary) {
  // The ';' inside the comment must not terminate the statement, and
  // the comment (a token boundary) is stripped to a space in the media
  // value.
  auto imports = ExtractImports("@import 'a.css' screen /*;*/;\n.x{}");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "a.css");
  EXPECT_EQ(imports[0].media, "screen");
}

TEST(FlattenImportsTest, CommentInMediaFlattensWithCommentStripped) {
  // "screen /*;*/" as a media condition: comment-blind scanning took
  // the ';' inside the comment as the terminator, emitting
  // "@media screen /* {...}" — an open comment swallowing the wrapper
  // and the child's styles.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a{color:blue}";
    return std::nullopt;
  };
  auto result = FlattenImports("@import 'a.css' screen /*;*/;\n.x{color:red}",
                               "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find("@media screen {"), std::string::npos);
  EXPECT_NE(result.css.find(".a{color:blue}"), std::string::npos);
  EXPECT_NE(result.css.find(".x{color:red}"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.css.find("/*"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
}

TEST(FlattenImportsTest, UnterminatedCommentInImportServesOriginal) {
  // The media scan never finds a terminator inside an open comment; the
  // unparseable @import makes the whole sheet pass through.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a{color:blue}";
    return std::nullopt;
  };
  std::string css = "@import 'a.css' screen /*;\n.x{color:red}";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(ExtractImportsTest, LayerStatementBraceInCommentIsStatement) {
  // "@layer x /*{*/;" is a layer STATEMENT: the '{' inside the comment
  // must not classify it as a block (which would end the prelude and
  // strand the second import).
  auto imports = ExtractImports(
      "@import \"a.css\";\n@layer x /*{*/;\n@import \"b.css\";\nbody {}");
  ASSERT_EQ(imports.size(), 2u);
  EXPECT_EQ(imports[0].url, "a.css");
  EXPECT_EQ(imports[1].url, "b.css");
}

TEST(FlattenImportsTest, LayerStatementBraceInCommentKeepsFlattening) {
  // Misclassifying the @layer statement as a block harvested only the
  // first import and left a live "@import \"b.css\"" after inlined
  // rules — dead to browsers, b's styles lost.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    if (url == "/b.css") return ".b { color: blue; }";
    return std::nullopt;
  };
  auto result = FlattenImports(
      "@import \"a.css\";\n@layer x /*{*/;\n@import \"b.css\";\n.x { }",
      "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find(".a {"), std::string::npos);
  EXPECT_NE(result.css.find(".b {"), std::string::npos);
  EXPECT_NE(result.css.find("@layer x /*{*/;"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

// ========== Structurally unbalanced children serve the original ==========
// A child whose braces or strings do not close cleanly changes how the
// parent parses once embedded: an unclosed block swallows the @media
// wrapper's closing '}' (media-gating the rest of the parent), a stray
// '}' closes the wrapper early so trailing rules escape their media
// condition.

TEST(FlattenImportsTest, ChildWithUnclosedBraceServesOriginal) {
  // Browsers close ".p{color:red" implicitly at EOF of the child sheet;
  // embedded under "@media print {...}" the wrapper's '}' would close
  // .p instead and print-gate the parent tail.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".p{color:red";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildEndingInABackslashServesOriginal) {
  // A trailing backslash would escape the wrapper's closing brace.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".p{color:red}\\";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildWithNewlineInStringServesOriginal) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return "a{b:\"x\n}}c{display:none}d{e:\"}";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildWithEscapedUrlNameServesOriginal) {
  for (const char* body :
       {"a{b:\\url({)}}c{display:none}", "a{b:\\75rl({)}}c{display:none}"}) {
    auto lookup = [body](std::string_view url) -> std::optional<std::string> {
      if (url == "/c.css") return std::string(body);
      return std::nullopt;
    };
    std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
    auto result = FlattenImports(css, "/style.css", lookup);

    EXPECT_TRUE(result.skipped_unresolved_import) << body;
    EXPECT_EQ(result.css, css) << body;
    EXPECT_EQ(result.imports_resolved, 0) << body;
  }
}

TEST(FlattenImportsTest, ChildWithEscapedNewlineInStringIsInlined) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return "a{b:\"a\\\nb\"}";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_EQ(result.imports_resolved, 1);
}

TEST(FlattenImportsTest, ChildWithStrayCloseBraceServesOriginal) {
  // Browsers drop the stray '}' (and .q) in the standalone child;
  // embedded under "@media print {...}" it would close the wrapper
  // early and .q would escape to ALL media.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".p{}}.q{color:red}";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildWithUnterminatedStringServesOriginal) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".p{content:\"abc";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\";\n.q { color: blue; }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildWithEscapedBraceStillFlattens) {
  // "\{" is an escaped ident code point, not a block opener; the child
  // is balanced and must keep flattening.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".a\\{b { color: red; }";
    return std::nullopt;
  };
  auto result =
      FlattenImports("@import \"c.css\";\n.q { }", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find(".a\\{b { color: red; }"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
}

// ========== Same URL under different media conditions ==========
// Browsers apply a sheet imported twice under different media in both
// conditions; URL-only dedup silently dropped the second one.  Dedup is
// keyed on (URL, effective media condition).

TEST(FlattenImportsTest, SameUrlDifferentMediaInlinedTwice) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    return std::nullopt;
  };
  auto result = FlattenImports(
      "@import \"a.css\" screen;\n@import \"a.css\" print;\nbody { }",
      "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find("@media screen"), std::string::npos);
  EXPECT_NE(result.css.find("@media print"), std::string::npos);
  size_t first = result.css.find(".a { color: red; }");
  ASSERT_NE(first, std::string::npos);
  EXPECT_NE(result.css.find(".a { color: red; }", first + 1),
            std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

TEST(FlattenImportsTest, SameUrlSameMediaDeduplicated) {
  // An identical (URL, media) repeat stays a duplicate: applied once by
  // browsers per condition, dropped from the output.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    return std::nullopt;
  };
  auto result = FlattenImports(
      "@import \"a.css\" print;\n@import \"a.css\" print;\nbody { }",
      "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  size_t first = result.css.find(".a { color: red; }");
  ASSERT_NE(first, std::string::npos);
  EXPECT_EQ(result.css.find(".a { color: red; }", first + 1),
            std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

// ========== At-keyword boundaries ==========
// "@importurl" and "@layerfoo" are unknown at-keywords browsers ignore
// entirely; prefix-matching them as @import/@layer inlined styles the
// original page never loaded.

TEST(ExtractImportsTest, ImportPrefixKeywordNotAnImport) {
  auto imports = ExtractImports("@importurl(\"a.css\");\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, LayerPrefixKeywordEndsPrelude) {
  auto imports = ExtractImports("@layerfoo;\n@import \"a.css\";\nbody {}");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, CharsetPrefixKeywordEndsPrelude) {
  auto imports = ExtractImports("@charsetx \"UTF-8\";\n@import \"a.css\";\n");
  EXPECT_TRUE(imports.empty());
}

TEST(ExtractImportsTest, ImportNoSpaceBeforeQuote) {
  // "@import\"a.css\"" is valid CSS: the quote is a keyword boundary.
  auto imports = ExtractImports("@import\"a.css\";\nbody {}");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "a.css");
}

TEST(FlattenImportsTest, UnknownAtKeywordWithImportPrefixLeftAlone) {
  // Cached or not, "@importurl(...)" must never be inlined — the sheet
  // passes through with no skip flag (there is nothing to flatten).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a { color: red; }";
    return std::nullopt;
  };
  std::string css = "@importurl(\"a.css\");\n.x { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
  EXPECT_EQ(result.css.find(".a {"), std::string::npos);
}

// ========== Consensus-round hardenings (probe regressions) ==========
// All one-sided: each case may only convert flatten → serve-original,
// never the reverse.

TEST(FlattenImportsTest, SemicolonInsideParenMediaServesOriginal) {
  // P1b: the statement scan is not component-value-aware, so it splits
  // at the ';' inside the parens, leaving the unbalanced fragment "(m"
  // as the media value.  Unbalanced parens are rejected, so the sheet
  // passes through instead of emitting a broken "@media (m {" wrapper
  // plus the live remnant "n);".
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a{color:blue}";
    return std::nullopt;
  };
  std::string css = "@import \"a.css\" (m;n);.x{color:red}";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);

  // P1: same mis-split with the ';' inside an unquoted url() token.
  std::string url_css = "@import \"a.css\" url(m;n);.x{color:red}";
  auto url_result = FlattenImports(url_css, "/style.css", lookup);
  EXPECT_TRUE(url_result.skipped_unresolved_import);
  EXPECT_EQ(url_result.css, url_css);
}

TEST(FlattenImportsTest, StrayBraceHiddenByUrlCommentServesOriginal) {
  // P2: "/*" inside an unquoted url() token is content, not a comment
  // opener — the child really contains a stray '}' that would close a
  // @media wrapper early and leak .b to all media.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return ".a{background:url(/*)} */ } .b{color:blue}";
    return std::nullopt;
  };
  std::string css = "@import \"c.css\" print;.x{color:red}";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, ChildEscapedBraceLayerStatementFlattens) {
  // P3b: "@layer x\{y;" is a layer STATEMENT — the escaped '{' is an
  // ident code point, not a block opener — so the child's @import
  // behind it is live and flattens at position (misclassifying block
  // form inlined the child verbatim with a dead mid-sheet @import).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css")
      return "@layer x\\{y;@import \"b.css\";.c{color:green}";
    if (url == "/b.css") return ".b{color:blue}";
    return std::nullopt;
  };
  auto result = FlattenImports("@import \"c.css\" print;.x{color:red}",
                               "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find("@media print"), std::string::npos);
  EXPECT_NE(result.css.find("@layer x\\{y;"), std::string::npos);
  EXPECT_NE(result.css.find(".b{color:blue}"), std::string::npos);
  EXPECT_NE(result.css.find(".c{color:green}"), std::string::npos);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 2);
}

TEST(FlattenImportsTest, ChildUnknownAtStatementThenImportServesOriginal) {
  // P4b: browsers drop "@layerfoo;" as if absent, so the child's
  // @import behind it is live.  ExtractImports cannot flatten through
  // an unknown at-rule, and inlining the child verbatim would strand
  // the live @import mid-parent.  Serve the original.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a.css") return ".a{color:red}";
    if (url == "/c.css") return "@layerfoo;@import \"b.css\";.c{}";
    if (url == "/b.css") return ".b{color:blue}";
    return std::nullopt;
  };
  std::string css = "@import \"a.css\";@import \"c.css\";.x{}";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_TRUE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css, css);
  EXPECT_EQ(result.imports_resolved, 0);
}

TEST(FlattenImportsTest, NestedMediaContextJoinsAndDedups) {
  // Nested wrappers compose the effective condition ("screen;print"
  // join key): d.css via m.css inlines as @media print nested inside
  // @media screen.  A second route with the SAME effective condition
  // (m2.css, also screen→print) deduplicates, while the parent's own
  // direct print import of d.css is a DIFFERENT effective condition
  // and inlines again.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/m.css") return "@import \"d.css\" print;\n.m { }";
    if (url == "/m2.css") return "@import \"d.css\" print;\n.m2 { }";
    if (url == "/d.css") return ".d { color: red; }";
    return std::nullopt;
  };
  std::string css =
      "@import \"m.css\" screen;\n"
      "@import \"m2.css\" screen;\n"
      "@import \"d.css\" print;\n"
      "body { }";
  auto result = FlattenImports(css, "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_EQ(result.css.find("@import"), std::string::npos);
  // Nested wrapping: the screen wrapper opens before the print-wrapped
  // d content inside it.
  size_t screen_pos = result.css.find("@media screen {");
  ASSERT_NE(screen_pos, std::string::npos);
  size_t nested_d = result.css.find("@media print {\n.d { color: red; }");
  ASSERT_NE(nested_d, std::string::npos);
  EXPECT_LT(screen_pos, nested_d);
  // d appears exactly twice: once nested under screen (the m2 route is
  // deduplicated) and once under the direct print wrapper.
  size_t first = result.css.find(".d { color: red; }");
  ASSERT_NE(first, std::string::npos);
  size_t second = result.css.find(".d { color: red; }", first + 1);
  ASSERT_NE(second, std::string::npos);
  EXPECT_EQ(result.css.find(".d { color: red; }", second + 1),
            std::string::npos);
  EXPECT_NE(result.css.find(".m {"), std::string::npos);
  EXPECT_NE(result.css.find(".m2 {"), std::string::npos);
  // m (+ its d), m2 (+ its deduplicated d), and the direct d.
  EXPECT_EQ(result.imports_resolved, 5);
}

TEST(FlattenImportsTest, ChildUnknownAtKeywordImportPrefixInlinedVerbatim) {
  // A child's "@importurl(...)" is ignored by browsers standalone and
  // embedded alike — it must not block flattening (it is not an
  // unparseable @import).
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/c.css") return "@importurl(\"x.css\");\n.c { color: red; }";
    return std::nullopt;
  };
  auto result =
      FlattenImports("@import \"c.css\";\n.q { }", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find("@importurl(\"x.css\");"), std::string::npos);
  EXPECT_NE(result.css.find(".c { color: red; }"), std::string::npos);
  EXPECT_EQ(result.imports_resolved, 1);
}

// ========== URL escape round-tripping across rebases (#994) ==========
// Extraction decodes escape sequences (css-syntax-3 §4.3.7) and the
// writers re-escape exactly once (§4.3.5), so a URL containing a quote,
// a backslash or a ')' survives any number of rebase levels unchanged.
// Extracting raw while re-escaping on emit added a backslash per level.
//
// A raw string literal containing \" is bound to a local before being
// used in an assertion: as a MACRO ARGUMENT such a literal is mis-scanned
// by MSVC's legacy preprocessor, which reads the \" as an escaped quote.
// As a plain function argument it is fine, which is how they appear
// elsewhere in this file.

TEST(ResolveUrlsTest, QuotedUrlWithDoubleQuoteEscapedExactlyOnce) {
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url("im\"g.png"); })",
                       "/css/theme/dark.css", "/css/style.css");
  const std::string kExpected = R"(url("theme/im\"g.png"))";
  EXPECT_NE(result.find(kExpected), std::string::npos) << result;
  // The tell-tale of double escaping.
  EXPECT_EQ(result.find(R"(\\)"), std::string::npos) << result;
}

TEST(ResolveUrlsTest, QuotedUrlWithBackslashEscapedExactlyOnce) {
  // Source "a\\b.png" is the URL a\b.png (one literal backslash); it
  // must come back out with exactly one level of escaping.
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url("a\\b.png"); })",
                       "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find(R"(url("theme/a\\b.png"))"), std::string::npos)
      << result;
  EXPECT_EQ(result.find(R"(\\\\)"), std::string::npos) << result;
}

TEST(ResolveUrlsTest, QuotedUrlHexEscapeDecodedThenReEscaped) {
  // "\22 " is a hex escape for '"' (css-syntax-3 §4.3.7): the trailing
  // space terminates the hex run and is not part of the URL.
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url("im\22 g.png"); })",
                       "/css/theme/dark.css", "/css/style.css");
  const std::string kExpected = R"(url("theme/im\"g.png"))";
  EXPECT_NE(result.find(kExpected), std::string::npos) << result;
}

TEST(ResolveUrlsTest, SingleQuotedUrlWithEscapedApostrophe) {
  // The URL is it's.png; emitted in a double-quoted string the
  // apostrophe needs no escape at all.
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url('it\'s.png'); })",
                       "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find(R"(url("theme/it's.png"))"), std::string::npos)
      << result;
}

TEST(ResolveUrlsTest, QuotedUrlWithCloseParenNeedsNoEscapeInString) {
  // ')' terminates an unquoted url token but is ordinary inside a
  // string, and the writer always emits the quoted form.
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url("a)b.png"); })",
                       "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find(R"(url("theme/a)b.png"))"), std::string::npos)
      << result;
}

TEST(ResolveUrlsTest, UnquotedUrlTokenEscapeDecoded) {
  // An escaped ')' inside an unquoted url token is content, not the
  // token terminator (css-syntax-3 §4.3.6).  Terminating on it truncated
  // the URL and spilled the remainder into the stylesheet as raw text.
  std::string result =
      ResolveUrlsInCss(R"(.x { background: url(im\)g.png); })",
                       "/css/theme/dark.css", "/css/style.css");
  EXPECT_NE(result.find(R"(url("theme/im)g.png"))"), std::string::npos)
      << result;
  EXPECT_EQ(result.find("g.png)"), std::string::npos) << result;
}

TEST(ResolveUrlsTest, StringFormImportUrlEscapedExactlyOnce) {
  std::string result = ResolveUrlsInCss(
      R"(@import "s\"b/x.css";)", "/css/theme/dark.css", "/css/style.css");
  const std::string kExpected = R"(@import "theme/s\"b/x.css";)";
  EXPECT_NE(result.find(kExpected), std::string::npos) << result;
  EXPECT_EQ(result.find(R"(\\)"), std::string::npos) << result;
}

TEST(ExtractImportsTest, HexEscapeInImportUrlDecoded) {
  // "\41 " is 'A'; the single trailing whitespace terminates the hex
  // digits and is consumed with them.
  auto imports = ExtractImports(R"(@import "a\41 b.css";)");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "aAb.css");
}

TEST(ExtractImportsTest, NullHexEscapeBecomesReplacementChar) {
  // A zero, surrogate, or out-of-range escape is U+FFFD (§4.3.7).
  auto imports = ExtractImports(R"(@import "a\0 b.css";)");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url,
            "a\xEF\xBF\xBD"
            "b.css");
}

TEST(ExtractImportsTest, StringLineContinuationDroppedFromUrl) {
  // A backslash before a newline in a string is a line continuation and
  // contributes nothing (css-syntax-3 §4.3.5).
  auto imports = ExtractImports("@import \"a\\\nb.css\";");
  ASSERT_EQ(imports.size(), 1u);
  EXPECT_EQ(imports[0].url, "ab.css");
}

TEST(FlattenImportsTest, MultiLevelRebaseKeepsQuotedUrlIntact) {
  // The #994 trigger: two rebase levels.  leaf.css is flattened in its
  // own context, rebased into mid.css's, then rebased again into the
  // root's — each level re-escaped what the previous level emitted.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a/mid.css") return R"(@import "b/leaf.css";.mid { })";
    if (url == "/a/b/leaf.css")
      return R"(.leaf { background: url("im\"g.png"); })";
    return std::nullopt;
  };
  auto result =
      FlattenImports(R"(@import "a/mid.css";body { })", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_EQ(result.imports_resolved, 2);
  const std::string kExpected = R"(url("a/b/im\"g.png"))";
  EXPECT_NE(result.css.find(kExpected), std::string::npos) << result.css;
  EXPECT_EQ(result.css.find(R"(\\)"), std::string::npos) << result.css;
}

TEST(FlattenImportsTest, MultiLevelRebaseKeepsBackslashUrlIntact) {
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a/mid.css") return R"(@import "b/leaf.css";.mid { })";
    if (url == "/a/b/leaf.css")
      return R"(.leaf { background: url("a\\b.png"); })";
    return std::nullopt;
  };
  auto result =
      FlattenImports(R"(@import "a/mid.css";body { })", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  EXPECT_NE(result.css.find(R"(url("a/b/a\\b.png"))"), std::string::npos)
      << result.css;
  // Two levels of doubling is what the defect produced.
  EXPECT_EQ(result.css.find(R"(\\\\)"), std::string::npos) << result.css;
}

TEST(FlattenImportsTest, RebaseIsIdempotentAcrossThreeLevels) {
  // Escaping must be a fixed point, not grow with depth.
  auto lookup = [](std::string_view url) -> std::optional<std::string> {
    if (url == "/a/l1.css") return R"(@import "b/l2.css";)";
    if (url == "/a/b/l2.css") return R"(@import "c/l3.css";)";
    if (url == "/a/b/c/l3.css")
      return R"(.l3 { background: url("q\"b\\s.png"); })";
    return std::nullopt;
  };
  auto result =
      FlattenImports(R"(@import "a/l1.css";body { })", "/style.css", lookup);

  EXPECT_FALSE(result.skipped_unresolved_import);
  const std::string kExpected = R"(url("a/b/c/q\"b\\s.png"))";
  EXPECT_NE(result.css.find(kExpected), std::string::npos) << result.css;
}

}  // namespace
}  // namespace pagespeed::css
