// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 — Critical-CSS validator tests
//
// Two halves: the pure document synthesis, and the CDP-driven validation run
// against a scripted browser (test/test_util/cdp_scripted_peer.h).

#include "src/browser/critical_css_validator.h"

#include <cstdint>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"
#include "src/browser/cdp_client.h"
#include "src/browser/visual_regression_gate.h"
#include "src/worker/cascade_layer_order.h"
#include "src/worker/html_scanner.h"
#include "test/test_util/cdp_scripted_peer.h"
#include "test/test_util/png_image.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;
using test::EncodePng;
using test::MakeSolidImage;

constexpr char kPage[] =
    "<!doctype html><html><head>"
    "<title>Home</title>"
    "<link rel=\"stylesheet\" href=\"/_astro/base.css\">"
    "<link rel=\"preconnect\" href=\"https://fonts.example\">"
    "<style>body{margin:0}</style>"
    "</head><body><h1>Hi</h1></body></html>";

// Everything between <style data-pagespeed-critical> and its </style>, or "" if
// absent.
std::string InjectedStyleBody(const std::string& doc) {
  const std::string open = "<style data-pagespeed-critical>";
  size_t a = doc.find(open);
  if (a == std::string::npos) return {};
  a += open.size();
  size_t b = doc.find("</style>", a);
  if (b == std::string::npos) return {};
  return doc.substr(a, b - a);
}

// The document with the injected style ELEMENT removed entirely.
std::string WithoutInjectedStyle(const std::string& doc) {
  const std::string open = "<style data-pagespeed-critical>";
  size_t a = doc.find(open);
  if (a == std::string::npos) return doc;
  size_t b = doc.find("</style>", a);
  if (b == std::string::npos) return doc;
  return doc.substr(0, a) + doc.substr(b + 8);
}

// ---------------------------------------------------------------------------
// Document synthesis
// ---------------------------------------------------------------------------

TEST(BuildValidationDocumentsTest, BothDocumentsCarryNoExternalStylesheetLink) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The declared sheet is gone from both — otherwise the reference would apply
  // it twice and the candidate would apply the full sheet it is supposed to be
  // rendering WITHOUT.
  EXPECT_EQ(docs.reference.find("base.css"), std::string::npos);
  EXPECT_EQ(docs.candidate.find("base.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);

  // A non-stylesheet <link> is left alone: it is not a source of styling and
  // removing it would be an unexplained difference from the served page.
  EXPECT_NE(docs.reference.find("preconnect"), std::string::npos);
  EXPECT_NE(docs.candidate.find("preconnect"), std::string::npos);
}

TEST(BuildValidationDocumentsTest, PreExistingStyleBlocksAreRemovedFromBoth) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The page's own <style> body is already part of the combined sheet (it is
  // the seed of it), so leaving it in place would apply it to the candidate for
  // free — the candidate would look better than the block being judged.
  EXPECT_EQ(docs.reference.find("body{margin:0}"), std::string::npos);
  EXPECT_EQ(docs.candidate.find("body{margin:0}"), std::string::npos);
  EXPECT_EQ(docs.style_blocks_removed, 1u);
}

TEST(BuildValidationDocumentsTest,
     ReferenceCarriesFullCssCandidateCarriesCriticalOnly) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                       ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  EXPECT_EQ(InjectedStyleBody(docs.reference), ".a{color:red}\n.b{color:blue}");
  EXPECT_EQ(InjectedStyleBody(docs.candidate), ".a{color:red}");
}

TEST(BuildValidationDocumentsTest, DocumentsDifferOnlyInTheInjectedStyleBody) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                       ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;

  // The fairness invariant. Any other byte that differs is a difference the
  // pixel diff would silently attribute to the CSS.
  EXPECT_EQ(WithoutInjectedStyle(docs.reference),
            WithoutInjectedStyle(docs.candidate));
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(docs.reference, docs.candidate));
}

TEST(BuildValidationDocumentsTest, DocumentsDifferOnlyInStyleBodyRejectsSkew) {
  // The predicate has to be able to say no, or it is decoration.
  EXPECT_FALSE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style><p>x</html>",
      "<html><style data-pagespeed-critical>a</style><p>y</html>"));
  // Missing marker on one side.
  EXPECT_FALSE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style></html>",
      "<html></html>"));
  // Same body, same surroundings.
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(
      "<html><style data-pagespeed-critical>a</style><p>x</html>",
      "<html><style data-pagespeed-critical>bb</style><p>x</html>"));
}

TEST(BuildValidationDocumentsTest, LinksAndStylesInsideCommentsSurvive) {
  // A commented-out stylesheet link does not style anything, so removing it
  // would be an unexplained difference from the page the visitor gets.
  std::string page =
      "<html><head><!-- <link rel=stylesheet href=/dead.css> -->"
      "<link rel=stylesheet href=/live.css></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.reference.find("dead.css"), std::string::npos);
  EXPECT_EQ(docs.reference.find("live.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, LinkTextInsideScriptIsNotMarkup) {
  std::string page =
      "<html><head><script>var s='<link rel=stylesheet href=/x.css>';</script>"
      "<link rel=stylesheet href=/real.css></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.reference.find("x.css"), std::string::npos)
      << "a string inside <script> is text, not a stylesheet link";
  EXPECT_EQ(docs.reference.find("real.css"), std::string::npos);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, QuotedAngleBracketInsideLinkTagIsNotTagEnd) {
  std::string page =
      "<html><head><link rel=\"stylesheet\" title=\"a>b\" href=/s.css>"
      "<meta name=keep></head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
  EXPECT_EQ(docs.reference.find("s.css"), std::string::npos);
  EXPECT_NE(docs.reference.find("<meta name=keep>"), std::string::npos)
      << "the tag scan stopped at the quoted '>' and ate the following markup";
}

TEST(BuildValidationDocumentsTest, MultiValuedRelIsStillAStylesheet) {
  std::string page =
      "<html><head><link rel=\"alternate stylesheet\" href=/alt.css>"
      "</head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
}

TEST(BuildValidationDocumentsTest, RelPreloadAsStyleIsNotAStylesheetLink) {
  // PR-E's primitive. A preload applies nothing on its own; the stylesheet it
  // points at is separately declared and separately stripped.
  std::string page =
      "<html><head><link rel=preload as=style href=/p.css>"
      "</head><body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.stylesheet_links_removed, 0u);
  EXPECT_NE(docs.reference.find("p.css"), std::string::npos);
}

TEST(BuildValidationDocumentsTest, HandlesHeadlessAndFragmentDocuments) {
  // No <head>: the injector falls through to </body>, then to document start.
  auto no_head = BuildValidationDocuments(
      "<html><body><link rel=stylesheet href=/a.css><p>x</p></body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(no_head.ok) << no_head.error;
  EXPECT_EQ(no_head.reference.find("a.css"), std::string::npos);
  EXPECT_EQ(InjectedStyleBody(no_head.reference), ".a{color:red}");
  EXPECT_TRUE(
      DocumentsDifferOnlyInStyleBody(no_head.reference, no_head.candidate));

  // A bare fragment with neither.
  auto fragment = BuildValidationDocuments("<p>only a paragraph</p>",
                                           ".a{color:red}", ".a{}");
  ASSERT_TRUE(fragment.ok) << fragment.error;
  EXPECT_EQ(InjectedStyleBody(fragment.reference), ".a{color:red}");
  EXPECT_NE(fragment.reference.find("only a paragraph"), std::string::npos);
  EXPECT_TRUE(
      DocumentsDifferOnlyInStyleBody(fragment.reference, fragment.candidate));
}

// The serve path injects the block before the page's FIRST stylesheet source
// (HtmlTransformFilter::InjectCriticalCss), so the validated pair
// carries its one block where that source stood — not at </head>, which would
// render a document shape the visitor never receives.
TEST(BuildValidationDocumentsTest, BlockStandsWhereTheFirstSourceStood) {
  auto docs = BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  // kPage: <title> then the stylesheet <link>, then a preconnect, then <style>.
  const std::string expected =
      "<!doctype html><html><head>"
      "<title>Home</title>"
      "<style data-pagespeed-critical>.a{color:red}</style>"
      "<link rel=\"preconnect\" href=\"https://fonts.example\">"
      "</head><body><h1>Hi</h1></body></html>";
  EXPECT_EQ(docs.reference, expected);
  EXPECT_EQ(docs.candidate, expected);
}

TEST(BuildValidationDocumentsTest, AnInlineStyleBeforeTheLinkIsTheAnchor) {
  auto docs = BuildValidationDocuments(
      "<html><head><meta charset=utf-8><style>b{}</style><title>T</title>"
      "<link rel=stylesheet href=/a.css></head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><meta charset=utf-8>"
            "<style data-pagespeed-critical>.a{}</style>"
            "<title>T</title></head><body>x</body></html>");
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(docs.reference, docs.candidate));
}

TEST(BuildValidationDocumentsTest, ABodySourceIsTheAnchorWhenHeadHasNone) {
  // Same as the serve path's </body> fallback with a sheet in the body: the
  // block precedes the sheet, not the </body>.
  auto docs = BuildValidationDocuments(
      "<html><body><p>x</p><link rel=stylesheet href=/a.css><p>y</p>"
      "</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><body><p>x</p>"
            "<style data-pagespeed-critical>.a{}</style>"
            "<p>y</p></body></html>");
}

TEST(BuildValidationDocumentsTest, ASourceInsideACommentIsNotTheAnchor) {
  auto docs = BuildValidationDocuments(
      "<html><head><!-- <link rel=stylesheet href=/dead.css> "
      "--><title>T</title>"
      "<link rel=stylesheet href=/live.css></head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><!-- <link rel=stylesheet href=/dead.css> -->"
            "<title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body>x</body></html>");
}

TEST(BuildValidationDocumentsTest, ABodySourceDoesNotMoveTheBlockPastHeadEnd) {
  // The serve path decides at </head>: a sheet that only appears in <body> is
  // not seen yet, so the block goes before </head>, not into the body.
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title></head><body><p>x</p>"
      "<link rel=stylesheet href=/a.css><p>y</p></body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body><p>x</p><p>y</p></body></html>");
}

TEST(BuildValidationDocumentsTest, TheBlockNeverPrecedesTheHeadPrelude) {
  // A sheet ahead of <meta charset> / a meta CSP / <base> is not the anchor;
  // the next source after the prelude is, else the </head> fallback.
  auto docs = BuildValidationDocuments(
      "<html><head><link rel=stylesheet href=/early.css>"
      "<META CHARSET=utf-8><title>T</title>"
      "<link rel=stylesheet href=/late.css></head><body></body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><META CHARSET=utf-8><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body></body></html>");

  docs = BuildValidationDocuments(
      "<html><head><style>b{}</style>"
      "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src *\">"
      "<base href=\"/x/\"><title>T</title></head><body></body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(
      docs.candidate,
      "<html><head>"
      "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src *\">"
      "<base href=\"/x/\"><title>T</title>"
      "<style data-pagespeed-critical>.a{}</style>"
      "</head><body></body></html>");
}

TEST(BuildValidationDocumentsTest, ANoscriptSourceIsNeverTheAnchor) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title>"
      "<noscript><link rel=stylesheet href=/a.css></noscript>"
      "<meta name=x><link rel=stylesheet href=/b.css>"
      "</head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title><meta name=x>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body>x</body></html>");
}

TEST(BuildValidationDocumentsTest, TheWorkersOwnMarkupIsReadLikeTheServePath) {
  // On the worker's own output: the previous block is removed and never
  // anchors (the filter deletes it before it looks for a source), and a
  // deferred primary (rel=preload + data-pagespeed-async) is a source again.
  auto docs = BuildValidationDocuments(
      "<html><head><style data-pagespeed-critical>.old{}</style>"
      "<title>T</title>"
      "<link rel=\"preload\" as=\"style\" href=\"/a.css\" data-pagespeed-async"
      " data-pagespeed-media=\"all\">"
      "<noscript data-pagespeed-async-fallback>"
      "<link rel=\"stylesheet\" href=\"/a.css\"></noscript>"
      "</head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  // The deferred-link state: the block stands where the deferred sheet was,
  // and the fallback copy, a <noscript>, is not rendered.
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body>x</body></html>");
  // The fallback copy goes with its <noscript> before the stylesheet removal
  // runs, so only the deferred primary is counted there.
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
  EXPECT_EQ(docs.noscript_elements_removed, 1u);
}

// The validation render runs with script execution disabled, in
// which Blink renders <noscript> content: a no-JS banner would take the top of
// both documents' fold and push the content the block is judged on out of it.
// Both documents leave every <noscript> out, as a browser running scripts
// parses it, and the placement is still the serve path's, decided with the
// <noscript> elements in place.
TEST(BuildValidationDocumentsTest, NoscriptContentIsNotRendered) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title>"
      "<noscript><style>.hero{display:none}</style>"
      "<link rel=stylesheet href=/nojs.css></noscript>"
      "<link rel=stylesheet href=/a.css></head><body>"
      "<NOSCRIPT><div class=nojs-banner>Enable JS</div></NOSCRIPT>"
      "<div class=hero>Hero</div>"
      "<noscript><img src=/pixel.gif></noscript></body></html>",
      ".hero{color:red}.nojs-banner{height:300px}", ".hero{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.noscript_elements_removed, 3u);
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.hero{color:red}</style>"
            "</head><body><div class=hero>Hero</div></body></html>");
  EXPECT_EQ(docs.reference,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>"
            ".hero{color:red}.nojs-banner{height:300px}</style>"
            "</head><body><div class=hero>Hero</div></body></html>");
  for (const std::string* doc : {&docs.reference, &docs.candidate}) {
    EXPECT_EQ(doc->find("noscript"), std::string::npos) << *doc;
    EXPECT_EQ(doc->find("NOSCRIPT"), std::string::npos) << *doc;
    EXPECT_EQ(doc->find("Enable JS"), std::string::npos) << *doc;
  }
}

// Review pages: each renders the hero for a scripting browser,
// and the first tokenizer either cut it out of both documents (two blank
// renders, which validated any block) or edited script source. Both documents
// must keep the page and leave only the <noscript> content out.
TEST(BuildValidationDocumentsTest, ThePageSurvivesTheNoscriptRemoval) {
  constexpr const char* kBodies[] = {
      "<noscript class=it's>NOJS</noscript>",      // c02
      "<noscript data-a=\"x\"\">NOJS</noscript>",  // c04, c23
      "<p title=it's>Hi</p><script>// don't\n"
      "if (a > b) { el.innerHTML = \"<noscript>\"; }</script>",  // c13, c24
      "<script><!--\ndocument.write('<script src=x.js></script>');\n"
      "var tag = \"<noscript>\";\n//--></script>",             // c22
      "<svg class=a/><noscript/><rect/></svg>",                // c18
      "<div><svg></div><noscript><div>NOJS</div></noscript>",  // c11
  };
  for (const char* body : kBodies) {
    SCOPED_TRACE(body);
    const std::string page = absl::StrCat(
        "<!doctype html><html><head><title>T</title>"
        "<link rel=stylesheet href=/app.css></head><body>",
        body, "<div class=hero>HERO</div><p>AFTER</p></body></html>");
    auto docs = BuildValidationDocuments(page, ".hero{color:red}.x{}",
                                         ".hero{color:red}");
    ASSERT_TRUE(docs.ok) << docs.error;
    for (const std::string* doc : {&docs.reference, &docs.candidate}) {
      EXPECT_NE(doc->find("<div class=hero>HERO</div><p>AFTER</p></body>"),
                std::string::npos)
          << *doc;
      EXPECT_EQ(doc->find("NOJS"), std::string::npos) << *doc;
    }
  }
}

// The stylesheet removal used to read a <noscript>'s content as
// markup before the <noscript> was removed. A <style> in it whose </style>
// comes after the </noscript> then ran through the page and took the hero.
TEST(BuildValidationDocumentsTest, NoscriptContentIsNeverReadAsMarkup) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title></head><body>"
      "<noscript><style>.x{}</noscript><div class=hero>HERO</div>"
      "<style>.y{}</style></body></html>",
      ".hero{color:red}.y{}", ".hero{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.hero{color:red}</style>"
            "</head><body><div class=hero>HERO</div></body></html>");
}

// FAIL CLOSED: a removal that may have cut the page short is never compared,
// since a blank reference matches a blank candidate.
TEST(BuildValidationDocumentsTest, AnUntrustedNoscriptRemovalRefuses) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title></head><body>"
      "<noscript><div class=hero>HERO</div><p>AFTER</p></body></html>",
      ".hero{color:red}.x{}", ".hero{color:red}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("<noscript>"), std::string::npos) << docs.error;
}

// The removal reads <noscript> content as raw text, as a browser running
// scripts does, and the placement replay reads it as markup, as the serve
// path's lexer does. Where a lexer comment's "-->" sits in a removed
// <noscript>, or a lexer comment starts in one and ends past it, the two
// disagree about which markup is comment text, so the validator refuses.
TEST(BuildValidationDocumentsTest, ANoscriptCrossingALexerCommentRefuses) {
  for (const char* head :
       {"<noscript><!-- </noscript> --><link rel=stylesheet href=/a.css>",
        "<noscript><!-- </noscript> --!><link rel=stylesheet href=/a.css>"}) {
    SCOPED_TRACE(head);
    auto docs = BuildValidationDocuments(
        absl::StrCat("<html><head><title>T</title>", head,
                     "</head><body><div class=hero>HERO</div></body></html>"),
        ".hero{color:red}.x{}", ".hero{color:red}");
    EXPECT_FALSE(docs.ok);
    EXPECT_NE(docs.error.find("overlap"), std::string::npos) << docs.error;
  }
  // A comment inside a <noscript>, a <noscript> inside a comment, or one after
  // a comment that `<!-->`, `<!--->` or `--!>` ends is read the same way by
  // both.
  for (const char* head :
       {"<noscript><!-- c --></noscript><link rel=stylesheet href=/a.css>",
        "<!-- <noscript>x</noscript> --><link rel=stylesheet href=/a.css>",
        "<!--><noscript>x --></noscript><link rel=stylesheet href=/a.css>",
        "<!---><noscript>x --></noscript><link rel=stylesheet href=/a.css>",
        "<!--a--!><noscript>--></noscript><link rel=stylesheet href=/a.css>"}) {
    SCOPED_TRACE(head);
    auto docs = BuildValidationDocuments(
        absl::StrCat("<html><head><title>T</title>", head,
                     "</head><body><div class=hero>HERO</div></body></html>"),
        ".hero{color:red}.x{}", ".hero{color:red}");
    EXPECT_TRUE(docs.ok) << docs.error;
  }
}

TEST(BuildValidationDocumentsTest, AScannerDisagreementRefuses) {
  const std::string page =
      "<html><head><title>T</title></head><body>"
      "<noscript><div>NOJS</div></noscript><div class=hero>HERO</div>"
      "</body></html>";
  HtmlScanner scanner;
  const HtmlScanResult scan = scanner.Scan("http://example.com/", page);
  ASSERT_TRUE(scan.success);
  auto agree =
      BuildValidationDocuments(page, ".hero{color:red}.x{}", ".hero{color:red}",
                               {}, kMaxValidationDocumentBytes, &scan.elements);
  EXPECT_TRUE(agree.ok) << agree.error;
  // A reading that kept other elements than the scanner after <body>.
  std::vector<CollectedElement> other = scan.elements;
  other.back().tag_name = "span";
  auto disagree =
      BuildValidationDocuments(page, ".hero{color:red}.x{}", ".hero{color:red}",
                               {}, kMaxValidationDocumentBytes, &other);
  EXPECT_FALSE(disagree.ok);
  EXPECT_NE(disagree.error.find("scanner"), std::string::npos)
      << disagree.error;
}

// The serve path places the block on the HtmlLexer's reading, which ends
// `<!-->` at once, as a browser does. The validator must place it the same
// way (here: the first link follows that comment, so the block goes before
// it) and write the comments as the serve path writes them (`<!-->` as
// `<!---->`, `--!>` as `-->`); and a comment with no end reads to the end,
// where no placement can be replayed, so the validator refuses instead.
TEST(BuildValidationDocumentsTest, CommentsAreReadAsTheServePathReadsThem) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title><!--><link rel=stylesheet href=/a.css>"
      "<!-- x --!><link rel=stylesheet href=/b.css></head><body>x</body>"
      "</html>",
      ".a{color:red}.b{}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title><!---->"
            "<style data-pagespeed-critical>.a{color:red}</style><!-- x -->"
            "</head><body>x</body></html>");
  auto open = BuildValidationDocuments(
      "<html><head><title>T</title></head><body><!-- <div class=hero>HERO"
      "</div></body></html>",
      ".a{color:red}.b{}", ".a{color:red}");
  EXPECT_FALSE(open.ok);
}

TEST(BuildValidationDocumentsTest, APreludeInsideNoscriptStillPlacesTheBlock) {
  // The serve path counts a prelude element inside <noscript> (the parity
  // test pins that), so the block goes to </head>, not before /early.css,
  // although the <noscript> itself is not rendered.
  auto docs = BuildValidationDocuments(
      "<html><head><link rel=stylesheet href=/early.css>"
      "<noscript><meta http-equiv=\"content-type\" content=\"text/html\">"
      "</noscript><title>T</title></head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body>x</body></html>");

  // Without the prelude, before the first sheet; the <noscript> that stood
  // between them is gone either way.
  docs = BuildValidationDocuments(
      "<html><head><title>T</title><link rel=stylesheet href=/early.css>"
      "<noscript><p>JS</p></noscript><link rel=stylesheet href=/late.css>"
      "</head><body>x</body></html>",
      ".a{color:red}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "</head><body>x</body></html>");
}

TEST(BuildValidationDocumentsTest, ALayeredCandidateTakesTheHeadEndFallback) {
  // Without a proven layer order (the default), the serve path keeps a block
  // that names a cascade layer after the page's sheets
  // (DecideCriticalCssLayerPlacement); both documents follow the candidate's
  // answer, whatever the reference carries.
  const char* page =
      "<html><head><title>T</title><link rel=stylesheet href=/app.css>"
      "<style>@layer theme{.hero{}}</style><meta name=x></head>"
      "<body>x</body></html>";
  auto docs = BuildValidationDocuments(page, ".a{}", "@layer theme{.a{}}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title><meta name=x>"
            "<style data-pagespeed-critical>@layer theme{.a{}}</style>"
            "</head><body>x</body></html>");
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(docs.reference, docs.candidate));

  docs = BuildValidationDocuments(page, "@layer theme{.a{}}", ".a{}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "<meta name=x></head><body>x</body></html>");
}

TEST(BuildValidationDocumentsTest, ALayeredCandidateWithAProvenOrderGoesFirst) {
  // With the page's layer order proven, a layered candidate stands where the
  // first source stood, and BOTH documents carry the order's statement in
  // front, so the reference also renders the full sheet in the author's layer
  // order and the two still differ only in the style body.
  const char* page =
      "<html><head><title>T</title><link rel=stylesheet href=/app.css>"
      "<style>@layer theme{.hero{}}</style><meta name=x></head>"
      "<body>x</body></html>";
  CascadeLayerOrder order;
  order.proven = true;
  order.reason.clear();
  order.names = {"reset", "theme"};
  auto docs =
      BuildValidationDocuments(page, "@layer reset{.b{}} @layer theme{.a{}}",
                               "@layer theme{.a{}}", order);
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>@layer reset,theme;"
            "@layer theme{.a{}}</style>"
            "<meta name=x></head><body>x</body></html>");
  EXPECT_EQ(docs.reference,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>@layer reset,theme;"
            "@layer reset{.b{}} @layer theme{.a{}}</style>"
            "<meta name=x></head><body>x</body></html>");
  EXPECT_TRUE(DocumentsDifferOnlyInStyleBody(docs.reference, docs.candidate));

  // An unlayered candidate gets no statement, even with an order.
  docs = BuildValidationDocuments(page, "@layer theme{.a{}}", ".a{}", order);
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title>"
            "<style data-pagespeed-critical>.a{}</style>"
            "<meta name=x></head><body>x</body></html>");

  // A candidate naming a layer the order lacks keeps the fallback.
  docs = BuildValidationDocuments(page, ".a{}", "@layer other{.a{}}", order);
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title><meta name=x>"
            "<style data-pagespeed-critical>@layer other{.a{}}</style>"
            "</head><body>x</body></html>");
}

// The serve path's parser reads inline SVG and MathML as browsers do: a <style>
// or <link> there is an SVG or MathML element, not a stylesheet source, and
// stays in both documents as it stands in the served page.
TEST(BuildValidationDocumentsTest, SvgStyleAndLinkStayInBothDocuments) {
  const std::string foreign =
      "<svg><style>.s{fill:red}</style>"
      "<link rel=\"stylesheet\" href=\"/svg.css\"></svg>";
  auto docs =
      BuildValidationDocuments("<html><head><title>T</title></head><body>" +
                                   foreign + "<p>x</p></body></html>",
                               ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.reference.find(foreign), std::string::npos) << docs.reference;
  EXPECT_NE(docs.candidate.find(foreign), std::string::npos) << docs.candidate;
  EXPECT_EQ(docs.style_blocks_removed, 0u);
  EXPECT_EQ(docs.stylesheet_links_removed, 0u);
}

TEST(BuildValidationDocumentsTest, MathStyleStaysInBothDocuments) {
  const std::string foreign =
      "<math><style>.m{}</style><mi><mglyph><style>.g{}</style></mglyph></mi>"
      "<annotation-xml><style>.x{}</style></annotation-xml></math>";
  auto docs =
      BuildValidationDocuments("<html><head><title>T</title></head><body>" +
                                   foreign + "<p>x</p></body></html>",
                               ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.candidate.find(foreign), std::string::npos) << docs.candidate;
  EXPECT_EQ(docs.style_blocks_removed, 0u);
}

// Inside an integration point (SVG foreignObject or title, MathML mi) the
// parser reads HTML again: an HTML <style> or stylesheet <link> there is a
// source and is removed like any other.
TEST(BuildValidationDocumentsTest, HtmlStyleInsideAnIntegrationPointIsRemoved) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title></head><body>"
      "<svg><foreignObject><style>.fo{}</style>"
      "<link rel=\"stylesheet\" href=\"/fo.css\"></foreignObject>"
      "<title><style>.t{}</style></title></svg>"
      "<math><mi><style>.mi{}</style></mi></math><p>x</p></body></html>",
      ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.style_blocks_removed, 3u);
  EXPECT_EQ(docs.stylesheet_links_removed, 1u);
  EXPECT_NE(docs.candidate.find("<svg><foreignObject></foreignObject><title>"
                                "</title></svg><math><mi></mi></math>"),
            std::string::npos)
      << docs.candidate;
}

// An SVG <style> is not raw text: its content is markup, so a tag that ends
// SVG content ends the style too, and an HTML <style> after it is a source.
// A CDATA section in SVG content is text.
TEST(BuildValidationDocumentsTest, SvgStyleContentIsMarkup) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title></head><body>"
      "<svg><style><div><style>.x{}</style></div></style></svg>"
      "<svg><![CDATA[<style>.c{}</style>]]></svg><p>x</p></body></html>",
      ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.style_blocks_removed, 1u);
  EXPECT_NE(docs.candidate.find("<svg><style><div></div></style></svg>"
                                "<svg><![CDATA[<style>.c{}</style>]]></svg>"),
            std::string::npos)
      << docs.candidate;
}

TEST(BuildValidationDocumentsTest, FullCssContainsStyleTerminatorFailsClosed) {
  // InjectCriticalCss refuses CSS containing `</style` — and reports that
  // refusal as success=TRUE with an EMPTY document. A caller checking only
  // `success` would hand an empty reference to the renderer, which would then
  // diff a blank page against a blank page and validate a page it never looked
  // at. This is the sharpest fail-closed case in the file.
  // Outside a CSS string: inside one, `</style` is escaped (see
  // LessThanInCssStringsIsEscapedAsTheServePathWritesIt).
  auto docs = BuildValidationDocuments(
      kPage, ".a{color:red}</style>\n.b{color:blue}", ".a{color:red}");
  EXPECT_FALSE(docs.ok);
  EXPECT_TRUE(docs.reference.empty());
  EXPECT_TRUE(docs.candidate.empty());
  EXPECT_NE(docs.error.find("injection"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, CriticalCssStyleTerminatorFailsClosed) {
  auto docs =
      BuildValidationDocuments(kPage, ".a{color:red}", ".a{color:red}</style ");
  EXPECT_FALSE(docs.ok);
  EXPECT_TRUE(docs.candidate.empty());
}

TEST(BuildValidationDocumentsTest, CssCarryingTheInjectionMarkerFailsClosed) {
  // CSS containing the marker the injector writes makes "the injected style
  // body" ambiguous — the pair would no longer be two documents differing in
  // one place, and the fairness invariant is what the whole comparison rests
  // on. The invariant is checked before either document is rendered.
  auto docs = BuildValidationDocuments(
      kPage, ".a{color:red}<style data-pagespeed-critical>", ".a{color:red}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("outside the injected style body"),
            std::string::npos)
      << docs.error;
  EXPECT_TRUE(docs.reference.empty());
  EXPECT_TRUE(docs.candidate.empty());
}

// Both documents carry the text the serve path writes: each `<` in a CSS
// string or comment as the escape `\3c ` (HtmlTransformFilter::
// InjectCriticalCss), so a Tailwind CSS v4 sheet, whose `@property` rules
// carry `syntax: "<color>"`, is compared, and a `</style>` in a string no
// longer refuses the reference.
TEST(BuildValidationDocumentsTest,
     LessThanInCssStringsIsEscapedAsTheServePathWritesIt) {
  auto docs = BuildValidationDocuments(
      kPage,
      "@property --tw-x{syntax:\"<color>\";inherits:false;initial-value:red}"
      ".a{content:\"</style>\"}.b{color:blue}",
      "@property --tw-x{syntax:\"<color>\";inherits:false;initial-value:red}"
      ".a{color:var(--tw-x)}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_NE(docs.candidate.find("<style data-pagespeed-critical>"
                                "@property --tw-x{syntax:\"\\3c color>\";"),
            std::string::npos)
      << docs.candidate;
  EXPECT_NE(docs.reference.find(".a{content:\"\\3c /style>\"}"),
            std::string::npos)
      << docs.reference;
  EXPECT_EQ(docs.reference.find("<color>"), std::string::npos)
      << docs.reference;
}

// A candidate block the serve path would refuse (a `<` that opens markup
// outside its strings and comments) is not validated either.
TEST(BuildValidationDocumentsTest, ACandidateTheServePathRefusesIsRefused) {
  for (const char* css : {".a{color:red}<b>", ".a{--x:a<!--}"}) {
    SCOPED_TRACE(css);
    auto docs = BuildValidationDocuments(kPage, ".a{color:red}", css);
    EXPECT_FALSE(docs.ok);
    EXPECT_NE(docs.error.find("could read as markup"), std::string::npos)
        << docs.error;
  }
  // The reference may hold one (a page's old `<!--` inside <style>).
  EXPECT_TRUE(
      BuildValidationDocuments(kPage, "<!-- .a{color:red} -->", ".a{color:red}")
          .ok);
}

// A stylesheet source inside a <select> never anchors the block, as on the
// serve path (IsNonDocumentSubtreeRoot): the block goes where the next
// source stood, not into the <select>.
TEST(BuildValidationDocumentsTest, ASourceInsideSelectDoesNotAnchor) {
  auto docs = BuildValidationDocuments(
      "<html><head><title>T</title><select><style>.s{}</style></select>"
      "<link rel=stylesheet href=/a.css><meta name=x content=y></head>"
      "<body>x</body></html>",
      ".a{color:red}", ".a{color:red}");
  ASSERT_TRUE(docs.ok) << docs.error;
  EXPECT_EQ(docs.candidate,
            "<html><head><title>T</title><select></select>"
            "<style data-pagespeed-critical>.a{color:red}</style>"
            "<meta name=x content=y></head><body>x</body></html>");
}

TEST(BuildValidationDocumentsTest, EmptyInputsFailClosed) {
  // An empty reference stylesheet would render the same blank fold as an empty
  // candidate: diff 0, "validated", and the gate opens on nothing.
  EXPECT_FALSE(BuildValidationDocuments(kPage, "", ".a{}").ok);
  EXPECT_FALSE(BuildValidationDocuments(kPage, ".a{}", "").ok);
  EXPECT_FALSE(BuildValidationDocuments("", ".a{}", ".a{}").ok);
}

TEST(BuildValidationDocumentsTest, OversizedDocumentFailsClosed) {
  std::string big_css(4096, 'x');
  auto docs = BuildValidationDocuments(kPage, big_css, ".a{}", {},
                                       /*max_document_bytes=*/1024);
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("too large"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, UnclosedStyleElementFailsClosed) {
  // Nothing sensible can be stripped out of a document whose <style> never
  // ends: the rest of the page is inside it.
  auto docs = BuildValidationDocuments(
      "<html><head><style>body{margin:0}</head><body>x</body></html>", ".a{}",
      ".b{}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("unterminated"), std::string::npos) << docs.error;
}

TEST(BuildValidationDocumentsTest, UnterminatedCommentFailsClosed) {
  auto docs = BuildValidationDocuments(
      "<html><head><!-- never closed <link rel=stylesheet href=/a.css>", ".a{}",
      ".b{}");
  EXPECT_FALSE(docs.ok);
  EXPECT_NE(docs.error.find("unterminated"), std::string::npos) << docs.error;
}

// ---------------------------------------------------------------------------
// The validation run
// ---------------------------------------------------------------------------

class CriticalCssValidatorCdpTest : public ::testing::Test {
 protected:
  static std::vector<uint8_t> TwoToneImage() {
    auto pixels = MakeSolidImage(10, 10, 128, 128, 128);
    for (size_t i = 50 * 4; i < pixels.size(); i += 4) {
      pixels[i + 0] = 60;
      pixels[i + 1] = 60;
      pixels[i + 2] = 60;
    }
    return pixels;
  }

  void SetUp() override {
    ASSERT_TRUE(peer_.Start());
    gate_ = std::make_unique<VisualRegressionGate>(peer_.client());
    // Not one uniform colour: a blank reference is refused outright,
    // so the fold drawn here has a darker lower half.
    identical_ = EncodePng(TwoToneImage(), 10, 10);
    // 4 of 100 pixels black => diff ratio 0.04.
    auto pixels = TwoToneImage();
    for (int i = 0; i < 4; ++i) {
      pixels[i * 4 + 0] = 0;
      pixels[i * 4 + 1] = 0;
      pixels[i * 4 + 2] = 0;
    }
    four_pixels_off_ = EncodePng(pixels, 10, 10);

    docs_ = BuildValidationDocuments(kPage, ".a{color:red}\n.b{color:blue}",
                                     ".a{color:red}");
    ASSERT_TRUE(docs_.ok) << docs_.error;
  }

  // The gate is destroyed BEFORE the peer's client, which is the ordering every
  // owner of one has to honour — see VisualRegressionGate's lifetime note.
  void TearDown() override {
    gate_.reset();
    peer_.Stop();
  }

  // Answer both captures, giving capture 2 `second` and capture 1 `first`.
  void ScriptTwoCaptures(const std::vector<uint8_t>& first,
                         const std::vector<uint8_t>& second) {
    peer_.SetResponder([this, first, second](int id, const std::string& method,
                                             const json& cmd) -> bool {
      if (method == "Page.setDocumentContent") {
        peer_.RespondToCommand(id, json::object());
        peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                        cmd.value("sessionId", ""));
        return true;
      }
      if (method == "Page.captureScreenshot") {
        ++shots_;
        peer_.RespondToCommand(
            id, {{"data", test::Base64Encode(shots_ == 1 ? first : second)}});
        return true;
      }
      return false;
    });
  }

  ValidationVerdict RunValidation(float threshold) {
    ValidationVerdict verdict;
    bool done = false;
    ValidateCriticalCss(gate_.get(), docs_, 10, 10, threshold,
                        [&](ValidationVerdict v) {
                          verdict = std::move(v);
                          done = true;
                        });
    peer_.PumpUntil([&done] { return done; }, 120);
    EXPECT_TRUE(done) << "validation never completed";
    return verdict;
  }

  test::ScriptedCdpPeer peer_;
  std::unique_ptr<VisualRegressionGate> gate_;
  ValidationDocuments docs_;
  std::vector<uint8_t> identical_;
  std::vector<uint8_t> four_pixels_off_;
  int shots_ = 0;
};

TEST_F(CriticalCssValidatorCdpTest, DiffBelowThresholdMarksValidated) {
  ScriptTwoCaptures(identical_, identical_);
  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_TRUE(v.validated) << v.failure_reason;
  EXPECT_FLOAT_EQ(v.diff_ratio, 0.0f);
  EXPECT_TRUE(v.failure_reason.empty());
  EXPECT_EQ(shots_, 2);
}

TEST_F(CriticalCssValidatorCdpTest, DiffAboveThresholdMarksNotValidated) {
  ScriptTwoCaptures(identical_, four_pixels_off_);
  ValidationVerdict v = RunValidation(0.005f);
  EXPECT_FALSE(v.validated);
  EXPECT_FLOAT_EQ(v.diff_ratio, 0.04f);
  EXPECT_FALSE(v.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest,
       ThresholdIsTheOnlyThingThatMovesTheVerdict) {
  // The same measurement, both sides of the threshold. Without this the
  // "validated" tests could be passing for any reason at all.
  ScriptTwoCaptures(identical_, four_pixels_off_);
  ValidationVerdict lenient = RunValidation(0.10f);
  EXPECT_TRUE(lenient.validated) << lenient.failure_reason;
  EXPECT_FLOAT_EQ(lenient.diff_ratio, 0.04f);
}

TEST_F(CriticalCssValidatorCdpTest, ScreenshotFailureMarksNotValidated) {
  peer_.SetResponder(
      [this](int id, const std::string& method, const json& cmd) -> bool {
        if (method == "Page.setDocumentContent") {
          peer_.RespondToCommand(id, json::object());
          peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                          cmd.value("sessionId", ""));
          return true;
        }
        if (method == "Page.captureScreenshot") {
          peer_.RespondError(id, -32000, "renderer gone");
          return true;
        }
        return false;
      });

  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_FALSE(v.validated);
  EXPECT_LT(v.diff_ratio, 0.0f) << "no measurement was made; do not claim one";
  EXPECT_FALSE(v.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest, SecondRenderFailureMarksNotValidated) {
  int targets = 0;
  peer_.SetResponder([&](int id, const std::string& method,
                         const json& cmd) -> bool {
    if (method == "Target.createTarget" && ++targets == 2) {
      peer_.RespondError(id, -32000, "out of targets");
      return true;
    }
    if (method == "Page.setDocumentContent") {
      peer_.RespondToCommand(id, json::object());
      peer_.SendEvent("Page.lifecycleEvent", {{"name", "networkIdle"}},
                      cmd.value("sessionId", ""));
      return true;
    }
    if (method == "Page.captureScreenshot") {
      peer_.RespondToCommand(id, {{"data", test::Base64Encode(identical_)}});
      return true;
    }
    return false;
  });

  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_FALSE(v.validated)
      << "the candidate was never rendered; there is nothing to validate";
  EXPECT_LT(v.diff_ratio, 0.0f);
}

TEST_F(CriticalCssValidatorCdpTest, ChromeUnavailableMarksNotValidated) {
  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(/*gate=*/nullptr, docs_, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  ASSERT_TRUE(done) << "the callback must fire even with no browser";
  EXPECT_FALSE(verdict.validated);
  EXPECT_LT(verdict.diff_ratio, 0.0f);
  EXPECT_FALSE(verdict.failure_reason.empty());
}

TEST_F(CriticalCssValidatorCdpTest, UnbuiltDocumentsMarkNotValidatedNoRender) {
  ValidationDocuments broken;
  broken.ok = false;
  broken.error = "injection aborted";

  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(gate_.get(), broken, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  ASSERT_TRUE(done);
  EXPECT_FALSE(verdict.validated);
  peer_.RunLoop();
  EXPECT_EQ(peer_.CountCommands("Target.createTarget"), 0u)
      << "a refused document pair must not reach the browser at all";
}

TEST_F(CriticalCssValidatorCdpTest, ComparisonOfNoPixelsMarksNotValidated) {
  // CompareScreenshots reports passed=true with total_pixels==0 when the
  // compare region is empty (VisualRegressionGateTest.ZeroViewportHeight pins
  // that). "Nothing differed because nothing was compared" is not a
  // measurement, and must not open the gate.
  ScriptTwoCaptures(identical_, identical_);

  ValidationVerdict verdict;
  bool done = false;
  ValidateCriticalCss(gate_.get(), docs_, 10, /*viewport_height=*/0,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  peer_.PumpUntil([&done] { return done; }, 120);
  ASSERT_TRUE(done);
  EXPECT_FALSE(verdict.validated)
      << "a comparison over zero pixels validated the page";
}

// Defence in depth: a reference that renders as one uniform
// colour (a blank page) matches a blank candidate, so it confirms nothing.
TEST_F(CriticalCssValidatorCdpTest, AUniformReferenceIsNeverValidated) {
  const auto blank = EncodePng(MakeSolidImage(10, 10, 255, 255, 255), 10, 10);
  ScriptTwoCaptures(blank, blank);
  ValidationVerdict verdict = RunValidation(kDefaultValidationDiffThreshold);
  EXPECT_FALSE(verdict.validated);
  EXPECT_NE(verdict.failure_reason.find("uniform"), std::string::npos)
      << verdict.failure_reason;
}

TEST_F(CriticalCssValidatorCdpTest, BothDocumentsReachTheBrowserUnaltered) {
  ScriptTwoCaptures(identical_, identical_);
  RunValidation(kDefaultValidationDiffThreshold);

  std::vector<std::string> sent;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") == "Page.setDocumentContent") {
      sent.push_back(cmd.value("params", json::object()).value("html", ""));
    }
  }
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0], docs_.reference);
  EXPECT_EQ(sent[1], docs_.candidate);
}

TEST_F(CriticalCssValidatorCdpTest, TearingDownTheBrowserRefusesInFlight) {
  // The reachable crash path, and the reason the gate is an owned member
  // rather than something a callback keeps alive: Chrome exits, the manager
  // drops its CDP components, and a comparison is still in flight holding a
  // raw client pointer and a 60 s timer. Once the client is gone that timer is
  // the ONLY way the capture can finish, so it is the common path.
  //
  // Teardown order here mirrors the manager's exactly: gate first, client
  // second.
  peer_.SetResponder(
      [this](int id, const std::string& method, const json&) -> bool {
        if (method != "Page.setDocumentContent") return false;
        peer_.RespondToCommand(id, json::object());
        // No lifecycle event: the capture is left genuinely in flight.
        return true;
      });

  ValidationVerdict verdict;
  verdict.validated = true;  // must be overwritten
  bool done = false;
  ValidateCriticalCss(gate_.get(), docs_, 10, 10,
                      kDefaultValidationDiffThreshold,
                      [&](ValidationVerdict v) {
                        verdict = std::move(v);
                        done = true;
                      });
  peer_.PumpUntil([&] { return peer_.SawCommand("Page.setDocumentContent"); },
                  40);
  ASSERT_FALSE(done) << "precondition: the comparison must still be in flight";

  gate_.reset();
  ASSERT_TRUE(done) << "an in-flight comparison must be resolved, not orphaned";
  EXPECT_FALSE(verdict.validated)
      << "a browser that went away confirmed nothing";
  EXPECT_LT(verdict.diff_ratio, 0.0f);

  // Now the client dies too. Nothing may touch it afterwards: drain the loop
  // long enough for a leaked timeout timer to have fired.
  peer_.Stop();
  SUCCEED();
}

TEST_F(CriticalCssValidatorCdpTest, BothCapturesUseTheRequestedViewport) {
  ScriptTwoCaptures(identical_, identical_);
  ValidationVerdict v = RunValidation(kDefaultValidationDiffThreshold);
  ASSERT_TRUE(v.validated) << v.failure_reason;

  // Both documents must be measured at the SAME viewport, or the comparison is
  // between two different pages.
  int seen = 0;
  for (const auto& cmd : peer_.received_commands()) {
    if (cmd.value("method", "") != "Emulation.setDeviceMetricsOverride") {
      continue;
    }
    auto params = cmd.value("params", json::object());
    EXPECT_EQ(params.value("width", 0), 10);
    EXPECT_EQ(params.value("height", 0), 10);
    ++seen;
  }
  EXPECT_EQ(seen, 2) << "each capture sets its own viewport";
}

}  // namespace
}  // namespace pagespeed
