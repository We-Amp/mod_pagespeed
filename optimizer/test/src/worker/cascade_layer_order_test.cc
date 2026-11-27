// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — the page's cascade-layer order

#include "src/worker/cascade_layer_order.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

LayerOrderSource Sheet(std::string css, bool conditional = false) {
  LayerOrderSource s;
  s.available = true;
  s.css = std::move(css);
  s.base_url = "https://example.com/";
  s.conditional = conditional;
  return s;
}

LayerOrderSource Missing(std::string reason = "not gathered") {
  LayerOrderSource s;
  s.unavailable_reason = std::move(reason);
  return s;
}

// A lookup over absolute URLs; relative hrefs resolve against the origin root.
LayerOrderImportLookup Imports(std::map<std::string, std::string> sheets) {
  return [sheets = std::move(sheets)](
             std::string_view /*base*/,
             std::string_view href) -> std::optional<LayerOrderImport> {
    std::string url(href);
    if (url.rfind("https://", 0) != 0) {
      url = "https://example.com/" + (url[0] == '/' ? url.substr(1) : url);
    }
    auto it = sheets.find(url);
    if (it == sheets.end()) return std::nullopt;
    return LayerOrderImport{it->second, url};
  };
}

CascadeLayerOrder Order(std::vector<LayerOrderSource> sources,
                        const LayerOrderImportLookup& lookup = {}) {
  return ComputeCascadeLayerOrder(sources, lookup);
}

using Names = std::vector<std::string>;

TEST(CascadeLayerOrderTest, DefaultIsNotProven) {
  CascadeLayerOrder order;
  EXPECT_FALSE(order.proven);
  EXPECT_FALSE(order.reason.empty());
  EXPECT_EQ(order.Statement(), "");
}

TEST(CascadeLayerOrderTest, NoSourcesAndNoLayers) {
  CascadeLayerOrder none = Order({});
  EXPECT_TRUE(none.proven);
  EXPECT_TRUE(none.reason.empty());
  EXPECT_TRUE(none.names.empty());
  EXPECT_EQ(none.Statement(), "");
  CascadeLayerOrder flat = Order({Sheet(".a{color:red}@media (x){.b{}}")});
  EXPECT_TRUE(flat.proven);
  EXPECT_TRUE(flat.names.empty());
}

TEST(CascadeLayerOrderTest, TailwindShapedSheet) {
  CascadeLayerOrder order =
      Order({Sheet("/*! tailwindcss */@layer properties;@layer theme,base,"
                   "components,utilities;@layer properties{@supports (x:y){*{"
                   "--tw:0}}}@layer theme{:root{--c:red}}@layer utilities{"
                   ".p-4{padding:1rem}@media (width>=48rem){.md\\:p-8{}}}")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names,
            (Names{"properties", "theme", "base", "components", "utilities"}));
  EXPECT_EQ(order.Statement(),
            "@layer properties,theme,base,components,utilities;");
}

TEST(CascadeLayerOrderTest, DocumentOrderAcrossSources) {
  // Case A from review: the <link>'s layers come first in the
  // document even though the combined sheet puts inline <style> bodies first.
  CascadeLayerOrder order =
      Order({Sheet("@layer theme{.a{}} @layer util{.b{}}"),
             Sheet("@layer reset{h1{color:black}}")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"theme", "util", "reset"}));
}

TEST(CascadeLayerOrderTest, ReMentionDoesNotMove) {
  CascadeLayerOrder order = Order({Sheet("@layer a{} @layer b{}"),
                                   Sheet("@layer b{.x{}} @layer a{.y{}}"),
                                   Sheet("@layer c; @layer a, b;")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"a", "b", "c"}));
}

TEST(CascadeLayerOrderTest, UnavailableSourceIsNotProven) {
  // Case B: a sheet the gather never read, wherever it stands.
  for (const auto& sources :
       {std::vector<LayerOrderSource>{Missing("cross-origin"),
                                      Sheet("@layer a{}")},
        std::vector<LayerOrderSource>{Sheet("@layer a{}"),
                                      Missing("cross-origin")}}) {
    CascadeLayerOrder order = Order(sources);
    EXPECT_FALSE(order.proven);
    EXPECT_NE(order.reason.find("cross-origin"), std::string::npos)
        << order.reason;
    EXPECT_TRUE(order.names.empty());
  }
}

TEST(CascadeLayerOrderTest, NestedAndDottedNames) {
  CascadeLayerOrder order =
      Order({Sheet("@layer a{@layer b{.x{}} @layer c, d;} @layer a.e; "
                   "@layer f.g.h;@layer a{@layer b{@layer i{}}}")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"a", "a.b", "a.c", "a.d", "a.e", "f", "f.g",
                                "f.g.h", "a.b.i"}));
  EXPECT_EQ(order.Statement(), "@layer a,a.b,a.c,a.d,a.e,f,f.g,f.g.h,a.b.i;");
}

TEST(CascadeLayerOrderTest, AnonymousLayersAreNotListed) {
  // After every named sibling: fine, and not listed.
  CascadeLayerOrder after =
      Order({Sheet("@layer a{} @layer{.x{}} @layer a{}")});
  ASSERT_TRUE(after.proven) << after.reason;
  EXPECT_EQ(after.names, (Names{"a"}));
  // Names inside an anonymous layer cannot be named from outside.
  CascadeLayerOrder inside = Order({Sheet("@layer b; @layer{@layer q{}}")});
  ASSERT_TRUE(inside.proven) << inside.reason;
  EXPECT_EQ(inside.names, (Names{"b"}));
  // An anonymous child of `a` does not affect a named sibling of `a`.
  CascadeLayerOrder other_parent =
      Order({Sheet("@layer a{@layer{.x{}}} @layer b{}")});
  ASSERT_TRUE(other_parent.proven) << other_parent.reason;
  EXPECT_EQ(other_parent.names, (Names{"a", "b"}));
}

TEST(CascadeLayerOrderTest, AnonymousBeforeANamedSiblingIsNotProven) {
  // A leading statement would move `a` ahead of the anonymous layer.
  for (const char* css :
       {"@layer{.x{}} @layer a{.y{}}", "@layer a{@layer{} @layer c{}}"}) {
    CascadeLayerOrder order = Order({Sheet(css)});
    EXPECT_FALSE(order.proven) << css;
    EXPECT_NE(order.reason.find("anonymous"), std::string::npos)
        << order.reason;
  }
  // Across sources too.
  EXPECT_FALSE(Order({Sheet("@layer{}"), Sheet("@layer a{}")}).proven);
}

TEST(CascadeLayerOrderTest, LayerFirstDeclaredUnderAConditionIsNotProven) {
  for (const char* css : {
           "@media (min-width:768px){@layer m{}} @layer m{}",
           "@supports (display:grid){@layer s{}}",
           "@container (width>1px){@layer k{}}",
           ".a{@layer n{}} @layer n{}",
           "@layer a{} @media print{@layer a{@layer b{}}}",
       }) {
    CascadeLayerOrder order = Order({Sheet(css)});
    EXPECT_FALSE(order.proven) << css;
    EXPECT_NE(order.reason.find("condition"), std::string::npos)
        << order.reason;
  }
  // A known layer mentioned again under a condition changes nothing.
  CascadeLayerOrder known =
      Order({Sheet("@layer m{} @media (min-width:768px){@layer m{.x{}}}")});
  ASSERT_TRUE(known.proven) << known.reason;
  EXPECT_EQ(known.names, (Names{"m"}));
  // `@media all` always applies.
  CascadeLayerOrder all = Order({Sheet("@media all{@layer x{}}")});
  ASSERT_TRUE(all.proven) << all.reason;
  EXPECT_EQ(all.names, (Names{"x"}));
}

TEST(CascadeLayerOrderTest, ConditionalSource) {
  // A <link media> / <noscript> source: new layers there are not proven,
  // known ones are fine.
  EXPECT_FALSE(Order({Sheet("@layer p{}", /*conditional=*/true)}).proven);
  CascadeLayerOrder known = Order(
      {Sheet("@layer p{}"), Sheet("@layer p{.x{}}", /*conditional=*/true)});
  ASSERT_TRUE(known.proven) << known.reason;
  EXPECT_EQ(known.names, (Names{"p"}));
  EXPECT_TRUE(Order({Sheet(".x{}", /*conditional=*/true)}).proven);
}

TEST(CascadeLayerOrderTest, MediaAttributeValues) {
  EXPECT_TRUE(LayerOrderMediaIsUnconditional(""));
  EXPECT_TRUE(LayerOrderMediaIsUnconditional("all"));
  EXPECT_TRUE(LayerOrderMediaIsUnconditional(" ALL "));
  EXPECT_FALSE(LayerOrderMediaIsUnconditional("screen"));
  EXPECT_FALSE(LayerOrderMediaIsUnconditional("print"));
  EXPECT_FALSE(LayerOrderMediaIsUnconditional("(min-width: 768px)"));
}

TEST(CascadeLayerOrderTest, ImportIntoANamedLayer) {
  // `@import url(x) layer(y)` declares y where the import stands, and the
  // imported sheet's layers become y's children.
  auto lookup = Imports(
      {{"https://example.com/lib.css", "@layer p{.a{}} @layer q{.b{}}"}});
  CascadeLayerOrder order =
      Order({Sheet("@import url(/lib.css) layer(y);\n@layer z{.c{}}")}, lookup);
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"y", "y.p", "y.q", "z"}));
  // The same with a quoted URL and a dotted layer name.
  CascadeLayerOrder dotted =
      Order({Sheet("@import \"lib.css\" layer(v.w);")}, lookup);
  ASSERT_TRUE(dotted.proven) << dotted.reason;
  EXPECT_EQ(dotted.names, (Names{"v", "v.w", "v.w.p", "v.w.q"}));
  // An unlayered import contributes its layers at the import's position.
  CascadeLayerOrder plain = Order(
      {Sheet("@layer first; @import url('/lib.css'); @layer z{}")}, lookup);
  ASSERT_TRUE(plain.proven) << plain.reason;
  EXPECT_EQ(plain.names, (Names{"first", "p", "q", "z"}));
}

TEST(CascadeLayerOrderTest, ImportNotInCacheIsNotProven) {
  CascadeLayerOrder order =
      Order({Sheet("@import url(/lib.css) layer(y); @layer z{}")});
  EXPECT_FALSE(order.proven);
  EXPECT_NE(order.reason.find("/lib.css"), std::string::npos) << order.reason;
  // Reported, so the serve path holds a revalidation back until it caches.
  EXPECT_TRUE(order.import_missing);
  // Unlayered too: its content could declare a layer.
  EXPECT_FALSE(Order({Sheet("@import 'https://fonts.example/f.css';")}).proven);
  // Any other refusal is not a missing import, and neither is a later missing
  // import behind it (the order stays unproven either way).
  CascadeLayerOrder other =
      Order({Sheet("@media (x){@layer m{}}"), Sheet("@import url(/lib.css);")});
  EXPECT_FALSE(other.proven);
  EXPECT_FALSE(other.import_missing);
  EXPECT_FALSE(Order({Missing()}).import_missing);
  EXPECT_FALSE(CascadeLayerOrder{}.import_missing);
}

TEST(CascadeLayerOrderTest, AnonymousImportLayer) {
  // Its content is inside an anonymous layer, so it is not needed.
  CascadeLayerOrder after =
      Order({Sheet("@layer z; @import url(/lib.css) layer;")});
  ASSERT_TRUE(after.proven) << after.reason;
  EXPECT_EQ(after.names, (Names{"z"}));
  // Before a named top-level layer, it is an anonymous layer ahead of it.
  EXPECT_FALSE(
      Order({Sheet("@import url(/lib.css) layer; @layer z{}")}).proven);
}

TEST(CascadeLayerOrderTest, ConditionalImport) {
  auto lookup = Imports({{"https://example.com/lib.css", "@layer p{}"}});
  EXPECT_FALSE(
      Order({Sheet("@import url(/lib.css) layer(y) screen;")}, lookup).proven);
  EXPECT_FALSE(
      Order({Sheet("@import url(/lib.css) supports(display:grid);")}, lookup)
          .proven);
  CascadeLayerOrder all = Order({Sheet("@import url(/lib.css) all;")}, lookup);
  ASSERT_TRUE(all.proven) << all.reason;
  EXPECT_EQ(all.names, (Names{"p"}));
}

TEST(CascadeLayerOrderTest, ImportAfterARuleIsIgnoredLikeABrowserDoes) {
  CascadeLayerOrder order =
      Order({Sheet(".a{} @import url(/missing.css) layer(y); @layer z{}")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"z"}));
}

TEST(CascadeLayerOrderTest, ImportCycleStopsAtTheDepthLimit) {
  auto lookup =
      Imports({{"https://example.com/a.css", "@import url(/a.css);"}});
  CascadeLayerOrder order = Order({Sheet("@import url(/a.css);")}, lookup);
  EXPECT_FALSE(order.proven);
  EXPECT_NE(order.reason.find("deeper"), std::string::npos) << order.reason;
}

TEST(CascadeLayerOrderTest, CommentsAndStringsAreNotRules) {
  CascadeLayerOrder order =
      Order({Sheet("/* @layer fake{} */ .a{content:\"@layer nope{\"}"
                   "<!-- @layer real{} -->")});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.names, (Names{"real"}));
}

TEST(CascadeLayerOrderTest, StrayBraceOrSemicolonIsNotProven) {
  // A browser reads `} @layer b{...}` as one invalid qualified rule and drops
  // it, so `b` is never declared there.
  for (const char* css :
       {".q{}} @layer b{.x{}} @layer a{}", ".q{}; @layer b{} @layer a{}",
        "@layer a{; @layer b{}}", "foo; @layer b{}", "@import url(x) }"}) {
    CascadeLayerOrder order = Order({Sheet(css)});
    EXPECT_FALSE(order.proven) << css;
  }
  // `;` between declarations, and empty declarations, are fine.
  CascadeLayerOrder decls =
      Order({Sheet(".a{color:red;;} @layer a{.b{x:y;}}")});
  ASSERT_TRUE(decls.proven) << decls.reason;
  EXPECT_EQ(decls.names, (Names{"a"}));
}

TEST(CascadeLayerOrderTest, EscapedAtKeywordIsNotProven) {
  // `@l\61yer` is @layer to a browser.
  CascadeLayerOrder order =
      Order({Sheet("@l\\61yer b{.x{}} @layer a{} @layer b{}")});
  EXPECT_FALSE(order.proven);
  EXPECT_NE(order.reason.find("escape"), std::string::npos) << order.reason;
  // Likewise a layer() import condition written with an escape.
  auto lookup = Imports({{"https://example.com/lib.css", ".l{}"}});
  CascadeLayerOrder import =
      Order({Sheet("@import url(/lib.css) l\\61yer(b);@layer a{} @layer b{}")},
            lookup);
  EXPECT_FALSE(import.proven);
  EXPECT_NE(import.reason.find("escape"), std::string::npos) << import.reason;
}

TEST(CriticalCssLayerPlacementTest, EscapedAtKeywordBlockNeedsTheOrder) {
  // A block carrying `@l\61yer` uses a layer to a browser: it is not placed
  // first as an unlayered block, and its escape keeps the fallback even with
  // a proven order.
  EXPECT_TRUE(CriticalCssMayUseCascadeLayer("@l\\61yer b{.x{}}"));
  EXPECT_TRUE(CriticalCssMayUseCascadeLayer("@\\6c ayer b{.x{}}"));
  EXPECT_FALSE(CriticalCssMayUseCascadeLayer(".a{content:\"\\2014\"}"));
  CascadeLayerOrder proven = Order({Sheet("@layer a,b;")});
  ASSERT_TRUE(proven.proven) << proven.reason;
  EXPECT_TRUE(DecideCriticalCssLayerPlacement("@l\\61yer b{.x{}}", proven)
                  .keep_fallback);
}

TEST(CascadeLayerOrderTest, ScriptThatMayInsertASheet) {
  LayerOrderSource script;
  script.is_script = true;
  // Before a sheet that declares a new layer: not proven.
  EXPECT_FALSE(Order({script, Sheet("@layer a{}")}).proven);
  // Before a sheet that only re-mentions known layers, or after them: fine.
  CascadeLayerOrder ok = Order(
      {Sheet("@layer a{}"), script, Sheet("@layer a{.x{}} .y{}"), script});
  ASSERT_TRUE(ok.proven) << ok.reason;
  EXPECT_EQ(ok.names, (Names{"a"}));
}

TEST(CascadeLayerOrderTest, ValidationBinding) {
  constexpr std::string_view kLayered = "@layer a{.x{}}";
  CascadeLayerOrder unproven;
  EXPECT_EQ(unproven.ValidationBinding(kLayered), "unproven");
  // `a` is not in this order: the block takes the fallback, retention keeps
  // every width, no "r2".
  CascadeLayerOrder none = Order({});
  EXPECT_EQ(none.ValidationBinding(kLayered), "proven ");
  CascadeLayerOrder two = Order({Sheet("@layer a, b;")});
  // The block goes first and retention narrows, so the binding
  // says so ("r2"), and records made before narrowing existed are made again.
  EXPECT_EQ(two.ValidationBinding(kLayered), "proven r2 @layer a,b;");
  // An anonymous layer keeps every width (the earlier layer wins for
  // !important), and so does CSS the walker cannot read: no "r2".
  EXPECT_EQ(two.ValidationBinding("@layer{.x{}}"), "proven @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer a{.x{}} @layer{.y{}}"),
            "proven @layer a,b;");
  // `!important` inside an anonymous layer is left out of the
  // block, so a sheet that may have one is marked "a2" (an anonymous @import
  // counts: its sheet is not read), proven or not.
  EXPECT_EQ(two.ValidationBinding("@import url(x.css) layer;@layer a{}"),
            "proven a2 @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer a{} @layer{.x{color:red!important}}"),
            "proven a2 @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer{.x{color:red!imp\\ortant}}"),
            "proven a2 @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer a{.x{color:red!important}}"),
            "proven r2 @layer a,b;")
      << "an !important in a named layer stays in the block";
  EXPECT_EQ(unproven.ValidationBinding("@layer{.x{color:red!important}}"),
            "unproven a2 u2");
  // A sheet with an anonymous layer whose block goes after the
  // sheets loses every anonymous-layer rule from the block: "u2". Not on a
  // page whose block goes first.
  EXPECT_EQ(unproven.ValidationBinding("@layer a{} @layer{.x{color:red}}"),
            "unproven u2");
  EXPECT_EQ(unproven.ValidationBinding("@layer a{.x{color:red}}"), "unproven");
  EXPECT_EQ(two.ValidationBinding("@layer a{} @layer{.x{color:red}}"),
            "proven @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer c{} @layer{.x{color:red}}"),
            "proven u2 @layer a,b;")
      << "proven, but the sheet names a layer the order lacks: fallback";
  // An at-rule keyword written with an escape is not decoded,
  // so a rule with a block under one may be an anonymous layer, and such a
  // block always goes after the sheets: "u2" (and "a2" with an `!important`),
  // proven or not.
  EXPECT_EQ(two.ValidationBinding("@l\\61yer a{.x{}}"),
            "proven u2 @layer a,b;");
  EXPECT_EQ(two.ValidationBinding("@layer a{} @l\\61yer{.x{color:red}}"),
            "proven u2 @layer a,b;");
  EXPECT_EQ(unproven.ValidationBinding("@l\\61yer{.x{color:red}}"),
            "unproven u2");
  EXPECT_EQ(unproven.ValidationBinding(
                "@media screen{@\\6c ayer{.x{color:red!important}}}"),
            "unproven a2 u2");
  // A sheet without layers binds to nothing: its block goes first without a
  // statement whatever the order, and its records keep hashing the sheet
  // alone, as they did before the order existed.
  EXPECT_EQ(unproven.ValidationBinding(".x{color:red}"), "");
  EXPECT_EQ(two.ValidationBinding(".x{color:red}"), "");
}

// The textual anonymous-layer detector counts an at-rule with a
// block whose keyword is written with an escape (`@l\61yer {` is `@layer {`
// to a browser), whatever its prelude, since the keyword is not decoded; a
// statement under such a keyword is not an anonymous layer block.
TEST(CascadeLayerOrderTest, EscapedAtKeywordCountsAsAnonymousLayer) {
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer{.x{}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer /**/ {.x{}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@\\6c ayer{.x{}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer a{.x{}}"))
      << "not decoded, so a named one counts too";
  EXPECT_TRUE(CssTextHasAnonymousLayer("@media screen{@l\\61yer{.x{}}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer/*;*/{.x{}}"))
      << "a `;` in a comment before the `{` does not end the rule";
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer /* a; */ {.x{}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer ';' {.x{}}"));
  EXPECT_TRUE(CssTextHasAnonymousLayer("@l\\61yer (a;b) {.x{}}"));
  EXPECT_FALSE(CssTextHasAnonymousLayer("@\\69mport url(a.css) /*{*/;.x{}"))
      << "a `{` in a comment does not make a statement a block";
  EXPECT_FALSE(CssTextHasAnonymousLayer("@\\69mport url(a.css);.x{}"))
      << "a statement is not a layer block";
  EXPECT_FALSE(CssTextHasAnonymousLayer("@layer a{.x{content:'\\61'}}"))
      << "an escape elsewhere is not one in the keyword";
  EXPECT_FALSE(CssTextHasAnonymousLayer(".a\\:b{} @layer a{}"));

  EXPECT_TRUE(CssAtRuleKeywordHasEscape("@l\\61yer"));
  EXPECT_TRUE(CssAtRuleKeywordHasEscape("@l\\61yer a"));
  EXPECT_TRUE(CssAtRuleKeywordHasEscape("@\\6c ayer"));
  EXPECT_TRUE(CssAtRuleKeywordHasEscape(" @\\69mport url(a.css)"));
  EXPECT_FALSE(CssAtRuleKeywordHasEscape("@layer"));
  EXPECT_FALSE(CssAtRuleKeywordHasEscape("@layer \\61"))
      << "an escape in the prelude is not one in the keyword";
  EXPECT_FALSE(CssAtRuleKeywordHasEscape("@media (min-width:1px)"));
  EXPECT_FALSE(CssAtRuleKeywordHasEscape(".a\\:b"));
  EXPECT_FALSE(CssAtRuleKeywordHasEscape(""));
}

TEST(CascadeLayerOrderTest, UnreadableNamesAreNotProven) {
  for (const char* css :
       {"@layer \\61{}", "@layer a b{}", "@layer a,{}", "@layer 1a{}",
        "@layer a..b;", "@layer revert-layer{}", "@layer ;", "@layer a,,b;"}) {
    EXPECT_FALSE(Order({Sheet(css)}).proven) << css;
  }
  CascadeLayerOrder spaced = Order({Sheet("@layer /*c*/ a /*d*/ , b ;")});
  ASSERT_TRUE(spaced.proven) << spaced.reason;
  EXPECT_EQ(spaced.names, (Names{"a", "b"}));
}

// --- the placement rule --------------------------------------------------

CascadeLayerOrder Proven(Names names) {
  CascadeLayerOrder o;
  o.proven = true;
  o.reason.clear();
  o.names = std::move(names);
  return o;
}

TEST(CriticalCssLayerPlacementTest, UnlayeredBlockGoesFirstWithoutAPrefix) {
  for (const CascadeLayerOrder& order :
       {CascadeLayerOrder{}, Proven({"a", "b"})}) {
    CriticalCssLayerPlacement p =
        DecideCriticalCssLayerPlacement(".a{color:red}", order);
    EXPECT_FALSE(p.keep_fallback);
    EXPECT_EQ(p.prefix, "");
  }
}

TEST(CriticalCssLayerPlacementTest, AnonymousLayerBlockNeedsTheOrderToo) {
  // Placed first without the statement, the block's anonymous layer would be
  // registered ahead of every named layer, where its !important declarations
  // beat theirs. So it takes the statement, or the fallback.
  CriticalCssLayerPlacement p =
      DecideCriticalCssLayerPlacement("@layer{.a{}}", Proven({"a", "b"}));
  EXPECT_FALSE(p.keep_fallback);
  EXPECT_EQ(p.prefix, "@layer a,b;");
  p = DecideCriticalCssLayerPlacement("@layer{.a{}}", CascadeLayerOrder{});
  EXPECT_TRUE(p.keep_fallback);
  EXPECT_EQ(p.prefix, "");
  EXPECT_TRUE(CriticalCssMayUseCascadeLayer("@LAYER {x{}}"));
  EXPECT_FALSE(CriticalCssMayUseCascadeLayer("@layers{} .player{}"));
}

TEST(CriticalCssLayerPlacementTest, LayeredBlockNeedsAProvenOrder) {
  CriticalCssLayerPlacement p =
      DecideCriticalCssLayerPlacement("@layer b{.a{}}", CascadeLayerOrder{});
  EXPECT_TRUE(p.keep_fallback);
  EXPECT_EQ(p.prefix, "");

  p = DecideCriticalCssLayerPlacement(
      "@layer b{.a{}} @layer a{@layer c{.b{}}} @media (x){@layer b{.c{}}}",
      Proven({"a", "a.c", "b"}));
  EXPECT_FALSE(p.keep_fallback);
  EXPECT_EQ(p.prefix, "@layer a,a.c,b;");
}

TEST(CriticalCssLayerPlacementTest,
     BlockNamingALayerTheOrderLacksKeepsFallback) {
  EXPECT_TRUE(DecideCriticalCssLayerPlacement("@layer b{} @layer x{}",
                                              Proven({"a", "b"}))
                  .keep_fallback);
  EXPECT_TRUE(
      DecideCriticalCssLayerPlacement("@layer a{@layer y{}}", Proven({"a"}))
          .keep_fallback);
  // An @import in the block, or a name this parser cannot read.
  EXPECT_TRUE(DecideCriticalCssLayerPlacement("@import url(x.css) layer(a);",
                                              Proven({"a"}))
                  .keep_fallback);
  EXPECT_TRUE(DecideCriticalCssLayerPlacement("@layer \\61{}", Proven({"a"}))
                  .keep_fallback);
}

TEST(CriticalCssLayerPlacementTest, ProvenOrderWithoutLayers) {
  // The detector is over-inclusive (a mention in a comment counts); the walk
  // finds no name, so the block goes first with nothing in front.
  CriticalCssLayerPlacement p =
      DecideCriticalCssLayerPlacement("/* @layer x; */ .a{}", Proven({}));
  EXPECT_FALSE(p.keep_fallback);
  EXPECT_EQ(p.prefix, "");
}

}  // namespace
}  // namespace pagespeed
