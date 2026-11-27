// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — stylesheet sources as the cascade-layer order reads them
// (HtmlScanResult::stylesheet_sources + BuildLayerOrderSources)

#include "src/worker/layer_order_sources.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"

namespace pagespeed {
namespace {

HtmlScanResult Scan(const std::string& html) {
  HtmlScanner scanner;
  HtmlScanResult r = scanner.Scan("https://example.com/page", html);
  EXPECT_TRUE(r.success);
  return r;
}

// What Worker::BuildCombinedCss does: the gather reads `stylesheets` by href
// from `cache`; then the sources are built and the order computed.
CascadeLayerOrder OrderFor(const std::string& html,
                           const std::map<std::string, std::string>& cache) {
  HtmlScanResult scan = Scan(html);
  std::vector<std::optional<GatheredSheet>> gathered(scan.stylesheets.size());
  for (size_t i = 0; i < scan.stylesheets.size(); ++i) {
    auto it = cache.find(scan.stylesheets[i].href);
    if (it != cache.end()) {
      gathered[i] =
          GatheredSheet{it->second, "https://example.com" + it->first};
    }
  }
  std::vector<LayerOrderSource> sources = BuildLayerOrderSources(
      scan.stylesheet_sources, "https://example.com/page", gathered);
  return ComputeCascadeLayerOrder(sources, {});
}

TEST(StylesheetSourcesTest, RecordsEverySourceInDocumentOrder) {
  HtmlScanResult r = Scan(
      "<html><head><style>.a{}</style>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"Stylesheet\" href=\"/upper.css\">"
      "<link rel=\"icon\" href=\"/i.png\">"
      "<link rel=\"alternate stylesheet\" href=\"/alt.css\">"
      "<link rel=\"preload\" as=\"style\" href=\"/hint.css\">"
      "<link rel=\"preload\" as=\"style\" href=\"/lcss.css\" "
      "onload=\"this.rel='stylesheet'\">"
      "<link rel=\"preload\" as=\"font\" href=\"/f.woff2\">"
      "<link rel=\"stylesheet\" href=\"/d.css\" disabled>"
      "<style media=\"print\">.p{}</style>"
      "<noscript><link rel=\"stylesheet\" href=\"/ns.css\">"
      "<style>.n{}</style></noscript>"
      "<template><style>.t{}</style></template>"
      "</head><body><svg><style>.s{}</style>"
      "<link rel=\"stylesheet\" href=\"/svg.css\"></svg>"
      "<math><style>.m{}</style></math></body></html>");
  const auto& s = r.stylesheet_sources;
  // <noscript> and <template> apply nothing when scripts run; an inline <svg>
  // <style> or <link> and a <math> <style> are SVG and MathML elements, not
  // the page's CSS.
  ASSERT_EQ(s.size(), 7u);
  EXPECT_FALSE(s[0].is_link);
  EXPECT_EQ(s[0].css, ".a{}");
  EXPECT_TRUE(s[1].is_link);
  EXPECT_EQ(s[1].href, "/a.css");
  ASSERT_GE(s[1].stylesheet_index, 0);
  EXPECT_EQ(r.stylesheets[s[1].stylesheet_index].href, "/a.css");
  EXPECT_FALSE(s[1].not_plain_stylesheet);
  // A rel token list the gather does not read: recorded, never gathered.
  EXPECT_EQ(s[2].href, "/upper.css");
  EXPECT_EQ(s[2].stylesheet_index, -1);
  EXPECT_EQ(s[3].href, "/alt.css");
  EXPECT_TRUE(s[3].not_plain_stylesheet);
  // A preload without onload applies nothing; a loadCSS preload is the sheet,
  // at its own position.
  EXPECT_EQ(s[4].href, "/lcss.css");
  EXPECT_FALSE(s[4].not_plain_stylesheet);
  EXPECT_EQ(s[5].href, "/d.css");
  EXPECT_TRUE(s[5].not_plain_stylesheet);
  EXPECT_EQ(s[6].media, "print");
  EXPECT_EQ(s[6].css, ".p{}");
}

TEST(StylesheetSourcesTest, NonCssTypesAndTitles) {
  HtmlScanResult r = Scan(
      "<html><head><style type=\"text/tailwindcss\">@layer b{}</style>"
      "<style type=\"text/plain\">@layer b{}</style>"
      "<link rel=\"stylesheet\" type=\"text/x-less\" href=\"/x.less\">"
      // The type rule: a <style> type takes no parameters, a <link> type may.
      "<style type=\"text/css; charset=utf-8\">@layer b{}</style>"
      "<style type=\"TEXT/CSS\">.c{}</style>"
      "<link rel=\"stylesheet\" type=\"text/css; charset=utf-8\" "
      "href=\"/c.css\">"
      "<link rel=\"stylesheet\" title=\"A\" href=\"/t.css\">"
      "</head><body></body></html>");
  const auto& s = r.stylesheet_sources;
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0].css, ".c{}");
  EXPECT_EQ(s[1].href, "/c.css");
  EXPECT_FALSE(s[1].titled);
  EXPECT_EQ(s[2].href, "/t.css");
  EXPECT_TRUE(s[2].titled);
  // The sources are the sheets the combined CSS reads, and nothing else.
  EXPECT_EQ(r.inline_css, ".c{}");
  ASSERT_EQ(r.stylesheets.size(), 2u);
  EXPECT_EQ(s[1].stylesheet_index, 0);
  EXPECT_EQ(s[2].stylesheet_index, 1);
}

TEST(StylesheetSourcesTest, ScriptsThatMayInsertAStylesheet) {
  HtmlScanResult r = Scan(
      "<html><head>"
      "<script>var x = 1;</script>"                                      // no
      "<script src=\"/sync.js\"></script>"                               // yes
      "<script src=\"/a.js\" async></script>"                            // no
      "<script src=\"/d.js\" defer></script>"                            // no
      "<script type=\"module\" src=\"/m.js\"></script>"                  // no
      "<script type=\"application/ld+json\">{\"@layer\":1}</script>"     // no
      "<script>document.write('<style>')</script>"                       // yes
      "<script>var s=document.createElement( 'style' );</script>"        // yes
      "<script type=\"module\">document.adoptedStyleSheets=[]</script>"  // yes
      "<noscript><script src=\"/n.js\"></script></noscript>"             // no
      "<script src=\"/w.js\" defer data-pagespeed-defer></script>"       // yes
      "<style>.a{}</style></head><body></body></html>");
  const auto& s = r.stylesheet_sources;
  ASSERT_EQ(s.size(), 6u);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_TRUE(s[i].may_insert_stylesheet) << i;
  }
  EXPECT_FALSE(s[5].may_insert_stylesheet);
  EXPECT_EQ(s[5].css, ".a{}");
}

TEST(StylesheetSourcesTest, ReadsTheWorkersOwnMarkupLikeTheServePath) {
  HtmlScanResult r = Scan(
      "<html><head><style data-pagespeed-critical>@layer x;.c{}</style>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" "
      "data-pagespeed-async data-pagespeed-media=\"all\">"
      "<noscript data-pagespeed-async-fallback><link rel=\"stylesheet\" "
      "href=\"/a.css\" data-pagespeed-async-fallback></noscript>"
      "</head><body></body></html>");
  const auto& s = r.stylesheet_sources;
  ASSERT_EQ(s.size(), 1u);
  EXPECT_TRUE(s[0].is_link);
  EXPECT_EQ(s[0].href, "/a.css");
  EXPECT_EQ(s[0].media, "all");
  EXPECT_FALSE(s[0].not_plain_stylesheet);
  ASSERT_GE(s[0].stylesheet_index, 0);
}

TEST(LayerOrderSourcesTest, CaseAInlineStyleAfterTheLink) {
  CascadeLayerOrder order = OrderFor(
      "<html><head><link rel=\"stylesheet\" href=\"/app.css\">"
      "<style>@layer reset{h1{}}</style></head><body></body></html>",
      {{"/app.css", "@layer theme{.a{}} @layer util{.b{}}"}});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.Statement(), "@layer theme,util,reset;");
}

TEST(LayerOrderSourcesTest, CaseBSheetNotGathered) {
  CascadeLayerOrder order = OrderFor(
      "<html><head><link rel=\"stylesheet\" href=\"https://cdn.example/x.css\">"
      "<link rel=\"stylesheet\" href=\"/app.css\"></head><body></body></html>",
      {{"/app.css", "@layer app{h1{}}"}});
  EXPECT_FALSE(order.proven);
  EXPECT_NE(order.reason.find("https://cdn.example/x.css"), std::string::npos)
      << order.reason;
}

TEST(LayerOrderSourcesTest, SourcesThatAreNotPlainSheets) {
  const char* load_css =
      "<link rel=\"preload\" as=\"style\" href=\"/app.css\" "
      "onload=\"this.rel='stylesheet'\">";
  for (const char* link :
       {load_css, "<link rel=\"stylesheet\" title=\"A\" href=\"/app.css\">",
        "<link rel=\"alternate stylesheet\" title=\"t\" href=\"/app.css\">",
        "<link rel=\"stylesheet\" href=\"/app.css\" disabled>",
        "<link rel=\"STYLESHEET\" href=\"/app.css\">"}) {
    CascadeLayerOrder order =
        OrderFor(std::string("<html><head>") + link +
                     "<style>@layer a{}</style></head><body></body></html>",
                 {{"/app.css", "@layer a{}"}});
    EXPECT_FALSE(order.proven) << link;
  }
}

TEST(LayerOrderSourcesTest, EmptyHrefLoadsNothing) {
  CascadeLayerOrder order = OrderFor(
      "<html><head><link rel=\"stylesheet\" href=\"\">"
      "<link rel=\"stylesheet\"><style>@layer a{}</style></head></html>",
      {});
  ASSERT_TRUE(order.proven) << order.reason;
  EXPECT_EQ(order.Statement(), "@layer a;");
}

TEST(LayerOrderSourcesTest, ConditionalSources) {
  // A media attribute or <noscript> makes a first mention unprovable.
  EXPECT_FALSE(OrderFor("<html><head><link rel=\"stylesheet\" href=\"/m.css\" "
                        "media=\"(max-width:600px)\"><style>@layer a{}</style>"
                        "</head></html>",
                        {{"/m.css", "@layer m{}"}})
                   .proven);
  // <noscript> content applies nothing when scripts run.
  CascadeLayerOrder noscript = OrderFor(
      "<html><head><noscript><style>@layer n{}</style></noscript>"
      "<style>@layer a{}</style></head></html>",
      {});
  ASSERT_TRUE(noscript.proven) << noscript.reason;
  EXPECT_EQ(noscript.Statement(), "@layer a;");
  // media="all" is unconditional.
  CascadeLayerOrder all = OrderFor(
      "<html><head><link rel=\"stylesheet\" href=\"/m.css\" media=\"all\">"
      "</head></html>",
      {{"/m.css", "@layer m{}"}});
  ASSERT_TRUE(all.proven) << all.reason;
  EXPECT_EQ(all.Statement(), "@layer m;");
}

TEST(LayerOrderSourcesTest, SourcesTheBrowserDoesNotApplyAreNotCounted) {
  // The review's fixture: the original renders `a` then `b`. Each of these
  // would, if counted, put `b` first.
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{.x{color:green}} @layer b{.x{color:blue}}"},
      {"/b.css", "@layer b{}"}};
  const char* preload_with_twin =
      "<link rel=\"preload\" as=\"style\" href=\"/b.css\">"
      "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>";
  for (const char* before : {
           "<style type=\"text/plain\">@layer b{}</style>",
           "<style type=\"text/tailwindcss\">@layer b{}</style>",
           "<link rel=\"stylesheet\" type=\"text/x-less\" href=\"/b.css\">",
           "<math><style>@layer b{}</style></math>",
           "<svg><link rel=\"stylesheet\" href=\"/b.css\"></svg>",
           "<template><style>@layer b{}</style></template>",
           "<noembed><style>@layer b{}</style></noembed>",
           "<noframes><style>@layer b{}</style></noframes>",
           "<noscript><style>@layer b{}</style></noscript>",
           "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>",
           "<style type=\"text/css; charset=utf-8\">@layer b{}</style>",
           "<link rel=\"preload\" as=\"style\" href=\"/b.css\">",
           preload_with_twin,
       }) {
    CascadeLayerOrder order =
        OrderFor(std::string("<html><head>") + before +
                     "<link rel=\"stylesheet\" href=\"/m.css\"></head></html>",
                 cache);
    ASSERT_TRUE(order.proven) << before << ": " << order.reason;
    EXPECT_EQ(order.Statement(), "@layer a,b;") << before;
  }
  // An HTML <style> inside an integration point does apply: counted, `b`
  // first, as in a browser.
  CascadeLayerOrder fo = OrderFor(
      "<html><head><svg><foreignObject><style>@layer b{}</style>"
      "</foreignObject></svg>"
      "<link rel=\"stylesheet\" href=\"/m.css\"></head></html>",
      cache);
  ASSERT_TRUE(fo.proven) << fo.reason;
  EXPECT_EQ(fo.Statement(), "@layer b,a;");
  // An <svg> <style> is an SVG element whose content is markup; it is not read
  // as the page's CSS, so it is not a source here.
  CascadeLayerOrder svg = OrderFor(
      "<html><head><svg><style>@layer b{}</style></svg>"
      "<link rel=\"stylesheet\" href=\"/m.css\"></head></html>",
      cache);
  ASSERT_TRUE(svg.proven) << svg.reason;
  EXPECT_EQ(svg.Statement(), "@layer a,b;");
}

TEST(LayerOrderSourcesTest, LoadCssPreloadCountsAtThePreloadPosition) {
  // The loadCSS rule decides: the preload (with onload) is the sheet, at
  // its own position, when a <noscript> link declares the same sheet (matched
  // by resolved URL); the layer order reads the very entry the combined CSS
  // reads.
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{.x{color:green}} @layer b{.x{color:blue}}"},
      {"/b.css", "@layer b{.x{color:red}}"},
      {"b.css", "@layer b{.x{color:red}}"}};
  const std::string preload =
      "<link rel=\"preload\" as=\"style\" href=\"/b.css\" "
      "onload=\"this.onload=null;this.rel='stylesheet'\">";
  const std::string twin =
      "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>";
  const std::string sheet = "<link rel=\"stylesheet\" href=\"/m.css\">";
  auto html = [](const std::string& head) {
    return "<html><head>" + head + "</head><body></body></html>";
  };

  HtmlScanResult scan = Scan(html(preload + twin + sheet));
  ASSERT_EQ(scan.stylesheets.size(), 2u);
  EXPECT_EQ(scan.stylesheets[0].href, "/b.css");
  ASSERT_EQ(scan.stylesheet_sources.size(), 2u);
  EXPECT_EQ(scan.stylesheet_sources[0].stylesheet_index, 0);
  EXPECT_EQ(scan.stylesheet_sources[1].stylesheet_index, 1);
  CascadeLayerOrder paired = OrderFor(html(preload + twin + sheet), cache);
  ASSERT_TRUE(paired.proven) << paired.reason;
  EXPECT_EQ(paired.Statement(), "@layer b,a;");

  // The twin spelled another way is the same sheet.
  CascadeLayerOrder respelled =
      OrderFor(html(preload +
                    "<noscript><link rel=\"stylesheet\" "
                    "href=\"https://EXAMPLE.com/b.css#x\"></noscript>" +
                    sheet),
               cache);
  ASSERT_TRUE(respelled.proven) << respelled.reason;
  EXPECT_EQ(respelled.Statement(), "@layer b,a;");

  // Without a twin the browser still applies it, but its sheet is not
  // gathered: not proven.
  CascadeLayerOrder alone = OrderFor(html(preload + sheet), cache);
  EXPECT_FALSE(alone.proven);
  EXPECT_NE(alone.reason.find("/b.css"), std::string::npos) << alone.reason;

  // Declared again by a later applying link: the gather reads it there, but a
  // browser applies it at the preload first. Not proven.
  EXPECT_FALSE(OrderFor(html(preload + twin + sheet +
                             "<link rel=\"stylesheet\" href=\"/b.css\">"),
                        cache)
                   .proven);

  // A twin without its preload is <noscript> CSS: not a source.
  CascadeLayerOrder twin_only = OrderFor(html(twin + sheet), cache);
  ASSERT_TRUE(twin_only.proven) << twin_only.reason;
  EXPECT_EQ(twin_only.Statement(), "@layer a,b;");
}

TEST(LayerOrderSourcesTest, TitledSheetSetsAreNotProven) {
  CascadeLayerOrder order = OrderFor(
      "<html><head><link rel=\"stylesheet\" title=\"A\" href=\"/e.css\">"
      "<link rel=\"stylesheet\" title=\"B\" href=\"/b.css\">"
      "<link rel=\"stylesheet\" href=\"/m.css\"></head></html>",
      {{"/e.css", ""}, {"/b.css", "@layer b{}"}, {"/m.css", "@layer a{}"}});
  EXPECT_FALSE(order.proven);
  EXPECT_NE(order.reason.find("sheet set"), std::string::npos) << order.reason;
}

TEST(LayerOrderSourcesTest, ScriptBeforeANewLayerIsNotProven) {
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{} @layer b{}"}};
  // document.write before the sheet.
  CascadeLayerOrder written = OrderFor(
      "<html><head><script>document.write('<style>@layer b{}</style>')"
      "</script><link rel=\"stylesheet\" href=\"/m.css\"></head></html>",
      cache);
  EXPECT_FALSE(written.proven);
  EXPECT_NE(written.reason.find("script"), std::string::npos) << written.reason;
  // A parser-blocking external script: its content is unknown.
  EXPECT_FALSE(OrderFor("<html><head><script src=\"/s.js\"></script>"
                        "<link rel=\"stylesheet\" href=\"/m.css\"></head>"
                        "</html>",
                        cache)
                   .proven);
  // After the last sheet that declares a layer: nothing it inserts can come
  // ahead of a layer the statement lists.
  EXPECT_TRUE(OrderFor("<html><head><link rel=\"stylesheet\" href=\"/m.css\">"
                       "<script>document.write('x')</script></head></html>",
                       cache)
                  .proven);
  // A deferred analytics script before the sheet (the probe fixture's shape).
  EXPECT_TRUE(OrderFor("<html><head><script defer src=\"/u.js\"></script>"
                       "<link rel=\"stylesheet\" href=\"/m.css\"></head>"
                       "</html>",
                       cache)
                  .proven);
}

TEST(LayerOrderSourcesTest, ReprocessedOutputGivesTheSameOrder) {
  // The worker re-scans its own output on revalidation: the previous block
  // (with its statement), the deferred primary and its <noscript> copy must
  // yield the order of the original page.
  const std::map<std::string, std::string> cache = {
      {"/app.css", "@layer theme{.a{}} @layer util{.b{}}"}};
  CascadeLayerOrder original = OrderFor(
      "<html><head><link rel=\"stylesheet\" href=\"/app.css\">"
      "<style>@layer reset{h1{}}</style></head><body></body></html>",
      cache);
  CascadeLayerOrder reprocessed = OrderFor(
      "<html><head><style data-pagespeed-critical>@layer theme,util,reset;"
      "@layer theme{.a{}}</style>"
      "<link rel=\"preload\" as=\"style\" href=\"/app.css\" "
      "data-pagespeed-async data-pagespeed-media=\"all\">"
      "<noscript data-pagespeed-async-fallback><link rel=\"stylesheet\" "
      "href=\"/app.css\" data-pagespeed-async-fallback></noscript>"
      "<style>@layer reset{h1{}}</style></head><body></body></html>",
      cache);
  ASSERT_TRUE(original.proven) << original.reason;
  ASSERT_TRUE(reprocessed.proven) << reprocessed.reason;
  EXPECT_EQ(reprocessed.names, original.names);
}

}  // namespace
}  // namespace pagespeed
