# CSS Library

CSS minification and `@import` flattening. Namespace: `pagespeed::css`.

## Key Files
- `css_minify.h` -- `MinifyCss(input, output)`: whitespace/comment removal
- `css_minify.cc` -- multi-phase minification implementation
- `css_token_stream.h` -- css-syntax-3-LITE tokenizer + token-stream
  equivalence oracle (fuzz-harness tooling, NOT library code; see
  `css_minify_fuzz.cc` and the header itself for the v1 channel design)
- `css_import_flattener.h` -- `@import` resolution and inlining
  - `ExtractImports(css)` → `vector<CssImport>` (URL + media query)
  - `ResolveUrlsInCss(css, import_url, parent_url)` -- rebases relative URLs
  - `FlattenImports(css, base_url, lookup_fn, max_depth)` → `FlattenResult` (recursive)
  - Flattening is all-or-nothing per sheet: any import that cannot be inlined
    (cache miss, depth limit, unsafe media, size cap, structurally unbalanced
    child — unterminated comment/string or unbalanced braces — or unparseable
    form) returns the original input unchanged with `skipped_unresolved_import`
    set
  - Statement scanning is comment-, string-, and escape-aware (`;`/`{` inside
    `/*...*/`, a string, or behind `\` never terminate/classify a statement);
    media values are comment-stripped at extraction, and unbalanced parens in
    a media value (a `;` hidden inside parens/unquoted `url()` mis-split the
    statement) or a live `@import` behind an unknown at-statement in a child
    force serve-original. Import dedup is keyed on (URL, effective media
    condition): the same sheet imported under different media inlines once per
    condition
  - URL escaping is decode-on-read / escape-once-on-write: extraction decodes
    escape sequences (css-syntax-3 §4.3.5/§4.3.7 — hex escapes, line
    continuations, escaped delimiters in unquoted `url()` tokens) and the
    writers re-escape only the delimiter, the backslash and newlines
    (§4.3.5). The two must stay inverses, or a URL containing `"`, `\` or `)`
    gains a backslash at every rebase level

## Minifier Algorithm

Tokenizer-based, not AST-based. The minifier does not parse CSS into a tree; it
operates as a multi-phase streaming character scanner with a state machine that
tracks strings, comments, and `url()` contexts. Five sequential phases:

1. **Phase 1** -- Strip comments, collapse runs of whitespace to single spaces,
   preserve string literals and unquoted `url()` content verbatim. Licence comments
   survive (#582): a comment starting `/*!`, or containing `@license` or
   `@preserve` anywhere (ASCII case-insensitive), is replaced by a one-byte
   sentinel (the first of `\x01`..`\x07` absent from the input; when none is free,
   preservation is off and such comments are stripped like any other) and its exact
   bytes are recorded in a `PreservedComments`. Phases 2-5 must copy the sentinel
   through unchanged: never drop, duplicate or reorder it.
   `RestorePreservedComments` puts the bytes back after Phase 5 in `MinifyCss`, and
   after Phase 2 in the token-stream oracle's `Normalize`, pairing by occurrence
   order. A preserved comment that runs to end of input is closed with `*/`. When
   the sentinel count differs from the recorded count, `RestorePreservedComments`
   returns false, and `MinifyCss` returns false with its output unchanged. Ordinary
   comments are still removed. The module's CSS filter must keep the same trigger
   set. The token-stream oracle accepts exactly the appended `*/` that closes such
   a comment.
2. **Phase 2** -- Remove whitespace around operators (`{`, `}`, `;`, `,`, combinators).
   Preserves spaces around `+` and `-` inside CSS math functions (the full
   CSS Values 4 set: `calc`/`min`/`max`/`clamp`/`round`/`mod`/`rem`/`abs`/
   `sign` plus trig/exp `sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`/
   `pow`/`sqrt`/`hypot`/`log`/`exp`, and Values 5 `calc-size`/`progress`/
   `random`; the `progress` boundary-match also covers `media-progress()`/
   `container-progress()`).
3. **Phase 3** -- Remove trailing semicolons before `}` (last-declaration optimization).
4. **Phase 4** -- Shorten decimal literals (`0.5` to `.5`, `-0.5` to `-.5`).
   Only fires at a number-token start: a `0` continuing an identifier
   (`.a0.5`, `a-0.5`, escaped chars) is ident content, not a number (#1163).
   Skips content inside `url()` to avoid corrupting data URIs.
5. **Phase 5** -- Collapse consecutive longhand properties into shorthands
   (padding, margin, border sides, overflow). Skips values containing `var()`,
   `!important`, or CSS-wide keywords. Recurses into nested blocks. The
   declaration splitter (`ParseBlockDecls`) ends a declaration only at a
   `;` outside `(...)`/`[...]` (`a{--x:(a;;b)}` had the empty
   segment between the `;;` dropped as an empty declaration), and the
   top-level scan and the brace matcher never treat a `{` or `}` inside
   an unclosed paren or bracket as a block edge (a nested simple block
   in CSS Syntax 3, not a rule block; a `)` met inside that nested `{`
   is content and does not close the paren). All three scanners share
   `ValueNest`, which, like Phase 3's depth counter, does not
   distinguish `)` from `]` — harmless, since a browser drops such a
   declaration either way; what matters is that the phases agree on
   where blocks are.

## CSS Minifier: Context Tracking

Never use brace depth to distinguish "selector context" from "declaration context".
Modern CSS (`@layer`, `@supports`, `@media`, `@container`) nests selectors inside
braced at-rule blocks, so `bd > 0` does NOT mean "inside a declaration".

**The rule:** always preserve whitespace before `:` because brace depth is unreliable
for determining context. In selector context the space is a descendant combinator
(`.prose :where(h2)`); removing it changes semantics. In declaration context the
space is harmless and virtually never emitted by build tools, so preserving it
costs nothing in practice. This is handled in Phase 2 of `css_minify.cc`.

Always test minifier changes with real-world CSS that includes `@layer`/`@supports`
nesting (e.g., Tailwind v4 output). See commits ba9afa8 and 22ba65e.

## Testing
```bash
bazel test //test/lib/css/...
```

### Key Test Files
- `test/lib/css/css_minify_test.cc` -- unit tests for `MinifyCss` (whitespace,
  comments, calc preservation, shorthand collapsing, decimal shortening, edge cases);
  the `ExpectMinify` helper doubles as an idempotence oracle for every input
- `test/lib/css/css_minify_corpus_test.cc` -- replays the 1.15 CSS
  fuzz corpus (`css_fuzz_corpus.h`, extracted verbatim from mod_pagespeed
  1.15's parser fuzz harness) through `MinifyCss`; asserts no crash, output
  never grows, and minify(minify(x)) == minify(x)
- `test/lib/css/css_import_flattener_test.cc` -- `@import` extraction, URL rebasing,
  recursive flattening
- `lib/css/css_minify_fuzz.cc` -- libFuzzer/deterministic-replay harness for
  `MinifyCss` (mirror of `lib/html/html_fuzz.cc`; bazel target
  `//lib/css:css_minify_fuzz`, tagged `manual`). Oracles: crash-free/no-throw
  under ASan, and strict idempotence (minify(minify(x)) == minify(x)) --
  restored as a hard oracle after the #1133 fix, which aligned Phase 1 and
  Phase 2 tokenization in both directions: Phase 1's kNormal now consumes
  backslash escapes (an escaped quote had opened a phantom string and an
  escaped '/' a phantom comment, breaking idempotence on trailing spaces
  and corrupting valid `\/` escapes in custom-property values), and Phase
  2 now mirrors Phase 1's unquoted-url() state (url content had been
  scanned as ordinary CSS, stripping spaces before operator chars and
  diverging on unterminated urls at end of input). The url()
  trailing-space trims are parity-guarded like the custom-property
  value trim: an escaped space ("\ " is a URL character) is never
  popped (#1154). Phases 3 and 5 are url()-opaque too (#1158): ';',
  '{' and '}' inside an unquoted url() are URL code points, so
  Phase 3's ';'-trim and Phase 5's declaration/block scanners
  (ParseBlockDecls, the brace matcher, the top-level loop, via
  ScanUnquotedUrl) skip url content verbatim. Phases 3 and 4 also
  mirror Phase 2's custom-property opacity (in_custom + custom_url,
  audit P3-d/P4-d): Phase 3's ';'-trim never fires inside a custom
  value but still trims the terminator (a{--x:v;} -> a{--x:v}), and
  Phase 4 no longer rewrites decimals inside custom values
  (--x:0.5 keeps its 0 -- an intentional behavior change) or inside
  identifiers (.a0.5 keeps its 0 -- the decimal rule fires only at a
  number-token start, #1163). Both phases
  consume backslash-escape pairs in kNormal (P3-a/P4-a), Phase 5's
  brace matcher tracks paren depth, and AtCustomPropertyColon's ident
  scan stops at combinators (#1156). Second oracle (audit PR-C):
  token-stream equivalence via the harness-local css-syntax-3-LITE
  tokenizer in `lib/css/css_token_stream.h` -- catches
  behavior-preserving corruption the idempotence oracle cannot see
  (e.g. #1158); v1 compares url-token, string-token, and
  custom-property-value-region channels byte-for-byte between input
  and output (see the header for sanctioned edits and blind spots).
  Deterministic main() replays a valid-CSS seed set (kValidCssSeeds)
  through both oracles before any file args. The new oracle's first
  catch: #1167 (Phase 5 rewrites brace-group content inside
  custom-property values); the {;x} face was re-found by libFuzzer
  and filed as #1168, since dup'd into #1167 -- the oracle red-lines
  on the custom-value faces until fixed. PR-D tail fixes: Phase 5's
  scanners (ParseBlockDecls, the top-level scan, the brace matcher)
  consume escape pairs outside strings (escaped ';' is value content,
  never a terminator; escaped quotes open no phantom strings, #1164);
  Phase 2's calc math-mode never strips a space that would glue
  '/'+'*' (or the mirror) into a comment token (#1159);
  TryCollapseFamily refuses empty longhand values and values whose
  edge chars make the emitted separator space Phase-2-trimmable
  (#1162). PR-E: brace groups inside declaration values are opaque --
  Phase 5 no longer recurses/collapses into them and Phase 3's
  ';'-trim no longer fires inside them (#1167; shared
  IsValueGroupBrace classification: declaration-like segment =
  top-level ':' with a property-name-ish run before it, custom-only
  at top level). PR-F: TryCollapseFamily also refuses paren-unbalanced
  longhand values (#1170 -- the rejoin reorders values, so a relocated
  paren shifted the next pass's block boundaries and with them Phase
  3's trim context; invalid-input-only strict red-line, now closed).
  The remaining strict-oracle red-line after PR-F: none on #1170's
  class; the token-stream oracle still has its disclosed over-catches
  on invalid input (comparator work, separate from the minifier).
