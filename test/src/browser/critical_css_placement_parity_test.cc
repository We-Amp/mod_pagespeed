// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — the validated render is the served render: placement parity
//
// The serve path (HtmlTransformFilter, an HtmlParse filter) and the validator
// (BuildValidationDocuments, a string scanner) each decide where the critical
// <style> goes. That decision is "before the first stylesheet
// source, within limits; a block that names a cascade layer only with the
// page's proven layer order in front of it", which is a rule both have to
// implement. If they disagree, the browser validates a
// document the visitor never receives. This test runs both on the same pages
// and requires the validator's candidate to be the served page with its
// stylesheet sources (and the worker's async-CSS scaffolding) removed.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/string_util.h"
#include "lib/base/string_writer.h"
#include "lib/html/compat/message_handler.h"
#include "lib/html/empty_html_filter.h"
#include "lib/html/html_element.h"
#include "lib/html/html_keywords.h"
#include "lib/html/html_name.h"
#include "lib/html/html_parse.h"
#include "lib/html/html_writer_filter.h"
#include "src/browser/critical_css_validator.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"
#include "src/worker/html_transform_filter.h"
#include "src/worker/layer_order_sources.h"

namespace pagespeed {
namespace {

using net_instaweb::HtmlElement;
using net_instaweb::HtmlName;

constexpr std::string_view kCritical = ".c{color:red}";

bool RelNamesStylesheet(std::string_view rel) {
  size_t i = 0;
  while (i < rel.size()) {
    while (i < rel.size() && rel[i] == ' ') ++i;
    size_t start = i;
    while (i < rel.size() && rel[i] != ' ') ++i;
    if (i > start && net_instaweb::StringCaseEqual(rel.substr(start, i - start),
                                                   "stylesheet")) {
      return true;
    }
  }
  return false;
}

// Removes what the validator strips (every stylesheet <link>, a deferred
// primary, every <style> but the injected block, every <noscript>) plus the
// worker-only async scaffolding the origin page never had (the loader
// <script>; its <noscript> fallback goes with the others). A parser, not a regex, so comments, script strings and
// unquoted attributes are read the way the serve path reads them.
class SourceStripper : public net_instaweb::EmptyHtmlFilter {
 public:
  explicit SourceStripper(net_instaweb::HtmlParse* parser) : parser_(parser) {}

  void StartElement(HtmlElement* element) override {
    bool drop = false;
    switch (element->keyword()) {
      case HtmlName::kStyle:
        drop = element->FindAttribute("data-pagespeed-critical") == nullptr;
        break;
      case HtmlName::kLink: {
        const char* rel = element->AttributeValue(HtmlName::kRel);
        drop = element->FindAttribute("data-pagespeed-async") != nullptr ||
               (rel != nullptr && RelNamesStylesheet(rel));
        break;
      }
      case HtmlName::kNoscript:
        // Every <noscript>, the worker's async fallback included: the
        // validator renders the page a browser running scripts sees, where
        // <noscript> content is raw text.
        drop = true;
        break;
      case HtmlName::kScript:
        drop = element->FindAttribute("data-pagespeed-async-loader") != nullptr;
        break;
      default:
        break;
    }
    if (drop) parser_->DeleteNode(element);
  }
  const char* Name() const override { return "SourceStripper"; }

 private:
  net_instaweb::HtmlParse* parser_;
};

// Runs `html` through the given filter (or none) and the writer.
std::string Rewrite(std::string_view html,
                    net_instaweb::HtmlFilter* (*make)(net_instaweb::HtmlParse*,
                                                      void*),
                    void* arg) {
  net_instaweb::HtmlKeywords::Init();
  net_instaweb::NullMessageHandler message_handler;
  net_instaweb::HtmlParse parser(&message_handler);
  net_instaweb::HtmlFilter* filter = make(&parser, arg);
  parser.AddFilter(filter);
  std::string output;
  net_instaweb::StringWriter writer(&output);
  net_instaweb::HtmlWriterFilter writer_filter(&parser);
  writer_filter.set_writer(&writer);
  parser.AddFilter(&writer_filter);
  if (!parser.StartParse("http://example.com/")) return "";
  parser.ParseText(html);
  parser.FinishParse();
  delete filter;
  return output;
}

struct ServeArgs {
  std::string_view critical_css;
  bool async_css;
  const CascadeLayerOrder* layer_order;
  const std::vector<std::string>* defer_scripts;
};

// The serve path with only critical-CSS injection (and optionally async CSS,
// and script deferral of `defer_scripts`).
std::string Serve(std::string_view html, std::string_view critical_css,
                  bool async_css, const CascadeLayerOrder& layer_order = {},
                  const std::vector<std::string>& defer_scripts = {}) {
  ServeArgs args{critical_css, async_css, &layer_order, &defer_scripts};
  return Rewrite(
      html,
      [](net_instaweb::HtmlParse* parser, void* arg) {
        const auto* a = static_cast<ServeArgs*>(arg);
        HtmlTransformConfig config;
        config.enable_critical_css = true;
        config.enable_lazy_load = false;
        config.enable_image_dimensions = false;
        config.enable_lcp_preload = false;
        config.enable_preconnect_injection = false;
        config.enable_async_css = a->async_css;
        config.enable_script_deferral = !a->defer_scripts->empty();
        auto* filter =
            new HtmlTransformFilter(parser, config, a->critical_css, nullptr,
                                    "", "https", {}, {}, {}, *a->defer_scripts);
        filter->set_document_is_text_html(true);
        filter->set_cascade_layer_order(*a->layer_order);
        return static_cast<net_instaweb::HtmlFilter*>(filter);
      },
      &args);
}

std::string RemoveSources(std::string_view served) {
  std::string out = Rewrite(
      served,
      [](net_instaweb::HtmlParse* parser, void*) {
        return static_cast<net_instaweb::HtmlFilter*>(
            new SourceStripper(parser));
      },
      nullptr);
  // The HTML writer spells the valueless marker attribute `=""`; the string
  // injector does not. Same attribute, same document.
  static constexpr std::string_view kWritten = "data-pagespeed-critical=\"\"";
  size_t at = out.find(kWritten);
  if (at != std::string::npos) {
    out.replace(at, kWritten.size(), "data-pagespeed-critical");
  }
  return out;
}

void ExpectParity(std::string_view page,
                  std::string_view critical_css = kCritical,
                  bool async_css = false,
                  const CascadeLayerOrder& layer_order = {},
                  std::string_view full_css = ".full{color:blue}") {
  SCOPED_TRACE(std::string(page));
  SCOPED_TRACE(async_css ? "async on" : "async off");
  const std::string served = Serve(page, critical_css, async_css, layer_order);
  ASSERT_NE(served.find("data-pagespeed-critical"), std::string::npos)
      << "the serve path did not inject: " << served;
  if (async_css) {
    ASSERT_NE(served.find("data-pagespeed-async"), std::string::npos)
        << "the serve path did not defer: " << served;
  }
  ValidationDocuments docs =
      BuildValidationDocuments(page, full_css, critical_css, layer_order);
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate, RemoveSources(served)) << "served: " << served;
}

// The page's layer order the way Worker::BuildCombinedCss computes it: the
// scanner's sources, the sheets in `cache` (keyed by href as written, and by
// absolute URL for @imports) as the gather would read them.
CascadeLayerOrder PageLayerOrder(
    const std::string& page, const std::map<std::string, std::string>& cache) {
  HtmlScanner scanner;
  HtmlScanResult scan = scanner.Scan("http://example.com/", page);
  std::vector<std::optional<GatheredSheet>> gathered(scan.stylesheets.size());
  for (size_t i = 0; i < scan.stylesheets.size(); ++i) {
    auto it = cache.find(scan.stylesheets[i].href);
    if (it != cache.end()) {
      gathered[i] = GatheredSheet{it->second, "http://example.com" + it->first};
    }
  }
  std::vector<LayerOrderSource> sources = BuildLayerOrderSources(
      scan.stylesheet_sources, "http://example.com/", gathered);
  return ComputeCascadeLayerOrder(
      sources,
      [&cache](std::string_view,
               std::string_view href) -> std::optional<LayerOrderImport> {
        auto it = cache.find(std::string(href));
        if (it == cache.end()) return std::nullopt;
        return LayerOrderImport{it->second,
                                "http://example.com" + std::string(href)};
      });
}

// Parity for a layered page with its order computed as the worker does, both
// async off and on; returns the served page (async off) for further checks.
std::string ExpectLayeredParity(const std::string& page,
                                std::string_view critical_css,
                                const std::map<std::string, std::string>& cache,
                                bool expect_proven) {
  const CascadeLayerOrder order = PageLayerOrder(page, cache);
  EXPECT_EQ(order.proven, expect_proven) << order.reason << "\n" << page;
  ExpectParity(page, critical_css, /*async_css=*/false, order);
  ExpectParity(page, critical_css, /*async_css=*/true, order);
  return Serve(page, critical_css, /*async_css=*/false, order);
}

TEST(CriticalCssPlacementParityTest, SingleSheet) {
  ExpectParity(
      "<html><head><meta charset=\"utf-8\"><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/a.css\"><script src=\"/x.js\"></script>"
      "</head><body><p>x</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, SeveralSheetsAndInlineStyles) {
  ExpectParity(
      "<html><head><title>T</title><style>.i{}</style>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"alternate stylesheet\" title=\"x\" href=\"/b.css\">"
      "<style>.j{}</style></head><body><p>x</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, DeferredSheets) {
  // Async CSS on: the served sheet is a rel=preload with the fallback and the
  // loader around it; the block must still stand where the validator puts it.
  ExpectParity(
      "<html><head><meta charset=\"utf-8\"><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/a.css\">"
      "<link rel=\"stylesheet\" href=\"/b.css\" media=\"screen\">"
      "<script src=\"/y.js\"></script></head><body><p>x</p></body></html>",
      kCritical, /*async_css=*/true);
  ExpectParity(
      "<html><head><title>T</title><style>.i{}</style>"
      "<link rel=\"stylesheet\" href=\"/a.css\"></head><body></body></html>",
      kCritical, /*async_css=*/true);
}

TEST(CriticalCssPlacementParityTest, SheetOnlyInBody) {
  ExpectParity(
      "<html><head><title>T</title></head><body><p>x</p>"
      "<link rel=\"stylesheet\" href=\"/a.css\"><p>y</p></body></html>");
  // No <head> at all: the </body> fallback, and the body sheet is the anchor.
  ExpectParity(
      "<html><body><p>x</p><link rel=\"stylesheet\" href=\"/a.css\">"
      "<p>y</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, NoHeadEndTag) {
  ExpectParity(
      "<html><head><title>T</title><link rel=\"stylesheet\" href=\"/a.css\">"
      "<body><p>x</p></body></html>");
  ExpectParity("<html><head><title>T</title><body><p>x</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, NoSheetAtAll) {
  ExpectParity(
      "<html><head><title>T</title></head><body><p>x</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, HeadPreludeAfterAnEarlySheet) {
  for (const char* prelude :
       {"<meta charset=\"utf-8\">",
        "<meta http-equiv=\"content-type\" content=\"text/html\">",
        "<meta http-equiv=\"Content-Security-Policy\" "
        "content=\"style-src 'self' 'unsafe-inline'\">",
        "<base href=\"https://example.com/\">"}) {
    ExpectParity(std::string("<html><head>") +
                 "<link rel=\"stylesheet\" href=\"/early.css\">" + prelude +
                 "<title>T</title><link rel=\"stylesheet\" href=\"/late.css\">"
                 "</head><body></body></html>");
    ExpectParity(std::string("<html><head>") +
                 "<link rel=\"stylesheet\" href=\"/early.css\">" + prelude +
                 "<title>T</title></head><body></body></html>");
  }
  // A prelude element inside <noscript> still counts, on both sides.
  ExpectParity(
      "<html><head><link rel=\"stylesheet\" href=\"/early.css\">"
      "<noscript><meta http-equiv=\"content-type\" content=\"text/html\">"
      "</noscript><link rel=\"stylesheet\" href=\"/late.css\">"
      "</head><body></body></html>");
}

// A Tailwind CSS v4 block, whose `@property` rules carry `syntax: "<color>"`
// and whose `content` strings hold `<` and `</style>`: the serve path writes
// each such `<` as the escape `\3c `, and the validator's candidate carries
// the same text.
TEST(CriticalCssPlacementParityTest, TailwindV4BlockIsWrittenTheSameWay) {
  constexpr std::string_view kTailwind =
      "@property --tw-gradient-from{syntax:\"<color>\";inherits:false;"
      "initial-value:#0000}"
      ".content-\\[\\'\\<\\'\\]{--tw-content:'<';content:var(--tw-content)}"
      ".x{&:hover{content:\"</style>\"}}"
      "@media (width<48rem){.y{display:none}}";
  for (bool async_css : {false, true}) {
    ExpectParity(
        "<html><head><title>T</title><link rel=\"stylesheet\" "
        "href=\"/a.css\"></head><body><p>x</p></body></html>",
        kTailwind, async_css);
  }
  const std::string served = Serve(
      "<html><head><title>T</title><link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body><p>x</p></body></html>",
      kTailwind, /*async_css=*/false);
  EXPECT_NE(served.find("syntax:\"\\3c color>\""), std::string::npos) << served;
}

// The validator renders the page without its <noscript>
// elements (a browser running scripts never renders them); the placement is
// the serve path's all the same, the worker's fallback copy included.
// A `<!-->` comment, which the serve path's HtmlLexer ends at once, as a
// browser does. The validator replays the lexer's reading, so the stylesheet
// right after it is the anchor on both sides.
TEST(CriticalCssPlacementParityTest, AShortCommentIsReadLikeTheServePath) {
  for (bool async_css : {false, true}) {
    ExpectParity(
        "<html><head><title>T</title><!--><link rel=\"stylesheet\" "
        "href=\"/a.css\"><!-- x --><meta name=\"x\" content=\"y\">"
        "<link rel=\"stylesheet\" href=\"/b.css\"></head><body></body>"
        "</html>",
        kCritical, async_css);
  }
}

// The other comment endings a browser and the serve path's lexer share: an
// empty `<!--->` and a `--!>`, ahead of the first sheet.
TEST(CriticalCssPlacementParityTest, EveryCommentEndingIsReadLikeTheServePath) {
  for (bool async_css : {false, true}) {
    for (const char* comment : {"<!--->", "<!-- a --!>", "<!---->",
                                "<!-- a -->", "<!-- a -!> b -->"}) {
      std::string page = "<html><head><title>T</title>";
      page += comment;
      page +=
          "<link rel=\"stylesheet\" href=\"/a.css\"><!-- x -->"
          "<meta name=\"x\" content=\"y\"><link rel=\"stylesheet\" "
          "href=\"/b.css\"></head><body></body></html>";
      ExpectParity(page, kCritical, async_css);
    }
  }
}

TEST(CriticalCssPlacementParityTest, NoscriptContentIsLeftOutOnBothSides) {
  for (bool async_css : {false, true}) {
    ExpectParity(
        "<html><head><title>T</title><noscript><style>.h{display:none}</style>"
        "</noscript><link rel=\"stylesheet\" href=\"/a.css\"></head><body>"
        "<noscript><div class=\"nojs-banner\">Enable JS</div></noscript>"
        "<div class=\"hero\">x</div></body></html>",
        kCritical, async_css);
  }
}

TEST(CriticalCssPlacementParityTest, SourcesInNonDocumentSubtreesNeverAnchor) {
  // The loadCSS pattern: the first rel=stylesheet is the <noscript> twin.
  ExpectParity(
      "<html><head><title>T</title>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\">"
      "<noscript><link rel=\"stylesheet\" href=\"/a.css\"></noscript>"
      "<meta name=\"x\" content=\"y\">"
      "<link rel=\"stylesheet\" href=\"/b.css\">"
      "</head><body></body></html>");
  ExpectParity(
      "<html><head><title>T</title>"
      "<noscript><style>.n{}</style></noscript><meta name=\"x\" content=\"y\">"
      "</head><body><template><style>.t{}</style></template>"
      "<svg><style>.s{}</style></svg><p>x</p></body></html>");
  ExpectParity(
      "<html><head><template><style>.t{}</style></template><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/a.css\"></head><body></body></html>");
  ExpectParity(
      "<html><body><svg/><link rel=\"stylesheet\" href=\"/a.css\">"
      "<p>x</p></body></html>");
}

TEST(CriticalCssPlacementParityTest, MarkupTheScannersMustReadAlike) {
  // Uppercase tags and an unquoted rel.
  ExpectParity(
      "<HTML><HEAD><TITLE>T</TITLE><LINK REL=STYLESHEET HREF=/a.css>"
      "</HEAD><BODY><P>x</P></BODY></HTML>");
  ExpectParity(
      "<html><head><title>T</title><link rel=stylesheet href=/a.css>"
      "</head><body></body></html>");
  // </head> and a sheet inside comments, a conditional comment, and markup
  // inside a script string: none of them is real.
  ExpectParity(
      "<html><head><!-- </head> <link rel=\"stylesheet\" href=\"/c.css\"> -->"
      "<title>T</title><!--[if IE]><link rel=\"stylesheet\" href=\"/ie.css\">"
      "<![endif]--><script>var s = \"<link rel='stylesheet' href='/s.css'>\";"
      "</script><link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body></body></html>");
}

TEST(CriticalCssPlacementParityTest, PreviousBlockIsNeitherKeptNorAnchor) {
  ExpectParity(
      "<html><head><style data-pagespeed-critical>.old{}</style>"
      "<title>T</title><link rel=\"stylesheet\" href=\"/a.css\">"
      "</head><body></body></html>");
}

TEST(CriticalCssPlacementParityTest, LayeredCaseAGoesFirstWithTheStatement) {
  // Case A: the combined sheet lists the inline <style> first, the document
  // does not. The computed order is the document's, both sides put the block
  // first, and both carry the same statement.
  const std::string page =
      "<html><head><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/app.css\">"
      "<style>@layer theme{.hero{padding:0}}</style>"
      "<script src=\"/x.js\"></script>"
      "</head><body><h1 class=\"hero\">x</h1></body></html>";
  const std::string served = ExpectLayeredParity(
      page,
      "@layer theme{.hero{padding:0}}\n@layer reset{h1{color:black}}\n"
      "@layer theme{h1{color:navy}}",
      {{"/app.css",
        "@layer reset{h1{color:black}} @layer theme{h1{color:navy}}"}},
      /*expect_proven=*/true);
  size_t block = served.find("@layer reset,theme;@layer theme{");
  ASSERT_NE(block, std::string::npos) << served;
  EXPECT_LT(block, served.find("/app.css")) << served;
}

TEST(CriticalCssPlacementParityTest, LayeredCaseBFallsBackOnBothSides) {
  // Case B: a cross-origin layered sheet the gather never reads.
  const std::string page =
      "<html><head><link rel=\"stylesheet\" href=\"https://cdn.example/x.css\">"
      "<link rel=\"stylesheet\" href=\"/app.css\"><meta name=\"x\" "
      "content=\"y\"></head><body></body></html>";
  const std::string served =
      ExpectLayeredParity(page, "@layer app{h1{color:navy}}",
                          {{"/app.css", "@layer app{h1{color:navy}}"}},
                          /*expect_proven=*/false);
  size_t block = served.find("<style data-pagespeed-critical");
  ASSERT_NE(block, std::string::npos) << served;
  EXPECT_LT(served.find("/app.css"), block) << served;
  EXPECT_EQ(served.find("@layer app;"), std::string::npos) << served;
}

TEST(CriticalCssPlacementParityTest, LayerShapesDecideAlikeOnBothSides) {
  struct Case {
    const char* name;
    std::string page;
    std::map<std::string, std::string> cache;
    const char* block;
    bool proven;
    const char* statement;  // expected in front of the block when proven
  };
  const std::string head =
      "<html><head><title>T</title><link rel=\"stylesheet\" href=\"/a.css\">";
  const std::string tail =
      "<meta name=\"x\" content=\"y\"></head><body></body></html>";
  const Case cases[] = {
      {"nested and dotted names",
       head + tail,
       {{"/a.css", "@layer a{@layer b{.x{}}} @layer a.c, d;"}},
       "@layer a{@layer b{.x{}}}",
       true,
       "@layer a,a.b,a.c,d;"},
      {"import into a layer",
       head + tail,
       {{"/a.css", "@import url(/lib.css) layer(lib);@layer app{.y{}}"},
        {"/lib.css", "@layer base{.z{}}"}},
       "@layer app{.y{}}",
       true,
       "@layer lib,lib.base,app;"},
      {"import not in cache",
       head + tail,
       {{"/a.css", "@import url(/lib.css) layer(lib);@layer app{.y{}}"}},
       "@layer app{.y{}}",
       false,
       nullptr},
      {"anonymous layer after the named ones",
       head + tail,
       {{"/a.css", "@layer app{.y{}} @layer{.w{}}"}},
       "@layer app{.y{}}",
       true,
       "@layer app;"},
      {"anonymous layer before a named one",
       head + tail,
       {{"/a.css", "@layer{.w{}} @layer app{.y{}}"}},
       "@layer app{.y{}}",
       false,
       nullptr},
      {"layer first declared inside @media",
       head + tail,
       {{"/a.css", "@media (min-width:768px){@layer wide{.v{}}} @layer app{}"}},
       "@layer app{.y{}}",
       false,
       nullptr},
      {"a later sheet re-mentions an earlier layer",
       head + "<style>@layer app{.u{}} @layer extra{}</style>" + tail,
       {{"/a.css", "@layer base{} @layer app{.y{}}"}},
       "@layer app{.y{}}",
       true,
       "@layer base,app,extra;"},
      {"link with a media attribute",
       "<html><head><title>T</title><link rel=\"stylesheet\" href=\"/a.css\" "
       "media=\"(min-width:768px)\">" +
           tail,
       {{"/a.css", "@layer app{.y{}}"}},
       "@layer app{.y{}}",
       false,
       nullptr},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const std::string served =
        ExpectLayeredParity(c.page, c.block, c.cache, c.proven);
    size_t block = served.find("<style data-pagespeed-critical");
    ASSERT_NE(block, std::string::npos) << served;
    if (c.proven) {
      EXPECT_NE(served.find(std::string(c.statement) + c.block),
                std::string::npos)
          << served;
      EXPECT_LT(block, served.find("/a.css")) << served;
    } else {
      EXPECT_LT(served.find("/a.css"), block) << served;
    }
  }
}

// The serve path strips NUL bytes from the block before it
// decides placement, and `@la\0yer b` is `@layer b` after that, a layer this
// order lacks, so the block takes the fallback. The validation documents must
// decide on the same text.
TEST(CriticalCssPlacementParityTest,
     NulBytesAreStrippedBeforeEitherSideDecides) {
  const std::string page =
      "<html><head><title>T</title><link rel=\"stylesheet\" href=\"/a.css\">"
      "<meta name=\"x\" content=\"y\"></head><body></body></html>";
  CascadeLayerOrder order;
  order.proven = true;
  order.reason.clear();
  order.names = {"a"};
  std::string block = "@la";
  block.push_back('\0');
  block += "yer b{.x{color:red}}";
  ASSERT_TRUE(DecideCriticalCssLayerPlacement("@layer b{.x{color:red}}", order)
                  .keep_fallback);
  ExpectParity(page, block, /*async_css=*/false, order);
  ExpectParity(page, block, /*async_css=*/true, order);
  const std::string served = Serve(page, block, /*async_css=*/false, order);
  EXPECT_LT(served.find("/a.css"),
            served.find("<style data-pagespeed-critical"))
      << served;
}

TEST(CriticalCssPlacementParityTest, ReviewCasesDecideAlikeOnBothSides) {
  // Review cases, on one fixture: m.css declares `a` then `b`, and
  // the original page renders `b`. Sources a browser does not apply must not
  // change the statement; shapes whose order a browser reads differently from
  // this walk must fall back.
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{.x{color:green}} @layer b{.x{color:blue}}"},
      {"/b.css", "@layer b{.x{color:red}}"},
      {"/e.css", ".x{}"}};
  const char* block = "@layer a{.x{color:green}} @layer b{.x{color:blue}}";
  struct Case {
    const char* name;
    const char* before;  // markup ahead of the m.css link
    bool proven;
  };
  const Case cases[] = {
      {"style type=text/plain", "<style type=\"text/plain\">@layer b{}</style>",
       true},
      {"style type=text/tailwindcss",
       "<style type=\"text/tailwindcss\">@layer b{}</style>", true},
      {"link type=text/x-less",
       "<link rel=\"stylesheet\" type=\"text/x-less\" href=\"/b.css\">", true},
      {"math style", "<math><style>@layer b{}</style></math>", true},
      {"svg link", "<svg><link rel=\"stylesheet\" href=\"/b.css\"></svg>",
       true},
      {"preload hint for the same sheet",
       "<link rel=\"preload\" as=\"style\" href=\"/m.css\">", true},
      {"preload without onload, with a noscript twin",
       "<link rel=\"preload\" as=\"style\" href=\"/b.css\">"
       "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>",
       true},
      {"noscript style", "<noscript><style>@layer b{}</style></noscript>",
       true},
      {"noscript link",
       "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>", true},
      {"template style", "<template><style>@layer b{}</style></template>",
       true},
      {"noembed style", "<noembed><style>@layer b{}</style></noembed>", true},
      {"noframes style", "<noframes><style>@layer b{}</style></noframes>",
       true},
      {"style type with parameters",
       "<style type=\"text/css; charset=utf-8\">@layer b{}</style>", true},
      {"loadCSS preload without a noscript twin",
       "<link rel=\"preload\" as=\"style\" href=\"/b.css\" "
       "onload=\"this.rel='stylesheet'\">",
       false},
      {"titled sheet sets",
       "<link rel=\"stylesheet\" title=\"A\" href=\"/e.css\">"
       "<link rel=\"stylesheet\" title=\"B\" href=\"/b.css\">",
       false},
      {"stray brace", "<style>.q{}} @layer b{.x{color:red}}</style>", false},
      {"stray semicolon", "<style>.q{}; @layer b{.x{color:red}}</style>",
       false},
      {"escaped at-keyword", "<style>@l\\61yer b{.x{color:red}}</style>",
       false},
      {"document.write",
       "<script>document.write('<style>@layer b{}<\\/style>')</script>", false},
      {"parser-blocking external script", "<script src=\"/s.js\"></script>",
       false},
      {"deferred external script", "<script defer src=\"/s.js\"></script>",
       true},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    const std::string page =
        std::string("<html><head><title>T</title>") + c.before +
        "<link rel=\"stylesheet\" href=\"/m.css\"><meta name=\"x\" "
        "content=\"y\"></head><body><p class=\"x\">t</p></body></html>";
    const std::string served =
        ExpectLayeredParity(page, block, cache, c.proven);
    const size_t at = served.find("<style data-pagespeed-critical");
    ASSERT_NE(at, std::string::npos) << served;
    if (c.proven) {
      EXPECT_NE(served.find(std::string("@layer a,b;") + block),
                std::string::npos)
          << served;
      EXPECT_LT(at, served.find("stylesheet\" href=\"/m.css\"")) << served;
    } else {
      EXPECT_LT(served.find("stylesheet\" href=\"/m.css\""), at) << served;
    }
  }
}

TEST(CriticalCssPlacementParityTest, LoadCssPreloadAnchorsOnBothSides) {
  // A browser running scripts applies a loadCSS preload's sheet
  // at the preload's position, so the block goes before the preload, not
  // after it (where it would win every tie against that sheet). The
  // validator keeps the preload (it renders without scripts, where it applies
  // nothing) and anchors on it the same way.
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{.x{color:green}} @layer b{.x{color:blue}}"},
      {"/b.css", "@layer b{.x{color:red}}"}};
  const std::string page =
      "<html><head><title>T</title>"
      "<link rel=\"preload\" as=\"style\" href=\"/b.css\" "
      "onload=\"this.onload=null;this.rel='stylesheet'\">"
      "<noscript><link rel=\"stylesheet\" href=\"/b.css\"></noscript>"
      "<link rel=\"stylesheet\" href=\"/m.css\"><meta name=\"x\" "
      "content=\"y\"></head><body><p class=\"x\">t</p></body></html>";
  // A layered block: the order is proven with b.css counted at the preload,
  // so `b` comes first.
  const std::string block =
      "@layer a{.x{color:green}} @layer b{.x{color:blue}}";
  const std::string served =
      ExpectLayeredParity(page, block, cache, /*expect_proven=*/true);
  const size_t at = served.find("<style data-pagespeed-critical");
  ASSERT_NE(at, std::string::npos) << served;
  EXPECT_NE(served.find("@layer b,a;" + block), std::string::npos) << served;
  EXPECT_LT(at, served.find("rel=\"preload\"")) << served;
  // An unlayered block anchors there too, async off and on.
  ExpectParity(page);
  ExpectParity(page, kCritical, /*async_css=*/true);
  const std::string plain = Serve(page, kCritical, /*async_css=*/false);
  EXPECT_LT(plain.find("<style data-pagespeed-critical"),
            plain.find("rel=\"preload\""))
      << plain;
  // Not a loadCSS preload: no onload, or inside <noscript>. They anchor
  // nothing, so the block goes before m.css.
  for (const char* preload :
       {"<link rel=\"preload\" as=\"style\" href=\"/b.css\">",
        "<noscript><link rel=\"preload\" as=\"style\" href=\"/b.css\" "
        "onload=\"x()\"></noscript>"}) {
    SCOPED_TRACE(preload);
    const std::string other =
        std::string("<html><head><title>T</title>") + preload +
        "<link rel=\"stylesheet\" href=\"/m.css\"></head><body></body></html>";
    ExpectParity(other);
    const std::string out = Serve(other, kCritical, /*async_css=*/false);
    EXPECT_LT(out.find("/b.css"), out.find("<style data-pagespeed-critical"))
        << out;
  }
}

TEST(CriticalCssPlacementParityTest, LayeredOutputIsAFixedPoint) {
  // Re-processing the served page recomputes the same order from it (the old
  // block is skipped, a deferred link is its sheet again) and yields the same
  // bytes: the statement does not accumulate.
  const std::string page =
      "<html><head><meta charset=\"utf-8\"><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/app.css\">"
      "<style>@layer theme{.hero{}}</style></head><body></body></html>";
  const std::map<std::string, std::string> cache = {
      {"/app.css", "@layer reset{h1{}} @layer theme{h1{}}"}};
  const std::string block = "@layer theme{.hero{}} @layer reset{h1{}}";
  for (bool async_css : {false, true}) {
    SCOPED_TRACE(async_css ? "async on" : "async off");
    CascadeLayerOrder order = PageLayerOrder(page, cache);
    ASSERT_TRUE(order.proven) << order.reason;
    const std::string first = Serve(page, block, async_css, order);
    CascadeLayerOrder again = PageLayerOrder(first, cache);
    ASSERT_TRUE(again.proven) << again.reason;
    EXPECT_EQ(again.names, order.names);
    const std::string second = Serve(first, block, async_css, again);
    EXPECT_EQ(first, second);
    size_t n = 0;
    for (size_t at = second.find("@layer reset,theme;");
         at != std::string::npos;
         at = second.find("@layer reset,theme;", at + 1)) {
      ++n;
    }
    EXPECT_EQ(n, 1u) << second;
  }
}

TEST(CriticalCssPlacementParityTest, WorkerDeferredScriptKeepsTheOrderFixed) {
  // Script deferral turns a parser-blocking script into `defer
  // data-pagespeed-defer`. The re-scan of that output must still read it as
  // the parser-blocking script it was (one that may document.write a sheet),
  // or the raw page is unproven, the reprocessed one proven, and placement
  // and the validation binding flip as the cached copy alternates.
  const std::string page =
      "<html><head><title>T</title><script src=\"/an.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/m.css\"><meta name=\"x\" "
      "content=\"y\"></head><body><p class=\"x\">t</p></body></html>";
  const std::map<std::string, std::string> cache = {
      {"/m.css", "@layer a{.x{color:green}} @layer b{.x{color:blue}}"}};
  const std::string block =
      "@layer a{.x{color:green}} @layer b{.x{color:blue}}";
  const std::vector<std::string> defer = {"an.js"};
  for (bool async_css : {false, true}) {
    SCOPED_TRACE(async_css ? "async on" : "async off");
    const CascadeLayerOrder raw = PageLayerOrder(page, cache);
    EXPECT_FALSE(raw.proven) << "precondition: the script comes first";
    const std::string first = Serve(page, block, async_css, raw, defer);
    ASSERT_NE(first.find("data-pagespeed-defer"), std::string::npos) << first;
    const CascadeLayerOrder again = PageLayerOrder(first, cache);
    EXPECT_EQ(again.proven, raw.proven) << again.reason << "\n" << first;
    EXPECT_EQ(again.ValidationBinding(block), raw.ValidationBinding(block));
    const std::string second = Serve(first, block, async_css, again, defer);
    EXPECT_EQ(first, second);
  }
  // An author's own `defer` is still not a parser-blocking script.
  const std::string author_defer =
      "<html><head><script defer src=\"/an.js\"></script>"
      "<link rel=\"stylesheet\" href=\"/m.css\"></head><body></body></html>";
  EXPECT_TRUE(PageLayerOrder(author_defer, cache).proven);
}

TEST(CriticalCssPlacementParityTest, LayeredBlocksTakeTheFallbackOnBothSides) {
  // Each page has a non-source element between its sheets and </head>, so the
  // first-source position and the fallback differ in the stripped document.
  // Case A: an inline <style> after the <link>.
  const char* case_a =
      "<html><head><title>T</title>"
      "<link rel=\"stylesheet\" href=\"/app.css\">"
      "<style>@layer theme{.hero{padding:0}}</style>"
      "<script src=\"/x.js\"></script>"
      "</head><body><h1 class=\"hero\">x</h1></body></html>";
  const char* block_a =
      "@layer theme{.hero{padding:0}}\n@layer reset{h1{color:black}}\n"
      "@layer theme{h1{color:navy}}";
  ExpectParity(case_a, block_a);
  ExpectParity(case_a, block_a, /*async_css=*/true);
  // Case B: a cross-origin layered sheet the combined sheet never contains.
  const char* case_b =
      "<html><head><link rel=\"stylesheet\" href=\"https://cdn.example/x.css\">"
      "<link rel=\"stylesheet\" href=\"/app.css\"><meta name=\"x\" "
      "content=\"y\">"
      "</head><body></body></html>";
  ExpectParity(case_b, "@layer app{h1{color:navy}}");
  ExpectParity(case_b, "@layer app{h1{color:navy}}", /*async_css=*/true);
}

}  // namespace
}  // namespace pagespeed
