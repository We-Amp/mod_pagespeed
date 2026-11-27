// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Unit tests for MarkdownExtractorFilter / ExtractAgentMarkdown.
// Pure string -> string; no CDP/Chrome. Each test pins one element mapping, a
// strip rule, whitespace handling, or the render-vs-static "moat" behavior.

#include "lib/html/markdown_extractor_filter.h"

#include <iterator>
#include <random>
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
  EXPECT_EQ(Md("<p>Tom &amp; Jerry &lt;1ms&gt;</p>"), "Tom & Jerry \\<1ms>");
}

// (1, security) Decode-then-escape preserves the structural-escape contract: a
// page that encodes '<script>' must decode to inert TEXT, never an active tag,
// and must not let a decoded entity forge a markdown block at the start of a
// paragraph. The decoded '<'/'>' are plain text in markdown (no breakout).
TEST_F(MarkdownExtractorFilterTest, DecodedScriptStaysInertText) {
  // The decoded text is literal '<script>alert(1)</script>' as body, escaped
  // so no Markdown renderer reads it as a tag.
  EXPECT_EQ(Md("<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>"),
            "\\<script>alert(1)\\</script>");
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
  EXPECT_EQ(Md("<main><p>&lt;</p></main>"), "\\<");
  EXPECT_EQ(Md("<main><p>a &amp; b &lt; c &gt; d</p></main>"),
            "a & b \\< c > d");
}

// A named reference above U+007F (&middot;, &copy;, &eacute;) decodes to its
// UTF-8 encoding, never to a lone Latin-1 byte.
TEST_F(MarkdownExtractorFilterTest, NamedNonAsciiEntityNoInvalidByte) {
  EXPECT_EQ(Md("<main><p>a&middot;b</p></main>"), "a\xC2\xB7\x62");
  EXPECT_EQ(Md("<main><p>a&copy;b</p></main>"), "a\xC2\xA9\x62");
  EXPECT_EQ(Md("<main><p>a&eacute;b</p></main>"), "a\xC3\xA9\x62");
  EXPECT_TRUE(MarkdownIsValidUtf8(Md("<main><p>a&eacute;b</p></main>")));
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

// A page nested past the parser's depth limit is passed through from there as
// raw text. The Markdown copy stops at that point: script bodies, hidden form
// values and other markup in the rest of the page are not copied.
TEST_F(MarkdownExtractorFilterTest, StopsWhereTheRestOfAPageIsPassedThrough) {
  std::string html = "<html><body><p>ordinary paragraph</p>";
  for (int i = 0; i < 520; ++i) html += "<div>";
  html +=
      "<script>secret()</script><input type=hidden value=tok123>"
      "<!-- hidden note --><p>after the limit</p>";
  for (int i = 0; i < 520; ++i) html += "</div>";
  html += "</body></html>";
  const std::string md = Md(html);
  EXPECT_NE(md.find("ordinary paragraph"), std::string::npos) << md;
  EXPECT_EQ(md.find("secret"), std::string::npos) << md;
  EXPECT_EQ(md.find("tok123"), std::string::npos) << md;
  EXPECT_EQ(md.find("hidden note"), std::string::npos) << md;
  EXPECT_EQ(md.find("<script"), std::string::npos) << md;
}

// Same, with more than one 4 KiB feed piece of ordinary content before the
// limit, so the switch is first seen in a later piece and the extraction runs
// again, a byte at a time, from the start of that piece.
TEST_F(MarkdownExtractorFilterTest,
       StopsWhereTheRestIsPassedThroughAfterManyPieces) {
  std::string html = "<html><body><p>ordinary paragraph</p>";
  while (html.size() < 3 * 4096 + 100) html += "<p>filler text here</p>";
  html += "<p>last ordinary</p>";
  for (int i = 0; i < 520; ++i) html += "<div>";
  html +=
      "<script>secret()</script><input type=hidden value=tok123>"
      "<!-- hidden note --><p>after the limit</p>";
  for (int i = 0; i < 520; ++i) html += "</div>";
  html += "</body></html>";
  const std::string md = Md(html);
  EXPECT_NE(md.find("ordinary paragraph"), std::string::npos) << md;
  EXPECT_NE(md.find("last ordinary"), std::string::npos) << md;
  EXPECT_EQ(md.find("secret"), std::string::npos) << md;
  EXPECT_EQ(md.find("tok123"), std::string::npos) << md;
  EXPECT_EQ(md.find("hidden note"), std::string::npos) << md;
}

// ---- <header> inside a section is content ---------------------------------

// A docs or example page puts its only H1 in a <header> inside <main>. That is
// the page's heading block, not the site banner: its eyebrow and H1 stay.
TEST_F(MarkdownExtractorFilterTest, HeaderInsideMainKeepsH1) {
  EXPECT_EQ(Md("<body><header><a href=\"/\">Site</a><nav>menu</nav></header>"
               "<main><header class=\"section-header\">"
               "<p class=\"eyebrow\">filter \xC2\xB7 CSS filters</p>"
               "<h1 id=\"filter\">Move CSS above scripts</h1></header>"
               "<dl><dt>Category:</dt><dd>CSS</dd></dl></main>"
               "<footer>foot</footer></body>"),
            "filter \xC2\xB7 CSS filters\n\n# Move CSS above scripts\n\n"
            "**Category:**\n\nCSS");
}

// Landing-page sections carry their H2 and intro text in a <header>.
TEST_F(MarkdownExtractorFilterTest, HeaderInsideSectionKeepsHeading) {
  EXPECT_EQ(Md("<main><section><header class=\"section-header\">"
               "<h2>Install. Configure. Verify.</h2><p>Three steps.</p>"
               "</header><p>Body.</p></section></main>"),
            "## Install. Configure. Verify.\n\nThree steps.\n\nBody.");
  // The same without a <main>: a section's header is still content.
  EXPECT_EQ(Md("<header>Banner</header><section><header><h2>Why</h2></header>"
               "<p>Because.</p></section>"),
            "## Why\n\nBecause.");
}

// The page banner <header> stays chrome, and <nav> and a section <footer>
// are still stripped inside <main>.
TEST_F(MarkdownExtractorFilterTest, BannerHeaderAndNavInMainStillStripped) {
  EXPECT_EQ(Md("<header><h1>Site name</h1></header>"
               "<main><nav>Docs menu</nav><h1>Page</h1><p>text</p>"
               "<footer>Edit this page</footer></main>"),
            "# Page\n\ntext");
}

// ---- code keeps its line breaks --------------------------------------------

// A multi-line command in a <code> styled white-space: pre (no <pre>) is a
// code block: joining its lines would make a different, wrong command.
TEST_F(MarkdownExtractorFilterTest, MultiLineCodeOutsidePreIsFenced) {
  EXPECT_EQ(Md("<main><div class=\"install-panel\">"
               "<code class=\"install-command\">curl -fsSL "
               "https://example.com/install.sh | sudo sh\n"
               "sudo apt-get install pagespeed-optimizer\n"
               "sudo apt-get install nginx-module-pagespeed</code>"
               "<button aria-label=\"Copy\"><svg></svg></button>"
               "</div></main>"),
            "```\ncurl -fsSL https://example.com/install.sh | sudo sh\n"
            "sudo apt-get install pagespeed-optimizer\n"
            "sudo apt-get install nginx-module-pagespeed\n```");
  // Continuation lines keep their backslash and indentation.
  EXPECT_EQ(Md("<main><div><code>docker run --rm -p 80:80 \\\n"
               "  --add-host=host.docker.internal:host-gateway \\\n"
               "  ghcr.io/example/image:latest</code></div></main>"),
            "```\ndocker run --rm -p 80:80 \\\n"
            "  --add-host=host.docker.internal:host-gateway \\\n"
            "  ghcr.io/example/image:latest\n```");
  // The language class tags the fence.
  EXPECT_EQ(Md("<div><code class=\"language-bash\">a\nb</code></div>"),
            "```bash\na\nb\n```");
}

// A <code> inside a sentence stays an inline span; its line breaks are source
// formatting and fold to spaces.
TEST_F(MarkdownExtractorFilterTest, MultiLineCodeInsideSentenceStaysInline) {
  EXPECT_EQ(Md("<p>Run <code>make\nall</code> now</p>"), "Run `make all` now");
  EXPECT_EQ(Md("<table><tr><td><code>a\nb</code></td></tr></table>"),
            "| `a b` |\n| --- |");
  EXPECT_EQ(Md("<ul><li><code>a\nb</code></li></ul>"), "- `a b`");
}

// Content with backtick runs gets a longer fence, so it never closes early.
TEST_F(MarkdownExtractorFilterTest, CodeBlockFenceOutgrowsBackticks) {
  EXPECT_EQ(Md("<div><code>echo ```\nx</code></div>"),
            "````\necho ```\nx\n````");
  EXPECT_EQ(Md("<pre>a\n````\nb</pre>"), "`````\na\n````\nb\n`````");
}

TEST_F(MarkdownExtractorFilterTest, BrInsidePreIsALineBreak) {
  EXPECT_EQ(Md("<pre>line1<br>line2<br/>line3</pre>"),
            "```\nline1\nline2\nline3\n```");
}

// A diff view renders one <div> per line inside <pre><code>. The lines stay
// inside one fence (escaped markup in them stays text), the indentation the
// source formatting adds is removed, and no empty fence follows.
TEST_F(MarkdownExtractorFilterTest, PreWithBlockLinesStaysOneFence) {
  EXPECT_EQ(Md("<main><pre><code>\n"
               "  <div class=\"line\">\n"
               "      \xE2\x80\xA6 1 unchanged line \xE2\x80\xA6\n"
               "    </div><div class=\"line\">\n"
               "        &lt;head&gt;\n"
               "    </div><div class=\"line del\">\n"
               "      -     &lt;style&gt;\n"
               "    </div><div class=\"line add\">\n"
               "      +     &lt;link rel=stylesheet&gt;\n"
               "    </div>\n"
               "</code></pre><p>After.</p></main>"),
            "```\n"
            "\xE2\x80\xA6 1 unchanged line \xE2\x80\xA6\n"
            "  <head>\n"
            "-     <style>\n"
            "+     <link rel=stylesheet>\n"
            "```\n\nAfter.");
  // Ordinary <pre> text keeps blank lines and indentation as written.
  EXPECT_EQ(Md("<pre>  a\n\n  b</pre>"), "```\n  a\n\n  b\n```");
}

// ---- every HTML5 reference decodes --------------------------------------------

// A text node with literal UTF-8 and references decodes every reference.
TEST_F(MarkdownExtractorFilterTest, DecodesHtml5ReferencesNextToUtf8) {
  EXPECT_EQ(Md("<main><p>injects critical CSS &mdash; optimized off the "
               "request path &middot; served from cache</p></main>"),
            "injects critical CSS \xE2\x80\x94 optimized off the request path "
            "\xC2\xB7 served from cache");
  EXPECT_EQ(Md("<main><p><a href=\"/download/\">Download &amp; run "
               "\xE2\x86\x92</a></p></main>"),
            "[Download & run \xE2\x86\x92](/download/)");
  EXPECT_EQ(Md("<main><table><tr><th>A</th></tr><tr><td>&check;</td></tr>"
               "</table></main>"),
            "| A |\n| --- |\n| \xE2\x9C\x93 |");
  EXPECT_EQ(Md("<main><p>a &bogus; b &rsquo; c</p></main>"),
            "a &bogus; b \xE2\x80\x99 c");
}

// The front-matter title decodes the same way.
TEST_F(MarkdownExtractorFilterTest, FrontMatterTitleDecodesReferences) {
  EXPECT_EQ(Md("<html><head><title>Download 2.10 \xE2\x80\x94 Apache, Docker "
               "&amp; Helm</title></head><body><p>x</p></body></html>"),
            "---\ntitle: Download 2.10 \xE2\x80\x94 Apache, Docker & Helm\n"
            "---\n\nx");
}

// ---- escaped text stays text -----------------------------------------------

// "&lt;img&gt;" in the page is the text "<img>". In the Markdown copy it is
// escaped so no renderer turns it into a live tag, in prose and table cells.
TEST_F(MarkdownExtractorFilterTest, EscapedMarkupStaysTextInCells) {
  EXPECT_EQ(Md("<main><table><tr><th>Filter</th><th>What</th></tr>"
               "<tr><td>insert_image_dimensions</td>"
               "<td>Adds width and height attributes to &lt;img&gt; tags</td>"
               "</tr><tr><td>inline_import_to_link</td>"
               "<td>Converts &lt;style&gt;@import&lt;/style&gt; to "
               "&lt;link&gt;</td></tr></table></main>"),
            "| Filter | What |\n| --- | --- |\n"
            "| insert_image_dimensions | Adds width and height attributes to "
            "\\<img> tags |\n"
            "| inline_import_to_link | Converts \\<style>@import\\</style> to "
            "\\<link> |");
}

TEST_F(MarkdownExtractorFilterTest, EscapedMarkupStaysTextInProse) {
  EXPECT_EQ(Md("<main><p>Use a &lt;picture&gt; element.</p></main>"),
            "Use a \\<picture> element.");
  EXPECT_EQ(Md("<main><h2>The &lt;img&gt; tag</h2></main>"),
            "## The \\<img> tag");
  EXPECT_EQ(Md("<main><p><a href=\"/x\">&lt;b&gt; tags</a></p></main>"),
            "[\\<b> tags](/x)");
  EXPECT_EQ(Md("<main><p>See &lt;https://example.com/&gt;</p></main>"),
            "See \\<https://example.com/>");
  EXPECT_EQ(Md("<main><p><img src=\"/i\" alt=\"&lt;b&gt;\"></p></main>"),
            "![\\<b>](/i)");
  // Text that reads as a reference keeps its '&' literal.
  EXPECT_EQ(Md("<main><p>Write &amp;lt; or &amp;#60; or &amp;copy;</p></main>"),
            "Write \\&lt; or \\&#60; or \\&copy;");
  // A plain '&' needs nothing.
  EXPECT_EQ(Md("<main><p>AT&amp;T &amp; co</p></main>"), "AT&T & co");
  // Code keeps its text literal: no escape inside backticks or fences.
  EXPECT_EQ(Md("<main><p>Use <code>&lt;img&gt;</code></p></main>"),
            "Use `<img>`");
}

namespace {

// True when every `ch` in `md` is escaped: preceded by an odd number of
// backslashes. This is how CommonMark (markdown-it, cmark-gfm) reads a
// backslash run: pairs are literal backslashes, an odd one escapes the next
// character. For test inputs without code spans or fences.
bool EveryCharEscaped(std::string_view md, char ch) {
  for (std::size_t i = 0; i < md.size(); ++i) {
    if (md[i] != ch) continue;
    std::size_t run = 0;
    while (run < i && md[i - 1 - run] == '\\') ++run;
    if (run % 2 == 0) return false;
  }
  return true;
}

}  // namespace

// A backslash in page text is a literal backslash, so it cannot cancel the
// escape of a following '<' or '|', even when the two come from different
// text nodes.
TEST_F(MarkdownExtractorFilterTest, LiteralBackslashCannotUnescapeMarkup) {
  const std::string a =
      Md("<main><p>a \\&lt;img src=x onerror=alert(1)&gt; b</p></main>");
  EXPECT_EQ(a, "a \\\\\\<img src=x onerror=alert(1)> b");
  EXPECT_TRUE(EveryCharEscaped(a, '<')) << a;
  const std::string split =
      Md("<main><p>a \\<span></span>&lt;b&gt;x</p></main>");
  EXPECT_EQ(split, "a \\\\\\<b>x");
  EXPECT_TRUE(EveryCharEscaped(split, '<')) << split;
  const std::string h = Md("<main><h2>\\\\&lt;i&gt;</h2></main>");
  EXPECT_TRUE(EveryCharEscaped(h, '<')) << h;
  const std::string link =
      Md("<main><p><a href=\"/x\">\\&lt;b&gt;</a></p></main>");
  EXPECT_TRUE(EveryCharEscaped(link, '<')) << link;
  const std::string alt =
      Md("<main><p><img src=\"/i\" alt=\"\\&lt;b&gt;\"></p></main>");
  EXPECT_EQ(alt, "![\\\\\\<b>](/i)");
  // In a table cell a backslash before '|' must not turn the escaped pipe
  // into a cell delimiter.
  const std::string cell =
      Md("<main><table><tr><td>x \\| y</td></tr></table></main>");
  EXPECT_EQ(cell, "| x \\\\\\| y |\n| --- |");
  EXPECT_TRUE(EveryCharEscaped(cell.substr(2, cell.find(" |\n") - 2), '|'))
      << cell;
  // A plain backslash in prose reads as one backslash.
  EXPECT_EQ(Md("<main><p>C:\\path</p></main>"), "C:\\\\path");
}

// Link, image and reference-definition syntax written as page text stays
// text: its brackets are escaped, so it cannot form a live link or image or
// close a link label early.
TEST_F(MarkdownExtractorFilterTest, BracketsInTextAreEscaped) {
  EXPECT_EQ(Md("<main><p>see [click](javascript:alert(1)) and "
               "![p](https://t.example/p.png)</p></main>"),
            "see \\[click\\](javascript:alert(1)) and "
            "!\\[p\\](https://t.example/p.png)");
  EXPECT_EQ(
      Md("<main><p><a href=\"/ok\">x](javascript:alert(1)) [y</a></p></main>"),
      "[x\\](javascript:alert(1)) \\[y](/ok)");
  EXPECT_EQ(Md("<main><p>[a]: https://e.example/</p></main>"),
            "\\[a\\]: https://e.example/");
  EXPECT_EQ(Md("<main><table><tr><td>[x](/y)</td></tr></table></main>"),
            "| \\[x\\](/y) |\n| --- |");
  // Code keeps its brackets.
  EXPECT_EQ(Md("<main><p><code>a[0]</code></p></main>"), "`a[0]`");
}

// ---- link text and inline code are trimmed ----------------------------------

TEST_F(MarkdownExtractorFilterTest, LinkTextIsTrimmed) {
  EXPECT_EQ(Md("<main><p><a href=\"/download/\"> Download &amp; run </a> "
               "<a href=\"/demo/\">\n  See the demo\n</a></p></main>"),
            "[Download & run](/download/) [See the demo](/demo/)");
  // The whitespace moves outside, so words stay apart.
  EXPECT_EQ(Md("<p>Go<a href=\"/x\"> here </a>now.</p>"), "Go [here](/x) now.");
  EXPECT_EQ(Md("<p><a href=\"/x\">a<br></a></p>"), "[a](/x)");
  EXPECT_EQ(Md("<p><a href=\"/x\"> <img src=\"/i\" alt=\"i\"> </a></p>"),
            "[![i](/i)](/x)");
}

TEST_F(MarkdownExtractorFilterTest, InlineCodeIsTrimmed) {
  EXPECT_EQ(Md("<p>Set <code>\n             rewrite_images,inline_images\n"
               "           </code> here</p>"),
            "Set `rewrite_images,inline_images` here");
  EXPECT_EQ(Md("<p><code>  x  </code></p>"), "`x`");
  EXPECT_EQ(Md("<p>a<code>   </code>b</p>"), "ab");
}

// ---- headings, explicit non-content markers, card links ----------------------

// An ATX heading is one line: a <br> in it is a space.
TEST_F(MarkdownExtractorFilterTest, BrInsideHeadingKeepsOneLine) {
  EXPECT_EQ(Md("<main><h1>Server-side image optimization:\n<br>\n"
               "<span>WebP, AVIF, and critical CSS.</span></h1></main>"),
            "# Server-side image optimization: WebP, AVIF, and critical CSS.");
  EXPECT_EQ(Md("<h2>a<br>b</h2>"), "## a b");
}

// data-pagefind-ignore only keeps text out of a search index; sites set it on
// related-links lists and other real content, so the copy keeps that text.
TEST_F(MarkdownExtractorFilterTest, PagefindIgnoredContentIsKept) {
  EXPECT_EQ(Md("<main><h1>Title</h1><p>Body.</p>"
               "<div data-pagefind-ignore><h2>Related filters</h2>"
               "<ul><li><a href=\"/a\">combine_css</a></li></ul></div></main>"),
            "# Title\n\nBody.\n\n## Related filters\n\n- [combine_css](/a)");
}

// The date of a "Last updated" line in a footer inside <main> (chrome, so
// left out) is still the page's date.
TEST_F(MarkdownExtractorFilterTest, DateFromChromeInsideMainIsKept) {
  EXPECT_EQ(Md("<html><head><title>T</title></head><body><main>"
               "<p>Body.</p><footer>Last updated <time "
               "datetime=\"2026-10-22\">Oct 22</time></footer>"
               "</main></body></html>"),
            "---\ntitle: T\ndate: 2026-10-22\n---\n\nBody.");
  // A date in the content wins over one in chrome that comes first.
  EXPECT_EQ(Md("<html><head><title>T</title></head><body><main>"
               "<nav><time datetime=\"2020-01-01\">old</time></nav>"
               "<article><h1>T</h1><time datetime=\"2026-11-01\">Nov 1"
               "</time><p>Body.</p></article></main></body></html>"),
            "---\ntitle: T\ndate: 2026-11-01\n---\n\n# T\n\nBody.");
  // A page-level footer's date (outside <main>) is not taken.
  EXPECT_EQ(Md("<html><head><title>T</title></head><body><main>"
               "<p>Body.</p></main><footer><time datetime=\"2020-01-01\">"
               "x</time></footer></body></html>"),
            "---\ntitle: T\n---\n\nBody.");
}

// Card links (an <a> wrapping a heading and a description) each make one
// link, with the heading and description joined, and stay apart.
TEST_F(MarkdownExtractorFilterTest, CardLinksKeepSeparators) {
  EXPECT_EQ(Md("<main><div><a href=\"/docs/image-filters/\">\n"
               "<h3>Image filters\n<span>34 filters</span>\n</h3>\n"
               "<p>Recompression and resizing.</p>\n</a>"
               "<a href=\"/docs/css-filters/\">\n<h3>CSS filters\n"
               "<span>15 filters</span>\n</h3>\n<p>Minify.</p>\n</a>"
               "</div></main>"),
            "[Image filters 34 filters: Recompression and resizing.]"
            "(/docs/image-filters/)\n\n"
            "[CSS filters 15 filters: Minify.](/docs/css-filters/)");
  // Plain inline links in a sentence are unchanged.
  EXPECT_EQ(Md("<p><a href=\"/a\">a</a><a href=\"/b\">b</a></p>"),
            "[a](/a)[b](/b)");
}

// ---- output is always well-formed UTF-8 -----------------------------------

// A literal no-break space (U+00A0, the two bytes C2 A0) reads as a space and
// never leaves half of its encoding behind.
TEST_F(MarkdownExtractorFilterTest, LiteralNbspFoldsToOneSpace) {
  EXPECT_EQ(Md("<main><p>of at least 16\xC2\xA0KB with no Range</p></main>"),
            "of at least 16 KB with no Range");
  EXPECT_EQ(Md("<main><p>Debian\xC2\xA0\x31\x32/13</p></main>"),
            "Debian 12/13");
  EXPECT_EQ(Md("<main><p>270,659\xC2\xA0\x42</p></main>"), "270,659 B");
  // A run of no-break spaces and ordinary spaces is one space.
  EXPECT_EQ(Md("<main><p>a \xC2\xA0\xC2\xA0 b</p></main>"), "a b");
  // A Latin-1 letter whose second byte is also 0xA0 is not whitespace.
  EXPECT_EQ(Md("<main><p>\xC3\xA0 la carte</p></main>"), "\xC3\xA0 la carte");
}

TEST_F(MarkdownExtractorFilterTest, LiteralNbspInPreAndCodeStaysValid) {
  const std::string pre = Md("<main><pre>a\xC2\xA0\x62</pre></main>");
  EXPECT_TRUE(MarkdownIsValidUtf8(pre)) << pre;
  EXPECT_EQ(pre, "```\na b\n```");
  const std::string code = Md("<main><p><code>a\xC2\xA0\x62</code></p></main>");
  EXPECT_TRUE(MarkdownIsValidUtf8(code)) << code;
  EXPECT_EQ(code, "`a b`");
  const std::string alt =
      Md("<main><p><img src=\"/i.png\" alt=\"a\xC2\xA0\x62\"></p></main>");
  EXPECT_TRUE(MarkdownIsValidUtf8(alt)) << alt;
  EXPECT_EQ(alt, "![a b](/i.png)");
}

// Image alt text keeps its non-ASCII letters and decodes its references.
TEST_F(MarkdownExtractorFilterTest, ImageAltKeepsUtf8) {
  EXPECT_EQ(Md("<main><p><img src=\"/c.png\" alt=\"caf\xC3\xA9\"></p></main>"),
            "![caf\xC3\xA9](/c.png)");
  const std::string md =
      Md("<main><p><img src=\"/c.png\" alt=\"a&nbsp;b\"></p></main>");
  EXPECT_TRUE(MarkdownIsValidUtf8(md)) << md;
  EXPECT_EQ(md, "![a b](/c.png)");
}

// Input that is not valid UTF-8 (a Latin-1 page, stray bytes) gives valid
// UTF-8 output: each invalid sequence becomes U+FFFD, deterministically.
TEST_F(MarkdownExtractorFilterTest, InvalidInputBytesAreReplaced) {
  EXPECT_EQ(Md("<main><p>caf\xE9 au lait</p></main>"),
            "caf\xEF\xBF\xBD au lait");
  EXPECT_EQ(Md("<main><p>a\x80\x62</p></main>"), "a\xEF\xBF\xBD\x62");
  EXPECT_EQ(Md("<main><pre>x\xFFy</pre></main>"), "```\nx\xEF\xBF\xBDy\n```");
  EXPECT_EQ(Md("<main><p><code>q\xC2</code></p></main>"), "`q\xEF\xBF\xBD`");
  // Bytes at the end of a text node are replaced like any others.
  EXPECT_EQ(Md("<main><p>Caf\xE9</p></main>"), "Caf\xEF\xBF\xBD");
}

namespace {

// Pieces a random page is assembled from: markup, entities, valid multi-byte
// characters (including C2 A0 and letters whose last byte is 0xA0) and, for the
// invalid-input property, lone and truncated bytes.
constexpr std::string_view kValidPieces[] = {
    "<p>",
    "</p>",
    "<pre>",
    "</pre>",
    "<code>",
    "</code>",
    "<h2>",
    "</h2>",
    "<a href=/x>",
    "</a>",
    "<td>",
    "<table><tr>",
    "</table>",
    "<li>",
    "<ul>",
    "</ul>",
    "<br>",
    "<header>",
    "</header>",
    "<img src=/i alt=\"",
    "\">",
    "text",
    " ",
    "\n",
    "&amp;",
    "&nbsp;",
    "&lt;b&gt;",
    "&mdash;",
    "&#x2014;",
    "&#160;",
    "&rarr;",
    "`",
    "\xC2\xA0",
    "\xC3\xA0",
    "\xE2\x80\x94",
    "\xF0\x9F\x98\x80",
    "caf\xC3\xA9",
    "\xE4\xB8\xAD",
    "#",
    "-",
};
constexpr std::string_view kInvalidPieces[] = {
    "\xA0", "\xC2", "\xE2\x80", "\xFF", "\x80", "\xED\xA0\x80", "\xC0\xAF",
};

std::string RandomPage(std::mt19937* rng, bool with_invalid) {
  std::string html = "<html><body><main>";
  std::uniform_int_distribution<int> len(1, 80);
  const int n = len(*rng);
  for (int i = 0; i < n; ++i) {
    if (with_invalid && (*rng)() % 6 == 0) {
      html += kInvalidPieces[(*rng)() % std::size(kInvalidPieces)];
    } else {
      html += kValidPieces[(*rng)() % std::size(kValidPieces)];
    }
  }
  html += "</main></body></html>";
  return html;
}

}  // namespace

// Property: for any valid UTF-8 page the copy is valid UTF-8 and introduces no
// replacement character; for any page at all the copy is valid UTF-8.
TEST_F(MarkdownExtractorFilterTest, OutputIsAlwaysWellFormedUtf8) {
  std::mt19937 rng(20261127);
  for (int i = 0; i < 3000; ++i) {
    const std::string html = RandomPage(&rng, /*with_invalid=*/false);
    const std::string md = Md(html);
    ASSERT_TRUE(MarkdownIsValidUtf8(md)) << "input: " << html;
    ASSERT_EQ(md.find("\xEF\xBF\xBD"), std::string::npos) << "input: " << html;
  }
  for (int i = 0; i < 3000; ++i) {
    const std::string html = RandomPage(&rng, /*with_invalid=*/true);
    const std::string md = Md(html);
    ASSERT_TRUE(MarkdownIsValidUtf8(md)) << "input: " << html;
    ASSERT_EQ(md, Md(html)) << "not deterministic for: " << html;
  }
}

}  // namespace
}  // namespace net_instaweb
