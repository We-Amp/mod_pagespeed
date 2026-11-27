---
title: 'Safe JavaScript minification: semicolon insertion, fail-safe'
description: 'Safe JavaScript minification means parsing, not stripping whitespace: semicolon insertion, regex-vs-divide and a fail-safe that ships the original as is.'
date: 2026-06-14
lastUpdated: 2026-09-06
author: 'Otto van der Schaaf'
tags: ['performance', 'deep-dive', 'filters', 'javascript']
draft: false
product: '2.0'
---

Here is a one-line JavaScript program that a naive minifier will silently break:

```js
return
  a + b
```

A whitespace stripper that joins lines sees `return a + b` and ships it. But the original returns `undefined`: JavaScript inserts a semicolon after `return` at the line break, and `a + b` becomes dead code. Remove the newline and you have changed what the program computes. Safe JavaScript minification has to know this, which is why the optimizer worker's `jm` filter does not strip whitespace. It tokenizes.

The reason is built into the language. The comment at the top of `lib/js/js_tokenizer.cc` puts it bluntly: in `(x + y) / z` that slash is division, but the same slash *could* be the start of a regex literal if the token before the `(` was `if`. So you have to track parse state. And whitespace can matter because of semicolon insertion, and deciding whether a given piece of whitespace matters needs not just the previous parse state but a look *ahead* to the next token. You cannot lex JavaScript without partly parsing it.

This is the original PageSpeed JS minifier, carried into the 2.0 rebuild after years of production use. The mechanism below is the shipped code in `lib/js`.

## Safe JavaScript minification starts with automatic semicolon insertion

The minifier runs through `JsMinifyingTokenizer` (in `lib/js/js_minify.cc`), which wraps the lower-level `JsTokenizer`. Its job on every newline is to answer one question: does this line break trigger automatic semicolon insertion (ASI)? If it does, the newline carries meaning and must survive; if it does not, the newline is free to delete.

The tokenizer answers that in `TryInsertLinebreakSemicolon()` (`js_tokenizer.cc`). It first skips past any following comments and whitespace into a lookahead queue, then decides based on the current parse state and the next real token. Two examples from the code:

- After an expression (parse state `kExpression`), it runs the `line_continuation_pattern` regex against the upcoming input. If the next token *could* continue the statement, no semicolon is inserted, so the newline is droppable. `kLineContinuationRegex` matches operators that can legally start a continuation line. Its leading character class is `[=(*/%^&|<>?:,.]`, so a line starting with any of those (`=`, `(`, `*`, `/`, `%`, `^`, `&`, `|`, `<`, `>`, `?`, `:`, `,`, `.`) continues. The regex also handles the cases a single character can't decide: a `!=` (but not a bare `!`), a `+`/`-` that is not part of `++`/`--`, and the `in`/`instanceof` keywords.
- After `return`, `throw`, `break`, `continue`, or `debugger`, the answer is always "insert." These are ECMAScript's restricted productions, where no line terminator is allowed between the keyword and its operand. In the tokenizer these become the parse states `kReturnThrow` and `kJumpKeyword`, and `TryInsertLinebreakSemicolon` falls straight through to inserting the semicolon. That is exactly the `return` / `a + b` case from the top of this post.

When ASI does fire, the tokenizer emits a `kSemiInsert` token; the minifying layer turns that into a `\n` in the output so the browser's own ASI re-inserts the semicolon. When it does not fire, the newline collapses to nothing. The result is meaning-preserving: the only line breaks that survive are the ones the program actually depends on.

There is a subtle case the code calls out by name. A block comment that contains a line terminator counts as a line break for ASI, not as a mere space. The comment in `js_minify.cc` gives the fixture: `return/*\n*/'str'` must not become `return'str'`, because that newline inside the comment is what inserts the semicolon. `IsAsiKeyword` exists specifically to handle this, and the tokenizer treats a newline-bearing block comment as `kLinebreak` rather than `kSpace`.

## Regex or divide: the same `/`, two meanings

The other half of the problem is the slash. `ConsumeSlash` in `js_tokenizer.cc` switches on the top of the parse stack to decide what a `/` means:

- After a `kExpression` (a literal, a `(...)`, a `foo[0]`, a closing paren or bracket), the slash is division. It calls `ConsumeOperator`.
- After `kStartOfInput`, an operator, a `?`, an open delimiter, a block header, or `return`/`throw`, the slash starts a regex literal. It calls `ConsumeRegex`.
- After a period, a block keyword, a jump keyword, or another keyword where a slash is illegal, it is a parse error.

This is the disambiguation the legacy comment summarized as `return/ x /g` returning a regex literal while `reTurn/ x /g` performs two divisions. The tokenizer reaches the answer by maintaining a stack of parse states (`kExpression`, `kOperator`, `kBlockKeyword`, `kBlockHeader`, `kReturnThrow`, and so on) and pushing or popping on every token. The long worked example in the header walks `if ([]) { foo: while(true) break; } else /x/.test('y');` through the stack one token at a time, showing how a slash after a block header is a regex while a slash after an expression is division.

The whitespace rules ride on the same machinery. `WhitespaceNeededBefore` keeps a single space when removing one would merge tokens: two names or numbers gluing together, a `.` getting absorbed as a decimal point onto a numeric literal that has no point yet, or operator characters fusing into a new operator or a line comment (`/` next to `/`, `+` next to `+`, `<` next to `!`, and a trailing `!` or `-` next to `-`). Everything else between tokens goes.

The tokenizer also bails out by design when the parse state is past the point of meaning. The header gives `[a}/x/i`: are those slashes a regex or division? "The question has no answer," so the tokenizer aborts rather than guess. There is a `kMaxParseStackDepth` of 4096 to stop pathologically nested input from exhausting memory, and unterminated strings, regexes, or template literals are errors too. Which brings us to what happens when the minifier gives up.

## The fail-safe: a parse error ships the original, untouched

A minifier that is willing to abort needs a safe thing to do when it aborts. The optimizer worker's answer is the guard in `src/worker/worker.cc`, in the JS branch of the optimization handler:

```cpp
bool js_ok = js::MinifyUtf8Js(&js_patterns, js_input, &minified_js);
if (!js_ok) {
  LogWarning(
      "JS minification had parse errors for %s, "
      "serving original (variant not written)",
      notification.url.c_str());
  // The tokenizer's error path emits the unlexable remainder raw,
  // so the output may be half-minified — never ship it.  Mark
  // processed: parse failure is deterministic for a given input,
  // so retrying on the next notification would loop forever.
  stats_.text_minify_parse_failures.fetch_add(1,
                                              std::memory_order_relaxed);
  MarkVariantProcessed(...);
  return;
}
```

When `MinifyUtf8Js` returns false, the worker discards `minified_js` entirely and writes no optimized variant. The customer keeps getting the original bytes. The comment spells out why the partial output cannot be trusted: on `kError`, `MinifyUtf8Js` appends the unlexable remainder of the input raw and returns false, so the buffer may be half-minified. Shipping that would be worse than shipping nothing.

Two more details from the guard matter operationally. It increments `stats_.text_minify_parse_failures`, so a file the minifier cannot handle shows up in worker stats rather than disappearing silently. And it calls `MarkVariantProcessed` even on failure: a parse error is deterministic for a given input, so without that mark the worker would re-attempt the same doomed file on every notification forever. The failure is recorded once and not retried.

The shape of this is the whole point. The optimizer is allowed to be conservative, to abort on inputs it cannot prove safe, precisely because the fallback is the unmodified original. The worst case for a file the minifier refuses is zero bytes saved, never a broken script. If you want to see where the optimized variant gets written when minification *does* succeed, that path is `WriteTextVariant`, and the rewrite-then-serve flow is covered in [how async rewriting works](/how-it-works/async-rewriting/).

## Related

- [/blog/remove-unused-javascript-chrome-coverage/](/blog/remove-unused-javascript-chrome-coverage/) — detecting unused JS with the Chrome Coverage API, the other half of the JS story
- [/blog/content-hash-urls/](/blog/content-hash-urls/) — why the `jm` filter's output URL carries a content hash
- [/blog/flatten-css-imports-edge-cases/](/blog/flatten-css-imports-edge-cases/) — the same conservative, abort-on-ambiguity approach applied to CSS
- [/blog/css-cache-inlining-for-coverage-api/](/blog/css-cache-inlining-for-coverage-api/) — cache-aware inlining for coverage measurement
- [/blog/fix-inp-wordpress-2026/](/blog/fix-inp-wordpress-2026/) — cutting JavaScript work to fix INP on WordPress

Minification shrinks the bytes you keep; [removing the unused JavaScript Chrome Coverage flags](/blog/remove-unused-javascript-chrome-coverage/) cuts the bytes you never needed. The `jm` filter is one of the optimizer worker's [optimization filters](/features/), and if you want to put it in front of your own scripts, [download mod_pagespeed](/download/) and watch the worker stats: `text_minify_parse_failures` will tell you immediately if any of your bundles trip the tokenizer, and the originals keep serving while you look. The [configuration docs](/docs/configuration/) cover the JS size cap and how to enable the filter. It is licensed under Apache-2.0 and free to run, so you can measure the savings on your own bundles.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
