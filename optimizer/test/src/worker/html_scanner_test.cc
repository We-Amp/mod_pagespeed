// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Test for the HTML Scanner

#include "src/worker/html_scanner.h"

#include <optional>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(HtmlScannerTest, ScanSimpleHtml) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<html><body>Hello</body></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.error_message.empty());
  ASSERT_EQ(2u, result.elements.size());

  EXPECT_EQ("html", result.elements[0].tag_name);
  EXPECT_EQ(0, result.elements[0].depth);
  EXPECT_EQ(0, result.elements[0].element_index);

  EXPECT_EQ("body", result.elements[1].tag_name);
  EXPECT_EQ(1, result.elements[1].depth);
  EXPECT_EQ(1, result.elements[1].element_index);
}

TEST(HtmlScannerTest, ScanEmptyHtml) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan("http://example.com/", "");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.elements.empty());
}

TEST(HtmlScannerTest, CollectsElementId) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<div id=\"main\">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  EXPECT_EQ("div", result.elements[0].tag_name);
  EXPECT_EQ("main", result.elements[0].id);
}

TEST(HtmlScannerTest, CollectsElementClasses) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<div class=\"container hero dark-mode\">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  ASSERT_EQ(3u, result.elements[0].classes.size());
  EXPECT_EQ("container", result.elements[0].classes[0]);
  EXPECT_EQ("hero", result.elements[0].classes[1]);
  EXPECT_EQ("dark-mode", result.elements[0].classes[2]);
}

TEST(HtmlScannerTest, CollectsStylesheetLinks) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/css/main.css\">"
      "<link rel=\"stylesheet\" href=\"/css/print.css\" media=\"print\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(2u, result.stylesheets.size());

  EXPECT_EQ("/css/main.css", result.stylesheets[0].href);
  EXPECT_TRUE(result.stylesheets[0].media.empty());

  EXPECT_EQ("/css/print.css", result.stylesheets[1].href);
  EXPECT_EQ("print", result.stylesheets[1].media);
}

// Regression (duplicate preload / combined_css double / template-hash flap):
// on a revalidation pass the worker re-scans its OWN optimized output, where the
// async-CSS transform has split one origin <link rel="stylesheet"> into a
// deferred primary plus a <noscript> fallback copy of the SAME href. The
// scanner must treat the pair as the single logical origin stylesheet — the
// fallback is the worker's own artifact (marked data-pagespeed-async-fallback),
// not a second origin sheet. Collecting both doubles the preload Early-Hints,
// doubles combined_css gathering, and flaps the template hash (stylesheet count
// 1<->2 between the raw-origin and reprocessed passes). Mirrors the existing
// data-pagespeed-critical <style> skip.
TEST(HtmlScannerTest, SkipsAsyncCssFallbackLink) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-media=\"screen\" data-pagespeed-async=\"\">"
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // Exactly one logical stylesheet: the primary. The <noscript> fallback copy
  // must NOT be collected as a second origin sheet.
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/a.css", result.stylesheets[0].href);
  // Both links share href=/a.css, so the surviving entry must be disambiguated:
  // the kept sheet is the deferred primary, not the fallback — proving the
  // guard skipped the fallback and kept the primary. The primary is
  // rel="preload" (not rel="stylesheet"), which is exactly why matching on
  // rel alone would drop it and leave the page with ZERO scanned stylesheets.
  // Its reported media is the RECORDED one: a deferred link carries no live
  // media attribute, and media-keyed decisions downstream (the Early-Hints
  // print skip) must see the author's value on this pass too.
  EXPECT_EQ("screen", result.stylesheets[0].media);
}

// The deferred primary is matched on rel="preload" + data-pagespeed-async,
// NOT on rel="preload" alone. The page is full of other preloads — the LCP
// image, fonts, the worker's own injected hints — and collecting any of them
// as a stylesheet would feed a non-CSS URL into combined_css gathering and
// into the stylesheet Early-Hints list.
TEST(HtmlScannerTest, OrdinaryPreloadsAreNotStylesheets) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"preload\" as=\"image\" href=\"/hero.jpg\">"
      "<link rel=\"preload\" as=\"font\" href=\"/f.woff2\" crossorigin>"
      "<link rel=\"preload\" as=\"style\" href=\"/not-ours.css\">"
      "<link rel=\"preload\" as=\"style\" href=\"/ours.css\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  ASSERT_TRUE(result.success);
  // Only the one carrying our marker. An author's own hand-written
  // `rel=preload as=style` is a hint about a sheet declared elsewhere, not a
  // stylesheet declaration — treating it as one would double-count the sheet.
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/ours.css", result.stylesheets[0].href);
}

// Regression (template-hash element-tree flap): on a revalidation pass the
// worker re-scans its OWN optimized output, which carries nodes the raw-origin
// HTML never had — the injected critical <style data-pagespeed-critical>, the
// async <noscript data-pagespeed-async-fallback> + its child <link>, the
// async-loader <script data-pagespeed-async-loader>, and preconnect/preload
// <link data-pagespeed-hint>. TemplateDetector::HashStructure hashes the full
// element tree, so collecting these extra nodes flaps the template hash between
// the raw and reprocessed passes (a spurious re-analysis + duplicate profile).
// The scanner must exclude its OWN injected markers from result.elements so the
// element list is a fixed point. Original customer nodes that merely carry a
// worker marker (data-pagespeed-async / -media on the deferred primary
// <link>, data-pagespeed-defer on a real <script>) are NOT injected and MUST be
// kept — they exist in the raw scan too.
TEST(HtmlScannerTest, ExcludesWorkerInjectedNodesFromElements) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      // Original customer <link>, async-swapped in place (KEEP).
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-media=\"screen\" data-pagespeed-async=\"\">"
      // Injected critical CSS (SKIP).
      "<style data-pagespeed-critical=\"\">.x{color:red}</style>"
      // Injected async <noscript> + child fallback <link> (SKIP both).
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      // Injected async-CSS loader <script> (SKIP).
      "<script src=\"/pagespeed_static/async_css.js\" defer "
      "data-pagespeed-async-loader=\"\"></script>"
      // Injected preconnect hint on a <link> (SKIP).
      "<link rel=\"preconnect\" href=\"https://cdn.example.com\" "
      "data-pagespeed-hint=\"\">"
      "</head><body>"
      "<h1>Hi</h1>"
      "<p>x</p>"
      // Injected speculation-rules hint — the SAME data-pagespeed-hint marker
      // as the <link> hints, but on a <script> and sitting in <body> (not
      // <head>). The skip is tag-agnostic, so this must be dropped too; the
      // deferred customer <script> after it must keep its contiguous index,
      // which also exercises the depth balance for an injected node mid-body.
      "<script type=\"speculationrules\" data-pagespeed-hint=\"\">"
      "{\"prefetch\":[{\"source\":\"list\",\"urls\":[\"/next\"]}]}</script>"
      // Original customer <script>, deferred in place (KEEP).
      "<script src=\"/app.js\" data-pagespeed-defer=\"\"></script>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  ASSERT_TRUE(result.success);

  // Only the 7 original-document elements survive; all 6 injected nodes (style,
  // noscript, fallback link, loader script, hint <link>, hint <script>) drop.
  std::vector<std::string> tags;
  tags.reserve(result.elements.size());
  for (const auto& elem : result.elements) {
    tags.push_back(elem.tag_name);
  }
  EXPECT_EQ(tags, (std::vector<std::string>{"html", "head", "link", "body",
                                            "h1", "p", "script"}));

  // The kept <link> is the deferred primary (proves we skipped only the
  // injected markers, not the original element that carries -async/-media).
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("screen", result.stylesheets[0].media);

  // element_index must stay contiguous (0..N-1): skipped injected nodes must
  // NOT advance the counter, or real-element indices would shift on the
  // reprocessed pass and CriticalCssExtractor's "first N elements" cutoff
  // (element_index < max_elements) would no longer be a fixed point.
  for (size_t i = 0; i < result.elements.size(); ++i) {
    EXPECT_EQ(static_cast<int>(i), result.elements[i].element_index)
        << "at position " << i;
  }
}

// The combined stylesheet the critical block is derived from is
// the CSS a client running scripts applies. A <style> or <link> inside
// <noscript> is raw text for such a client (the lexer still parses it as
// markup), so collecting it put no-JS-only rules into the block every browser
// is served.
std::vector<std::string> Hrefs(const HtmlScanResult& result) {
  std::vector<std::string> out;
  out.reserve(result.stylesheets.size());
  for (const auto& s : result.stylesheets) out.push_back(s.href);
  return out;
}

TEST(HtmlScannerTest, SkipsNoscriptStyle) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><style>.a{color:red}</style>"
      "<noscript><style>.n{display:block!important}</style></noscript>"
      "<style>.b{color:blue}</style></head>"
      "<body><noscript><style>.m{}</style></noscript><p>x</p></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(".a{color:red}\n.b{color:blue}", result.inline_css);
}

TEST(HtmlScannerTest, SkipsNoscriptStylesheetLink) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<html><head><link rel=\"stylesheet\" href=\"/a.css\">"
                   "<noscript><link rel=\"stylesheet\" href=\"/n.css\">"
                   "</noscript></head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(std::vector<std::string>{"/a.css"}, Hrefs(result));
}

// The element list the critical-CSS extractor matches against is
// the page a browser running scripts renders. A <noscript> is raw text there,
// and <template>, <noembed> and <noframes> content never renders, so neither
// the element nor anything inside it is collected. They are kept aside, at the
// position they would have had, for the template hash; element_index still
// counts them.
TEST(HtmlScannerTest, ElementsInsideNoscriptAreNotCollected) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><noscript><link rel=\"stylesheet\" href=\"/n.css\">"
      "</noscript></head><body>"
      "<noscript><div class=\"nojs-banner\" id=\"nojs\"><p class=\"nb\">"
      "Enable JS</p></div></noscript>"
      "<div class=\"hero\">Hero</div>"
      "<template><span class=\"tpl\"></span></template>"
      "<NOSCRIPT><img class=\"pixel\" src=\"/p.gif\"></NOSCRIPT>"
      "<noembed><b class=\"ne\"></b></noembed>"
      "<noframes><i class=\"nf\"></i></noframes>"
      "<svg><noscript><text class=\"svgns\"></text></noscript></svg>"
      "<p class=\"after\">x</p></body></html>");
  ASSERT_TRUE(result.success);

  std::vector<std::string> tags;
  tags.reserve(result.elements.size());
  for (const auto& e : result.elements) {
    tags.push_back(e.tag_name);
    for (const auto& cls : e.classes) {
      EXPECT_NE(cls, "nojs-banner");
      EXPECT_NE(cls, "nb");
      EXPECT_NE(cls, "tpl");
      EXPECT_NE(cls, "pixel");
      EXPECT_NE(cls, "ne");
      EXPECT_NE(cls, "nf");
      EXPECT_NE(cls, "svgns");
    }
    EXPECT_NE(e.id, "nojs");
  }
  EXPECT_EQ(tags, (std::vector<std::string>{"html", "head", "body", "div",
                                            "svg", "p"}));

  // html(0) head(1) [noscript(2) link(3)] body(4) [noscript(5) div(6) p(7)]
  // div(8): the hero keeps the index it has with the <noscript> elements
  // counted.
  ASSERT_GE(result.elements.size(), 4u);
  EXPECT_EQ(result.elements[3].element_index, 8);

  // Left-out elements, in document order, at the position they stood.
  std::vector<std::pair<size_t, std::string>> inert;
  inert.reserve(result.inert_elements.size());
  for (const auto& e : result.inert_elements) {
    inert.emplace_back(e.position, e.tag_name);
  }
  EXPECT_EQ(inert, (std::vector<std::pair<size_t, std::string>>{{2, "noscript"},
                                                                {2, "link"},
                                                                {3, "noscript"},
                                                                {3, "div"},
                                                                {3, "p"},
                                                                {4, "template"},
                                                                {4, "span"},
                                                                {4, "NOSCRIPT"},
                                                                {4, "img"},
                                                                {4, "noembed"},
                                                                {4, "b"},
                                                                {4, "noframes"},
                                                                {4, "i"},
                                                                {5, "noscript"},
                                                                {5, "text"}}));
}

// Which <noscript> content changes what a render with scripts
// off shows. Those pages get a salted validation binding and are validated
// again once; the GTM snippet and tracking pixels must not.
TEST(HtmlScannerTest, ClassifiesNoscriptContentThatRendersWithScriptsOff) {
  struct Case {
    const char* name;
    const char* body;
    bool affects;
  };
  const Case cases[] = {
      {"GTM snippet",
       "<noscript><iframe "
       "src=\"https://www.googletagmanager.com/ns.html?id=GTM-X\" "
       "height=\"0\" width=\"0\" style=\"display:none;visibility:hidden\">"
       "</iframe></noscript>",
       false},
      {"tracking pixel",
       "<noscript><img height=\"1\" width=\"1\" style=\"display:none\" "
       "src=\"https://www.facebook.com/tr?id=1&ev=PageView\"/></noscript>",
       false},
      {"meta description and script",
       "<noscript><meta name=description "
       "content=\"x\"><script>x()</script></noscript>",
       false},
      {"meta refresh",
       "<noscript><meta http-equiv=refresh "
       "content=\"0;url=/nojs\"></noscript>",
       true},
      {"meta refresh, uppercase",
       "<noscript><META HTTP-EQUIV=\"Refresh\" "
       "content=\"5\"></noscript>",
       true},
      {"whitespace only", "<noscript>\n  </noscript>", false},
      {"worker's own fallback copy",
       "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
       "data-pagespeed-async=\"\" data-pagespeed-media=\"all\">"
       "<noscript data-pagespeed-async-fallback=\"\"><link rel=\"stylesheet\" "
       "href=\"/a.css\" data-pagespeed-async-fallback=\"\"></noscript>",
       false},
      {"loadCSS twin",
       "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
       "onload=\"this.rel='stylesheet'\"><noscript><link rel=\"stylesheet\" "
       "href=\"/a.css\"></noscript>",
       false},
      {"in svg", "<svg><noscript><text>x</text></noscript></svg>", false},
      {"in template", "<template><noscript><p>x</p></noscript></template>",
       false},
      {"banner", "<noscript><div class=\"nojs\">Enable JS</div></noscript>",
       true},
      {"text", "<noscript>Please enable JavaScript</noscript>", true},
      {"style", "<noscript><style>.hero{display:none}</style></noscript>",
       true},
      {"style in a hidden element",
       "<noscript><div hidden><style>.h{}</style></div></noscript>", true},
      {"stylesheet link",
       "<noscript><link rel=\"stylesheet\" href=\"/n.css\">"
       "</noscript>",
       true},
      {"lazy-load image fallback",
       "<noscript><img src=\"/hero.jpg\" width=\"800\" height=\"400\">"
       "</noscript>",
       true},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    HtmlScanner scanner;
    HtmlScanResult result =
        scanner.Scan("http://example.com/",
                     absl::StrCat("<html><head></head><body>", c.body,
                                  "<div class=\"hero\">x</div></body></html>"));
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.noscript_affects_render, c.affects);
  }
}

TEST(HtmlScannerTest, SkipsInertSubtreesAndForeignStyleAndLink) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><template><style>.t{}</style>"
      "<link rel=\"stylesheet\" href=\"/t.css\"></template></head><body>"
      "<noembed><style>.e{}</style>"
      "<link rel=\"stylesheet\" href=\"/e.css\"></noembed>"
      "<noframes><style>.f{}</style></noframes>"
      "<math><style>.m{}</style></math>"
      // An SVG <style> or <link> is an SVG element, not an HTML one: its
      // content is markup, not the page's CSS.
      "<svg><style>.s{fill:red}</style>"
      "<link rel=\"stylesheet\" href=\"/s.css\"></svg>"
      "<link rel=\"stylesheet\" href=\"/body.css\"></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ("", result.inline_css);
  EXPECT_EQ(std::vector<std::string>{"/body.css"}, Hrefs(result));
}

// The parser reads inline SVG and MathML as browsers do: a <style> there is an
// SVG or MathML element whose content is markup, so it is neither inline CSS
// for the critical block nor a stylesheet source for the combined CSS and the
// layer order, wherever it stands in foreign content.
TEST(HtmlScannerTest, ForeignStyleIsNotPageCss) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head></head><body>"
      "<svg><style>.s{fill:red}</style><g><style>.g{}</style></g></svg>"
      "<math><mrow><style>.m{}</style></mrow></math>"
      "<svg><desc><svg><style>.d{}</style></svg></desc></svg>"
      "<math><mi><mglyph><style>.mg{}</style></mglyph></mi></math>"
      "<math><annotation-xml><style>.ax{}</style></annotation-xml></math>"
      "<style>.page{}</style></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(".page{}", result.inline_css);
  ASSERT_EQ(result.stylesheet_sources.size(), 1u);
  EXPECT_EQ(result.stylesheet_sources[0].css, ".page{}");
}

// Inside an integration point (SVG foreignObject, desc and title; MathML mi,
// mo, mn, ms and mtext; annotation-xml with an HTML encoding) elements are
// HTML again: a <style> or stylesheet <link> there applies to the document.
TEST(HtmlScannerTest, HtmlStyleAndLinkInIntegrationPointsApply) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head></head><body>"
      "<svg><foreignObject><style>.fo{}</style>"
      "<link rel=\"stylesheet\" href=\"/fo.css\"></foreignObject></svg>"
      "<math><mi><style>.mi{}</style></mi>"
      "<annotation-xml encoding=\"text/html\">"
      "<link rel=\"stylesheet\" href=\"/ax.css\"></annotation-xml></math>"
      "</body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(".fo{}\n.mi{}", result.inline_css);
  EXPECT_EQ((std::vector<std::string>{"/fo.css", "/ax.css"}), Hrefs(result));
}

// A <noscript>, <template>, <noembed> or <noframes> in inline SVG or MathML
// stays out of `elements` with what it holds, as in HTML content.
TEST(HtmlScannerTest, LeavesForeignNoscriptAndTemplateOutOfElements) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><body><svg><noscript><text class=\"svgns\"></text></noscript>"
      "<template><g class=\"svgtpl\"></g></template></svg>"
      "<math><noembed><mi class=\"mathne\"></mi></noembed>"
      "<noframes><mn></mn></noframes></math></body></html>");
  ASSERT_TRUE(result.success);
  std::vector<std::string> tags;
  tags.reserve(result.elements.size());
  for (const auto& e : result.elements) tags.push_back(e.tag_name);
  EXPECT_EQ(tags, (std::vector<std::string>{"html", "body", "svg", "math"}));
  std::vector<std::string> inert;
  inert.reserve(result.inert_elements.size());
  for (const auto& e : result.inert_elements) inert.push_back(e.tag_name);
  EXPECT_EQ(inert,
            (std::vector<std::string>{"noscript", "text", "template", "g",
                                      "noembed", "mi", "noframes", "mn"}));
}

// An <svg> or <iframe> inside an integration point is an HTML-placed replaced
// element again, so its width before author CSS is read like any other.
TEST(HtmlScannerTest, ReplacedElementsInIntegrationPointsMayExceedAPhone) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><body><svg><foreignObject><svg class=\"fo\"></svg>"
      "<iframe class=\"fi\" width=\"560\"></iframe></foreignObject>"
      "<g><svg class=\"nested\"></svg></g></svg></body></html>");
  ASSERT_TRUE(result.success);
  auto wide = [&](const std::string& cls) -> std::optional<bool> {
    for (const auto& e : result.elements) {
      for (const auto& c : e.classes) {
        if (c == cls) return e.may_exceed_viewport;
      }
    }
    return std::nullopt;
  };
  EXPECT_EQ(wide("fo"), true) << "an svg inside foreignObject";
  EXPECT_EQ(wide("fi"), true) << "an iframe inside foreignObject";
  EXPECT_EQ(wide("nested"), false) << "an svg inside an svg";
}

TEST(HtmlScannerTest, SkipsNestedNonDocumentSubtrees) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head></head><body>"
      "<template><noscript><style>.tn{}</style></noscript></template>"
      "<noscript><template><style>.nt{}</style></template></noscript>"
      "<noscript><div><svg><style>.ns{}</style></svg></div></noscript>"
      "<div><noscript><p><style>.deep{}</style></p></noscript></div>"
      // Closed subtrees do not leak: this one is collected.
      "<style>.after{}</style></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(".after{}", result.inline_css);
  EXPECT_TRUE(result.stylesheets.empty());
}

// A `type` the browser does not treat as CSS: the element is ignored, so its
// bytes are not the page's CSS (Tailwind's browser build reads
// <style type="text/tailwindcss"> itself; LESS sources are compiled by
// script).
TEST(HtmlScannerTest, SkipsStyleAndLinkWithANonCssType) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head>"
      "<style type=\"text/tailwindcss\">@theme{--x:1}</style>"
      "<style type=\"text/plain\">.p{}</style>"
      "<style type=\"text/css; charset=utf-8\">.params{}</style>"
      "<style type=\"TEXT/CSS\">.a{}</style>"
      "<style type=\"\">.b{}</style>"
      "<style>.c{}</style>"
      "<link rel=\"stylesheet/less\" type=\"text/x-less\" href=\"/s.less\">"
      "<link rel=\"stylesheet\" type=\"text/x-less\" href=\"/t.less\">"
      "<link rel=\"stylesheet\" type=\"text/plain\" href=\"/p.txt\">"
      "<link rel=\"stylesheet\" type=\"text/css\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" type=\"Text/CSS; charset=utf-8\" "
      "href=\"/b.css\">"
      "<link rel=\"stylesheet\" href=\"/c.css\">"
      "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(".a{}\n.b{}\n.c{}", result.inline_css);
  EXPECT_EQ((std::vector<std::string>{"/a.css", "/b.css", "/c.css"}),
            Hrefs(result));
}

// The loadCSS pattern: the author preloads the sheet and an onload handler
// flips that same element's rel to stylesheet; the <noscript> link is the
// sheet's only declaration. The sheet applies to a client running scripts,
// where the PRELOAD stands, so it is counted once, at the preload's position,
// with the preload's href and (when the preload has none) the twin's media.
constexpr char kOnload[] = " onload=\"this.onload=null;this.rel='stylesheet'\"";

TEST(HtmlScannerTest, CountsLoadCssSheetOnceAtThePreload) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      std::string("<html><head><link rel=\"stylesheet\" href=\"/first.css\">"
                  "<link rel=\"preload\" as=\"style\" href=\"/a.css\"") +
          kOnload +
          ">"
          // An applying sheet between the preload and its twin: the browser
          // applies a.css (at the preload) before b.css.
          "<link rel=\"stylesheet\" href=\"/b.css\">"
          "<noscript><link rel=\"stylesheet\" href=\"/a.css\" media=\"screen\">"
          "</noscript>"
          // The same twin again: still one sheet.
          "<noscript><link rel=\"stylesheet\" href=\"/a.css\"></noscript>"
          // Twin before its preload: the sheet sits at the preload.
          "<noscript><link rel=\"stylesheet\" href=\"/late.css\"></noscript>"
          "<link rel=\"stylesheet\" href=\"/c.css\">"
          "<link rel=\"PRELOAD\" as=\"STYLE\" href=\"/late.css\"" +
          kOnload +
          ">"
          // A twin whose sheet an applying link already declares: that link
          // is the declaration, the preload adds nothing.
          "<link rel=\"preload\" as=\"style\" href=\"/d.css\"" +
          kOnload +
          ">"
          "<noscript><link rel=\"stylesheet\" href=\"/d.css\"></noscript>"
          "<link rel=\"stylesheet\" href=\"/d.css\">"
          "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ((std::vector<std::string>{"/first.css", "/a.css", "/b.css",
                                      "/c.css", "/late.css", "/d.css"}),
            Hrefs(result));
  ASSERT_EQ(6u, result.stylesheets.size());
  EXPECT_EQ("screen", result.stylesheets[1].media);
}

// What does NOT make a <noscript> twin count: a preload no script applies (no
// onload), the worker's own hint, a preload that is itself inside <noscript>,
// and no preload at all. Each would pull a no-JS-only sheet into the block.
TEST(HtmlScannerTest, NoscriptTwinNeedsAScriptLoadedPreload) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      std::string(
          "<html><head>"
          "<link rel=\"preload\" as=\"style\" href=\"/plain.css\">"
          "<noscript><link rel=\"stylesheet\" href=\"/plain.css\"></noscript>"
          "<link rel=\"preload\" as=\"style\" href=\"/h.css\" "
          "data-pagespeed-hint=\"\"") +
          kOnload +
          ">"
          "<noscript><link rel=\"stylesheet\" href=\"/h.css\"></noscript>"
          "<noscript><link rel=\"preload\" as=\"style\" href=\"/n.css\"" +
          kOnload +
          ">"
          "<link rel=\"stylesheet\" href=\"/n.css\"></noscript>"
          "<noscript><link rel=\"stylesheet\" href=\"/alone.css\"></noscript>"
          "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.stylesheets.empty()) << Hrefs(result).size();
}

// Under a cross-host <base href> the page host's absolute
// spelling of a root-relative href is a different sheet, so it is not the
// preload's twin (the preload is not counted) and, as a <noscript> stylesheet
// no applying link declares, it changes the no-JS render; the CDN spelling is
// the same sheet on both counts. Before the fix the identities were the other
// way round: the scanner stamped the page's host on the hostless href.
TEST(HtmlScannerTest, CrossHostBaseResolvesHostlessSheetsAgainstTheBase) {
  HtmlScanner scanner;
  const std::string head =
      "<html><head><base href=\"https://cdn.example.com/assets/\">";
  auto preload_with_twin = [&](std::string_view twin) {
    return scanner.Scan(
        "http://example.com/",
        absl::StrCat(head, "<link rel=\"preload\" as=\"style\" href=\"/a.css\"",
                     kOnload, "><noscript><link rel=\"stylesheet\" href=\"",
                     twin, "\"></noscript></head><body></body></html>"));
  };
  HtmlScanResult cdn = preload_with_twin("https://cdn.example.com/a.css");
  ASSERT_TRUE(cdn.success);
  EXPECT_EQ(std::vector<std::string>{"/a.css"}, Hrefs(cdn));
  EXPECT_FALSE(cdn.noscript_affects_render);
  HtmlScanResult page_host = preload_with_twin("http://example.com/a.css");
  ASSERT_TRUE(page_host.success);
  EXPECT_TRUE(page_host.stylesheets.empty()) << Hrefs(page_host)[0];
  EXPECT_TRUE(page_host.noscript_affects_render);

  auto declared_with_noscript = [&](std::string_view noscript_href) {
    return scanner.Scan(
        "http://example.com/",
        absl::StrCat(head, "<link rel=\"stylesheet\" href=\"/a.css\">",
                     "<noscript><link rel=\"stylesheet\" href=\"",
                     noscript_href,
                     "\"></noscript></head><body></body></html>"));
  };
  HtmlScanResult cdn_declared =
      declared_with_noscript("https://cdn.example.com/a.css");
  ASSERT_TRUE(cdn_declared.success);
  EXPECT_EQ(std::vector<std::string>{"/a.css"}, Hrefs(cdn_declared));
  EXPECT_FALSE(cdn_declared.noscript_affects_render);
  HtmlScanResult page_host_declared =
      declared_with_noscript("http://example.com/a.css");
  ASSERT_TRUE(page_host_declared.success);
  EXPECT_TRUE(page_host_declared.noscript_affects_render);
}

// The preload and its twin are matched as the sheet they fetch, not as
// strings: a relative, a root-relative and an absolute same-host spelling of
// one sheet are one sheet, resolved against the document's <base>. A hostless
// spelling takes the base URL's host, as the browser resolves it: under a
// cross-host <base href> a root-relative href is the CDN's sheet;
// a root-relative <base href> keeps the page's host, a
// protocol-relative one has its own.
TEST(HtmlScannerTest, LoadCssTwinMatchesTheResolvedSheet) {
  HtmlScanner scanner;
  const struct {
    const char* page;
    const char* head;
    const char* preload;
    const char* twin;
  } kCases[] = {
      {"http://example.com/", "", "/css/a.css", "css/a.css"},
      {"http://example.com/", "", "http://example.com/a.css", "/a.css"},
      {"http://example.com/", "", "//EXAMPLE.com/a.css#x", "./a.css"},
      {"http://example.com/p/page.html", "", "../a.css", "/a.css"},
      {"http://example.com/", "<base href=\"/sub/\">", "a.css", "/sub/a.css"},
      {"http://example.com/", "<base href=\"/sub/\">",
       "http://example.com/sub/a.css", "a.css"},
      {"http://example.com/", "<base href=\"https://cdn.example.com/assets/\">",
       "/css/a.css", "https://cdn.example.com/css/a.css"},
      {"http://example.com/", "<base href=\"https://cdn.example.com/assets/\">",
       "css/a.css", "//CDN.example.com/assets/css/a.css"},
      {"http://example.com/", "<base href=\"//cdn.example.com/\">", "/a.css",
       "https://cdn.example.com/a.css"},
  };
  for (const auto& c : kCases) {
    HtmlScanResult result = scanner.Scan(
        c.page, std::string("<html><head>") + c.head +
                    "<link rel=\"preload\" as=\"style\" href=\"" + c.preload +
                    "\"" + kOnload +
                    "><noscript><link rel=\"stylesheet\" href=\"" + c.twin +
                    "\"></noscript></head><body></body></html>");
    ASSERT_TRUE(result.success);
    EXPECT_EQ(std::vector<std::string>{c.preload}, Hrefs(result))
        << c.page << " " << c.preload << " vs " << c.twin;
  }

  // And a spelling of the same sheet already declared by an applying link
  // stops the preload counting a second time.
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      std::string("<html><head><link rel=\"stylesheet\" href=\"css/a.css\">"
                  "<link rel=\"preload\" as=\"style\" href=\"/css/a.css\"") +
          kOnload +
          "><noscript><link rel=\"stylesheet\" "
          "href=\"http://example.com/css/a.css\"></noscript>"
          "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_EQ(std::vector<std::string>{"css/a.css"}, Hrefs(result));

  // A different host is a different sheet.
  result = scanner.Scan(
      "http://example.com/",
      std::string("<html><head>"
                  "<link rel=\"preload\" as=\"style\" "
                  "href=\"http://cdn.example.net/a.css\"") +
          kOnload +
          "><noscript><link rel=\"stylesheet\" href=\"/a.css\"></noscript>"
          "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.stylesheets.empty());
}

// A <noscript> sheet's origin is not render-blocking for a client running
// scripts, so it does not outrank a real render-blocking origin.
TEST(HtmlScannerTest, NoscriptStylesheetOriginIsNotRenderBlocking) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head>"
      "<noscript><link rel=\"stylesheet\" href=\"https://ns.example/a.css\">"
      "</noscript>"
      "<link rel=\"preload\" as=\"font\" href=\"https://f1.example/f.woff2\">"
      "<link rel=\"preload\" as=\"font\" href=\"https://f2.example/f.woff2\">"
      "<link rel=\"preload\" as=\"font\" href=\"https://f3.example/f.woff2\">"
      "<link rel=\"stylesheet\" href=\"https://rb.example/b.css\">"
      "</head><body></body></html>");
  ASSERT_TRUE(result.success);
  ASSERT_EQ(4u, result.third_party_origins.size());
  EXPECT_EQ("https://rb.example", result.third_party_origins[0].origin);
  EXPECT_EQ("https://ns.example", result.third_party_origins[1].origin);
}

// The scan of the worker's own output must equal the scan of the raw page
// (the template hash and the combined stylesheet are keyed on it). The
// deferral's <noscript> twin is never a second source, and neither is an
// author <noscript> sheet on either pass.
TEST(HtmlScannerTest, ReprocessedOutputScansLikeTheRawPage) {
  HtmlScanner scanner;
  const std::string raw =
      "<html><head><link rel=\"stylesheet\" href=\"/a.css\">"
      "<noscript><style>.n{}</style>"
      "<link rel=\"stylesheet\" href=\"/n.css\"></noscript>"
      "<style>.b{}</style></head><body><p>x</p></body></html>";
  const std::string reprocessed =
      "<html><head><style data-pagespeed-critical=\"\">.b{}</style>"
      "<link rel=\"preload\" href=\"/a.css\" as=\"style\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">"
      "<script src=\"/.pagespeed/async-css.js\" defer "
      "data-pagespeed-async-loader=\"\"></script>"
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/a.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript>"
      "<noscript><style>.n{}</style>"
      "<link rel=\"stylesheet\" href=\"/n.css\"></noscript>"
      "<style>.b{}</style></head><body><p>x</p></body></html>";
  // Output of a build that still deferred the author's <noscript> link: the
  // link became a deferred primary inside <noscript>, with a nested twin.
  const std::string legacy =
      "<html><head><link rel=\"stylesheet\" href=\"/a.css\">"
      "<noscript><style>.n{}</style>"
      "<link rel=\"preload\" href=\"/n.css\" as=\"style\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">"
      "<noscript data-pagespeed-async-fallback=\"\">"
      "<link rel=\"stylesheet\" href=\"/n.css\" "
      "data-pagespeed-async-fallback=\"\"></noscript></noscript>"
      "<style>.b{}</style></head><body><p>x</p></body></html>";
  for (const std::string& html : {raw, reprocessed, legacy}) {
    HtmlScanResult result = scanner.Scan("http://example.com/", html);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(std::vector<std::string>{"/a.css"}, Hrefs(result)) << html;
    EXPECT_EQ(".b{}", result.inline_css) << html;
  }
}

TEST(HtmlScannerTest, IgnoresNonStylesheetLinks) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"icon\" href=\"/favicon.ico\">"
      "<link rel=\"preload\" href=\"/font.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.stylesheets.empty());
}

TEST(HtmlScannerTest, CollectsInlineCss) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<style>.container { max-width: 1200px; }</style>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(".container { max-width: 1200px; }", result.inline_css);
}

TEST(HtmlScannerTest, CollectsMultipleInlineStyleTags) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<style>.a { color: red; }</style>"
      "<style>.b { color: blue; }</style>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // Multiple style blocks should be joined with newlines
  EXPECT_EQ(".a { color: red; }\n.b { color: blue; }", result.inline_css);
}

TEST(HtmlScannerTest, TracksDepthCorrectly) {
  HtmlScanner scanner;
  std::string html =
      "<html>"
      "<body>"
      "<div>"
      "<p>Text</p>"
      "</div>"
      "</body>"
      "</html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.elements.size());

  EXPECT_EQ("html", result.elements[0].tag_name);
  EXPECT_EQ(0, result.elements[0].depth);

  EXPECT_EQ("body", result.elements[1].tag_name);
  EXPECT_EQ(1, result.elements[1].depth);

  EXPECT_EQ("div", result.elements[2].tag_name);
  EXPECT_EQ(2, result.elements[2].depth);

  EXPECT_EQ("p", result.elements[3].tag_name);
  EXPECT_EQ(3, result.elements[3].depth);
}

TEST(HtmlScannerTest, HandlesNestedElements) {
  HtmlScanner scanner;
  std::string html =
      "<section id=\"hero\" class=\"header-section\">"
      "<div class=\"container\">"
      "<nav class=\"main-nav\">"
      "<ul><li><a href=\"/\">Home</a></li></ul>"
      "</nav>"
      "</div>"
      "</section>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(6u, result.elements.size());

  // section
  EXPECT_EQ("section", result.elements[0].tag_name);
  EXPECT_EQ("hero", result.elements[0].id);
  ASSERT_EQ(1u, result.elements[0].classes.size());
  EXPECT_EQ("header-section", result.elements[0].classes[0]);

  // div.container
  EXPECT_EQ("div", result.elements[1].tag_name);
  ASSERT_EQ(1u, result.elements[1].classes.size());
  EXPECT_EQ("container", result.elements[1].classes[0]);

  // nav.main-nav
  EXPECT_EQ("nav", result.elements[2].tag_name);
  ASSERT_EQ(1u, result.elements[2].classes.size());
  EXPECT_EQ("main-nav", result.elements[2].classes[0]);
}

TEST(HtmlScannerTest, HandlesElementsWithoutAttributes) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/", "<p>Plain paragraph</p>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  EXPECT_EQ("p", result.elements[0].tag_name);
  EXPECT_TRUE(result.elements[0].id.empty());
  EXPECT_TRUE(result.elements[0].classes.empty());
}

TEST(HtmlScannerTest, AssignsElementIndexesSequentially) {
  HtmlScanner scanner;
  std::string html = "<div></div><span></span><p></p><a></a>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.elements.size());
  EXPECT_EQ(0, result.elements[0].element_index);
  EXPECT_EQ(1, result.elements[1].element_index);
  EXPECT_EQ(2, result.elements[2].element_index);
  EXPECT_EQ(3, result.elements[3].element_index);
}

TEST(HtmlScannerTest, HandlesWhitespaceInClassAttribute) {
  HtmlScanner scanner;
  // Test various whitespace characters between classes
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<div class=\"  a  \t  b  \n  c  \">Content</div>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.elements.size());
  ASSERT_EQ(3u, result.elements[0].classes.size());
  EXPECT_EQ("a", result.elements[0].classes[0]);
  EXPECT_EQ("b", result.elements[0].classes[1]);
  EXPECT_EQ("c", result.elements[0].classes[2]);
}

TEST(HtmlScannerTest, ScanMultipleCalls) {
  // Test that multiple scan calls work correctly
  HtmlScanner scanner;

  HtmlScanResult result1 =
      scanner.Scan("http://a.com/", "<div id=\"a\"></div>");
  EXPECT_TRUE(result1.success);
  EXPECT_EQ("a", result1.elements[0].id);

  HtmlScanResult result2 =
      scanner.Scan("http://b.com/", "<div id=\"b\"></div>");
  EXPECT_TRUE(result2.success);
  EXPECT_EQ("b", result2.elements[0].id);
}

TEST(HtmlScannerTest, HandlesCompleteHtmlDocument) {
  HtmlScanner scanner;
  std::string html =
      "<!DOCTYPE html>\n"
      "<html lang=\"en\">\n"
      "<head>\n"
      "<meta charset=\"UTF-8\">\n"
      "<title>Test Page</title>\n"
      "<link rel=\"stylesheet\" href=\"/styles.css\">\n"
      "<style>body { margin: 0; }</style>\n"
      "</head>\n"
      "<body>\n"
      "<header id=\"header\" class=\"site-header\">\n"
      "<nav class=\"main-nav\">Nav</nav>\n"
      "</header>\n"
      "<main class=\"content\">Main content</main>\n"
      "<footer>Footer</footer>\n"
      "</body>\n"
      "</html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);

  // Check stylesheets
  ASSERT_EQ(1u, result.stylesheets.size());
  EXPECT_EQ("/styles.css", result.stylesheets[0].href);

  // Check inline CSS
  EXPECT_EQ("body { margin: 0; }", result.inline_css);

  // Check that key elements are found
  bool found_header = false;
  bool found_nav = false;
  bool found_main = false;
  bool found_footer = false;

  for (const auto& elem : result.elements) {
    if (elem.tag_name == "header" && elem.id == "header") {
      found_header = true;
      EXPECT_EQ(1u, elem.classes.size());
      EXPECT_EQ("site-header", elem.classes[0]);
    }
    if (elem.tag_name == "nav" && !elem.classes.empty() &&
        elem.classes[0] == "main-nav") {
      found_nav = true;
    }
    if (elem.tag_name == "main") {
      found_main = true;
    }
    if (elem.tag_name == "footer") {
      found_footer = true;
    }
  }

  EXPECT_TRUE(found_header);
  EXPECT_TRUE(found_nav);
  EXPECT_TRUE(found_main);
  EXPECT_TRUE(found_footer);
}

// ========== LCP Candidate Detection Tests ==========

TEST(HtmlScannerTest, DetectsLcpCandidateInHeroSection) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div>Some content</div>"
      "<section class=\"hero\">"
      "<img src=\"/images/hero.jpg\">"
      "</section>"
      "<img src=\"/images/other.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, DetectsLcpCandidateInHeader) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<header>"
      "<img src=\"/images/logo.png\">"
      "</header>"
      "<img src=\"/images/content.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/logo.png", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, FallsBackToFirstBodyImg) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"/images/first.jpg\"></div>"
      "<div><img src=\"/images/second.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/first.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, HeroCandidateOverridesFallback) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"/images/first.jpg\"></div>"
      "<section class=\"hero-banner\">"
      "<img src=\"/images/hero.jpg\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src)
      << "Hero candidate should override the fallback first-img candidate";
}

TEST(HtmlScannerTest, SkipsDataUrlsForLcp) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP\">"
      "<img src=\"/images/real.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/real.jpg", result.lcp_candidate.src)
      << "data: URLs should be skipped for LCP candidate";
}

TEST(HtmlScannerTest, NoLcpCandidateWithoutImages) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div>Just text content</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.lcp_candidate.src.empty());
}

TEST(HtmlScannerTest, DetectsLcpInArticleTag) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<article>"
      "<img src=\"/images/article-hero.jpg\">"
      "</article>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/article-hero.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, DetectsLcpBySrcset) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\">"
      "<img src=\"/images/hero.jpg\" srcset=\"/images/hero-2x.jpg 2x\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_EQ("/images/hero-2x.jpg 2x", result.lcp_candidate.srcset);
}

// ========== M1: LCP sizes attribute detection ==========

TEST(HtmlScannerTest, DetectsLcpWithSizes) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\">"
      "<img src=\"hero.jpg\" "
      "srcset=\"hero-800.jpg 800w\" "
      "sizes=\"(max-width: 600px) 100vw, 50vw\">"
      "</section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("hero.jpg", result.lcp_candidate.src);
  EXPECT_EQ("hero-800.jpg 800w", result.lcp_candidate.srcset);
  EXPECT_EQ("(max-width: 600px) 100vw, 50vw", result.lcp_candidate.sizes);
}

// ========== M2: Hero class word-boundary matching ==========

TEST(HtmlScannerTest, SuperheroClassNotTreatedAsHero) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div class=\"superhero-card\"><img src=\"villain.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "superhero-card" should NOT match hero pattern; the image should be
  // selected only as the first-body-img fallback, not as a hero candidate.
  EXPECT_EQ("villain.jpg", result.lcp_candidate.src);
}

TEST(HtmlScannerTest, HeroPrefixWithHyphenMatches) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"other.jpg\"></div>"
      "<section class=\"hero-section\"><img src=\"real-hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "hero-section" should match as hero (prefix + hyphen)
  EXPECT_EQ("real-hero.jpg", result.lcp_candidate.src)
      << "hero-section class should be detected as hero container";
}

TEST(HtmlScannerTest, CookiebannerNotTreatedAsHero) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div class=\"cookiebanner\"><img src=\"close.png\"></div>"
      "<div><img src=\"real.jpg\"></div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "cookiebanner" should NOT match "banner" pattern; fallback picks first img
  EXPECT_EQ("close.png", result.lcp_candidate.src)
      << "cookiebanner should not be treated as hero; first img is fallback";
}

TEST(HtmlScannerTest, CaseInsensitiveHeroClass) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"other.jpg\"></div>"
      "<section class=\"HERO\"><img src=\"upper.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("upper.jpg", result.lcp_candidate.src)
      << "HERO (uppercase) should match as hero container";
}

TEST(HtmlScannerTest, DetectsLcpInMainTag) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<main><img src=\"main-hero.jpg\"></main>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("main-hero.jpg", result.lcp_candidate.src)
      << "<main> should be detected as hero container";
}

TEST(HtmlScannerTest, NestedHeroContainerPropagates) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<main><section class=\"hero\"><div class=\"wrapper\">"
      "<img src=\"deep.jpg\">"
      "</div></section></main>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("deep.jpg", result.lcp_candidate.src)
      << "Hero ancestor status should propagate through nested containers";
}

TEST(HtmlScannerTest, MultipleHeroSectionsPicksFirst) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><img src=\"first-hero.jpg\"></section>"
      "<section class=\"hero\"><img src=\"second-hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("first-hero.jpg", result.lcp_candidate.src)
      << "First hero section image should be selected";
}

// ========== <picture> and non-LCP image handling ==========

TEST(HtmlScannerTest, LcpCandidateInsidePictureIsFlagged) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><picture>"
      "<source srcset=\"/images/hero.avif\" type=\"image/avif\">"
      "<img src=\"/images/hero.jpg\">"
      "</picture></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_TRUE(result.lcp_candidate.in_picture)
      << "An <img> child of <picture> must be flagged: a <source> sibling "
         "may win selection, so preloading its src risks a double download";
}

TEST(HtmlScannerTest, LcpCandidateOutsidePictureNotFlagged) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<section class=\"hero\"><img src=\"/images/hero.jpg\"></section>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src);
  EXPECT_FALSE(result.lcp_candidate.in_picture);
}

TEST(HtmlScannerTest, LcpFallbackSkipsTrackingPixel) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"/pixel.gif\" width=\"1\" height=\"1\">"
      "<img src=\"/images/real.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/real.jpg", result.lcp_candidate.src)
      << "A 1x1 beacon must not become the LCP candidate";
}

TEST(HtmlScannerTest, LcpHeroSkipsHiddenImage) {
  HtmlScanner scanner;
  std::string html =
      "<html><body><section class=\"hero\">"
      "<img src=\"/spacer.gif\" hidden>"
      "<img src=\"/lazy-placeholder.png\" style=\"display: none\">"
      "<img src=\"/images/hero.jpg\">"
      "</section></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("/images/hero.jpg", result.lcp_candidate.src)
      << "hidden / display:none images are not rendered and must not "
         "become the LCP candidate";
}

// A browser running scripts never creates an <img> inside
// <noscript> (its content is raw text there), so such an image cannot be the
// LCP element, and a preload or Early Hint for it fetches an image the page
// never shows. Here the hero container holds a <noscript> image and no
// placeholder before it (a no-JS fallback for a CSS background, say), so the
// next real image is the candidate; with the src-less placeholder in front it
// is the script-loaded hero, pinned by LcpHeroOnlyInsideNoscript. The
// candidate's element_index stays comparable with `elements`: the inert
// elements advance it like any other.
TEST(HtmlScannerTest, LcpSkipsImgInsideNoscript) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<html><body>"
                   "<div class=\"hero\">"
                   "<noscript><img src=\"/hero-noscript.jpg\" width=\"1200\" "
                   "height=\"600\"></noscript>"
                   "</div>"
                   "<main><img id=\"real\" src=\"/real.jpg\"></main>"
                   "</body></html>");

  ASSERT_TRUE(result.success);
  EXPECT_EQ("/real.jpg", result.lcp_candidate.src)
      << "the <noscript> image is not created by a scripting browser";
  EXPECT_FALSE(result.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) [noscript(3) img(4)] main(5) img(6).
  EXPECT_EQ(result.lcp_candidate.element_index, 6);
  bool found = false;
  for (const auto& e : result.elements) {
    if (e.id == "real") {
      found = true;
      EXPECT_EQ(e.element_index, result.lcp_candidate.element_index);
    }
  }
  EXPECT_TRUE(found);
}

// The script-loaded hero. The hero container holds a
// lazy-load placeholder (an <img> without src that the loader fills in) and
// its <noscript> copy, nothing else. The <noscript> image is never the
// candidate, and the page gets no candidate at all rather
// than the next image, which is probably below the fold: the hero is what the
// browser paints largest (Chromium 145 reports the loader-filled <img> as its
// LCP entry), and the markup does not say which bytes the loader fetches.
TEST(HtmlScannerTest, LcpHeroOnlyInsideNoscript) {
  HtmlScanner scanner;
  const std::string hero =
      "<div class=\"hero\">"
      "<img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\" width=\"1200\" height=\"600\">"
      "</noscript>"
      "</div>";

  HtmlScanResult none = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body>", hero, "<p>text</p></body></html>"));
  ASSERT_TRUE(none.success);
  EXPECT_TRUE(none.lcp_candidate.src.empty())
      << "got " << none.lcp_candidate.src;
  EXPECT_EQ(none.lcp_candidate.element_index, -1);
  EXPECT_TRUE(none.lcp_candidate.script_loaded_hero);

  HtmlScanResult next = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body>", hero,
                   "<div><img id=\"below\" src=\"/below.jpg\"></div>"
                   "</body></html>"));
  ASSERT_TRUE(next.success);
  EXPECT_TRUE(next.lcp_candidate.src.empty())
      << "the image after a script-loaded hero is not the candidate, got "
      << next.lcp_candidate.src;
  EXPECT_EQ(next.lcp_candidate.element_index, -1);
  EXPECT_TRUE(next.lcp_candidate.script_loaded_hero);

  // The decision drops a fallback found before the hero (a real hero image
  // would have overridden it) and rules out every image in a container that
  // was already open: here the <main> around everything, so /below.jpg and
  // /aside.jpg are not hinted either.
  HtmlScanResult wrapped = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body>"
                   "<div class=\"nav\"><img src=\"/logo.png\"></div>"
                   "<main>",
                   hero,
                   "<div><img src=\"/below.jpg\"></div>"
                   "<div class=\"grid\"><img src=\"/aside.jpg\"></div>"
                   "</main>"
                   "</body></html>"));
  ASSERT_TRUE(wrapped.success);
  EXPECT_TRUE(wrapped.lcp_candidate.src.empty())
      << "got " << wrapped.lcp_candidate.src;
  EXPECT_TRUE(wrapped.lcp_candidate.script_loaded_hero);

  // A hero container opened after the decision is new ground: its eligible
  // image is the candidate, and the decision is withdrawn.
  HtmlScanResult later_hero = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body>", hero,
                   "<div><img src=\"/below.jpg\"></div>"
                   "<section><img id=\"later\" src=\"/section.jpg\"></section>"
                   "</body></html>"));
  ASSERT_TRUE(later_hero.success);
  EXPECT_EQ("/section.jpg", later_hero.lcp_candidate.src);
  EXPECT_FALSE(later_hero.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) [noscript(4) img(5)] div(6) img(7)
  // section(8) img(9).
  EXPECT_EQ(later_hero.lcp_candidate.element_index, 9);
}

// <header> and every <section> are hero containers, so a
// lazy-loaded logo with its <noscript> copy alone in the <header> must not
// silence the real hero in the <section> or <main> after it. The decision
// made at the header's close yields to an eligible image whose innermost hero
// container was opened after the decision, whatever its depth, but not to one
// in a container already open at the decision.
TEST(HtmlScannerTest, LazyLogoInHeaderDoesNotSilenceTheRealHero) {
  HtmlScanner scanner;
  const std::string header =
      "<header><a href=\"/\"><img data-src=\"/logo.png\" class=\"lazy\"></a>"
      "<noscript><img src=\"/logo.png\"></noscript></header>";

  HtmlScanResult section =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body>", header,
                                "<section class=\"hero\"><img id=\"real\" "
                                "src=\"/real-hero.jpg\"></section>"
                                "</body></html>"));
  ASSERT_TRUE(section.success);
  EXPECT_EQ("/real-hero.jpg", section.lcp_candidate.src);
  EXPECT_FALSE(section.lcp_candidate.script_loaded_hero);
  // html(0) body(1) header(2) a(3) img(4) [noscript(5) img(6)] section(7)
  // img(8).
  EXPECT_EQ(section.lcp_candidate.element_index, 8);
  bool found = false;
  for (const auto& e : section.elements) {
    if (e.id == "real") {
      found = true;
      EXPECT_EQ(e.element_index, section.lcp_candidate.element_index);
    }
  }
  EXPECT_TRUE(found);

  HtmlScanResult main_after =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body>", header,
                                "<main><img src=\"/real-hero.jpg\"></main>"
                                "</body></html>"));
  ASSERT_TRUE(main_after.success);
  EXPECT_EQ("/real-hero.jpg", main_after.lcp_candidate.src);
  EXPECT_FALSE(main_after.lcp_candidate.script_loaded_hero);

  // The later container has to be a hero container: an image in a plain
  // <div> after the header is the fallback the decision rules out.
  HtmlScanResult plain_div = scanner.Scan(
      "http://example.com/", absl::StrCat("<html><body>", header,
                                          "<div><img src=\"/below.jpg\"></div>"
                                          "</body></html>"));
  ASSERT_TRUE(plain_div.success);
  EXPECT_TRUE(plain_div.lcp_candidate.src.empty())
      << "got " << plain_div.lcp_candidate.src;
  EXPECT_TRUE(plain_div.lcp_candidate.script_loaded_hero);

  // "Opened after the decision" is about when the container opened, at any
  // depth. A sibling <section> inside the same <main> is such a container
  // (its own landmark says hero, and this includes a content section below
  // the fold), while an image directly in that <main> is not (pinned above,
  // `wrapped` in LcpHeroOnlyInsideNoscript).
  HtmlScanResult nested = scanner.Scan(
      "http://example.com/",
      "<html><body><main>"
      "<div class=\"hero\"><img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\"></noscript></div>"
      "<section class=\"features\"><img src=\"/feature.jpg\"></section>"
      "</main></body></html>");
  ASSERT_TRUE(nested.success);
  EXPECT_EQ("/feature.jpg", nested.lcp_candidate.src);
  EXPECT_FALSE(nested.lcp_candidate.script_loaded_hero);

  // A fresh sibling container shallower than the decided one is fresh all
  // the same (review R1): the decision is made at the nested container's
  // close, and the <section> after its wrapper opened later.
  HtmlScanResult shallower = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<main><div class=\"hero\"><img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\"></noscript></div></main>"
      "<section class=\"hero\"><img src=\"/real-hero.jpg\"></section>"
      "</body></html>");
  ASSERT_TRUE(shallower.success);
  EXPECT_EQ("/real-hero.jpg", shallower.lcp_candidate.src);
  EXPECT_FALSE(shallower.lcp_candidate.script_loaded_hero);

  HtmlScanResult banner_wrapper = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<header><div class=\"banner\"><a href=\"/\"><img data-src=\"/logo.png\" "
      "class=\"lazy\"></a><noscript><img src=\"/logo.png\"></noscript></div>"
      "</header>"
      "<section class=\"hero\"><img src=\"/real-hero.jpg\"></section>"
      "</body></html>");
  ASSERT_TRUE(banner_wrapper.success);
  EXPECT_EQ("/real-hero.jpg", banner_wrapper.lcp_candidate.src);
  EXPECT_FALSE(banner_wrapper.lcp_candidate.script_loaded_hero);

  // The outer <section>'s own image /x.jpg is in a container already open at
  // the decision and stays ruled out; the next <section> is fresh and its
  // /y.jpg is the candidate.
  HtmlScanResult outer_then_sibling = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<section><section><img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\"></noscript></section>"
      "<img src=\"/x.jpg\"></section>"
      "<section><img src=\"/y.jpg\"></section>"
      "</body></html>");
  ASSERT_TRUE(outer_then_sibling.success);
  EXPECT_EQ("/y.jpg", outer_then_sibling.lcp_candidate.src);
  EXPECT_FALSE(outer_then_sibling.lcp_candidate.script_loaded_hero);

  // The <img>'s own hero class is not a container that could hold the
  // <noscript> copy: such a placeholder starts nothing.
  HtmlScanResult own_class =
      scanner.Scan("http://example.com/",
                   "<html><body>"
                   "<img class=\"hero lazy\" data-src=\"/hero.jpg\">"
                   "<noscript><img src=\"/hero.jpg\"></noscript>"
                   "<div><img src=\"/below.jpg\"></div>"
                   "</body></html>");
  ASSERT_TRUE(own_class.success);
  EXPECT_EQ("/below.jpg", own_class.lcp_candidate.src);
  EXPECT_FALSE(own_class.lcp_candidate.script_loaded_hero);
}

// The rule is narrow. A lazy-load placeholder outside a hero
// container, or one without the <noscript> copy, or one whose container also
// holds an eligible image of its own, is not a script-loaded hero: the next
// eligible image (or that image) is the candidate, as before.
TEST(HtmlScannerTest, LazyPlaceholderOutsideHeroKeepsTheNextImage) {
  HtmlScanner scanner;

  // Not in a hero container: the placeholder's <div> has no hero class and
  // no landmark ancestor.
  HtmlScanResult not_hero = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<div class=\"slot\">"
      "<img data-src=\"/ad.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/ad.jpg\" width=\"300\" height=\"250\">"
      "</noscript>"
      "</div>"
      "<div><img id=\"real\" src=\"/real.jpg\"></div>"
      "</body></html>");
  ASSERT_TRUE(not_hero.success);
  EXPECT_EQ("/real.jpg", not_hero.lcp_candidate.src);
  EXPECT_FALSE(not_hero.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) [noscript(4) img(5)] div(6) img(7).
  EXPECT_EQ(not_hero.lcp_candidate.element_index, 7);
  bool found = false;
  for (const auto& e : not_hero.elements) {
    if (e.id == "real") {
      found = true;
      EXPECT_EQ(e.element_index, not_hero.lcp_candidate.element_index);
    }
  }
  EXPECT_TRUE(found);

  // In a hero container but without the <noscript> copy: not the pattern.
  HtmlScanResult no_twin =
      scanner.Scan("http://example.com/",
                   "<html><body>"
                   "<div class=\"hero\"><img data-src=\"/hero.jpg\" "
                   "class=\"lazy\"></div>"
                   "<div><img src=\"/next.jpg\"></div>"
                   "</body></html>");
  ASSERT_TRUE(no_twin.success);
  EXPECT_EQ("/next.jpg", no_twin.lcp_candidate.src);
  EXPECT_FALSE(no_twin.lcp_candidate.script_loaded_hero);

  // The hero container holds an eligible image of its own after the
  // placeholder: that image is the hero candidate.
  HtmlScanResult own_image =
      scanner.Scan("http://example.com/",
                   "<html><body>"
                   "<section class=\"hero\">"
                   "<img data-src=\"/deco.jpg\" class=\"lazy\">"
                   "<noscript><img src=\"/deco.jpg\"></noscript>"
                   "<img src=\"/hero.jpg\">"
                   "</section>"
                   "<div><img src=\"/below.jpg\"></div>"
                   "</body></html>");
  ASSERT_TRUE(own_image.success);
  EXPECT_EQ("/hero.jpg", own_image.lcp_candidate.src);
  EXPECT_FALSE(own_image.lcp_candidate.script_loaded_hero);

  // A page without any placeholder whose first eligible image is simply
  // later in the document gets that image, exactly as before.
  HtmlScanResult plain = scanner.Scan("http://example.com/",
                                      "<html><body>"
                                      "<div class=\"hero\"><h1>Title</h1></div>"
                                      "<div><img src=\"/later.jpg\"></div>"
                                      "</body></html>");
  ASSERT_TRUE(plain.success);
  EXPECT_EQ("/later.jpg", plain.lcp_candidate.src);
  EXPECT_FALSE(plain.lcp_candidate.script_loaded_hero);
}

// What counts as a lazy-load placeholder. No usable src (absent,
// empty or a data: stand-in) plus a data-* attribute naming the real source
// or a lazy-loader class; the <noscript> copy follows it in the same hero
// container. A src-less <img> with neither marker is not one. Since the
// stand-in placeholder rule an
// <img> with a lazy data attribute and a real src is one too once the copy's
// src differs (`/plain.jpg` here; LazyPlaceholderStandInSrc has the shapes),
// while a lazy class alone leaves a real src the hero.
TEST(HtmlScannerTest, LazyPlaceholderShapes) {
  HtmlScanner scanner;
  auto scan = [&](std::string_view img) {
    return scanner.Scan(
        "http://example.com/",
        absl::StrCat("<html><body><header>", img,
                     "<noscript><img src=\"/hero.jpg\"></noscript></header>"
                     "<div><img src=\"/below.jpg\"></div></body></html>"));
  };

  for (std::string_view img : {
           "<img data-lazy-src=\"/hero.jpg\">",
           "<img data-srcset=\"/hero.jpg 1200w\" data-sizes=\"auto\">",
           "<img data-original=\"/hero.jpg\">",
           "<img src=\"\" data-src=\"/hero.jpg\">",
           "<img src=\"data:image/gif;base64,R0lGOD\" data-src=\"/hero.jpg\">",
           "<img class=\"lazyload\" data-bg=\"/hero.jpg\">",
           "<img class=\"img-fluid lazy-img\">",
           "<img src=\"/plain.jpg\" data-src=\"/hero.jpg\">",
       }) {
    HtmlScanResult result = scan(img);
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.lcp_candidate.script_loaded_hero) << img;
    EXPECT_TRUE(result.lcp_candidate.src.empty()) << img;
  }

  for (std::string_view img : {
           "<img alt=\"no source at all\">",
           "<img class=\"lazysizes-off\">",
           "<img src=\"/plain.jpg\" class=\"lazy\">",
           "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">",
       }) {
    HtmlScanResult result = scan(img);
    ASSERT_TRUE(result.success);
    EXPECT_FALSE(result.lcp_candidate.script_loaded_hero) << img;
    EXPECT_FALSE(result.lcp_candidate.src.empty()) << img;
  }

  // A hidden placeholder (the markup says it does not render) is not the
  // hero, so it starts nothing.
  HtmlScanResult hidden = scan("<img data-src=\"/hero.jpg\" hidden>");
  ASSERT_TRUE(hidden.success);
  EXPECT_FALSE(hidden.lcp_candidate.script_loaded_hero);
  EXPECT_EQ("/below.jpg", hidden.lcp_candidate.src);
}

// A lazy-load placeholder may carry a real-looking src. A
// stand-in file name (`/blank.gif`) next to a lazy data attribute is a
// placeholder outright, twin or not; any other src with a lazy data attribute
// is the candidate as before until its <noscript> copy turns out to carry a
// different src, which withdraws it. The twin leg applies inside a hero
// container, where the pattern lives; outside one only the name leg does.
TEST(HtmlScannerTest, LazyPlaceholderStandInSrc) {
  HtmlScanner scanner;
  const std::string twin = "<noscript><img src=\"/hero.jpg\"></noscript>";
  const std::string below = "<div><img src=\"/below.jpg\"></div>";

  // The issue's fixture (review probe P3b): before the fix /blank.gif was the
  // candidate and got the preload.
  HtmlScanResult blank = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/blank.gif\" data-src=\"/hero.jpg\" "
                   "class=\"lazy\">",
                   twin, "</div>", below, "</body></html>"));
  ASSERT_TRUE(blank.success);
  EXPECT_TRUE(blank.lcp_candidate.src.empty()) << blank.lcp_candidate.src;
  EXPECT_EQ(blank.lcp_candidate.element_index, -1);
  EXPECT_TRUE(blank.lcp_candidate.script_loaded_hero);

  // A stand-in name without the twin is still a placeholder: the next
  // eligible image is the candidate, as for a src-less one (index pinned).
  HtmlScanResult named_no_twin =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body><div class=\"hero\">"
                                "<img src=\"/img/placeholder-300x200.png\" "
                                "data-src=\"/hero.jpg\"></div>",
                                below, "</body></html>"));
  ASSERT_TRUE(named_no_twin.success);
  EXPECT_EQ("/below.jpg", named_no_twin.lcp_candidate.src);
  EXPECT_FALSE(named_no_twin.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) div(4) img(5).
  EXPECT_EQ(named_no_twin.lcp_candidate.element_index, 5);

  // The name leg, token by token: whole stand-in names, sizes and numbers
  // match; a name that merely contains such a word does not.
  for (std::string_view src : {
           "/blank.gif",
           "/assets/Placeholder.PNG?v=3",
           "/lazy_pixel.svg#x",
           "/1x1.png",
           "/transparent-1x1.gif",
           "/spacer2.gif",
           "/loading.gif",
           "https://cdn.example.com/i/lazy-placeholder-640x360.jpg",
       }) {
    EXPECT_TRUE(IsLazyStandInSrc(src)) << src;
  }
  for (std::string_view src : {
           "/lazy-river.jpg",
           "/hero-loading-dock.jpg",
           "/blankets.jpg",
           "/placeholder/hero.jpg",
           "/img/300x200.jpg",
           "/.gif",
           "",
       }) {
    EXPECT_FALSE(IsLazyStandInSrc(src)) << src;
  }

  // A lazy data attribute with a real src and no twin stays the candidate,
  // exactly as before (index pinned).
  HtmlScanResult real_no_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\"></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(real_no_twin.success);
  EXPECT_EQ("/hero-lqip.jpg", real_no_twin.lcp_candidate.src);
  EXPECT_FALSE(real_no_twin.lcp_candidate.script_loaded_hero);
  EXPECT_EQ(real_no_twin.lcp_candidate.element_index, 3);

  // With a twin whose src differs, the twin names the real image and the
  // <img> was a stand-in: no candidate.
  HtmlScanResult real_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\">",
                   twin, "</div>", below, "</body></html>"));
  ASSERT_TRUE(real_twin.success);
  EXPECT_TRUE(real_twin.lcp_candidate.src.empty())
      << real_twin.lcp_candidate.src;
  EXPECT_TRUE(real_twin.lcp_candidate.script_loaded_hero);

  // A twin with the same src confirms nothing: the <img> is the hero. "Same"
  // is the resolved reference, so an absolute spelling of the page's own
  // URL, or a `./` one, is the same image; a different
  // query is not known to be.
  for (std::string_view twin_src : {
           "/hero.jpg",
           "http://example.com/hero.jpg",
           "HTTP://EXAMPLE.COM/hero.jpg#top",
           "./hero.jpg",
           "//example.com/hero.jpg",
       }) {
    HtmlScanResult same_twin = scanner.Scan(
        "http://example.com/",
        absl::StrCat("<html><body><div class=\"hero\">"
                     "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">"
                     "<noscript><img src=\"",
                     twin_src, "\"></noscript></div>", below,
                     "</body></html>"));
    ASSERT_TRUE(same_twin.success);
    EXPECT_EQ("/hero.jpg", same_twin.lcp_candidate.src) << twin_src;
    EXPECT_FALSE(same_twin.lcp_candidate.script_loaded_hero) << twin_src;
    EXPECT_EQ(same_twin.lcp_candidate.element_index, 3) << twin_src;
  }
  HtmlScanResult query_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">"
                   "<noscript><img src=\"/hero.jpg?v=2\"></noscript></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(query_twin.success);
  EXPECT_TRUE(query_twin.lcp_candidate.script_loaded_hero);

  // Under a cross-host <base href> a root-relative src fetches from the
  // base's host, as the browser resolves it: the CDN spelling of the same
  // path is the same image, the page host's is not.
  auto with_base = [&](std::string_view twin_src) {
    return scanner.Scan(
        "http://example.com/",
        absl::StrCat("<html><head><base href=\"https://cdn.example.com/"
                     "assets/\"></head><body><div class=\"hero\">"
                     "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">"
                     "<noscript><img src=\"",
                     twin_src, "\"></noscript></div>", below,
                     "</body></html>"));
  };
  HtmlScanResult cdn_twin = with_base("https://cdn.example.com/hero.jpg");
  ASSERT_TRUE(cdn_twin.success);
  EXPECT_EQ("/hero.jpg", cdn_twin.lcp_candidate.src);
  EXPECT_FALSE(cdn_twin.lcp_candidate.script_loaded_hero);
  HtmlScanResult page_host_twin = with_base("http://example.com/hero.jpg");
  ASSERT_TRUE(page_host_twin.success);
  EXPECT_TRUE(page_host_twin.lcp_candidate.script_loaded_hero)
      << page_host_twin.lcp_candidate.src;
  // A root-relative <base href> has no host of its own, so a hostless src
  // keeps the page's host: the page host's spelling is the same image.
  HtmlScanResult sub_base_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><head><base href=\"/sub/\"></head><body>"
                   "<div class=\"hero\">"
                   "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">"
                   "<noscript><img src=\"http://example.com/hero.jpg\">"
                   "</noscript></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(sub_base_twin.success);
  EXPECT_EQ("/hero.jpg", sub_base_twin.lcp_candidate.src);
  EXPECT_FALSE(sub_base_twin.lcp_candidate.script_loaded_hero);

  // The copy has to be plausible: a <noscript> image the markup declares
  // invisible (a tracking pixel's no-JS fallback in the same landmark, a
  // hidden image) is not the author's copy of the hero, so the <img> stays
  // the candidate. Before the review fix the pixel
  // confirmed the pattern and the page got no candidate.
  for (std::string_view page : {
           "<html><body><main>"
           "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\" class=\"lazyload\">"
           "<p>text</p><noscript><img src=\"https://px.example/tr?id=1\" "
           "width=\"1\" height=\"1\"></noscript></main></body></html>",
           "<html><body><div class=\"hero\">"
           "<img src=\"/hero.jpg\" data-src=\"/hero.jpg\">"
           "<noscript><img src=\"/other.jpg\" hidden></noscript></div>"
           "<div><img src=\"/below.jpg\"></div></body></html>",
       }) {
    HtmlScanResult pixel = scanner.Scan("http://example.com/", page);
    ASSERT_TRUE(pixel.success);
    EXPECT_EQ("/hero.jpg", pixel.lcp_candidate.src) << page;
    EXPECT_FALSE(pixel.lcp_candidate.script_loaded_hero) << page;
    EXPECT_EQ(pixel.lcp_candidate.element_index, 3) << page;
  }
  // The same for a src-less placeholder: a 1x1 copy is no
  // copy, so the pattern is not confirmed and the next image is the
  // candidate, as for a placeholder without a copy (review probe P22, which
  // gave no candidate before).
  HtmlScanResult pixel_copy = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img data-src=\"/hero.jpg\" class=\"lazy\">"
                   "<noscript><img src=\"/px.gif\" width=\"1\" height=\"1\">"
                   "</noscript></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(pixel_copy.success);
  EXPECT_EQ("/below.jpg", pixel_copy.lcp_candidate.src);
  EXPECT_FALSE(pixel_copy.lcp_candidate.script_loaded_hero);

  // A lazy class alone says nothing about where the real source is: with a
  // real src the <img> is the hero, twin or not, even when the name looks
  // like a stand-in (the rule asks for the data attribute).
  HtmlScanResult class_only =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body><div class=\"hero\">"
                                "<img src=\"/blank.gif\" class=\"lazy\">",
                                twin, "</div>", below, "</body></html>"));
  ASSERT_TRUE(class_only.success);
  EXPECT_EQ("/blank.gif", class_only.lcp_candidate.src);
  EXPECT_FALSE(class_only.lcp_candidate.script_loaded_hero);

  // The container holds an eligible image of its own after the withdrawn
  // stand-in: that image is the hero candidate, as for a src-less placeholder.
  HtmlScanResult own_image = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\">",
                   twin, "<img src=\"/real.jpg\"></div>", below,
                   "</body></html>"));
  ASSERT_TRUE(own_image.success);
  EXPECT_EQ("/real.jpg", own_image.lcp_candidate.src);
  EXPECT_FALSE(own_image.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) [noscript(4) img(5)] img(6).
  EXPECT_EQ(own_image.lcp_candidate.element_index, 6);

  // An eligible image before the twin arrives keeps the stand-in as the
  // candidate (the container holds more than the placeholder, so the pattern
  // is off, and the stand-in was the first eligible image), as before.
  HtmlScanResult image_before_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\">"
                   "<img src=\"/real.jpg\">",
                   twin, "</div>", below, "</body></html>"));
  ASSERT_TRUE(image_before_twin.success);
  EXPECT_EQ("/hero-lqip.jpg", image_before_twin.lcp_candidate.src);
  EXPECT_FALSE(image_before_twin.lcp_candidate.script_loaded_hero);

  // Outside a hero container the name leg applies (the stand-in is skipped
  // and the next image is the candidate, as for a src-less placeholder), the
  // twin leg does not (a real src with a lazy data attribute stays the
  // fallback candidate).
  HtmlScanResult outside_named = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"slot\">"
                   "<img src=\"/blank.gif\" data-src=\"/ad.jpg\">"
                   "<noscript><img src=\"/ad.jpg\"></noscript></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(outside_named.success);
  EXPECT_EQ("/below.jpg", outside_named.lcp_candidate.src);
  EXPECT_FALSE(outside_named.lcp_candidate.script_loaded_hero);
  HtmlScanResult outside_real = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"slot\">"
                   "<img src=\"/ad-lqip.jpg\" data-src=\"/ad.jpg\">"
                   "<noscript><img src=\"/ad.jpg\"></noscript></div>",
                   below, "</body></html>"));
  ASSERT_TRUE(outside_real.success);
  EXPECT_EQ("/ad-lqip.jpg", outside_real.lcp_candidate.src);
  EXPECT_FALSE(outside_real.lcp_candidate.script_loaded_hero);

  // A fallback found before the hero is dropped by the decision, as for a
  // src-less placeholder.
  HtmlScanResult with_logo = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"nav\"><img src=\"/logo.png\">"
                   "</div><div class=\"hero\">"
                   "<img src=\"/blank.gif\" data-src=\"/hero.jpg\">",
                   twin, "</div>", below, "</body></html>"));
  ASSERT_TRUE(with_logo.success);
  EXPECT_TRUE(with_logo.lcp_candidate.src.empty())
      << with_logo.lcp_candidate.src;
  EXPECT_TRUE(with_logo.lcp_candidate.script_loaded_hero);
}

// A small eligible image inside the placeholder's hero
// container (an icon in the call-to-action) neither cancels the script-loaded
// hero pattern nor becomes the candidate. Small is a declared width x height
// under kSmallImageAreaPx2 (10,000: below 100x100); an image whose size is
// unknown or at the threshold cancels the pattern as before. On a page
// without the <noscript> copy, or without a placeholder at all, the icon is
// what the first-eligible-image rule picked before and still does.
TEST(HtmlScannerTest, SmallIconInsideLazyHeroIsNotTheCandidate) {
  HtmlScanner scanner;
  const std::string ph =
      "<img data-src=\"/hero.jpg\" class=\"lazy\">"
      "<noscript><img src=\"/hero.jpg\" width=\"1200\" height=\"600\">"
      "</noscript>";
  const std::string below = "<div><img src=\"/below.jpg\"></div>";
  auto icon = [](std::string_view attrs) {
    return absl::StrCat("<div class=\"cta\"><img src=\"/icon.png\" ", attrs,
                        "></div>");
  };

  // The issue's fixtures (review probes P15b and P15): before the fix
  // /icon.png was the candidate.
  for (std::string_view attrs : {
           "width=\"48\" height=\"48\"",
           "width=\"16\" height=\"16\"",
           "width=\"99\" height=\"99\"",
           "width=\"200\" height=\"40\"",
           "width=\"48px\" height=\" 48 \"",
           "style=\"width: 48px; height: 48px\"",
           "style=\"width:48px\" height=\"48\"",
           "style=\"width:600px;width:48px\" height=\"48\"",
       }) {
    HtmlScanResult result = scanner.Scan(
        "http://example.com/",
        absl::StrCat("<html><body><div class=\"hero\">", ph, icon(attrs),
                     "</div>", below, "</body></html>"));
    ASSERT_TRUE(result.success);
    EXPECT_TRUE(result.lcp_candidate.src.empty())
        << attrs << " got " << result.lcp_candidate.src;
    EXPECT_EQ(result.lcp_candidate.element_index, -1) << attrs;
    EXPECT_TRUE(result.lcp_candidate.script_loaded_hero) << attrs;
  }

  // At or above the threshold, or without a declared size on both axes, the
  // image is the container's own and cancels the pattern as before.
  for (std::string_view attrs : {
           "width=\"100\" height=\"100\"",
           "width=\"200\" height=\"50\"",
           "width=\"48\"",
           "alt=\"icon\"",
           "width=\"50%\" height=\"48\"",
           "style=\"width:auto\" width=\"48\" height=\"48\"",
           "style=\"max-width:48px\" width=\"48\"",
       }) {
    HtmlScanResult result = scanner.Scan(
        "http://example.com/",
        absl::StrCat("<html><body><div class=\"hero\">", ph, icon(attrs),
                     "</div>", below, "</body></html>"));
    ASSERT_TRUE(result.success);
    EXPECT_EQ("/icon.png", result.lcp_candidate.src) << attrs;
    EXPECT_FALSE(result.lcp_candidate.script_loaded_hero) << attrs;
  }

  // The icon before the copy is tolerated too.
  HtmlScanResult icon_first = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img data-src=\"/hero.jpg\" class=\"lazy\">",
                   icon("width=\"48\" height=\"48\""),
                   "<noscript><img src=\"/hero.jpg\"></noscript></div>", below,
                   "</body></html>"));
  ASSERT_TRUE(icon_first.success);
  EXPECT_TRUE(icon_first.lcp_candidate.src.empty())
      << icon_first.lcp_candidate.src;
  EXPECT_TRUE(icon_first.lcp_candidate.script_loaded_hero);

  // The general ranking is untouched: without a placeholder, an icon that is
  // the first eligible image in the hero container is the candidate exactly
  // as before (index pinned).
  HtmlScanResult no_placeholder = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\"><h1>Title</h1>",
                   icon("width=\"48\" height=\"48\""), "</div>", below,
                   "</body></html>"));
  ASSERT_TRUE(no_placeholder.success);
  EXPECT_EQ("/icon.png", no_placeholder.lcp_candidate.src);
  EXPECT_FALSE(no_placeholder.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) h1(3) div(4) img(5).
  EXPECT_EQ(no_placeholder.lcp_candidate.element_index, 5);

  // With a placeholder but no copy the pattern is not confirmed, and the icon
  // is the candidate as before: set aside, then taken at the container's
  // close (index pinned), or when a larger image of the container's own
  // arrives (that image is not the first eligible one).
  HtmlScanResult no_twin =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body><div class=\"hero\">"
                                "<img data-src=\"/hero.jpg\" class=\"lazy\">",
                                icon("width=\"48\" height=\"48\""), "</div>",
                                below, "</body></html>"));
  ASSERT_TRUE(no_twin.success);
  EXPECT_EQ("/icon.png", no_twin.lcp_candidate.src);
  EXPECT_FALSE(no_twin.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) div(4) img(5).
  EXPECT_EQ(no_twin.lcp_candidate.element_index, 5);
  HtmlScanResult no_twin_then_big = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img data-src=\"/hero.jpg\" class=\"lazy\">",
                   icon("width=\"48\" height=\"48\""),
                   "<img src=\"/big.jpg\"></div>", below, "</body></html>"));
  ASSERT_TRUE(no_twin_then_big.success);
  EXPECT_EQ("/icon.png", no_twin_then_big.lcp_candidate.src);
  EXPECT_FALSE(no_twin_then_big.lcp_candidate.script_loaded_hero);

  // With the copy seen, a larger image of the container's own cancels the
  // pattern and is the candidate; the icon stays set aside.
  HtmlScanResult twin_then_big = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">", ph,
                   icon("width=\"48\" height=\"48\""),
                   "<img src=\"/big.jpg\"></div>", below, "</body></html>"));
  ASSERT_TRUE(twin_then_big.success);
  EXPECT_EQ("/big.jpg", twin_then_big.lcp_candidate.src);
  EXPECT_FALSE(twin_then_big.lcp_candidate.script_loaded_hero);
  // html(0) body(1) div(2) img(3) [noscript(4) img(5)] div(6) img(7) img(8).
  EXPECT_EQ(twin_then_big.lcp_candidate.element_index, 8);

  // The stand-in shapes get the same tolerance.
  HtmlScanResult stand_in = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/blank.gif\" data-src=\"/hero.jpg\">"
                   "<noscript><img src=\"/hero.jpg\"></noscript>",
                   icon("width=\"48\" height=\"48\""), "</div>", below,
                   "</body></html>"));
  ASSERT_TRUE(stand_in.success);
  EXPECT_TRUE(stand_in.lcp_candidate.src.empty()) << stand_in.lcp_candidate.src;
  EXPECT_TRUE(stand_in.lcp_candidate.script_loaded_hero);
  HtmlScanResult lqip = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\">"
                   "<noscript><img src=\"/hero.jpg\"></noscript>",
                   icon("width=\"48\" height=\"48\""), "</div>", below,
                   "</body></html>"));
  ASSERT_TRUE(lqip.success);
  EXPECT_TRUE(lqip.lcp_candidate.src.empty()) << lqip.lcp_candidate.src;
  EXPECT_TRUE(lqip.lcp_candidate.script_loaded_hero);
  // Without the copy the provisional stand-in is the candidate, not the icon.
  HtmlScanResult lqip_no_twin = scanner.Scan(
      "http://example.com/",
      absl::StrCat("<html><body><div class=\"hero\">"
                   "<img src=\"/hero-lqip.jpg\" data-src=\"/hero.jpg\">",
                   icon("width=\"48\" height=\"48\""), "</div>", below,
                   "</body></html>"));
  ASSERT_TRUE(lqip_no_twin.success);
  EXPECT_EQ("/hero-lqip.jpg", lqip_no_twin.lcp_candidate.src);
  EXPECT_FALSE(lqip_no_twin.lcp_candidate.script_loaded_hero);

  // A hidden image is not small, it is not eligible at all: skipped as
  // before, whatever its size.
  HtmlScanResult hidden =
      scanner.Scan("http://example.com/",
                   absl::StrCat("<html><body><div class=\"hero\">", ph,
                                icon("width=\"400\" height=\"300\" hidden"),
                                "</div>", below, "</body></html>"));
  ASSERT_TRUE(hidden.success);
  EXPECT_TRUE(hidden.lcp_candidate.src.empty()) << hidden.lcp_candidate.src;
  EXPECT_TRUE(hidden.lcp_candidate.script_loaded_hero);
}

// <template>, <noembed> and <noframes> content is never rendered
// either (the scanner's `inert` rule), so an <img> in there is no candidate.
TEST(HtmlScannerTest, LcpSkipsImgInsideTemplateNoembedNoframes) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<html><body>"
                   "<template><img src=\"/t.jpg\"></template>"
                   "<noembed><img src=\"/ne.jpg\"></noembed>"
                   "<noframes><img src=\"/nf.jpg\"></noframes>"
                   "<img id=\"real\" src=\"/real.jpg\">"
                   "</body></html>");

  ASSERT_TRUE(result.success);
  EXPECT_EQ("/real.jpg", result.lcp_candidate.src);
  // html(0) body(1) [template(2) img(3) noembed(4) img(5) noframes(6)
  // img(7)] img(8).
  EXPECT_EQ(result.lcp_candidate.element_index, 8);
  ASSERT_FALSE(result.elements.empty());
  EXPECT_EQ(result.elements.back().id, "real");
  EXPECT_EQ(result.elements.back().element_index, 8);
}

// The scanner's inert scope is the lexer's markup parse of
// <noscript> (kSometimesLiteralTags in lib/html/html_lexer.cc), so a nested
// <noscript> closes only the inner element and /outer.jpg stays inside the
// outer one. A browser's RAWTEXT rule ends the outer <noscript> at the FIRST
// `</noscript>` byte sequence, so Chromium 145 creates /outer.jpg
// (document.images = [/outer.jpg, /real.jpg]). This test pins the scanner's
// side of that divergence: the longer scope skips an image the browser does
// create and the next real image is the candidate, which is the less harmful
// side.
TEST(HtmlScannerTest, LcpNestedNoscriptScopeIsTheLexersNotTheBrowsers) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<noscript><noscript><img src=\"/inner.jpg\"></noscript>"
      "<img src=\"/outer.jpg\" width=\"1200\" height=\"600\"></noscript>"
      "<main><img id=\"real\" src=\"/real.jpg\"></main>"
      "</body></html>");

  ASSERT_TRUE(result.success);
  EXPECT_EQ("/real.jpg", result.lcp_candidate.src);
  // html(0) body(1) [noscript(2) noscript(3) img(4) img(5)] main(6) img(7).
  EXPECT_EQ(result.lcp_candidate.element_index, 7);
  ASSERT_FALSE(result.elements.empty());
  EXPECT_EQ(result.elements.back().id, "real");
  EXPECT_EQ(result.elements.back().element_index, 7);
}

// The fallback considers the first 50 body images; images a
// scripting browser never creates do not use up that window.
TEST(HtmlScannerTest, InertImagesDoNotCountTowardLcpFallbackWindow) {
  HtmlScanner scanner;
  std::string html = "<html><body>";
  for (int i = 0; i < 50; ++i) {
    html += "<noscript><img src=\"/n.jpg\"></noscript>";
  }
  html += "<img src=\"/real.jpg\"></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  ASSERT_TRUE(result.success);
  EXPECT_EQ("/real.jpg", result.lcp_candidate.src);
  // html(0) body(1), then 50 x [noscript img] = 100 inert elements.
  EXPECT_EQ(result.lcp_candidate.element_index, 102);
}

// ========== Third-Party Origin Extraction Tests ==========

TEST(HtmlScannerTest, ExtractsThirdPartyOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">"
      "<script src=\"https://analytics.example.com/track.js\"></script>"
      "</head><body>"
      "<img src=\"https://images.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.third_party_origins.size());
  // Render-blocking resources should come first (stylesheet, sync script).
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
  EXPECT_EQ("https://analytics.example.com",
            result.third_party_origins[1].origin);
  EXPECT_EQ("https://images.example.com", result.third_party_origins[2].origin);
}

// Preconnect ordering must not flap between the raw and reprocessed passes.
// Origins are emitted best-first (stable_sort on a priority bucket), and a
// cross-origin stylesheet earns the render-blocking bucket. Once deferred, that
// same <link> reads rel="preload" — so unless the classifier recognises the
// deferred primary, the reprocessed pass drops it to the ordinary head-resource
// bucket and the preconnect list comes out in a different ORDER than the raw
// pass produced. Same page, same resources, different hints depending on who
// last processed it.
//
// The <link rel="icon"> is what makes the demotion observable and must come
// FIRST: it is a head resource in both worlds, so on a correct scan the
// stylesheet's higher bucket lifts it above the icon, while a demoted
// stylesheet ties with the icon and the stable sort leaves the icon in front.
// (An image would not work here — body resources sort after head ones either
// way, so the order would be identical under the bug.)
TEST(HtmlScannerTest, DeferredPrimaryKeepsItsRenderBlockingPreconnectRank) {
  HtmlScanner scanner;
  const char* kRawSheet =
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/style.css\">";
  const char* kDeferredSheet =
      "<link rel=\"preload\" as=\"style\" "
      "href=\"https://cdn.example.com/style.css\" "
      "data-pagespeed-media=\"all\" data-pagespeed-async=\"\">";

  for (const char* sheet : {kRawSheet, kDeferredSheet}) {
    std::string html =
        std::string("<html><head>") +
        "<link rel=\"icon\" href=\"https://icons.example.com/f.ico\">" + sheet +
        "</head><body>Hello</body></html>";

    HtmlScanResult result = scanner.Scan("http://example.com/", html);

    ASSERT_TRUE(result.success) << sheet;
    ASSERT_EQ(2u, result.third_party_origins.size()) << sheet;
    EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin)
        << "the deferred stylesheet's origin must keep the render-blocking "
           "rank it had before deferral, ahead of a mere head resource: "
        << sheet;
    EXPECT_EQ("https://icons.example.com", result.third_party_origins[1].origin)
        << sheet;
  }
}

TEST(HtmlScannerTest, FiltersSameOrigin) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"http://example.com/style.css\">"
      "<script src=\"https://cdn.other.com/lib.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.other.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, DeduplicatesOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/a.css\">"
      "<link rel=\"stylesheet\" href=\"https://cdn.example.com/b.css\">"
      "<script src=\"https://cdn.example.com/app.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, CapsOriginsAtFour) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://cdn1.example.com/a.css\">"
      "<link rel=\"stylesheet\" href=\"https://cdn2.example.com/b.css\">"
      "<script src=\"https://cdn3.example.com/c.js\"></script>"
      "<script src=\"https://cdn4.example.com/d.js\"></script>"
      "<script src=\"https://cdn5.example.com/e.js\"></script>"
      "<script src=\"https://cdn6.example.com/f.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(4u, result.third_party_origins.size());
}

TEST(HtmlScannerTest, SkipsRelativeUrlsForOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"/css/style.css\">"
      "<script src=\"/js/app.js\"></script>"
      "</head><body>"
      "<img src=\"/images/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty())
      << "Relative URLs should not produce origins";
}

TEST(HtmlScannerTest, PrioritizesRenderBlockingResources) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script defer src=\"https://defer.example.com/d.js\"></script>"
      "<link rel=\"stylesheet\" href=\"https://css.example.com/style.css\">"
      "<script src=\"https://sync.example.com/s.js\"></script>"
      "</head><body>"
      "<img src=\"https://img.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.third_party_origins.size());
  // Render-blocking (stylesheet, sync script) should come before
  // deferred script and body image.
  EXPECT_EQ("https://css.example.com", result.third_party_origins[0].origin);
  EXPECT_EQ("https://sync.example.com", result.third_party_origins[1].origin);
}

TEST(HtmlScannerTest, ProtocolRelativeOrigins) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script src=\"//cdn.example.com/lib.js\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("//cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, RecordsCorsModeEvidencePerOrigin) {
  HtmlScanner scanner;
  // Plain stylesheets/scripts/images fetch no-cors; a crossorigin attribute,
  // an ES module script, or a font preload marks the origin CORS-mode.
  std::string html =
      "<html><head>"
      "<link rel=\"stylesheet\" href=\"https://css.example.com/style.css\">"
      "<script src=\"https://cors.example.com/app.js\" crossorigin>"
      "</script>"
      "<script type=\"module\" src=\"https://mod.example.com/m.js\"></script>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://fonts.example.com/a.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(4u, result.third_party_origins.size());
  // Render-blocking (stylesheet + the sync crossorigin script) sort first.
  EXPECT_EQ("https://css.example.com", result.third_party_origins[0].origin);
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "A plain stylesheet fetches no-cors";
  EXPECT_EQ("https://cors.example.com", result.third_party_origins[1].origin);
  EXPECT_TRUE(result.third_party_origins[1].crossorigin)
      << "An explicit crossorigin attribute marks the origin CORS-mode";
  EXPECT_EQ("https://mod.example.com", result.third_party_origins[2].origin);
  EXPECT_TRUE(result.third_party_origins[2].crossorigin)
      << "ES module scripts always fetch in CORS mode";
  EXPECT_EQ("https://fonts.example.com", result.third_party_origins[3].origin);
  EXPECT_TRUE(result.third_party_origins[3].crossorigin)
      << "Font fetches are always CORS-mode";
}

TEST(HtmlScannerTest, PlainImageOriginIsNoCors) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<img src=\"https://images.example.com/photo.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "A plain <img> fetches no-cors";
}

TEST(HtmlScannerTest, MixedModeOriginFirstSeenDecides) {
  // An origin fetched in BOTH modes keeps the crossorigin bit of the
  // first-seen resource: warming one of its pools still beats the old
  // always-crossorigin behavior, and one entry per origin keeps the
  // 4-entry budget simple.
  HtmlScanner scanner;
  std::string plain_first =
      "<html><head>"
      "<script src=\"https://mixed.example.com/app.js\"></script>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://mixed.example.com/a.woff2\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", plain_first);
  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_FALSE(result.third_party_origins[0].crossorigin)
      << "Plain script seen first: the origin stays no-cors";

  std::string cors_first =
      "<html><head>"
      "<link rel=\"preload\" as=\"font\" "
      "href=\"https://mixed.example.com/a.woff2\">"
      "<script src=\"https://mixed.example.com/app.js\"></script>"
      "</head><body></body></html>";

  result = scanner.Scan("http://example.com/", cors_first);
  EXPECT_TRUE(result.success);
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_TRUE(result.third_party_origins[0].crossorigin)
      << "Font preload seen first: the origin is CORS-mode";
}

TEST(HtmlScannerTest, NoOriginsInEmptyPage) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan("http://example.com/",
                                       "<html><body>Just text</body></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty());
}

// ========== Coverage: URL edge cases ==========

TEST(HtmlScannerTest, UrlWithoutTrailingSlash) {
  HtmlScanner scanner;
  // URL without a path (no trailing slash). ExtractOrigin should return
  // the full URL as the origin.
  std::string html =
      "<html><head>"
      "<script src=\"https://cdn.example.com\"></script>"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // The origin for "https://cdn.example.com" (no path) should be
  // "https://cdn.example.com" itself.
  ASSERT_EQ(1u, result.third_party_origins.size());
  EXPECT_EQ("https://cdn.example.com", result.third_party_origins[0].origin);
}

TEST(HtmlScannerTest, InvalidUrlInSrcAttribute) {
  HtmlScanner scanner;
  // Malformed/relative URLs should not produce third-party origins.
  std::string html =
      "<html><head>"
      "<script src=\"not-a-url\"></script>"
      "<link rel=\"stylesheet\" href=\"also/not/absolute\">"
      "</head><body>"
      "<img src=\"relative/image.jpg\">"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.third_party_origins.empty())
      << "Malformed/relative URLs should not produce origins";
}

// ========== Coverage: HasHeroClass dash-prefix branch ==========

// When a non-hero tag (e.g. <div>) has a hero-prefixed class like
// "hero-banner", the HasHeroClass function must detect it via the
// starts_with + dash check (lines 307-309 of html_scanner.cc).
// Previous tests used <section class="hero-section"> which triggered
// the hero check via the tag keyword, short-circuiting HasHeroClass.
TEST(HtmlScannerTest, DivWithHeroDashClassIsHeroContainer) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"fallback.jpg\"></div>"
      "<div class=\"hero-carousel\">"
      "<img src=\"hero-img.jpg\">"
      "</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  // "hero-carousel" on a <div> should match via HasHeroClass prefix+dash.
  // The hero candidate should override the fallback first-img.
  EXPECT_EQ("hero-img.jpg", result.lcp_candidate.src)
      << "div.hero-carousel should be detected as hero via class prefix";
}

// Test "banner-" prefix on a non-hero tag.
TEST(HtmlScannerTest, DivWithBannerDashClassIsHeroContainer) {
  HtmlScanner scanner;
  std::string html =
      "<html><body>"
      "<div><img src=\"fallback.jpg\"></div>"
      "<div class=\"banner-top\">"
      "<img src=\"banner-img.jpg\">"
      "</div>"
      "</body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  EXPECT_EQ("banner-img.jpg", result.lcp_candidate.src)
      << "div.banner-top should be detected as hero via class prefix";
}

// ========== Coverage: Empty URL causes StartParse failure ==========

TEST(HtmlScannerTest, EmptyUrlFailsParsing) {
  HtmlScanner scanner;
  // Passing an empty URL should cause StartParse to return false,
  // triggering the error path at lines 364-366.
  HtmlScanResult result = scanner.Scan("", "<html><body>Hello</body></html>");

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error_message, "Failed to start parsing: invalid URL");
  EXPECT_TRUE(result.elements.empty());
}

// ========== SRI: integrity-pinned subresources (issue #656) ==========

TEST(HtmlScannerTest, CollectsIntegrityPinnedUrls) {
  HtmlScanner scanner;
  std::string html =
      "<html><head>"
      "<script src=\"/pinned.js\" integrity=\"sha384-abc\"></script>"
      "<link rel=\"stylesheet\" href=\"/pinned.css\" "
      "integrity=\"sha384-def\">"
      "<link rel=\"preload\" as=\"script\" href=\"/preload.js\" "
      "integrity=\"sha384-ghi\">"
      "<script src=\"/free.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/free.css\">"
      "</head><body></body></html>";

  HtmlScanResult result = scanner.Scan("http://example.com/", html);

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.integrity_pinned_urls.size());
  EXPECT_EQ("/pinned.js", result.integrity_pinned_urls[0]);
  EXPECT_EQ("/pinned.css", result.integrity_pinned_urls[1]);
  EXPECT_EQ("/preload.js", result.integrity_pinned_urls[2]);
}

TEST(HtmlScannerTest, NoIntegrityNoPinnedUrls) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><script src=\"/app.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/style.css\"></head></html>");

  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.integrity_pinned_urls.empty());
}

// ========== Script src collection + author <base> detection ==========

TEST(HtmlScannerTest, CollectsScriptSrcsInDocumentOrder) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><head><script src=\"/a.js\"></script>"
      "<script>var inline = 1;</script>"
      "<script src=\"https://cdn.example.net/b.js\"></script></head>"
      "<body><script src=\"c.js\"></script></body></html>");

  EXPECT_TRUE(result.success);
  ASSERT_EQ(3u, result.script_srcs.size());
  EXPECT_EQ("/a.js", result.script_srcs[0]);
  EXPECT_EQ("https://cdn.example.net/b.js", result.script_srcs[1]);
  EXPECT_EQ("c.js", result.script_srcs[2]);
}

TEST(HtmlScannerTest, DetectsAuthorBaseHref) {
  HtmlScanner scanner;
  HtmlScanResult with_base = scanner.Scan(
      "http://example.com/",
      "<html><head><base href=\"/sub/\"></head><body></body></html>");
  EXPECT_TRUE(with_base.success);
  EXPECT_TRUE(with_base.has_base_href);
  EXPECT_EQ("/sub/", with_base.base_href);

  HtmlScanner scanner2;
  HtmlScanResult without_href = scanner2.Scan(
      "http://example.com/",
      "<html><head><base target=\"_blank\"></head><body></body></html>");
  EXPECT_TRUE(without_href.success);
  // An href-less <base> sets no base URL and must not count.
  EXPECT_FALSE(without_href.has_base_href);
  EXPECT_TRUE(without_href.base_href.empty());

  HtmlScanner scanner3;
  HtmlScanResult no_base = scanner3.Scan(
      "http://example.com/", "<html><head></head><body></body></html>");
  EXPECT_TRUE(no_base.success);
  EXPECT_FALSE(no_base.has_base_href);
}

// Only the FIRST base-with-href sets the document base URL (HTML spec); an
// href-less <base target> before it must not block the capture.
TEST(HtmlScannerTest, FirstBaseHrefWins) {
  HtmlScanner scanner;
  HtmlScanResult result =
      scanner.Scan("http://example.com/",
                   "<html><head><base target=\"_blank\">"
                   "<base href=\"/first/\"><base href=\"/second/\"></head>"
                   "<body></body></html>");
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.has_base_href);
  EXPECT_EQ("/first/", result.base_href);
}

}  // namespace

// The hidden bit is the markup's own visibility evidence, so consumers that
// walk the element list (the critical-CSS extractor's position:fixed
// anchoring) can tell a closed panel from an open one without re-parsing.
TEST(HtmlScannerTest, CollectsTheMarkupsVisibilityEvidence) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><body><div class=\"a\">x</div><div class=\"b\" hidden>y</div>"
      "<div class=\"c\" style=\"display: none\">z</div>"
      "<img class=\"d\" width=\"1\" height=\"1\" src=\"p.gif\"></body></html>");
  ASSERT_TRUE(result.success);
  auto find = [&](const std::string& cls) -> const CollectedElement* {
    for (const auto& e : result.elements) {
      for (const auto& c : e.classes) {
        if (c == cls) return &e;
      }
    }
    return nullptr;
  };
  ASSERT_NE(find("a"), nullptr);
  EXPECT_FALSE(find("a")->hidden);
  ASSERT_NE(find("b"), nullptr);
  EXPECT_TRUE(find("b")->hidden) << "the hidden attribute";
  ASSERT_NE(find("c"), nullptr);
  EXPECT_TRUE(find("c")->hidden) << "inline display:none";
  ASSERT_NE(find("d"), nullptr);
  EXPECT_TRUE(find("d")->hidden) << "a 1 px beacon";
}

// The wide bit marks a rendered replaced element whose width, before author
// CSS, may exceed a 320 px phone: the critical-CSS extractor keeps its rules
// for the mobile and tablet blocks wherever it sits.
TEST(HtmlScannerTest, CollectsWhetherAReplacedElementMayExceedAPhone) {
  HtmlScanner scanner;
  HtmlScanResult result = scanner.Scan(
      "http://example.com/",
      "<html><body>"
      "<svg class=\"a\" viewBox=\"0 0 24 24\"><svg class=\"a2\"></svg></svg>"
      "<svg class=\"b\" width=\"24\" viewBox=\"0 0 24 24\"></svg>"
      "<svg class=\"c\" width=\"100%\"></svg>"
      "<iframe class=\"d\" style=\"border:0; WIDTH : 20rem\"></iframe>"
      "<iframe class=\"e\" width=\"560\" height=\"315\"></iframe>"
      "<img class=\"f\" src=\"big.jpg\">"
      "<img class=\"g\" src=\"big.jpg\" width=\"1600\" "
      "style=\"max-width: 100%\">"
      "<img class=\"h\" src=\"x.jpg\" width=\"\" style=\"width:\">"
      "<video class=\"i\" style=\"width: 300px; width: 640px\"></video>"
      "<audio class=\"j\" src=\"a.mp3\"></audio>"
      "<audio class=\"k\" src=\"a.mp3\" controls></audio>"
      "<noscript><img class=\"l\" src=\"big.jpg\"></noscript>"
      "<div class=\"m\"></div>"
      "<img class=\"n\" src=\"s.png\" width=\"320\">"
      "</body></html>");
  ASSERT_TRUE(result.success);
  auto wide = [&](const std::string& cls) -> std::optional<bool> {
    for (const auto& e : result.elements) {
      for (const auto& c : e.classes) {
        if (c == cls) return e.may_exceed_viewport;
      }
    }
    return std::nullopt;
  };
  EXPECT_EQ(wide("a"), true) << "an svg without a width (300 px fallback)";
  EXPECT_EQ(wide("a2"), false) << "an svg inside an svg";
  EXPECT_EQ(wide("b"), false) << "a width attribute that fits";
  EXPECT_EQ(wide("c"), false) << "a percentage width";
  EXPECT_EQ(wide("d"), false) << "an inline width that fits (20rem = 320px)";
  EXPECT_EQ(wide("e"), true) << "the YouTube snippet's width=\"560\"";
  EXPECT_EQ(wide("f"), true) << "an image sized by its own pixels";
  EXPECT_EQ(wide("g"), false) << "an inline max-width that fits";
  EXPECT_EQ(wide("h"), true) << "empty widths are no width";
  EXPECT_EQ(wide("i"), true) << "the last inline width wins";
  EXPECT_EQ(wide("j"), false) << "audio without controls renders nothing";
  EXPECT_EQ(wide("k"), true) << "audio with controls";
  EXPECT_EQ(wide("m"), false) << "not a replaced element";
  EXPECT_EQ(wide("n"), false) << "exactly 320 fits";
  // <noscript> content is not rendered with scripting on; the scanner may or
  // may not collect it, but it is never wide.
  EXPECT_NE(wide("l"), true);
}

// The one resolution rule every consumer of a scan shares. A
// reference resolves against the page URL, or the first <base href> resolved
// against it; a hostless result is the page's own path unless the base names
// another host, in which case it gets that host (and the document's scheme),
// as the browser fetches it. A root-relative <base href> keeps the page's
// host, an explicit default port does not make a base foreign, and a
// protocol-relative reference takes the document's scheme.
TEST(HtmlScannerTest, DocumentBaseResolvesReferencesAsTheBrowserDoes) {
  HtmlScanner scanner;
  const struct {
    const char* page;
    const char* head;
    const char* href;
    const char* resolved;
    const char* host;
    bool cross_host;
  } kCases[] = {
      {"https://example.com/a/p.html", "", "/css/a.css", "/css/a.css",
       "example.com", false},
      {"https://example.com/a/p.html", "", "b.css",
       "https://example.com/a/b.css", "example.com", false},
      {"https://example.com/a/p.html", "", "//cdn.example.com/c.css",
       "https://cdn.example.com/c.css", "example.com", false},
      {"https://example.com/a/p.html", "<base href=\"/sub/\">", "a.css",
       "/sub/a.css", "example.com", false},
      {"https://example.com/a/p.html", "<base href=\"/sub/\">", "/a.css",
       "/a.css", "example.com", false},
      {"https://example.com/a/p.html",
       "<base href=\"https://EXAMPLE.com:443/\">", "/a.css", "/a.css",
       "example.com:443", false},
      {"https://example.com/a/p.html",
       "<base href=\"https://cdn.example.com/assets/\">", "/css/a.css",
       "https://cdn.example.com/css/a.css", "cdn.example.com", true},
      {"https://example.com/a/p.html",
       "<base href=\"https://cdn.example.com/assets/\">", "css/a.css",
       "https://cdn.example.com/assets/css/a.css", "cdn.example.com", true},
      {"https://example.com/a/p.html",
       "<base href=\"https://cdn.example.com/assets/\">",
       "https://example.com/own.css", "https://example.com/own.css",
       "cdn.example.com", true},
      {"http://example.com/a/p.html", "<base href=\"//cdn.example.com/\">",
       "/a.css", "http://cdn.example.com/a.css", "cdn.example.com", true},
      {"http://example.com/a/p.html", "<base href=\"//cdn.example.com/\">",
       "a.css", "http://cdn.example.com/a.css", "cdn.example.com", true},
      // Only the first <base href> counts.
      {"https://example.com/a/p.html",
       "<base href=\"https://cdn.example.com/\"><base href=\"/other/\">",
       "a.css", "https://cdn.example.com/a.css", "cdn.example.com", true},
  };
  for (const auto& c : kCases) {
    HtmlScanResult scan = scanner.Scan(
        c.page,
        absl::StrCat("<html><head>", c.head, "<link rel=\"stylesheet\" href=\"",
                     c.href, "\"></head><body></body></html>"));
    ASSERT_TRUE(scan.success) << c.page << " " << c.head;
    const DocumentBase base = DocumentBaseOf(c.page, scan);
    EXPECT_EQ(c.host, base.host) << c.head << " " << c.href;
    EXPECT_EQ(c.cross_host, base.cross_host) << c.head << " " << c.href;
    EXPECT_EQ(c.resolved, ResolveAgainstBase(base, c.href))
        << c.head << " " << c.href;
  }

  // The worker's page URL is the cache-normalized path, so the page's own
  // host and scheme come from the caller (nginx's Host header and scheme).
  // Without them a base with a host cannot be told from the page's own, and
  // the result stays hostless, as before.
  HtmlScanResult scan = scanner.Scan(
      "/a/p.html",
      "<html><head><base href=\"https://cdn.example.com/assets/\">"
      "<link rel=\"stylesheet\" href=\"/css/a.css\"></head><body></body>"
      "</html>");
  ASSERT_TRUE(scan.success);
  const DocumentBase known =
      DocumentBaseOf("/a/p.html", scan, "example.com", "https");
  EXPECT_TRUE(known.cross_host);
  EXPECT_EQ("https://cdn.example.com/css/a.css",
            ResolveAgainstBase(known, "/css/a.css"));
  const DocumentBase own =
      DocumentBaseOf("/a/p.html", scan, "CDN.example.com", "https");
  EXPECT_FALSE(own.cross_host);
  EXPECT_EQ("/css/a.css", ResolveAgainstBase(own, "/css/a.css"));
  const DocumentBase unknown = DocumentBaseOf("/a/p.html", scan);
  EXPECT_FALSE(unknown.cross_host);
  EXPECT_EQ("cdn.example.com", unknown.host);
  EXPECT_EQ("/css/a.css", ResolveAgainstBase(unknown, "/css/a.css"));
  // A reference with a scheme resolves against nothing: an absolute URL as
  // it is, and a data: or javascript: href is no location on any host, with
  // or without a cross-host base.
  for (std::string_view href :
       {"data:text/css,x", "javascript:void(0)", "HTTPS://x.example/a.css",
        "mailto:a@example.com"}) {
    EXPECT_EQ(href, ResolveAgainstBase(known, href)) << href;
    EXPECT_EQ(href, ResolveAgainstBase(own, href)) << href;
  }
  // Not a scheme: a colon after a slash or a query is part of the path.
  EXPECT_EQ("https://cdn.example.com/a/b:c.css",
            ResolveAgainstBase(known, "/a/b:c.css"));
  // A hostless page URL without a base: relative hrefs stay the page's own
  // path, which the caller keys by the page host.
  HtmlScanResult plain =
      scanner.Scan("/a/p.html",
                   "<html><head><link rel=\"stylesheet\" href=\"b.css\"></head>"
                   "<body></body></html>");
  ASSERT_TRUE(plain.success);
  EXPECT_EQ(
      "/a/b.css",
      ResolveAgainstBase(
          DocumentBaseOf("/a/p.html", plain, "example.com", "https"), "b.css"));
}

}  // namespace pagespeed
