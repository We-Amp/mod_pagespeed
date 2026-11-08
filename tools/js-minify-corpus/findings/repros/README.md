# Minimal repros — tokenizer minifier corpus spike (2026-07-21)

Clean-room, hand-minimal repros for the shapes the corpus run surfaced, plus
the hand-authored real-world pattern cases in the `c*` files (own section
below). Every file here parses in node, except
`n5-generator-comment-idempotence.js` (see its table row); verify:
`node --check <file>`; `.mjs` via `node --input-type=module --check < <file>`).

Expected behavior per file, verified with:

```sh
bazel-bin/tools/js-minify-corpus/js_minify_probe --mode=tokenizer <file> /tmp/out.js
```

## New shapes (exit 1 "unmodelable" → byte-preserved pass-through)

| File | Shape | Notes |
| --- | --- | --- |
| `n1-spread-call.js` | `h(...[])` | spread in call args |
| `n1-spread-array.js` | `[...[]]` | spread in array literal |
| `n1-spread-object.js` | `{...{}}` | spread in object literal |
| `n2-generator-decl.js` | `function* n() {}` | generator declaration |
| `n2-generator-expr.js` | `var g = function*() {};` | generator expression |
| `n2-async-generator.js` | `async function* n() {}` | async generator |
| `n2-generator-method.js` | `{ *n() {} }` | generator method |
| `n3-object-method.js` | `{ m() {} }` | object method shorthand |
| `n3-object-getter.js` | `{ get v() {} }` | object-literal getter |
| `n3-object-setter.js` | `{ set v(x) {} }` | object-literal setter |
| `n3-async-method.js` | `{ async m() {} }` | async method shorthand |
| `n3-computed-getter.js` | `{ get ['k']() {} }` | computed-name getter |
| `n4-u2028-in-string.js` | `'a<U+2028>b'` | raw U+2028 in string literal (legal since ES2019) |
| `n4-u2029-in-string.js` | `'a<U+2029>b'` | raw U+2029 in string literal |
| `n5-shebang.js` | `#!...` | shebang line (node CLI scripts) |
| `n6-arrow-try-catch.js` | `() => { try {} catch (e) {} }` | try/catch (or try/finally) inside an arrow body |
| `n6-arrow-for-in.js` | `() => { for (var k in o) {} }` | for/for-in/for-of inside an arrow body |
| `n6-arrow-while.js` | `() => { while (c) {} }` | while/do-while/switch inside an arrow body |
| `n5-generator-comment-await.js` | `{*/**/await(){}, y:1}` | generator method with a comment between `*` and a keyword name; valid input |
| `n5-generator-comment-idempotence.js` | `({*/**/a` + newline + `{z` | **deliberately not valid JavaScript** (minimized fuzz artifact for the idempotence axis); pinned by the goldens and the cross-product gate only, no parseable shape reproduces the newline-dependent difference |

## Known residual families (disclosed; included as controls)

| File | Family | Status this run |
| --- | --- | --- |
| `k3-export-default-regex.mjs` | K3: `export default function(){}` + newline + `/` | still unmodelable (exit 1, pass-through) |
| `k4-class.js` | K4: class bodies | unmodelable by design (exit 1, pass-through) |
| `k1-import-regex-control.mjs` | K1: `import x from 'y'` + newline + `/regex/` | **clean**: exit 0, newline preserved, output parses |

K2 (`[` / tagged template after a completed operand) was exercised by the
synthetic corpus (`shape_s029/059/089`, `exec_asi_index`, `exec_asi_tagged`,
module-forms tagged-template variant): no unwanted semicolon observed on
these shapes — newline preserved, semantic probes matched.

## Real-world pattern cases (`c*`)

Hand-authored, small cases for common real-world shapes that are hard for a
minifier. They are not minimized findings: they were written to cover the
patterns, and the run below is their first verified result. Every file
parses in node under the goal the harness classified it under.

| File | Pattern | Result this run |
| --- | --- | --- |
| `c01-regex-after-block-brace.js` | regex statement opening right after `}` with no `;` | exit 0, output parses, idempotent |
| `c02-regex-after-keyword.js` | regex in operand position of `return`/`typeof`/`void`/`case`/`do`/`of`/`new`/`delete`/`instanceof` | exit 0, output parses, idempotent |
| `c03-regex-after-control-paren.js` | regex right after the `)` of an `if`/`while`/`for` header, next to real division | exit 0, output parses, idempotent |
| `c04-template-nested-interp.js` | `${}` nested three deep, braces/backticks/regex inside the substitutions | exit 0, output parses, idempotent |
| `c05-template-raw-newlines.js` | raw line breaks and tabs in templates, tagged template, `String.raw`, escaped `${` | exit 0, output parses, idempotent |
| `c06-asi-paren-index-template.js` | line break before `(`, `[`, `.` and a template (no semicolon is inserted there, so the join must keep the meaning) | exit 0, output parses, idempotent |
| `c07-asi-prefix-postfix.js` | `i` newline `++` newline `j` (`i; ++j;`) | exit 0, newline preserved, idempotent |
| `c08-leading-semicolon.js` | defensive leading `;` before `[`, `(`, an arrow and a template | exit 0, output parses, idempotent |
| `c09-return-newline.js` | `return` + newline (restricted production) before `{`, `(`, `/re/` and inside parens | exit 0, newline preserved, idempotent |
| `c10-unicode-escape-identifiers.js` | `\uXXXX` in identifiers, keys and method names, including escapes spelling keywords; break between two escaped identifiers | exit 0, output parses, idempotent |
| `c11-unicode-raw-identifiers.js` | raw non-ASCII identifiers, keys, strings and regex source | exit 0, output parses, idempotent |
| `c12-html-comment-markers.js` | Annex-B `<!--` anywhere and `-->` at line start | exit 0, comments dropped, output parses, idempotent |
| `c13-html-close-comment-asi.js` | `i` newline `--> ...` — the break keeps `-->` a comment instead of `i-- >` | exit 0, newline preserved, idempotent |
| `c14-script-close-in-strings.js` | `</script` inside string, template and regex literals, plain and escaped | exit 0, bytes preserved, idempotent |
| `c15-numeric-separators-exponents.js` | `_` separators, exponents, BigInt, `5 .toString(2)`, `5..toFixed(1)`, `.5e+2` | exit 0, output parses, idempotent |
| `c16-object-getter-setter.js` | object accessors incl. names `get`, `set`, `static`, `async`, computed and numeric | exit 0, output parses, idempotent |
| `c17-class-accessors.js` | class accessors, static and computed, names colliding with accessor keywords | exit 0, output parses, idempotent |
| `c18-private-class-fields.js` | `#` fields/methods/statics, `#count in o` brand check | exit 0, output parses, idempotent |
| `c19-optional-chaining.js` | `?.`, `?.()`, `?.[]`, `??`, `??=`/`||=`/`&&=`, and `a ? .5 : b` | exit 0, output parses, idempotent |
| `c20-labelled-loops.js` | labelled `for`/`for-of`, `continue`/`break` to an outer label, labelled block, `empty: ;` | exit 0, output parses, idempotent |
| `c21-regex-vs-division.js` | division and regex slashes mixed inside single expressions and adjacent statements | exit 0, output parses, idempotent |
| `c22-module-forms.mjs` | module goal: top-level `await`, dynamic `import()`, `import.meta`, regex after `export` | exit 0, output parses, idempotent |
| `c23-unicode-codepoint-escape-identifier.js` | `\u{...}` code-point escape in an identifier, next to the same escape in a string | exit 1 (pass-through); the string-literal form alone minifies |

`c23` isolates the only decline among them: an ES2015 `\u{...}` code-point
escape **inside an identifier** makes the tokenizer return "unmodelable"
(the same escape inside a string literal does not). That is the designed
byte-preserving contract, not a correctness failure.
