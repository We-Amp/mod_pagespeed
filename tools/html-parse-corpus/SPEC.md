# HTML parse event-stream spec (v1)

Canonical contract for the differential HTML parse harness shared by
mod_pagespeed 1.15 (`pagespeed/kernel/html`) and ModPageSpeed 2.0
(`lib/html`). Phase 1 (this directory, in the mpp repo) implements the
reference probe, seed corpus and goldens. Phase 2 re-implements a probe in
the optimizer that produces **byte-identical** streams for the same inputs, and the
checked-in `goldens/manifest.json` gates both repos.

Tracking: the pagespeed-optimizer issue tracker. Pattern mirrored from the D1
JS-kernel effort (`tools/js-minify-corpus/`).

A *probe* runs the product's real lexer/parser (`HtmlParse` in 1.15) over an
input byte string with a recording event sink attached, and prints a
deterministic, line-oriented **parse event stream** to stdout. Two products
conform to this spec iff their probes emit the same stream for every input.

## 1. Scope: what the stream does and does not capture

The stream contains ONLY behavior both products are expected to agree on
post-convergence (both repos landed their convergence changes):

- the sequence of parse events: document start/end, element start (with
  attributes in parse order), element end (with close style), characters,
  CDATA, comments, IE directives, directives;
- the final document doctype (enum name);
- the terminal `size-limit-exceeded` flag.

The stream MUST NOT contain:

- **Message-handler text.** 1.15 reports lexer syntax errors through
  `MessageHandler`; 2.0 amputated `MessageHandler`. Recovery from a syntax
  error is still observable — it shows up in the event sequence itself — but
  the human-readable message text is not part of the contract. The 1.15
  reference probe attaches a `NullMessageHandler`.
- **URL-validity judgments** (the `GoogleUrl` seam). `HtmlParse::StartParse`
  parses a base URL with `GoogleUrl` and refuses to parse when it is
  invalid; 2.0's URL machinery differs. The event-generation path itself
  never consults URL validity, so the seam is normalized away by contract:
  **the probe always parses with the fixed base URL
  `http://html-parse-corpus.invalid/` and content type `text/html`
  (`kContentTypeHtml`)**. The URL never appears in the stream. A probe must
  treat "base URL failed to parse" as a harness bug (exit 2), never as
  input-dependent behavior.
- Line numbers, timing, memory addresses, filter names, or anything else
  not listed below.

Entity decoding of attribute values **is** compared, exactly as implemented
by each product's `HtmlElement::Attribute::DecodedValueOrNull()` equivalent
(including which inputs count as decoding errors). Character-data content is
compared as raw bytes (the lexer does not decode entities in text).

## 2. Parse-session setup (normative)

For each input (an arbitrary byte string; no encoding assumptions, bytes
used as-is):

1. Create a fresh parser + recording sink per input. No state carries over.
2. Start the parse with base URL `http://html-parse-corpus.invalid/` and
   content type `text/html`.
3. Feed the entire input in one `ParseText` call, then `FinishParse`.
   No intermediate flushes, no explicit `set_size_limit` — only the lexer's
   unconditional 10 MB token ceiling (`kMaxTokenSize`) and the unconditional
   nesting cap (`kMaxNestingDepth = 512`) apply, both of which are part of
   the compared behavior.
4. Collect the sink's stream, append the trailer lines (§3.4), print to
   stdout as a single byte string.

## 3. Stream format

Bytes: UTF-8-agnostic; the stream itself is pure 7-bit ASCII. Lines are
terminated by a single LF (`\n`, 0x0A). No CR, no trailing spaces, no final
blank line beyond the last line's LF. The same input MUST always produce a
byte-identical stream.

### 3.1 Field escaping

Several fields carry input-derived bytes (tag/attribute name spellings,
attribute values). They are escaped so a rendered field never contains a
byte outside `[0x21, 0x7E]` and never contains `\`:

- byte `0x5C` (`\`) → the two bytes `\\`
- any byte `< 0x21` or `> 0x7E` → `\xHH`, lowercase hex, exactly two digits
  (this includes space `0x20` → `\x20`)
- all other bytes (printable ASCII except `\`) are emitted as-is.

Fields are separated by single spaces, so escaping makes the grammar
unambiguous. The hash fields (§3.3) cover the *unescaped* content bytes.

### 3.2 FNV-1a 64

Content-bearing leaf events (characters, CDATA, comment, IE directive,
directive) are rendered as a byte length plus a 64-bit FNV-1a hash of the
content, keeping streams compact while divergence stays pinpointable:

```
h = 14695981039346656037  (0xcbf29ce484222325)
for each byte b of the content, in order:
    h = h XOR b
    h = (h * 1099511628211) mod 2^64   (0x100000001b3)
```

Rendered as `%016x` (16 lowercase hex digits, zero-padded).

The hash input is exactly the node's content byte string as delivered to the
event sink — for 1.15 that is `HtmlLeafNode::contents()`: for a directive,
the bytes between `<!` and `>`; for a comment, the bytes between `<!--` and
`-->`; for CDATA, the bytes between `<![CDATA[` and `]]>`; for characters,
the raw literal run. No decoding, no normalization, no NUL termination.

### 3.3 Event lines

One line per event, in the order the sink receives them:

| Line | Format |
|---|---|
| document start | `document-start` |
| element start | `start <kw> <spelling>[ <attr>]*` |
| element end | `end <kw> <spelling> <style>` |
| characters | `chars len=<n> fnv=<h>` |
| CDATA | `cdata len=<n> fnv=<h>` |
| comment | `comment len=<n> fnv=<h>` |
| IE directive | `iedirective len=<n> fnv=<h>` |
| directive | `directive len=<n> fnv=<h>` |
| document end | `document-end` |

`<n>` is the decimal content byte length; `<h>` the FNV-1a hash (§3.2).

**Names.** `<kw>` is the canonical lowercase keyword string for the
element/attribute keyword (the gperf table entry, e.g. `div`, `href`), or
`-` when the name is not a recognized keyword (`kNotAKeyword`). `<spelling>`
is the escaped (§3.1) source spelling of the name as preserved by the parser
(1.15: `HtmlName::value()`, which keeps the original case for non-canonical
spellings — e.g. `<DIV>` renders `start div DIV`).

**Attributes.** `<attr>` = `<kw> <spelling> <q><m><payload>`, concatenated
without spaces into one field:

```
<attr> := <kw> ":" <spelling> "=" <q> <m> <payload>
```

- `<q>`: quote style in source — `n` no quote, `s` single quote, `d` double
  quote.
- `<m>`: value marker —
  - `~`: attribute has no value at all (`<div hidden>`); `<payload>` empty.
  - `v`: value decoded successfully; `<payload>` is the escaped (§3.1)
    decoded value bytes (may be empty: `<div a="">` → `a:a=dv`).
    The decoded value is a NUL-terminated C string with strlen semantics:
    a decoded NUL byte (e.g. `a="&#0;"`) truncates the payload there —
    as-implemented, and identical in both products.
  - `!`: decoding error (`DecodedValueOrNull()` returned null for a present
    value); `<payload>` is the escaped raw (still-escaped) value bytes.
    Rendering the raw form here keeps undecodable values comparable.

Attributes appear in source parse order; duplicates are not deduplicated.

**Close styles.** `<style>` is one of `auto`, `implicit`, `explicit`,
`brief`, `unclosed`, `invisible` (1.15: `HtmlElement::Style` — `AUTO_CLOSE`,
`IMPLICIT_CLOSE`, `EXPLICIT_CLOSE`, `BRIEF_CLOSE`, `UNCLOSED`, `INVISIBLE`).
Elements the parser never closed before end-of-input appear with the style
the parser assigns at close-out (`unclosed` in 1.15, which maps them to
`AUTO_CLOSE`-derived handling — see residual policy §5 if products differ
here by construction).

**Flush events are not rendered.** The reference session (§2) flushes only
once, inside `FinishParse`; flush boundaries carry no compared behavior.

### 3.4 Trailer

After `document-end`, exactly two trailer lines:

```
doctype <ENUM>
flags <FLAGS>
```

- `<ENUM>`: the final document doctype, one of `UNKNOWN`, `HTML_5`,
  `HTML_4_STRICT`, `HTML_4_TRANSITIONAL`, `XHTML_5`, `XHTML_1_1`,
  `XHTML_1_0_STRICT`, `XHTML_1_0_TRANSITIONAL`, `OTHER_XHTML`. (1.15 note:
  `DocType::DocTypeEnum` is private; the probe maps via the public static
  `DocType` constants, with `OTHER_XHTML` as the elimination fallback.)
- `<FLAGS>`: `none`, or `size-limit-exceeded` when the parser reported its
  size/token ceiling tripped (1.15: `HtmlParse::size_limit_exceeded()`).

The nesting cap (`kMaxNestingDepth = 512`) has no flag of its own: tripping
it truncates the event sequence, which is itself the compared signal, and
both products converge on that behavior.

### 3.5 Example

Input: `<!DOCTYPE html><DIV class="a&amp;b" hidden>hi &amp; bye<br/>`

```
document-start
directive len=12 fnv=af63bd4c8601b7be
start div DIV -:class=dv a&b -:hidden=n~
chars len=11 fnv=xxxxxxxxxxxxxxxx
start br br
end br br brief
document-end
doctype HTML_5
flags none
```

(Hash values above are illustrative, not computed.)

## 4. Probe exit codes

| Code | Meaning |
|---|---|
| 0 | clean parse; `flags none` |
| 2 | harness error: usage, I/O, or the fixed base URL refused (never input-dependent) |
| 3 | parse completed with `flags size-limit-exceeded` |
| signal | crashes propagate as signals; the harness records `signal-N` |

Exit code is part of the recorded golden status, so the size-limit path is
compared too. A probe MUST NOT exit non-zero for any other input-dependent
condition — malformed HTML is normal input and yields exit 0.

## 5. Residual-divergence policy

The manifest records exactly ONE expected output per input, never per-side.
If the two products' probes disagree on an input, it is either:

1. **a bug** — fix the diverging side (in a convergence PR) and regenerate
   the goldens in the same change; or
2. **a known, reasoned asymmetry** — the input is added to
   `EXCLUSIONS.md` (checked in next to this spec) with the input path, the
   nature of the divergence, and the reason it is excluded. Excluded inputs
   stay in the corpus for each side's own crash-oracle fuzzing but carry no
   cross-product golden.

`EXCLUSIONS.md` starts empty. Adding an entry requires the same review bar
as a code change.

## 6. Versioning

This is spec v1. Any change to the stream format, session setup, or exit
codes bumps the version, regenerates every golden in the same commit, and
lands in both repos together (the manifest's generator self-hash pins the
tooling version).
