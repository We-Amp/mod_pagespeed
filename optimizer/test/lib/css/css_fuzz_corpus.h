// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// CSS fuzz corpus extracted for the replay/idempotence oracle.
// Source: mod_pagespeed 1.15,
// third_party/css_parser/src/webutil/css/parser_fuzz.cc, the kCorpus array,
// at origin/master commit 913e41a77 (fetched 2026-07-27). The entries are
// copied verbatim — do not edit, reformat, or "fix" individual entries:
// their byte content (truncation, malformed UTF-8, escapes at EOF) is the
// test signal. The source file's original Apache-2.0 header is retained in
// full below; see LICENSE.apache-2.0 in this directory for the license text.
//
// Re-sync (manual, per #1112): when 1.15's parser_fuzz.cc kCorpus changes,
// re-extract the array verbatim into kCssFuzzCorpus below and update the
// pinned SHA/date above. There is no automation — check
// `git log -- third_party/css_parser/src/webutil/css/parser_fuzz.cc` on the
// 1.15 repo opportunistically when CSS parser work lands there (e.g. mpp
// #664); the replay/idempotence oracle in css_minify_corpus_test.cc and the
// #1132 fuzz target exercise whatever is here, so a stale corpus fails safe
// (less coverage, never a false pass).
//
// Copyright 2026 We-Amp B.V.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PAGESPEED_TEST_LIB_CSS_CSS_FUZZ_CORPUS_H_
#define PAGESPEED_TEST_LIB_CSS_CSS_FUZZ_CORPUS_H_

namespace pagespeed::css {

// A small, deterministic corpus of inputs aimed at the cursor-advance and
// look-ahead hazards the DCHECKs are supposed to catch: truncation right after
// a token-introducing byte, unterminated constructs, escapes at EOF, deep
// nesting, and malformed UTF-8. Kept literal so the test is hermetic.
inline const char* const kCssFuzzCorpus[] = {
    // Empty / minimal.
    "",
    " ",
    "{",
    "}",
    ":",
    ";",
    "/",
    "\\",
    "@",
    "#",
    ".",

    // Unterminated constructs (cursor runs to EOF mid-token).
    "/* comment never closed",
    "a { content: \"unterminated string",
    "a { content: 'unterminated string",
    "a { background: url(unterminated",
    "a { background: url(\"unterminated",
    "@media screen and (",
    "@media",
    "@import",
    "@font-face",
    "@keyframes",
    "@keyframes x {",
    "a[",
    "a[attr",
    "a[attr=",
    "a[attr=\"",

    // Escapes at / past the edge.
    "a { content: \"\\",
    "a { content: \\",
    "\\41",
    "\\",
    "a\\",
    "a { width: \\000041",
    "a { content: \"\\26",
    "url(\\",

    // Numbers / units truncated.
    "a { width: 1",
    "a { width: 1.",
    "a { width: .e",
    "a { width: 1e",
    "a { width: +",
    "a { width: -",
    "a { width: 0x",
    "a { color: #",
    "a { color: #f",
    "a { color: #ggg }",
    // Regression: out-of-range double->int in rgb() color parsing tripped UBSan
    // float-cast-overflow at value.cc GetIntegerValue (workflow NUMCOLOR-1).
    "a{color:rgb(99999999999,0,0)}",
    "a{color:rgb(-99999999999,0,0)}",
    "a { color: rgb(1e308, 0, 0) }",

    // Functions / parens nesting (depth + unbalanced).
    "a { width: calc(",
    "a { width: calc(calc(calc(",
    "a { background: rgb(",
    "a { transform: translate(translate(translate(translate(",

    // Malformed UTF-8 (lone continuation, truncated multibyte, overlong-ish).
    "a { content: \"\x80\" }",
    "a { content: \"\xC0\" }",
    "a { content: \"\xE0\x80\" }",
    "a { content: \"\xF0\x80\x80\" }",
    "\xFF\xFE",
    "@media \xC2",

    // Selectors / combinators truncated.
    "a >",
    "a >>",
    "a ~",
    "a +",
    "a::",
    "a:not(",
    "* |",
    "|",

    // Plausible-but-broken rules.
    "a { ; ; ; }",
    "a {{{{{{{{",
    "}}}}}}}}",
    "a { color: red !",
    "a { color: red !important",
    "@charset",
    "@charset \"",
    "@namespace",

    // Conditional group rules (@supports/@layer/@container): truncated
    // preludes, statement forms, unterminated bodies, strings that hide
    // block/paren delimiters, and MQ4 raw media expressions.
    "@supports",
    "@supports (",
    "@supports (a:b)",
    "@supports (a:b){",
    "@layer",
    "@layer a,",
    "@layer a;@layer b{",
    "@container (width >",
    "@media (width >= ",
    "@media (400px<=width<=700px){",
    "@supports (\"{\"):{",
    "@supports \"unterminated string {",
    "@supports (a:b){@import url(x);@charset \"utf-8\";}",
    "@layer a.b{@media screen{@supports (c:d){e{f:url(g)}}}}",

    // Stray ';' at statement position inside an @media body, crossed with
    // truncation: the recovery skip must not run off the end of the buffer.
    "@media screen{.a{b:c};",
    "@media screen{;",
    "@media screen{;}",
    "@media screen{.a{b:c}; ) }",
};

// 2.0-local additions — NOT part of the 1.15 extraction above, kept in a
// separate array so a verbatim re-sync of kCssFuzzCorpus never touches
// them.  Minimized reproducers for bugs found by the #1132 fuzz target
// (or its reviews) and fixed since; they pin the replay/idempotence
// oracle to the fixed behavior.
inline const char* const kCssFuzzCorpusLocal[] = {
    // #1158/#1160: ';', '{' and '}' are legal unquoted-url code points —
    // Phase 3's ';'-trim and Phase 5's scanners must not fire inside.
    "a{background:url(x;}y)}",
    "url(; }",
    // Parenthesized: adjacent literals in an array initializer trip
    // -Wstring-concatenation (macOS CI) otherwise.
    ("a{padding-top:url(x;padding-right:1px);padding-bottom:1px;padding-left:"
     "1px}"),
    // Audit PR-B: custom-property opacity (#1156 combinator stop-set,
    // P3-d trim guard, P4-a escape guard).
    "--+ y: }",
    "a{--x:{;}}",
    ".a\\0.5{c:d}",
    // PR-D tail fixes: #1164 (escaped ';' is value content, never a
    // terminator — incl. the glue variant swallowing a custom
    // property, and the phantom-string face where an escaped quote
    // skewed block boundaries), #1159 (calc tightening must not glue
    // '/'+'*' into a comment token), #1162 (empty or
    // separator-unstable longhand values refuse collapse).
    "a{b:c\\;}",
    "a{m:\\;;--z:url(x)}",
    "a{b:c\\'d'e{x;}}",
    "a{b:calc(1 / *2)}",
    "{overflow-y:;overflow-x::}",
    "a{overflow-x:v:;overflow-y:w}",
    // #1163: "0.5" inside an identifier is ident content, not a number —
    // Phase 4's decimal strip fires only at a number-token start
    // (incl. the dashed-ident two-char lookback: "a-0.5" is an ident,
    // bare "-0.5" is a signed number).
    ".a0.5{c:d}",
    "a-0.5{x:y}",
    // #1175: the decimal rule decodes escape structure — a hex escape's
    // consumed whitespace terminator is not a token separator (".a\35 0.5"
    // is class "a50"), and escape content before a '-' continues the
    // ident (".a\26 -0.5"), so the '0' stays in both.
    ".a\\35 0.5{c:d}",
    "a\\5c 0.5{x:y}",
    ".a\\000035 0.5{c:d}",
    ".a\\353535 0.5{c:d}",
    ".a\\\\-0.5{c:d}",
    ".a\\ -0.5{c:d}",
    ".a\\'-0.5{c:d}",
    ".a\\26 -0.5{c:d}",
    // PR-E: #1167 — brace groups inside declaration values are opaque
    // (leading/trailing ';' and false shorthand collapse inside them).
    "a{--z:{;x}}",
    "a{b:{L;}}",
    ("a{--z:{padding-top:1px;padding-right:1px;padding-bottom:1px;"
     "padding-left:1px}}"),
    // #1170: shorthand collapse refuses longhand values with unbalanced
    // parens/brackets (the rejoin reorders values; a relocated bracket
    // moves the next pass's block boundaries / custom-value extent —
    // pass-instability on invalid input).  Nightly-artifact
    // minimizations plus the issue's repro and the sweep's bracket face.
    "{padding-top:);padding-left:(;padding-right:p{Y;padding-bottom:)};}",
    "{padding-top:;padding-left:(;padding-right:@;padding-bottom::{)};}",
    ("a {overflow-y:-!(U -&y4;overflow-x:):w)}4;ov;rfov;;x-rlow-x:):] }"),
    ("--?:{padding-left:q;padding-top:x;padding-right:];"
     "padding-bottom:[};}"),
    ("--$:{padding-right:d;padding-bottom:x;padding-left:a\tb;"
     "padding-top:(}}})}"),
    ("{({;padding-left:w{(];padding-top:v;padding-right:x;"
     "padding-bottom:[}{)}}"),
    // #1170's operator-space face: an unterminated "calc(" leaves Phase
    // 2's math mode on, making a separator space next to a '*'/'
    // value edge trimmable next pass (sweep-residual minimizations).
    ("calc({padding-top:x;padding-left:p;padding-right:x;"
     "padding-bottom:*}"),
    ("calc({padding-top:x;padding-left:*y;padding-right:z;"
     "padding-bottom:w}"),
};

}  // namespace pagespeed::css

#endif  // PAGESPEED_TEST_LIB_CSS_CSS_FUZZ_CORPUS_H_
