// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for MarkdownExtractorFilter / ExtractAgentMarkdown.
// Pure string -> string; no CDP/Chrome. Each test pins one element mapping, a
// strip rule, whitespace handling, or the render-vs-static "moat" behavior.

#include "lib/html/markdown_extractor_filter.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/html/html_keywords.h"

namespace net_instaweb {
namespace {

class MarkdownExtractorFilterTest : public testing::Test {
 protected:
  void SetUp() override { HtmlKeywords::Init(); }
  std::string Md(std::string_view html) {
    return ExtractAgentMarkdown(html, "http://test.com/page.html");
  }
};

// ---- headings / paragraphs ------------------------------------------------

TEST_F(MarkdownExtractorFilterTest, Headings) {
  EXPECT_EQ(Md("<h1>Title</h1><h2>Sub</h2>"), "# Title\n\n## Sub");
  EXPECT_EQ(Md("<h3>a</h3><h4>b</h4><h5>c</h5><h6>d</h6>"),
            "### a\n\n#### b\n\n##### c\n\n###### d");
}

TEST_F(MarkdownExtractorFilterTest, Paragraphs) {
  EXPECT_EQ(Md("<p>Hello world</p>"), "Hello world");
  EXPECT_EQ(Md("<p>one</p><p>two</p>"), "one\n\ntwo");
  // Bare text and <div> both become blocks.
  EXPECT_EQ(Md("<div>a</div><div>b</div>"), "a\n\nb");
}

// ---- inline: links, images, emphasis, code --------------------------------

TEST_F(MarkdownExtractorFilterTest, Links) {
  EXPECT_EQ(Md("<p>See <a href=\"/x\">here</a> now</p>"), "See [here](/x) now");
  // No href -> bare bracketed text (still readable).
  EXPECT_EQ(Md("<p><a>bare</a></p>"), "[bare]()");
}

TEST_F(MarkdownExtractorFilterTest, EmptyLinkInHeadingIsOmitted) {
  EXPECT_EQ(Md("<h2 id=\"roll-back\">Roll back<a class=\"heading-anchor\" "
               "href=\"#roll-back\" aria-label=\"Link to Roll back\">"
               "</a></h2>"),
            "## Roll back");
}

TEST_F(MarkdownExtractorFilterTest, IconOnlyLinkIsOmitted) {
  EXPECT_EQ(Md("<p>Docs <a href=\"/d\"><svg viewBox=\"0 0 1 1\">"
               "<path d=\"M0\"/></svg></a>here</p>"),
            "Docs here");
  EXPECT_EQ(Md("<p>Go<a href=\"/g\" aria-label=\"Go there\">"
               "<span aria-hidden=\"true\">&rarr;</span></a></p>"),
            "Go");
  EXPECT_EQ(Md("<p>Go<a href=\"/g\"><i class=\"icon\"></i></a></p>"), "Go");
}

TEST_F(MarkdownExtractorFilterTest, AsteriskTextLinksAreKept) {
  EXPECT_EQ(Md("<p>Note<a href=\"#fn\">*</a></p>"), "Note[*](#fn)");
  EXPECT_EQ(Md("<p>Note<a href=\"#fn\">**</a></p>"), "Note[**](#fn)");
  EXPECT_EQ(Md("<p>Note<a href=\"#fn\">* *</a></p>"), "Note[* *](#fn)");
}

TEST_F(MarkdownExtractorFilterTest, TextInNestedSpansKeepsLink) {
  EXPECT_EQ(Md("<p><a href=\"/n\"><span><span>deep</span></span></a></p>"),
            "[deep](/n)");
  EXPECT_EQ(Md("<p><a href=\"/n\"><span aria-hidden=\"true\">x</span>"
               "<span>shown</span></a></p>"),
            "[shown](/n)");
}

TEST_F(MarkdownExtractorFilterTest, EntityOnlyLinkIsKept) {
  EXPECT_EQ(Md("<p>a<a href=\"/e\">&amp;</a></p>"), "a[&](/e)");
}

TEST_F(MarkdownExtractorFilterTest, ImageLinkWithAltIsKept) {
  EXPECT_EQ(Md("<p><a href=\"/h\"><img src=\"/l.png\" alt=\"Logo\"></a></p>"),
            "[![Logo](/l.png)](/h)");
}

TEST_F(MarkdownExtractorFilterTest, NormalLinkIsKept) {
  EXPECT_EQ(Md("<h2>Title <a href=\"/x\">more</a></h2>"),
            "## Title [more](/x)");
}

TEST_F(MarkdownExtractorFilterTest, WhitespaceOnlyLinkIsOmitted) {
  EXPECT_EQ(Md("<p>a<a href=\"/w\">  \n </a>b</p>"), "ab");
  EXPECT_EQ(Md("<p>a <a href=\"/w\"> </a>b</p>"), "a b");
}

TEST_F(MarkdownExtractorFilterTest, Images) {
  EXPECT_EQ(Md("<p><img src=\"/a.png\" alt=\"cat\"></p>"), "![cat](/a.png)");
  EXPECT_EQ(Md("<p><img src=\"/a.png\"></p>"), "![](/a.png)");
  // No src -> nothing emitted (never fetched; nothing to reference).
  EXPECT_EQ(Md("<p>x<img alt=\"y\">z</p>"), "xz");
}

TEST_F(MarkdownExtractorFilterTest, Emphasis) {
  EXPECT_EQ(Md("<p><strong>bold</strong> and <em>it</em></p>"),
            "**bold** and *it*");
  EXPECT_EQ(Md("<p><b>b</b><i>i</i></p>"), "**b***i*");
}

TEST_F(MarkdownExtractorFilterTest, InlineCode) {
  EXPECT_EQ(Md("<p>Use <code>x()</code></p>"), "Use `x()`");
}

// ---- lists ----------------------------------------------------------------

TEST_F(MarkdownExtractorFilterTest, UnorderedList) {
  EXPECT_EQ(Md("<ul><li>a</li><li>b</li></ul>"), "- a\n- b");
}

TEST_F(MarkdownExtractorFilterTest, OrderedList) {
  EXPECT_EQ(Md("<ol><li>a</li><li>b</li><li>c</li></ol>"), "1. a\n2. b\n3. c");
}

TEST_F(MarkdownExtractorFilterTest, NestedList) {
  EXPECT_EQ(Md("<ul><li>a<ul><li>b</li></ul></li></ul>"), "- a\n  - b");
}

// ---- blockquote / pre / hr / br -------------------------------------------

TEST_F(MarkdownExtractorFilterTest, Blockquote) {
  EXPECT_EQ(Md("<blockquote><p>quoted</p></blockquote>"), "> quoted");
}

TEST_F(MarkdownExtractorFilterTest, PreservesPreVerbatim) {
  EXPECT_EQ(Md("<pre>line1\nline2</pre>"), "```\nline1\nline2\n```");
  // Whitespace inside <pre> is preserved (not collapsed).
  EXPECT_EQ(Md("<pre>  indented\n    more</pre>"),
            "```\n  indented\n    more\n```");
}

TEST_F(MarkdownExtractorFilterTest, HorizontalRule) {
  EXPECT_EQ(Md("<p>a</p><hr><p>b</p>"), "a\n\n---\n\nb");
}

TEST_F(MarkdownExtractorFilterTest, LineBreak) {
  EXPECT_EQ(Md("<p>line1<br>line2</p>"), "line1  \nline2");
}

// ---- tables ---------------------------------------------------------------

TEST_F(MarkdownExtractorFilterTest, GfmTable) {
  EXPECT_EQ(Md("<table><tr><th>A</th><th>B</th></tr>"
               "<tr><td>1</td><td>2</td></tr></table>"),
            "| A | B |\n| --- | --- |\n| 1 | 2 |");
}

TEST_F(MarkdownExtractorFilterTest, TableCellPipeEscaped) {
  EXPECT_EQ(Md("<table><tr><td>a|b</td></tr></table>"), "| a\\|b |\n| --- |");
}

// ---- strip rules (the §4-P1 structural drop) ------------------------------

TEST_F(MarkdownExtractorFilterTest, StripsScriptAndStyle) {
  EXPECT_EQ(Md("<p>keep</p><script>var x=1;</script>"
               "<style>.a{color:red}</style><p>more</p>"),
            "keep\n\nmore");
}

TEST_F(MarkdownExtractorFilterTest, StripsNoscriptTemplateSvg) {
  EXPECT_EQ(Md("<p>a</p><noscript>n</noscript>"
               "<template><p>t</p></template>"
               "<svg><text>s</text></svg><p>b</p>"),
            "a\n\nb");
}

TEST_F(MarkdownExtractorFilterTest, StripsChromeNavHeaderFooter) {
  EXPECT_EQ(Md("<nav>menu</nav><main>content</main><footer>foot</footer>"),
            "content");
  EXPECT_EQ(Md("<header>h</header><p>body</p>"), "body");
}

TEST_F(MarkdownExtractorFilterTest, StripsHeadContent) {
  // <title> in <head> must not leak as BODY text; it is promoted to YAML
  // front-matter (Issue F). <meta> with no description/canonical adds nothing.
  EXPECT_EQ(Md("<html><head><title>Page</title>"
               "<meta name=\"viewport\" content=\"x\"></head>"
               "<body><p>body</p></body></html>"),
            "---\ntitle: Page\n---\n\nbody");
}

// KILL-METRIC GUARD: <main>/<article> are content, never dropped.
TEST_F(MarkdownExtractorFilterTest, PreservesMainAndArticle) {
  EXPECT_EQ(Md("<article><h1>T</h1><p>body</p></article>"), "# T\n\nbody");
  EXPECT_EQ(Md("<main><p>real</p></main>"), "real");
}

// ---- inline hidden drop ---------------------------------------------------

TEST_F(MarkdownExtractorFilterTest, DropsInlineHidden) {
  EXPECT_EQ(Md("<p>vis</p><p style=\"display:none\">secret</p>"), "vis");
  EXPECT_EQ(Md("<p>vis</p><p style=\"display: none\">secret</p>"), "vis");
  EXPECT_EQ(Md("<div hidden>secret</div><div>shown</div>"), "shown");
  EXPECT_EQ(Md("<div aria-hidden=\"true\">secret</div><div>shown</div>"),
            "shown");
  // aria-hidden=false must NOT drop.
  EXPECT_EQ(Md("<div aria-hidden=\"false\">kept</div>"), "kept");
}

// ---- whitespace -----------------------------------------------------------

TEST_F(MarkdownExtractorFilterTest, CollapsesWhitespace) {
  EXPECT_EQ(Md("<p>a   b\n\n  c</p>"), "a b c");
  EXPECT_EQ(Md("  <p>  trimmed  </p>  "), "trimmed");
}

// ---- unknown tags / edge cases --------------------------------------------

TEST_F(MarkdownExtractorFilterTest, UnknownTagsPassText) {
  EXPECT_EQ(Md("<p>Hello <custom-tag>world</custom-tag></p>"), "Hello world");
  EXPECT_EQ(Md("<p>a <span>b</span> c</p>"), "a b c");
}

TEST_F(MarkdownExtractorFilterTest, EmptyInputIsEmpty) {
  EXPECT_EQ(Md(""), "");
  EXPECT_EQ(Md("<html><body></body></html>"), "");
  EXPECT_EQ(Md("<div id=\"root\"></div>"), "");
}

// ---- the moat: rendered substantive vs static near-empty ------------------

TEST_F(MarkdownExtractorFilterTest, MoatRenderedVsStatic) {
  // A CSR shell before hydration: an empty mount point -> empty markdown.
  const std::string kStatic =
      "<html><body><div id=\"root\"></div></body></html>";
  EXPECT_EQ(Md(kStatic), "");

  // The SAME page after hydration (the rendered outerHTML the worker reads) ->
  // substantive markdown. This binary gap is what the agent_optimize moat sells.
  const std::string kRendered =
      "<html><body><div id=\"root\">"
      "<main><h1>Hydrated</h1><p>Real content here.</p>"
      "<ul><li>one</li><li>two</li></ul></main>"
      "</div></body></html>";
  EXPECT_EQ(Md(kRendered), "# Hydrated\n\nReal content here.\n\n- one\n- two");
}

// A non-article page (index/listing) must fail OPEN to full extraction, not
// collapse to empty (the structural first cut has no Readability gate).
TEST_F(MarkdownExtractorFilterTest, NonArticlePageStillExtracts) {
  EXPECT_EQ(Md("<body><h2>Links</h2>"
               "<ul><li><a href=\"/a\">A</a></li>"
               "<li><a href=\"/b\">B</a></li></ul></body>"),
            "## Links\n\n- [A](/a)\n- [B](/b)");
}

// ---- P1.5 adversarial-review fixes ----------------------------------------

// MF-2 + SF-1: a content root (main/article/role=main) nested inside stripped
// chrome must be RECOVERED via the fail-open re-extract, never silently dropped.
TEST_F(MarkdownExtractorFilterTest, MainInsideNavRecovered) {
  EXPECT_EQ(Md("<nav><main><h1>T</h1><p>body</p></main></nav>"), "# T\n\nbody");
}
TEST_F(MarkdownExtractorFilterTest, ArticleInsideFooterRecovered) {
  // footer-nested article recovered (alongside the footer's other text — the
  // fail-open tradeoff; P2's Readability pass refines precision).
  std::string md = Md("<footer><article>A</article></footer><p>x</p>");
  EXPECT_NE(md.find('A'), std::string::npos);
  EXPECT_NE(md.find('x'), std::string::npos);
}
TEST_F(MarkdownExtractorFilterTest, RoleMainPreservedEvenInsideChrome) {
  EXPECT_EQ(Md("<div role=\"main\"><p>real</p></div>"), "real");
  EXPECT_EQ(Md("<nav><div role=\"main\"><p>real</p></div></nav>"), "real");
}

// MF-3: entity-decoded href/src/alt must be sanitized so a page cannot forge a
// new markdown block/link into the agent feed.
TEST_F(MarkdownExtractorFilterTest, HrefInjectionNeutralized) {
  // href with a decoded newline + ')' + '# PWNED' must not break out.
  std::string md = Md("<p><a href=\"/foo&#10;# PWNED\">t</a></p>");
  EXPECT_EQ(md.find('\n'), std::string::npos);    // no forged new line/block
  EXPECT_NE(md.find("[t]("), std::string::npos);  // still a single link
  EXPECT_EQ(md.find(") # PWNED"), std::string::npos);
}
TEST_F(MarkdownExtractorFilterTest, ImageAltAndSrcInjectionEscaped) {
  // alt that tries to close the image and open an attacker link.
  std::string md = Md("<p><img src=\"/a.png\" alt=\"](http://evil/)![\"></p>");
  EXPECT_EQ(md.find("](http://evil/)"), std::string::npos);  // no raw breakout
  EXPECT_NE(md.find("\\]"), std::string::npos);              // alt escaped
  // src that tries to break the parens.
  std::string md2 =
      Md("<p><img src=\"/a.png) ![x](http://evil/\" alt=\"y\"></p>");
  EXPECT_NE(md2.find("%29"), std::string::npos);  // ')' encoded
  EXPECT_EQ(md2.find("](http://evil/"), std::string::npos);
}

// MF-4: a backtick run inside <pre> must not break out of the code fence.
TEST_F(MarkdownExtractorFilterTest, PreFenceAdaptive) {
  EXPECT_EQ(Md("<pre>a ``` b</pre>"), "````\na ``` b\n````");
}

// SF-2: a backtick inside inline <code> must not close the span early.
TEST_F(MarkdownExtractorFilterTest, InlineCodeBacktickAdaptive) {
  EXPECT_EQ(Md("<p><code>a`b</code></p>"), "``a`b``");
}

// SF-3: paragraph text whose first char is a block marker is escaped so it can't
// forge a heading/list/quote.
TEST_F(MarkdownExtractorFilterTest, LeadingMarkerEscaped) {
  EXPECT_EQ(Md("<p># not a heading</p>"), "\\# not a heading");
  EXPECT_EQ(Md("<p>- not a list</p>"), "\\- not a list");
  // a real heading is unaffected.
  EXPECT_EQ(Md("<h1>Title</h1>"), "# Title");
}

// MF-1: deep nesting must not emit an unbounded per-block prefix.
TEST_F(MarkdownExtractorFilterTest, BlockquoteAndListDepthClamped) {
  std::string deep_bq;
  for (int i = 0; i < 50; ++i) deep_bq += "<blockquote>";
  deep_bq += "<p>x</p>";
  for (int i = 0; i < 50; ++i) deep_bq += "</blockquote>";
  std::string md = Md(deep_bq);
  // prefix clamped to <= 8 "> " -> at most 16 chars of prefix before "x".
  EXPECT_NE(md.find('x'), std::string::npos);
  EXPECT_LE(md.find('x'), static_cast<size_t>(17));

  std::string deep_list;
  for (int i = 0; i < 50; ++i) deep_list += "<ul><li>i";
  for (int i = 0; i < 50; ++i) deep_list += "</li></ul>";
  std::string mdl = Md(deep_list);
  // deepest indent clamped to <= 15 levels * 2 = 30 spaces.
  EXPECT_NE(mdl.find("- "), std::string::npos);
}

// MF-1: total output is bounded regardless of input size (~8 MiB cap).
TEST_F(MarkdownExtractorFilterTest, OutputSizeCapped) {
  std::string html = "<body>";
  const std::string block = "<p>" + std::string(1800, 'a') + "</p>";
  for (int i = 0; i < 6000; ++i) html += block;  // ~10.8 MB input
  html += "</body>";
  std::string md = Md(html);
  EXPECT_LT(md.size(),
            static_cast<size_t>(9u << 20));  // capped well under input
}

// Consolidated review: the ~8 MiB cap must also bound the INTERMEDIATE staging
// buffers, not just the final output. A single never-flushed block holding one
// huge text node would otherwise grow cur_ without limit — markdown_ stays
// empty until the block flushes, so the output-size cap never trips.
TEST_F(MarkdownExtractorFilterTest, SingleGiantBlockBounded) {
  std::string html =
      "<body><p>" + std::string(20u << 20, 'a') + "</p></body>";  // 20 MB node
  std::string md = Md(html);
  EXPECT_LT(md.size(), static_cast<size_t>(9u << 20));
}

// Same bound for one huge inline <code> span (the code_buf_ staging path).
TEST_F(MarkdownExtractorFilterTest, GiantInlineCodeBounded) {
  std::string html =
      "<body><p><code>" + std::string(20u << 20, 'a') + "</code></p></body>";
  std::string md = Md(html);
  EXPECT_LT(md.size(), static_cast<size_t>(9u << 20));
}

// ---- Issue F: citation-readiness -----------------------------------------

// (1) Entity decode: body-text entities must be decoded so the agent reads the
// human glyphs, not '&amp;'/'&lt;'. Decode runs BEFORE structural escaping.
TEST_F(MarkdownExtractorFilterTest, DecodesHtmlEntities) {
  EXPECT_EQ(Md("<p>Tom &amp; Jerry &lt;1ms&gt;</p>"), "Tom & Jerry <1ms>");
}

// (1, security) Decode-then-escape preserves the structural-escape contract: a
// page that encodes '<script>' must decode to inert TEXT, never an active tag,
// and must not let a decoded entity forge a markdown block at the start of a
// paragraph. The decoded '<'/'>' are plain text in markdown (no breakout).
TEST_F(MarkdownExtractorFilterTest, DecodedScriptStaysInertText) {
  // The decoded text is literal '<script>alert(1)</script>' as body, not a tag.
  EXPECT_EQ(Md("<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>"),
            "<script>alert(1)</script>");
  // A decoded leading block marker is still escaped after decode (no forged
  // heading): '&#35;' -> '#' must become '\#'.
  EXPECT_EQ(Md("<p>&#35; not a heading</p>"), "\\# not a heading");
}

// (1, NBSP) Unescape emits Latin-1 0xA0 for &nbsp;; fold it to an ASCII space so
// the feed never ships a lone invalid-UTF-8 byte.
TEST_F(MarkdownExtractorFilterTest, DecodesNbspToSpace) {
  EXPECT_EQ(Md("<p>a&nbsp;b</p>"), "a b");
  // And it collapses like any other whitespace.
  EXPECT_EQ(Md("<p>a&nbsp;&nbsp;b</p>"), "a b");
}

// ---- Issue F blocker: entity-decode must never destroy non-ASCII text ------

namespace {
// Reject the presence of any lone Latin-1 byte (a non-UTF-8 single byte) and any
// malformed UTF-8 by re-validating the produced markdown end-to-end. A correct
// extractor's output is always well-formed UTF-8.
bool MarkdownIsValidUtf8(std::string_view s) {
  std::size_t i = 0;
  const std::size_t n = s.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t need;
    unsigned int cp;
    if (c < 0x80) {
      ++i;
      continue;
    } else if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07;
    } else {
      return false;  // lone continuation byte or invalid lead (e.g. 0xB7).
    }
    if (i + need >= n) return false;
    for (std::size_t k = 1; k <= need; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;  // overlong, surrogate, or out-of-range.
    }
    i += need + 1;
  }
  return true;
}
}  // namespace

// BLOCKER 1: the prod input is Chrome's rendered outerHTML, where em-dashes,
// curly quotes, and accented chars are literal multibyte UTF-8 — not entities.
// Unescape errors (decoding_error=true) on any byte > 127 and returns EMPTY, so
// the whole text node was being dropped. A literal-UTF-8 sentence must survive
// VERBATIM (raw-fallback preserves the original bytes).
TEST_F(MarkdownExtractorFilterTest, PreservesLiteralUtf8TextNode) {
  // "It’s a 5—second café test." — curly apostrophe (U+2019),
  // em dash (U+2014), and an accented 'é' (U+00E9), all as real UTF-8 bytes.
  const std::string html =
      "<main><p>It\xE2\x80\x99s a 5\xE2\x80\x94second caf\xC3\xA9 test.</p>"
      "</main>";
  const std::string expected =
      "It\xE2\x80\x99s a 5\xE2\x80\x94second caf\xC3\xA9 test.";
  EXPECT_EQ(Md(html), expected);
  EXPECT_TRUE(MarkdownIsValidUtf8(Md(html)));
}

// Regression guard: pure-ASCII entities still decode to their glyphs. (A leading
// '>' from a standalone &gt; is still EscapeLeadingMarker-escaped by the security
// contract, so the entities are exercised mid-text where no marker-escape fires.)
TEST_F(MarkdownExtractorFilterTest, DecodesAsciiEntities) {
  EXPECT_EQ(Md("<main><p>&amp;</p></main>"), "&");
  EXPECT_EQ(Md("<main><p>&lt;</p></main>"), "<");
  EXPECT_EQ(Md("<main><p>a &amp; b &lt; c &gt; d</p></main>"), "a & b < c > d");
}

// BLOCKER 2: a named/numeric entity that decodes to a single Latin-1 byte
// 0x80-0xFF (&middot;=0xB7, &copy;=0xA9, &eacute;=0xE9) must NOT ship that lone
// invalid byte. Under the raw-fallback design they stay as literal entity text;
// either way the output must be valid UTF-8 and contain none of the raw bytes.
TEST_F(MarkdownExtractorFilterTest, NamedNonAsciiEntityNoInvalidByte) {
  for (const char* html :
       {"<main><p>a&middot;b</p></main>", "<main><p>a&copy;b</p></main>",
        "<main><p>a&eacute;b</p></main>"}) {
    const std::string md = Md(html);
    EXPECT_TRUE(MarkdownIsValidUtf8(md)) << "not valid UTF-8 for: " << html;
    EXPECT_EQ(md.find('\xB7'), std::string::npos) << "raw 0xB7 in: " << html;
    EXPECT_EQ(md.find('\xA9'), std::string::npos) << "raw 0xA9 in: " << html;
    EXPECT_EQ(md.find('\xE9'), std::string::npos) << "raw 0xE9 in: " << html;
    EXPECT_FALSE(md.empty()) << "text dropped for: " << html;
  }
  // Documented behavior: raw-fallback leaves the original entity text intact.
  EXPECT_EQ(Md("<main><p>a&middot;b</p></main>"), "a&middot;b");
}

// NBSP must fold to an ASCII space EVERYWHERE — in normal collapsed text AND in
// the <pre> verbatim branch (which bypasses the whitespace pass).
TEST_F(MarkdownExtractorFilterTest, NbspFoldedEverywhere) {
  const std::string normal = Md("<main><p>a&nbsp;b</p></main>");
  EXPECT_EQ(normal, "a b");
  EXPECT_EQ(normal.find('\xA0'), std::string::npos);

  const std::string pre = Md("<main><pre>a&nbsp;b</pre></main>");
  EXPECT_EQ(pre.find('\xA0'), std::string::npos);
  EXPECT_NE(pre.find("a b"), std::string::npos);
}

// A text node mixing literal UTF-8 and an ASCII entity must keep BOTH the
// accented word and the trailing text (the &amp; may stay literal under
// raw-fallback; the assertion is that no text is dropped).
TEST_F(MarkdownExtractorFilterTest, MixedUtf8AndEntity) {
  const std::string md = Md("<main><p>caf\xC3\xA9 &amp; more</p></main>");
  EXPECT_TRUE(MarkdownIsValidUtf8(md));
  EXPECT_NE(md.find("caf\xC3\xA9"), std::string::npos);  // accented word kept
  EXPECT_NE(md.find("more"), std::string::npos);         // trailing text kept
}

// (5) A block element wrapped in an anchor (card markup) must flatten to a
// single '[text](href)', not split into a dangling '[' + heading + '](href)'.
TEST_F(MarkdownExtractorFilterTest, AnchorWrappingHeadingStaysOneLink) {
  EXPECT_EQ(Md("<a href=\"/x/\"><h2>Title</h2><p>desc</p></a>"),
            "[Title: desc](/x/)");
}

// (3) An <article>-scoped <header> is content (blog title/date/byline), not
// chrome: its <h1> is preserved; the <time> stamp is captured (not body text).
TEST_F(MarkdownExtractorFilterTest, ArticleHeaderTitleAndDatePreserved) {
  EXPECT_EQ(Md("<article><header>"
               "<time datetime=\"2026-05-28\">May 28</time>"
               "<h1>Post</h1></header><p>body</p></article>"),
            "# Post\n\nbody");
}

// (4) Bare-<body> chrome (skip-link, promo banner) that sits OUTSIDE a <main>/
// <article> content root must be dropped when such a root exists.
TEST_F(MarkdownExtractorFilterTest, BareBodyChromeOutsideMainDropped) {
  EXPECT_EQ(Md("<body><a href=\"#m\">Skip to main content</a>"
               "<div id=\"banner\">Promo</div>"
               "<main><p>real</p></main></body>"),
            "real");
}

// (2) <title>/<meta description>/<link canonical> become YAML front-matter, not
// leaked body text.
TEST_F(MarkdownExtractorFilterTest, EmitsFrontMatterFromHead) {
  EXPECT_EQ(Md("<html><head><title>T</title>"
               "<meta name=\"description\" content=\"D\">"
               "<link rel=\"canonical\" href=\"https://x/\"></head>"
               "<body><main><p>b</p></main></body></html>"),
            "---\ntitle: T\ndescription: D\ncanonical: https://x/\n---\n\nb");
}

// ---- citation-readiness round 2 (post-v2.0.27 audit) ----------------------

// Issue F fixed entity decode for body TEXT nodes (AppendText), but inline
// <code> spans accumulate into a separate code_buf_ that bypassed the decode,
// so the agent read a literal '&lt;style&gt;' instead of '<style>'. Markdown
// does NOT unescape inside backticks, so the entity must be decoded here too.
// (100 occurrences across 8 prod pages: css-filters tables, features, blogs.)
TEST_F(MarkdownExtractorFilterTest, DecodesEntitiesInsideInlineCode) {
  EXPECT_EQ(Md("<p>Use <code>&lt;style&gt;</code> tags</p>"),
            "Use `<style>` tags");
  EXPECT_EQ(Md("<p><code>a &amp;&amp; b</code></p>"), "`a && b`");
  // Decoding inside a table cell's inline code (the css-filters prod pattern).
  EXPECT_EQ(Md("<table><tr><td><code>&lt;link&gt;</code></td></tr></table>"),
            "| `<link>` |\n| --- |");
}

// The code-span decode must be UTF-8-safe: a literal multibyte glyph inside
// <code> must survive verbatim, never be erased by the Unescape non-ASCII trap.
TEST_F(MarkdownExtractorFilterTest, InlineCodePreservesNonAsciiVerbatim) {
  EXPECT_EQ(Md("<p><code>caf\xC3\xA9()</code></p>"), "`caf\xC3\xA9()`");
}

// Issue F (2) follow-up: the front-matter description is sourced from a decoded
// attribute value, which HtmlKeywords::Unescape returns EMPTY for on any byte
// > 127 (em-dash, curly quote, accent) — silently dropping the description on
// every prod page whose meta description contains an em-dash (about,
// ai-readability, 2 blogs). The metadata pass must be UTF-8-safe.
TEST_F(MarkdownExtractorFilterTest, FrontMatterDescriptionPreservesNonAscii) {
  // "Fast \xE2\x80\x94 really" = "Fast — really" (U+2014 EM DASH).
  const std::string md =
      Md("<html><head><title>T</title>"
         "<meta name=\"description\" content=\"Fast \xE2\x80\x94 really\">"
         "</head><body><main><p>b</p></main></body></html>");
  EXPECT_NE(md.find("description: Fast \xE2\x80\x94 really"),
            std::string::npos);
}

// A fenced code block must keep its language label (Astro/Shiki emits
// <pre data-language="nginx">), so the agent gets a tagged fence ```nginx.
TEST_F(MarkdownExtractorFilterTest, FencedCodeKeepsLanguageLabel) {
  EXPECT_EQ(Md("<pre data-language=\"nginx\">pagespeed on;</pre>"),
            "```nginx\npagespeed on;\n```");
  // A bare <pre> with no language stays an untagged fence (no regression).
  EXPECT_EQ(Md("<pre>plain</pre>"), "```\nplain\n```");
}

// Hand-authored (non-Shiki) blocks carry the language on a <code class=
// "language-X"> token, not a <pre data-language>; that fence must be tagged too.
TEST_F(MarkdownExtractorFilterTest, FencedCodeLanguageFromCodeClass) {
  EXPECT_EQ(Md("<pre><code class=\"language-bash\">npm i</code></pre>"),
            "```bash\nnpm i\n```");
  // A non-language class token must not be mistaken for a language.
  EXPECT_EQ(Md("<pre><code class=\"hljs\">x</code></pre>"), "```\nx\n```");
}

// A <details>/<summary> disclosure (the FAQ pattern on pricing/features/1.1)
// emits the summary as a heading so each Q is a citable anchor, not a bare
// paragraph indistinguishable from its answer.
TEST_F(MarkdownExtractorFilterTest, DetailsSummaryBecomesHeading) {
  EXPECT_EQ(Md("<details><summary>Is it free?</summary><p>Yes.</p></details>"),
            "### Is it free?\n\nYes.");
}

// ---- review hardening (round-2 adversarial pass) --------------------------

// A decoded numeric newline ('&#10;') inside a <code> span must be folded to a
// space: otherwise it would end the paragraph, orphan the opening backtick, and
// let the trailing text forge an ATX heading (a structure-forgery escape the
// rest of the filter is hardened against).
TEST_F(MarkdownExtractorFilterTest, InlineCodeFoldsDecodedNewline) {
  const std::string md = Md("<p>x<code>a&#10;&#10;# I</code></p>");
  EXPECT_EQ(md, "x`a  # I`");
  EXPECT_EQ(md.find("\n# "), std::string::npos);  // no forged heading
}

// A backtick produced by DECODING (&#96;) inside a code span must widen the
// adaptive fence so the span cannot be closed early.
TEST_F(MarkdownExtractorFilterTest, InlineCodeDecodedBacktickAdaptiveFence) {
  EXPECT_EQ(Md("<p><code>a&#96;b</code></p>"), "``a`b``");
}

// A <summary> wrapped in an open <a> (malformed/adversarial: a summary outside
// <details>) must flatten into the single link like any other block element
// (Issue F (5) parity), NOT forge a '### ' heading and shatter the anchor.
TEST_F(MarkdownExtractorFilterTest,
       SummaryInsideAnchorFlattensNoForgedHeading) {
  const std::string md = Md("<a href=\"/x\"><summary>Q</summary><p>d</p></a>");
  EXPECT_EQ(md, "[Q: d](/x)");
  EXPECT_EQ(md.find("### "), std::string::npos);  // no forged heading
}

// ---- stacked-commit fixes (remaining-issues analysis) ---------------------

// A <select> (interactive form control) and its <label> are hard-stripped: the
// 1.1 docs ship a server-rendered mobile-nav <select> INSIDE <main> whose ~40
// <option> page names would otherwise leak into the agent feed.
TEST_F(MarkdownExtractorFilterTest, StripsSelectAndLabel) {
  EXPECT_EQ(Md("<main><label>Navigate docs</label>"
               "<select><optgroup label=\"G\"><option>Getting Started</option>"
               "<option>Upgrading</option></optgroup></select>"
               "<p>real body</p></main>"),
            "real body");
  EXPECT_EQ(Md("<main><p>before</p><select><option>X</option>"
               "<option>Y</option></select><p>after</p></main>"),
            "before\n\nafter");
  EXPECT_EQ(Md("<select><option>a</option></select>"), "");
  // REGRESSION GUARD: <svg> still hard-stripped after the IsHardStripByName
  // refactor that also added <label>.
  EXPECT_EQ(Md("<p>a</p><svg><path/></svg><p>b</p>"), "a\n\nb");
}

// A <dl>/<dt>/<dd> definition list emits the term in bold and the definition as
// a following paragraph so an agent binds question to answer (home FAQ shape).
TEST_F(MarkdownExtractorFilterTest, DefinitionListTermBoldDefinition) {
  EXPECT_EQ(Md("<dl><dt>Term</dt><dd>Definition.</dd></dl>"),
            "**Term**\n\nDefinition.");
  EXPECT_EQ(Md("<dl><dt>Q1</dt><dd>A1</dd><dt>Q2</dt><dd>A2</dd></dl>"),
            "**Q1**\n\nA1\n\n**Q2**\n\nA2");
  // Multiple <dd> per <dt> (the home FAQ Q1 shape).
  EXPECT_EQ(Md("<dl><dt>K</dt><dd>one</dd><dd>two</dd></dl>"),
            "**K**\n\none\n\ntwo");
  // dt/dd wrapped in a <div> (the actual home FAQ DOM).
  EXPECT_EQ(Md("<dl><div><dt>Does it add latency?</dt>"
               "<dd>No. Sub-millisecond.</dd></div></dl>"),
            "**Does it add latency?**\n\nNo. Sub-millisecond.");
  // EscapeLeadingMarker still applies to the (attacker-influenced) term body.
  EXPECT_EQ(Md("<dl><dt>- danger</dt><dd>ok</dd></dl>"),
            "**\\- danger**\n\nok");
  // A term carrying its own emphasis (inline <strong>, or a literal '*') must
  // NOT be double-wrapped into malformed nesting ("**Hello **World****").
  EXPECT_EQ(Md("<dl><dt>Hello <strong>World</strong></dt><dd>x</dd></dl>"),
            "Hello **World**\n\nx");
  const std::string md = Md("<dl><dt>*x*</dt><dd>y</dd></dl>");
  EXPECT_EQ(md, "*x*\n\ny");
  EXPECT_EQ(md.find("***"), std::string::npos);  // no triple-asterisk run
}

// A block child (kP/kDiv/kSection/kAside) of an open <li> folds into the item
// line instead of detaching: the features grid's icon-badge rows become one
// citable line each, segments separated by a single space (consistent
// regardless of inter-block source whitespace). Worker input is Chrome's
// rendered DOM, so &check; is a literal ✓ (\xE2\x9C\x93) here, not an entity.
TEST_F(MarkdownExtractorFilterTest, ListItemFoldsBlockChildren) {
  EXPECT_EQ(
      Md("<ul><li><span>x</span><div><p>Title</p><p>Desc</p></div></li></ul>"),
      "- x Title Desc");
  EXPECT_EQ(Md("<ul><li><span>\xE2\x9C\x93</span><div>"
               "<p>WebP/AVIF transcoding</p>"
               "<p>Content-negotiated format selection.</p></div></li></ul>"),
            "- \xE2\x9C\x93 WebP/AVIF transcoding Content-negotiated format "
            "selection.");
  // Inter-block whitespace (pretty-printed HTML) must NOT change the join: still
  // a single space, never a doubled space or a dropped separator.
  EXPECT_EQ(Md("<ul><li><span>x</span><div><p>Title</p>\n  "
               "<p>Desc</p></div></li></ul>"),
            "- x Title Desc");
}

// REGRESSION GUARDS for the list-item fold (it touches the hot list path).
TEST_F(MarkdownExtractorFilterTest, ListItemFoldRegressionGuards) {
  EXPECT_EQ(Md("<ul><li>a</li><li>b</li></ul>"), "- a\n- b");  // plain items
  EXPECT_EQ(Md("<ul><li>a<ul><li>b</li></ul></li></ul>"),
            "- a\n  - b");  // nested real list still detaches
  // Inline-only "Not in scope" pattern (no block child) unchanged.
  EXPECT_EQ(Md("<ul><li><span>Edge delivery</span>: use your CDN.</li></ul>"),
            "- Edge delivery: use your CDN.");
  // A block-in-anchor inside a list item is still owned by the anchor flatten.
  EXPECT_EQ(Md("<ul><li><a href=\"/x\"><div>card title</div>"
               "<div>card body</div></a></li></ul>"),
            "- [card title: card body](/x)");
  // A nested <pre> inside a list item still fences on its own (not folded).
  EXPECT_EQ(Md("<ul><li>run<pre>x=1</pre></li></ul>"),
            "- run\n\n```\nx=1\n```");
}

// SECURITY: a list item can carry an interior newline (a <br>, or a folded
// block child with a <br>); the text after it sits at column 0 and must NOT be
// able to forge a heading/list/quote/table into the agent feed. The marker is
// backslash-escaped per interior line.
TEST_F(MarkdownExtractorFilterTest,
       ListItemInteriorNewlineCannotForgeStructure) {
  // Folded path (the adversarial-review ship-blocker).
  const std::string folded =
      Md("<ul><li><div>x<br></div><p># PWNED</p></li></ul>");
  EXPECT_EQ(folded.find("\n# "), std::string::npos);  // no column-0 heading
  EXPECT_NE(folded.find("\\#"), std::string::npos);   // escaped instead
  // Non-folded <br> path (pre-existing list-item branch, now also covered).
  const std::string br = Md("<ul><li>a<br># x</li></ul>");
  EXPECT_EQ(br.find("\n# "), std::string::npos);
  EXPECT_EQ(br.find("\n- "), std::string::npos);
}

// A block marker at the FIRST character of a list-item body must also be escaped:
// the list-item FlushBlock branch is the only block emitter that historically
// skipped EscapeLeadingMarker, so attacker-influenced text like "# PWNED" inside
// an <li> emitted "- # PWNED", which a CommonMark consumer parses as an ATX
// heading nested in the list item — the same structure-forgery the interior-line
// escape (and every other block branch) exists to prevent.
TEST_F(MarkdownExtractorFilterTest,
       ListItemFirstLineMarkerCannotForgeStructure) {
  EXPECT_EQ(Md("<ul><li># PWNED</li></ul>"), "- \\# PWNED");
  EXPECT_EQ(Md("<ul><li>> quote</li></ul>"), "- \\> quote");
  EXPECT_EQ(Md("<ul><li>| a | b |</li></ul>"), "- \\| a | b |");
}

}  // namespace
}  // namespace net_instaweb
