# Minimal repros — tokenizer minifier corpus spike (2026-07-21)

Clean-room, hand-minimal repros for the shapes the corpus run surfaced.
Every file here parses in node (verify: `node --check <file>`; `.mjs` via
`node --input-type=module --check < <file>`).

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
