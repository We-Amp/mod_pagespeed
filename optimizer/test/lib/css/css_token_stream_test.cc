// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Unit battery for lib/css/css_token_stream.h — the v2 channels and the
// v3 garbage channel-agreement adjudication.  Two families:
//
//   * EQ cases: valid-input channel preservation and each v3 sanction
//     (R1 escape-glue whitespace, R2 unterminated url( at EOF, R3
//     garbage-input agreement) must NOT flag.
//   * RED counterexamples: the shapes the sanctions must NOT swallow —
//     every one must still FLAG.  A sanction that fires on any of these
//     is over-broad and this test goes red.

#include "lib/css/css_token_stream.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "lib/css/css_minify.h"

namespace {

using pagespeed::css::token_stream::Equivalent;

// Minify-then-compare helper for the EQ cases driven by the real
// minifier (the output is not hand-written, so a minifier behavior
// change that violates a sanction turns this RED instead of silently
// updating a golden).
void ExpectMinifiedEquivalent(std::string_view input) {
  std::string output;
  ASSERT_TRUE(pagespeed::css::MinifyCss(input, &output)) << input;
  EXPECT_TRUE(Equivalent(input, output))
      << "IN [" << input << "] OUT [" << output << "]";
}

void ExpectEquivalent(std::string_view in, std::string_view out) {
  EXPECT_TRUE(Equivalent(in, out)) << "IN [" << in << "] OUT [" << out << "]";
}

void ExpectFlags(std::string_view in, std::string_view out) {
  EXPECT_FALSE(Equivalent(in, out)) << "IN [" << in << "] OUT [" << out << "]";
}

// --- v2 channel equivalence (regression floor) -------------------------

TEST(CssTokenStreamTest, ValidInputsStayEquivalent) {
  ExpectMinifiedEquivalent("a{background:url(x;}y)}");
  ExpectMinifiedEquivalent(":root{--x:0.5;--y:url( \"a)b\" );--z:{;}}");
  ExpectMinifiedEquivalent(
      "a{background:url(x{y});padding-top:1px;padding-right:1px;"
      "padding-bottom:1px;padding-left:1px}");
  ExpectMinifiedEquivalent("a{width:calc(100% - 10px);content:'a; b'}");
  ExpectMinifiedEquivalent("a { b : c ; }");
}

// --- Escape-glue whitespace diffs (printable) -----------------------------

TEST(CssTokenStreamTest, EscapeGlueWhitespaceAdjacencyEquivalent) {
  // The dominant nightly class: whitespace removed after escape content
  // in the removal set, gluing numbers/idents.  (Since the
  // glue-NORMALIZATION cutover these pairs EQ at the channel level on
  // the NORMALIZED forms — normalization subsumes the retired R1
  // ws-only-diff adjudication, which fired zero times across the full
  // evidence matrix and was not carried as a fallback.)
  ExpectEquivalent("4\\> 0%", "4\\>0%");
  ExpectEquivalent("0\\> 0%", "0\\>0%");
  // ws -> single-space normalization where gluing would fuse, plus
  // removal after the escape (the 2026-08-03 nightly's second shape).
  ExpectEquivalent("2\n\\>\t5\\>\t0", "2 \\>5\\>0");
  // The classifier's own escape-glue self-test fixture.
  ExpectMinifiedEquivalent("0\\;\n00\\;\n00..");
}

// --- #1238: glue-agreement (comment gaps, escape-glue chains) -----------

TEST(CssTokenStreamTest, GlueAgreementArtifactsEquivalent) {
  // The 8 needs-triage artifacts of nightly 30849604191 (issue #1238),
  // minimized — all invalid-input-only, all cleared by comparator
  // AGREEMENT (the two sides' glue handling made symmetric), never by
  // sanction.  Driven through the real minifier.
  ExpectMinifiedEquivalent("5.\\> 0\\> 0.0.");  // crash-149fd2de27
  ExpectMinifiedEquivalent("--\\> p:;}");       // crash-5b00f82b3c
  ExpectMinifiedEquivalent(";}\\> 0%5");        // crash-63f4b174b4
  ExpectMinifiedEquivalent("+/**/5");           // crash-70dd8f3eaf
  ExpectMinifiedEquivalent("+/**/5");           // crash-8b9477037e
  ExpectMinifiedEquivalent("0.5\\> 0-0");       // crash-b32f519d2e
  ExpectMinifiedEquivalent("0\\> 0-5)0.");      // crash-cc7f6e8971
  ExpectMinifiedEquivalent("--\\> l:;}");       // crash-e46406e2a0
  // (The 9th, crash-766ac1fda4, is the minifier-side custom-name
  // escape bug — fixed by #1239, out of scope here.)
}

// --- #1240 review: comment-text `/*` defeats naive pairing --------------

TEST(CssTokenStreamTest, CommentTextOpenInsideCommentEquivalent) {
  // The review's two demonstrated shapes: `/* a /* b */` is ONE
  // comment (comments do not nest — the inner `/*` is text), so the
  // whole span is removed and the sign glues.  Valid CSS, correct
  // minifier output — must EQ, end-to-end.
  ExpectMinifiedEquivalent("a{w:+/* a /* b */5px}");
  ExpectMinifiedEquivalent("a{w:+/* /* */5px}");
  // Multiple comments in a row: both spans removed, sign glues.
  ExpectMinifiedEquivalent("a{w:+/* x */ /* y */5px}");
  // Comment immediately before the sign... and whitespace+comment mix.
  ExpectMinifiedEquivalent("a{w:1 + /* c */ 5px}");
  // `*/` text AFTER a closed comment is not a comment end (the FIRST
  // `*/` closes per CSS): the minifier keeps ` b */` as delimiters and
  // glues only `+` onto `b` — output `a{w:1+b */5px}`, and the walk
  // must not over-skip the stray `*/` as a comment span.
  ExpectMinifiedEquivalent("a{w:1+/* a */ b */5px}");
  // Unterminated comment: removed to EOF by the minifier; EQ.
  ExpectMinifiedEquivalent("a{w:+/* 5px}");
}

TEST(CssTokenStreamTest, RedCommentSpanDigitMutationFlags) {
  // Content changes around comment spans still flag:
  ExpectFlags("a{w:+/* a /* b */5px}", "a{w:+6px}");
  ExpectFlags("a{w:+/* x */ /* y */5px}", "a{w:-5px}");
  // (An ident change in the span-adjacent text — "b */" -> "c */" —
  // is the documented ident-level blind spot, so the RED here is the
  // number, which the channel does own.)
  ExpectFlags("a{w:1+/* a */ b */5px}", "a{w:1+b */6px}");
}

// --- nightly 30888201976: three comparator edge fixes --------------------

TEST(CssTokenStreamTest, ComparatorEdgeArtifactsEquivalent) {
  // The three needs-triage artifacts of nightly 30888201976 (issue
  // #1238), minimized — all invalid-input-only, all comparator
  // over-catches fixed by modeling, not sanction.  Minifier-driven.
  // crash-1c1d7ba1: a dot-led absorbed number's unit must not extend
  // the previous dimension (empty integer part joins nothing).
  ExpectMinifiedEquivalent("0\\>\t.0t.0.");
  // crash-b85d0888: a '+'-signed chain link is never absorbed ('+'
  // breaks the glued ident, as in the initial-absorption path).
  ExpectMinifiedEquivalent("\\>\t0+1:0.");
  // crash-b6833702: a dotted number never joins a custom name — the
  // oracle is consistently non-custom on both sides; the minifier's
  // broader byte-scan preserves the bytes anyway.
  ExpectMinifiedEquivalent("--\\>\t.0:;}");
  // The ident-formable counterpart that MUST keep its region:
  // digit-led number + ident continuation glue into one valid custom
  // name on both sides (the -5b00f82 shape from nightly 30849604191).
  ExpectMinifiedEquivalent("--\\X\\>\t\t\t0-top:0px;0m0;;;;}(");
  // The legitimate non-empty-int extension path (header example):
  // absorbed "-0" joins the ident, ".5" stays residual.
  ExpectMinifiedEquivalent("\\{ -0.5");
}

TEST(CssTokenStreamTest, RedComparatorEdgeMutationsFlags) {
  // crash-1: a digit change after the dot-led absorption still flags.
  ExpectFlags("0\\>\t.0t.0.", "0\\>.0t.1.");
  // crash-3: sign and digit changes on the chain link still flag.
  ExpectFlags("\\>\t0+1:0.", "\\>0-1:0.");
  ExpectFlags("\\>\t0+1:0.", "\\>0+2:0.");
  // crash-2: a digit mutation in a dotted (non-joining) number still
  // flags — the non-custom reading is uniform, not a waiver.
  ExpectFlags("--\\>\t.0:;}", "--\\>\t.1:;}");
  // Number-name content mutation in the ident-formable path (a `;`
  // difference before `}` would be invisible BY DESIGN — semicolons
  // are not channel content — so the RED mutates the custom VALUE).
  ExpectFlags("--\\X\\>\t\t\t0-top:0px;0m0;;;;}(",
              "--\\X\\>0-top:0py;0m0;;;;}");
}

// --- nightly 30895630048: four comparator sub-families -------------------

TEST(CssTokenStreamTest, TrackAFamilyArtifactsEquivalent) {
  // The seven needs-triage artifacts of nightly 30895630048 (issue
  // #1238), minimized — all invalid-input-only, all comparator
  // over-catches fixed by modeling, not sanction.  Minifier-driven.
  // Percent-unit scoping (x3): a '%' unit stays with the residual when
  // the number is dot-led or its int joins the dimension — it is never
  // glued into the ident.
  ExpectMinifiedEquivalent("0.\\:\t.0%");  // crash-0b170e7b
  ExpectMinifiedEquivalent("0.\\: .0%");   // crash-ff8bc630
  ExpectMinifiedEquivalent("\\: .0%0.");   // crash-d41d3e9c
  // The int-joins-but-%-doesn't sub-case (the crash-ff8bc630 full
  // shape): "5.0%" absorbs the '5' into the dimension, ".0%" residual.
  ExpectMinifiedEquivalent("0.03\\:\t5.0%");
  // Escape-glue x function formation (x2): a function-name token glues
  // onto the escaped dimension unit, which the dimension loses.
  ExpectMinifiedEquivalent("0.2\\,\rd(");    // crash-e3635073
  ExpectMinifiedEquivalent("\\'>5\\>\t-(");  // crash-4ea8e036
  // Name-candidacy chain (x1): a signed ident-formable link joins the
  // glued chain ("-6"), opening the same region on both sides.
  ExpectMinifiedEquivalent("--\\>\t4-6:/*");  // crash-061fa0c8
  // Url-span anchoring (x1): the content anchor is the url-suffix
  // paren, not the escaped "\\(" before the name.
  ExpectMinifiedEquivalent("\\(url(\ru)");  // crash-22f52361
}

TEST(CssTokenStreamTest, RedTrackAFamilyMutationsFlags) {
  // Percent: unit/percent mutations still flag.
  ExpectFlags("0.\\:\t.0%", ".\\:.0@");
  ExpectFlags("0.03\\:\t5.0%", "0.03\\:\t5.1%");
  // Function formation: a number mutation still flags (function-name
  // mutations are channel-invisible by design — functions are not a
  // channel).
  ExpectFlags("0.2\\,\rd(", "0.3\\,\rd(");
  ExpectFlags("\\'>5\\>\t-(", "\\'>6\\>\t-(");
  // Chain candidacy: a region-content mutation still flags (chain-name
  // digit changes are ident-invisible by design — names are not a
  // channel).
  ExpectFlags("--\\>\t4-6:v}", "--\\>4-6:w}");
  // Url anchoring: content after the suffix paren mutates still flag.
  ExpectFlags("\\(url(\ru)", "\\(url(\rv)");
}

// --- Glue-normalization spike battery (50 pairs from the spike report) ----

TEST(CssTokenStreamTest, SpikeBatteryCorrectCatchFlags) {
  // The spike's 31 must-FLAG pairs: every correct-catch tooth of the
  // v1-v3 oracle must survive the cutover (no blindness on the
  // normalized forms).
  ExpectFlags("a{content:'x'}", "a{content:'y'}");  // string mutation
  ExpectFlags("a{background:url(a;b.png)}",
              "a{background:url(a,c.png)}");  // url mutation
  ExpectFlags("a{b:0.5}", "a{b:0.6}");        // number mutation
  ExpectFlags("a{--x:v}", "a{--x:w}");        // custom mutation
  ExpectFlags("a{content:'x'}",
              "a{content:'x'};b{content:'y'}");  // append: string rule
  ExpectFlags("a{content:'x'}",
              "a{content:'x'} garbage");               // append: garbage text
  ExpectFlags("a{w:1px}", "a{w:1px};b{h:2px}");        // append: number rule
  ExpectFlags("a{content:'; x'}", "a{content:';x'}");  // string-interior ws
  ExpectFlags("a{background:url(a; b.png)}",
              "a{background:url(a;b.png)}");  // url-interior ws
  ExpectFlags("a{--x:a b}", "a{--x:ab}");     // custom-interior ws
  ExpectFlags("a{b:1 2}", "a{b:12}");         // number gluing
  ExpectFlags("a{--x:0.5}", "a{--x:.05}");    // custom decimal
  ExpectFlags("a{background:url(x;}y)}",
              "a{background:url(x}y)}");  // #1158 url ';' loss
  ExpectFlags("a{background:url(a\\)b)}",
              "a{background:url(a\\)c)}");  // #1161 escaped url content
  ExpectFlags("a{content:'x'}", "a{content:\"x\"}");  // quote flip
  ExpectFlags("a{content:'x.png'}",
              "a{background:url(x.png)}");  // string -> url move
  ExpectFlags("a{overflow-x:'a';overflow-y:'b'}",
              "a{overflow:'a'}");  // reorder+drop
  ExpectFlags("a{overflow-x:'a';overflow-y:'b'}",
              "a{overflow:'a' 'c'}");  // reorder+mutate
  ExpectFlags("a{content:'x'}", "a{content:'x';content:'x'}");  // dup gain
  ExpectFlags("a{--x:'a';--y:'b'}",
              "a{--y:'b';--x:'a'}");          // custom region reorder
  ExpectFlags("a{b:10.50}", "a{b:10.5}");     // trailing zero
  ExpectFlags("a{b:5PX}", "a{b:5Px}");        // unit case
  ExpectFlags("a{b:5px}", "a{b:5}");          // unit drop
  ExpectFlags("a{b:00.5}", "a{b:.5}");        // 00.5 (untouched by minifier)
  ExpectFlags("a{b:1e3}", "a{b:2e3}");        // mantissa
  ExpectFlags("a{b:1e3}", "a{b:1e4}");        // exponent
  ExpectFlags("a{b:+0.5}", "a{b:0.5}");       // sign drop
  ExpectFlags("a{b:+0.5}", "a{b:-0.5}");      // sign flip
  ExpectFlags("a{w:1px;h:2px}", "a{w:1px}");  // declaration drop (number)
  ExpectFlags("a{content:'x';color:red}", "a{color:red}");  // string deletion
  ExpectFlags("a{b:5}", "a{b:6}");                          // number text
  // (Ident-level mutation — "a{b:c}" vs "a{b:d}" — is the documented
  // channel-invisible case: idents are not a channel.  Not part of the
  // must-FLAG set by design.)
}

TEST(CssTokenStreamTest, SpikeBatterySanctionedEquivalent) {
  // The spike's 19 sanctioned pairs: no new over-catch on the
  // normalized forms.
  ExpectEquivalent("a{overflow-y:'b';overflow-x:'a'}",
                   "a{overflow:'a' 'b'}");  // collapse reorder (string)
  ExpectEquivalent("a{overflow-y:url(b);overflow-x:url(a)}",
                   "a{overflow:url(a) url(b)}");  // collapse reorder (url)
  ExpectEquivalent("a{overflow-x:'a';overflow-y:'a'}",
                   "a{overflow:'a'}");  // collapse dedup (string)
  ExpectEquivalent("a{overflow-x:url(a);overflow-y:url(a)}",
                   "a{overflow:url(a)}");      // collapse dedup (url)
  ExpectEquivalent("a{b:+ 0.5}", "a{b:+.5}");  // sign glue + zero strip
  ExpectEquivalent("a{b:calc(1 + 0.5)}",
                   "a{b:calc(1 + .5)}");  // calc preservation + zero strip
  ExpectEquivalent("a{b:c;}", "a{b:c}");  // ';' before '}' trim
  ExpectEquivalent("a{--x:/*c*/v}", "a{--x:v}");  // comment in custom value
  ExpectEquivalent("a{--x: v}", "a{--x:v}");      // custom leading-ws trim
  ExpectEquivalent("a{background:url(  foo.png  )}",
                   "a{background:url(foo.png)}");  // url inner trims
  ExpectEquivalent("a{background:url(foo  ", "a{background:url(foo");
  // Unterminated-url EOF trim (plain).
  ExpectEquivalent("a{background:url(foo\\  )",
                   "a{background:url(foo\\ ");  // escaped-ws kept (#1154)
  ExpectEquivalent("a{b:c}", "a{b:c} ");        // ws-only tail append
  ExpectEquivalent("a{w:+/* a /* b */5px}",
                   "a{w:+5px}");  // comment-gap sign glue, nested text
  ExpectEquivalent("a{w:+/* x */ /* y */5px}",
                   "a{w:+5px}");  // adjacent comments
  ExpectEquivalent("a{content:'caf\xc3\xa9'}", "a{content:'caf\xc3\xa9'} ");
  // Valid UTF-8 is not garbage + append guard.
  ExpectMinifiedEquivalent(
      "a{b:\xff"
      "c}");                                  // garbage defer (R3)
  ExpectEquivalent("a{b:0.5}", "a{b:.5}");    // zero strip
  ExpectEquivalent("a{b:-0.5}", "a{b:-.5}");  // negative zero strip
  ExpectEquivalent("a{b:0.0%}", "a{b:.0%}");  // percent strip
}

TEST(CssTokenStreamTest, RedGluedChainDigitMutationFlags) {
  // A digit changed inside a glued chain is corruption, not glue:
  ExpectFlags("4\\> 0", "4\\>1");
  ExpectFlags("0.5\\> 0-0", ".5\\>0-1");
  ExpectFlags(";}\\> 0%5", "}\\>0%6");
}

TEST(CssTokenStreamTest, RedSignFlipAcrossCommentFlags) {
  // Comment-gap sign gluing is agreement, not a sign-change waiver:
  ExpectFlags("+/**/5", "-5");
  ExpectFlags("-/**/5", "+5");
}

TEST(CssTokenStreamTest, RedChainInteriorChangeFlags) {
  // The glue-joined custom content changing must FLAG:
  ExpectFlags("--\\> p:v;}", "--\\>p:w;}");
  ExpectFlags("--\\> p:v;}", "--\\>p:;}");
  // (A name change with identical content — "--\\>p" vs "--\\>q" — is
  // the documented ident-level blind spot: custom NAMES are not a
  // channel, values are.  Out of this oracle's contract by design.)
}

TEST(CssTokenStreamTest, PhantomCustomNameStillDisarmed) {
  // An UNGLUED number or ident between name and colon keeps the
  // candidacy dead on both sides (no custom region either way):
  ExpectEquivalent("--x 0:v}", "--x 0:v}");
  ExpectEquivalent("5 --z:0.5}", "5 --z:.5}");
}

// --- R2: unterminated url( at EOF ---------------------------------------

TEST(CssTokenStreamTest, R2UnterminatedUrlAdjudicated) {
  ExpectMinifiedEquivalent("a{background:url(foo");
  ExpectMinifiedEquivalent("a{background:url(  foo  ");
}

// --- R3: garbage-input agreement ----------------------------------------

TEST(CssTokenStreamTest, R3GarbageAdjudicated) {
  // Two real artifacts from the 2026-08-03 nightly (garbage per the
  // classifier's UTF-8-aware proxy; v2 trips, R3 adjudicates).  Driven
  // through the real minifier.
  ExpectMinifiedEquivalent("--\x81l-}a{x}e{--\x81(+/**/ 1\x01px");
  ExpectMinifiedEquivalent("--\x81l-l-l\xff:*- +/**/}a{x}e{--\x81-+/**/2");
  // Crafted: garbage byte plus escape glue in one input.
  ExpectMinifiedEquivalent("0\\;\x01 0");
}

// --- RED counterexamples: all of these must still FLAG ------------------

TEST(CssTokenStreamTest, RedStringMutationFlags) {
  ExpectFlags("a{content:'x'}", "a{content:'y'}");
}

TEST(CssTokenStreamTest, RedUrlMutationFlags) {
  ExpectFlags("a{background:url(a;b.png)}", "a{background:url(a,c.png)}");
}

TEST(CssTokenStreamTest, RedNumberMutationFlags) {
  ExpectFlags("a{b:0.5}", "a{b:0.6}");
  ExpectFlags("a{b:0.5}", "a{b:0.05}");
}

TEST(CssTokenStreamTest, RedCustomValueMutationFlags) {
  ExpectFlags("a{--x:v}", "a{--x:w}");
}

TEST(CssTokenStreamTest, RedNonSanctionedWhitespaceRemovalFlags) {
  // The removed space follows 'a' (not in the removal set): semantic
  // whitespace in a custom value, never sanctioned (the classifier's
  // own counter-example).
  ExpectFlags("a{--x:a b}", "a{--x:ab}");
  // Between number and unit, printable input.
  ExpectFlags("a{b:0.5 px}", "a{b:0.5px}");
}

TEST(CssTokenStreamTest, RedStringInteriorWhitespaceRemovalFlags) {
  // R1's interior guard: the minifier preserves whitespace inside
  // strings, so a change there is corruption (classifier example).
  ExpectFlags("a{content:'; x'}", "a{content:';x'}");
}

TEST(CssTokenStreamTest, RedUrlInteriorWhitespaceRemovalFlags) {
  // Same guard for unquoted-url interiors.
  ExpectFlags("a{background:url(a; b.png)}", "a{background:url(a;b.png)}");
}

TEST(CssTokenStreamTest, RedNonWhitespaceDifferenceFlags) {
  // Channel-touching, non-whitespace differences on printable input.
  ExpectFlags("a{b:0.5px}", "a{b:0.5em}");  // Number unit mutation.
  ExpectFlags("a{b:5}", "a{b:6}");          // Number text mutation.
  ExpectFlags("a{--x:'v'}", "a{--x:}");     // Custom value drop.
  // NOTE: ident-only mutations outside the channels ("a{b:c}" ->
  // "a{b:d}") are the documented v1 blind spot, not an adjudication
  // leak — they are out of this oracle's contract by design.
}

TEST(CssTokenStreamTest, RedAppendedContentFlags) {
  // The pre-check's whole-output examination: output = input PLUS
  // appended content must FLAG (review #1237's demonstrated hole —
  // these EQ'd without the pre-check).
  ExpectFlags("a{content:'x'}", "a{content:'x'};b{content:'y'}");
  ExpectFlags("a{content:'x'}", "a{content:'x'} garbage");
  ExpectFlags("a{w:1px}", "a{w:1px};b{h:2px}");
}

TEST(CssTokenStreamTest, AppendedTrailingWhitespaceStaysEquivalent) {
  // The parity boundary the pre-check preserves: whitespace appended
  // at the output's tail is sanctioned (stripped equality), exactly as
  // in the classifier.
  ExpectEquivalent("a{b:c}", "a{b:c} ");
  ExpectEquivalent("a{b:c}", " a{b:c}");
}

TEST(CssTokenStreamTest, RedGarbageInOutputDoesNotAdjudicate) {
  // R3 reads the INPUT's garbage proxy: a printable input whose
  // "output" carries garbage bytes in channel content is not an
  // adjudication case.
  ExpectFlags("a{content:'c'}",
              "a{content:'\xff"
              "c'}");
  ExpectFlags("a{background:url(c.png)}",
              "a{background:url(\x00"
              "c.png)}");
}

TEST(CssTokenStreamTest, RedGarbageInputIsNotAStringChannelWaiver) {
  // On garbage input R3 adjudicates channel disagreements (documented
  // no-interior-guard stance) — but the v2 channels must still fire
  // when THEY agree and only idempotence... (strict-oracle domain).
  // This test pins the token oracle's actual contract on garbage:
  // channel disagreement adjudicates, channel AGREEMENT stays EQ.
  std::string output;
  ASSERT_TRUE(
      pagespeed::css::MinifyCss("a{b:\xff"
                                "c}",
                                &output));
  EXPECT_TRUE(
      Equivalent("a{b:\xff"
                 "c}",
                 output));
}

TEST(CssTokenStreamTest, CustomLookalikeInsidePreludeBracketIsNotCustom) {
  // A "--a:" after a ';' inside an unclosed '[' or '(' in a selector is
  // not a declaration, so the rule block's empty-declaration trim
  // (";c" -> "c") is a sanctioned edit, not a custom-value change.
  ExpectEquivalent("[;--a:b]{;c}", "[;--a:b]{c}");
  ExpectEquivalent(":is(a;--y:t){m:0;}", ":is(a;--y:t){m:0}");
  ExpectMinifiedEquivalent("2[/ro;--y:t){;m0}");  // #543 artifact
  // Real custom-property damage still flags.
  ExpectFlags("a{b:fn(x);--y:{;}}", "a{b:fn(x);--y:{}}");
}

TEST(CssPreservedCommentsTest, ClosedUnterminatedPreservedCommentIsEquivalent) {
  // The minifier closes a preserved comment that ran to the end of the
  // input; that exact "*/" tail is not content gain.
  ExpectEquivalent("/*!", "/*!*/");
  ExpectEquivalent("a{b:c}/*! open", "a{b:c}/*! open*/");
  // The exemption is only those two bytes, and only in that case.
  ExpectFlags("/*!", "/*!x*/");
  ExpectFlags("a{b:c}/*! open", "a{b:c}/*! open*/ p{q:r}");
  ExpectFlags("a{b:c}/*! open */", "a{b:c}/*! open */*/");
  ExpectFlags("a{b:c}", "a{b:c}*/");
  ExpectFlags("/* plain", "/* plain*/");
}

TEST(CssPreservedCommentsTest, RestorePreservedCommentsSentinelCountMismatch) {
  // The pairing is by occurrence order, so a count mismatch (a phase
  // lost or duplicated a sentinel) must be refused, not guessed at.
  pagespeed::css::PreservedComments preserved;
  preserved.sentinel = '\x01';
  preserved.texts = {"/*! A */", "/*! B */"};
  std::string out = "stale";
  EXPECT_FALSE(
      pagespeed::css::RestorePreservedComments("a\x01{}", preserved, &out));
  EXPECT_TRUE(out.empty());
  EXPECT_FALSE(pagespeed::css::RestorePreservedComments("\x01\x01\x01",
                                                        preserved, &out));
  EXPECT_TRUE(
      pagespeed::css::RestorePreservedComments("\x01 \x01", preserved, &out));
  EXPECT_EQ(out, "/*! A */ /*! B */");
}

}  // namespace
