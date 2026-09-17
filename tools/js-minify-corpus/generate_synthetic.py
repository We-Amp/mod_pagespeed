#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Seeded synthetic JS corpus generator for minifier stress testing.

Deterministic: same --seed produces byte-identical output. Emits two kinds of
probes into the destination directory, plus an index.json describing them:

  shape_NNN.js / .mjs   parse-only probes targeting tokenizer hazard shapes
                        (ASI traps, modern syntax combos, comment torture,
                        unicode, giant flat files, deep nesting, ...).
  exec_NNN.js / .mjs    executable self-checking probes: programs that print
                        "RESULT <value>" when run in node. The oracle runs the
                        original and the minified output and diffs stdout, so
                        these catch meaning-changing minifications that still
                        parse.

Inputs are intended to be VALID JavaScript (the oracle separately verifies
that inputs parse; a generator bug producing invalid input is reported, not
silently absorbed).

Stdlib only.
"""

import argparse
import json
import os
import random
import sys

DEFAULT_SEED = 20260721

# ---------------------------------------------------------------------------
# Building blocks
# ---------------------------------------------------------------------------

IDENTS = ["alpha", "beta", "gamma", "x", "y", "z", "foo", "bar", "_tmp", "$el",
          "value", "result", "item0", "cfg"]
NUMS = ["0", "1", "2", "3", "7", "42", "100", "3.14", "0.5", ".25", "1e3",
        "0x1F", "0b1010", "0o17", "1_000_000", "0755", "1.5e-3"]
STRS = ['"hello"', "'world'", '"a\\nb"', "'it\\'s'", '"\\u00e9"', '"\\x41"',
        '"tab\\tend"', '""', "'\\\\'"]


def ri(rng, n):
    return rng.randrange(n)


def pick(rng, xs):
    return xs[ri(rng, len(xs))]


def ident(rng):
    return pick(rng, IDENTS)


def num(rng):
    return pick(rng, NUMS)


def s(rng):
    return pick(rng, STRS)


def expr(rng, depth=0):
    """Small random expression, guaranteed side-effect free."""
    if depth > 2:
        return pick(rng, [ident(rng), num(rng), s(rng)])
    kind = ri(rng, 10)
    if kind == 0:
        # Fully parenthesized: keeps `-x ** y` and precedence mixes legal.
        return "(%s) %s (%s)" % (expr(rng, depth + 1),
                                 pick(rng, ["+", "-", "*", "/", "%", "**",
                                            "&", "|", "^", "<<", ">>"]),
                                 expr(rng, depth + 1))
    if kind == 1:
        return "(%s) %s (%s)" % (expr(rng, depth + 1),
                                 pick(rng, ["===", "!==", "<", ">", "<=",
                                            ">="]),
                                 expr(rng, depth + 1))
    if kind == 2:
        return "(%s) ? (%s) : (%s)" % (expr(rng, depth + 1),
                                       expr(rng, depth + 1),
                                       expr(rng, depth + 1))
    if kind == 3:
        # ?? cannot mix with && / || unparenthesized (SyntaxError by spec).
        return "(%s) %s (%s)" % (expr(rng, depth + 1),
                                 pick(rng, ["&&", "||", "??"]),
                                 expr(rng, depth + 1))
    if kind == 4:
        return "(%s)" % expr(rng, depth + 1)
    if kind == 5:
        return "[%s]" % ", ".join(expr(rng, depth + 1)
                                  for _ in range(ri(rng, 4)))
    if kind == 6:
        return "{a: %s, b: %s}" % (expr(rng, depth + 1), expr(rng, depth + 1))
    if kind == 7:
        # Parenthesized operand: avoids `++`/`--` adjacency and unary-before-**
        # syntax errors when nesting unaries.
        return "(%s(%s))" % (pick(rng, ["!", "~", "-", "+", "typeof ",
                                        "void "]), expr(rng, depth + 1))
    if kind == 8:
        return "%s.%s" % (ident(rng), pick(rng, ["a", "b", "length"]))
    return "%s[%s]" % (ident(rng), num(rng))


def regex_lit(rng):
    body = pick(rng, [
        "ab+c", "[a-z]+", "\\d{2,4}", "[/]slash", "a\\/b", "[^/]*",
        "(x|y)*z", "\\s\\S", "^caret$", "[.*+?^${}()|\\[\\]\\\\]",
    ])
    flags = pick(rng, ["", "g", "i", "gi", "m", "gy", "s"])
    return "/%s/%s" % (body, flags)


# ---------------------------------------------------------------------------
# Parse-only shape families. Each returns a list of code lines.
# ---------------------------------------------------------------------------

def sh_asi_paren(rng):
    # Expression completed at line end, next line starts with '('. Joining
    # changes a statement boundary into a call.
    return [
        "var %s = %s" % (ident(rng), expr(rng, 1)),
        "(%s, %s);" % (expr(rng, 1), expr(rng, 1)),
    ]


def sh_asi_bracket(rng):
    # Next line starts with '[': membership access vs new array statement.
    return [
        "var %s = %s" % (ident(rng), pick(rng, ["[1, 2, 3]", "alpha", "cfg"])),
        "[%s, %s].forEach(function(v) { return v; });" % (num(rng), num(rng)),
    ]


def sh_asi_regex(rng):
    # Regex statement after a restricted production (`return` + line break:
    # ASI must fire) and after an if-block close brace. Both inputs are
    # valid; dropping the line break after `return` changes the meaning.
    return [
        "function f() {",
        "  return",
        "  %s;" % regex_lit(rng),
        "}",
        "f();",
        "if (f()) {",
        "}",
        "%s.test(%s);" % (regex_lit(rng), s(rng)),
    ]


def sh_asi_pm(rng):
    # Line starting with unary +/-: continuation of an arithmetic expression
    # vs a new statement.
    sign = pick(rng, ["-", "+"])
    return [
        "var %s = %s" % (ident(rng), num(rng)),
        "%s%s;" % (sign, expr(rng, 1)),
    ]


def sh_asi_backtick(rng):
    # Line starting with a template literal: tagged-template call vs new
    # expression statement.
    return [
        "var tag = function(s) { return s.raw ? s.raw[0] : s; }",
        "var %s = %s" % (ident(rng), pick(rng, ["tag", "42", "alpha"])),
        "`tpl %s`;" % ident(rng),
    ]


def sh_asi_return(rng):
    # return<newline>expr: ASI must insert a semicolon; dropping the line
    # break changes the return value.
    return [
        "function f() {",
        "  return",
        "  { a: %s };" % num(rng),
        "}",
        "f();",
    ]


def sh_asi_postfix(rng):
    # i<newline>++<newline>j: ASI parses as `i; ++j;` (++ is a restricted
    # production). Joining the lines yields the invalid `i++j`.
    v = ident(rng)
    w = pick(rng, [x for x in IDENTS if x != v])
    return [
        "var %s = %s, %s = %s;" % (v, num(rng), w, num(rng)),
        v,
        "++",
        "%s;" % w,
    ]


def sh_asi_let(rng):
    # 'let' as line-final token: ASI hazard between let-as-identifier and
    # let-declaration.
    return [
        "var let = %s;" % num(rng),
        "let",
        "%s = %s;" % (ident(rng), num(rng)),
    ]


def sh_do_while(rng):
    v = ident(rng)
    return [
        "var %s = 0;" % v,
        "do %s++" % v,
        "while (%s < %s);" % (v, num(rng)),
    ]


def sh_nullish(rng):
    return [
        "var cfg = { a: null, b: 0, c: { d: undefined } };",
        "var n1 = cfg.a ?? %s;" % expr(rng, 1),
        "cfg.b ??= %s;" % num(rng),
        "var n2 = cfg?.c?.d ?? cfg?.a?.x ?? %s;" % num(rng),
        "var n3 = (cfg.a ?? cfg.b) ? cfg?.c : null;",
    ]


def sh_optional_chain(rng):
    return [
        "var obj = { m: function() { return { r: 1 }; }, list: [{ v: 2 }] };",
        "var o1 = obj?.m?.()?.r;",
        "var o2 = obj?.list?.[0]?.v;",
        "var o3 = obj.m?.().r ?? -1;",
        "var o4 = obj?.['li' + 'st']?.[1]?.v ?? 'missing';",
    ]


def sh_destructure(rng):
    return [
        "var { a = 1, b: { c = [2, 3], d: dd = 4 } = {}, ...rest } =",
        "    { b: { c: [9] }, extra: true };",
        "var [p, , [q] = [], ...tail] = [1, 2, [3], 4, 5];",
        "function g({ m = 0 } = {}) { return m; }",
        "var { length = 0 } = %s;" % s(rng),
    ]


def sh_spread(rng):
    return [
        "var arr = [1, 2, 3];",
        "var more = [...arr, 4, ...[5, 6]];",
        "var obj = { x: 1, ...{ y: 2, z: 3 }, w: 4 };",
        "function h() { return arguments.length; }",
        "var cnt = h(...more, ...[7, 8]);",
        "var str = [...'abc'];",
    ]


def sh_template_nested(rng):
    return [
        "var inner = 'IN', n = 5;",
        "var t1 = `outer ${`mid ${inner} ${n + 1}`} done`;",
        "var t2 = `${n > 3 ? `big ${n}` : 'small'}`;",
        "var t3 = `multi",
        "line ${n} template`;",
        "var t4 = String.raw`raw \\n ${n}`;",
    ]


def sh_dynamic_import(rng):
    return [
        "var spec = './dep-' + %s + '.js';" % num(rng),
        "var pending = import(spec).catch(function() { return null; });",
        "var meta = import.meta.url;",
        "import(spec).then(function(m) { return m; }, function() {});",
    ]


def sh_module_forms(rng):
    choice = ri(rng, 5)
    if choice == 0:
        return [
            "import def from './mod.js';",
            "import { a as b, c } from './mod2.js';",
            "export { b, c };",
            "export default function() { return b; };",
        ]
    if choice == 1:
        return [
            "import './side-effect.js';",
            "export * as ns from './ns.js';",
            "export const pi = 3.14159;",
        ]
    if choice == 2:
        return [
            "export default function() {}",
            regex_lit(rng) + ".test('ab');",
        ]
    if choice == 3:
        return [
            "import def from './mod.js';",
            regex_lit(rng) + ".test('ab');",
        ]
    return [
        "const local = %s;" % num(rng),
        "export default local",
        "`tagged`;",
    ]


def sh_generators(rng):
    return [
        "function* gen() {",
        "  yield %s;" % expr(rng, 1),
        "  yield* [1, 2, 3];",
        "  var got = yield 'ping';",
        "  return got;",
        "}",
        "var it = gen();",
        "it.next(); it.next();",
    ]


def sh_async(rng):
    return [
        "async function af() {",
        "  var v = await Promise.resolve(%s);" % num(rng),
        "  return v + 1;",
        "}",
        "var ap = af().then(function(x) { return x; });",
        "async function* agen() {",
        "  for await (const chunk of []) { yield chunk; }",
        "}",
        "var runner = async () => await af();",
    ]


def sh_labels(rng):
    return [
        "outer: for (var i = 0; i < 3; i++) {",
        "  inner: for (var j = 0; j < 3; j++) {",
        "    if (j === 1) continue outer;",
        "    if (i === 2) break outer;",
        "  }",
        "}",
        "block: { break block; var never = 1; }",
        "done:;",
    ]


def sh_getset(rng):
    return [
        "var store = { _v: 1,",
        "  get v() { return this._v; },",
        "  set v(x) { this._v = x * 2; },",
        "  get ['k' + 1]() { return 7; }",
        "};",
        "store.v = 5;",
        "var got = store.v + store.k1;",
    ]


def sh_class_known(rng):
    # Known residual family K4: class bodies are byte-preserved by design.
    # Included so the corpus exercises the pass-through path.
    return [
        "class Widget extends Object {",
        "  static count = 0;",
        "  #priv = %s;" % num(rng),
        "  constructor(v) { super(); this.v = v ?? 1; }",
        "  get value() { return this.#priv; }",
        "  static make() { return new Widget(); }",
        "  ['computed']() { return 1; }",
        "}",
        "var w = new Widget(2);",
    ]


def sh_regex_positions(rng):
    # Regex literals in positions that stress the division heuristic:
    # after ')', after ']', after '}', after identifiers, after keywords.
    # Lines are independently valid (regex statements follow complete
    # statements, never an operand that would force division).
    return [
        "var r1 = %s;" % regex_lit(rng),
        "if (r1.test('ab')) { var hit = %s; }" % regex_lit(rng),
        "var arr = [1].map(function(x) { return x; });",
        "%s.exec('ab');" % regex_lit(rng),
        "function check(x) { return typeof x === 'object' ? %s : null; }"
        % regex_lit(rng),
        "var div = 100 / 2 / 5;",
        "var mix = div / 2, re = %s;" % regex_lit(rng),
        "var afterIf = 1;",
        "if (afterIf) { var block = 2; }",
        "%s.test('tail');" % regex_lit(rng),
    ]


def sh_comment_torture(rng):
    return [
        "/* banner %s */" % ident(rng),
        "var a = 1 /* mid */ + /* mid2 */ 2;",
        "var b = 3; // trailing comment",
        "var c = /* before */ %s;" % num(rng),
        "var s1 = 'not a /* comment';",
        "var s2 = \"nor */ this\";",
        "// line comment with a `backtick` and a /regex-lookalike/",
        "var d = 4;/*@cc_on @*/",
        "/*@if (@_jscript) var cc = 1; @end @*/",
        "var e = 5",
        "// ASI via line comment after expression",
        "(function() {})();",
    ]


def sh_sgml_comments(rng):
    # Annex-B HTML-like comments: <!-- anywhere a line comment is allowed,
    # --> only at the start of a line ( sloppy script goal).
    return [
        "<!-- html-open comment",
        "var sg1 = %s;" % num(rng),
        "var sg2 = 2;",
        "--> html-close comment at line start",
        "var sg3 = 3;",
    ]


def sh_unicode(rng):
    return [
        "var \\u03c0 = 3.14;",
        "var \\u{1F600} = 'emoji-ident-attempt';".replace(
            "\\u{1F600}", "smile"),  # keep identifiers conservative
        "var caf\\u00e9 = 'coffee';".replace("caf\\u00e9", "café"),
        "var 變量 = %s;" % num(rng),
        "var s1 = 'é\u00e9 \u2028 line-sep-in-string \u2029';",
        "var s2 = '\U0001F600 emoji';",
        "var re = /[é-ü]/u;",
        "變量 += 1;",
    ]


def sh_numbers(rng):
    return [
        "var hex = 0xDEADBEEF;",
        "var bin = 0b11110000, oct = 0o777, legacy = 0755;",
        "var big = 123456789012345678901234567890n;",
        "var sep = 1_000_000.5_5e1_0;",
        "var frac = 5 .toString();",
        "var two = 5..toFixed(1);",
        "var half = .5e-1;",
        "var neg = -0, inf = 1e999;",
    ]


def sh_ternary_regex_mix(rng):
    return [
        "var q = %s ? %s : %s;" % (expr(rng, 1), regex_lit(rng), num(rng)),
        "var w = alpha?.b ?? /fallback/i;",
        "var e = (q && /x/) || " + regex_lit(rng) + ";",
    ]


def sh_control_flow(rng):
    return [
        "for (var i = 0, n = 10; i < n; i += 2) { if (i > 5) break; }",
        "for (var k in { a: 1 }) { var kin = k; }",
        "for (var v of [1, 2]) { var vof = v; }",
        "switch (%s) {" % num(rng),
        "  case 1: var one = 1; break;",
        "  case %s: %s.test('x'); break;" % (num(rng), regex_lit(rng)),
        "  default: var def = 0;",
        "}",
        "while (false) { var w = 1; }",
        "if (true) var bare = 1; else var bare2 = 2;",
    ]


def sh_iife_arrow(rng):
    return [
        "var r1 = (function(x) { return x * 2; })(%s);" % num(rng),
        "var r2 = ((x) => x + 1)(%s);" % num(rng),
        "var r3 = (x => x => x)(1)(2);",
        "var r4 = (() => {",
        "  return",
        "  %s;" % num(rng),
        "})();",
        "var r5 = async () => ({});",
    ]


def sh_eval_with_sloppy(rng):
    # Sloppy-mode-only constructs (script goal, not module, not strict).
    return [
        "var obj = { p: %s };" % num(rng),
        "with (obj) { var seen = p; }",
        "var ev = eval('1 + 2');",
        "function dup(a, b) { var argumentsLen = arguments.length; }",
        "delete obj.p;",
    ]


def sh_try_finally(rng):
    return [
        "try { throw new Error('x'); } catch { var es2019 = 1; }",
        "try { var t = 1; } catch (e) { var c = 2; } finally { var f = 3; }",
        "try {",
        "  throw %s;" % regex_lit(rng),
        "} catch (re) { var caught = re instanceof RegExp; }",
    ]


def sh_known_import_regex(rng):
    # Known residual family K1: import + newline + regex. Included to
    # measure the pass-through rate of disclosed shapes.
    return [
        "import def from './dep.js';",
        "%s.test('target');" % regex_lit(rng),
        "export var done = 1;",
    ]


def sh_known_export_default_regex(rng):
    # Known residual family K3.
    return [
        "export default function() {}",
        "%s.exec('target');" % regex_lit(rng),
    ]


def sh_known_bracket_after_operand(rng):
    # Known residual family K2.
    return [
        "var v = compute()",
        "['a', 'b'].forEach(function(x) { return x; });",
        "function compute() { return 1; }",
    ]


SHAPE_BUILDERS_SCRIPT = [
    ("asi-paren", sh_asi_paren),
    ("asi-bracket", sh_asi_bracket),
    ("asi-regex", sh_asi_regex),
    ("asi-pm", sh_asi_pm),
    ("asi-backtick", sh_asi_backtick),
    ("asi-return", sh_asi_return),
    ("asi-postfix", sh_asi_postfix),
    ("asi-let", sh_asi_let),
    ("do-while-asi", sh_do_while),
    ("nullish", sh_nullish),
    ("optional-chain", sh_optional_chain),
    ("destructure", sh_destructure),
    ("spread", sh_spread),
    ("template-nested", sh_template_nested),
    ("generators", sh_generators),
    ("async", sh_async),
    ("labels", sh_labels),
    ("getset", sh_getset),
    ("class-known-K4", sh_class_known),
    ("regex-positions", sh_regex_positions),
    ("comment-torture", sh_comment_torture),
    ("sgml-comments", sh_sgml_comments),
    ("unicode", sh_unicode),
    ("numbers", sh_numbers),
    ("ternary-regex", sh_ternary_regex_mix),
    ("control-flow", sh_control_flow),
    ("iife-arrow", sh_iife_arrow),
    ("sloppy", sh_eval_with_sloppy),
    ("try-finally", sh_try_finally),
    ("known-K2-bracket", sh_known_bracket_after_operand),
]

SHAPE_BUILDERS_MODULE = [
    ("module-forms", sh_module_forms),
    ("dynamic-import", sh_dynamic_import),
    ("known-K1-import-regex", sh_known_import_regex),
    ("known-K3-export-default-regex", sh_known_export_default_regex),
]


# ---------------------------------------------------------------------------
# Executable self-checking probes. Each returns (lines, names) where the
# program prints one RESULT line. The oracle compares stdout of original vs
# minified, so every line-break-sensitive shape below is a live semantic trap.
# ---------------------------------------------------------------------------

def ex_asi_break(rng):
    # break<newline>label: ASI must fire (restricted production), giving an
    # unlabeled break of the inner loop. Joining yields `break outer;`,
    # which breaks the outer loop instead -- observable in `hits`.
    # `var outer` keeps the ASI parse of `outer;` a harmless expression
    # (labels and variables live in separate namespaces).
    return [
        "var hits = 0;",
        "var outer = 5;",
        "outer: for (var i = 0; i < 3; i++) {",
        "  for (var j = 0; j < 3; j++) {",
        "    if (j === 1) break",
        "    outer;",
        "    hits++;",
        "  }",
        "}",
        "console.log('RESULT', hits);",
    ]


def ex_asi_index(rng):
    return [
        "var arr = [10, 20, 30];",
        "var picked = arr",
        "[1];",
        "console.log('RESULT', picked);",
    ]


def ex_asi_regex_div(rng):
    # return<newline>/regex: restricted production, so ASI returns undefined
    # and the regex is a separate statement. Joining makes f() return the
    # regex -- observable.
    return [
        "function f() {",
        "  return",
        "  /2/g;",
        "}",
        "var z = f();",
        "var kind = (z instanceof RegExp) ? 're' : String(z);",
        "console.log('RESULT', kind);",
    ]


def ex_asi_pm(rng):
    return [
        "var a = 5",
        "-2;",
        "var b = 5",
        "+3;",
        "console.log('RESULT', a, b);",
    ]


def ex_asi_tagged(rng):
    return [
        "var calls = 0;",
        "function tag(s) { calls++; return 'tagged'; }",
        "var v = tag",
        "`hello`;",
        "console.log('RESULT', v, calls);",
    ]


def ex_asi_return(rng):
    return [
        "function f() {",
        "  return",
        "  42;",
        "}",
        "console.log('RESULT', String(f()));",
    ]


def ex_postfix(rng):
    # i<newline>++<newline>j parses as `i; ++j;` -> i stays 1, j becomes 11.
    return [
        "var i = 1, j = 10;",
        "i",
        "++",
        "j;",
        "console.log('RESULT', i, j);",
    ]


def ex_nullish_chain(rng):
    return [
        "var cfg = { a: null, b: 0, c: { d: 7 } };",
        "var out = [cfg.a ?? 1, cfg.b ?? 2, cfg?.c?.d ?? 3, cfg?.x?.y ?? 4];",
        "cfg.a ??= 5; cfg.b ??= 6;",
        "console.log('RESULT', out.join(','), cfg.a, cfg.b);",
    ]


def ex_spread_destructure(rng):
    return [
        "var arr = [1, 2, 3];",
        "var [x, ...rest] = [...arr, 4];",
        "var { a = 9, b: { c = 8 } = {} } = { b: {} };",
        "function sum() { return [].reduce.call(arguments,",
        "  function(p, v) { return p + v; }, 0); }",
        "console.log('RESULT', x, rest.length, a, c, sum(...arr));",
    ]


def ex_template(rng):
    return [
        "var n = 5, inner = 'IN';",
        "var t = `o${`m${inner}${n + 1}`}d`;",
        "var u = `${n > 3 ? `B${n}` : 's'}`;",
        "console.log('RESULT', t, u);",
    ]


def ex_loops_labels(rng):
    return [
        "var hits = 0;",
        "outer: for (var i = 0; i < 4; i++) {",
        "  for (var j = 0; j < 4; j++) {",
        "    if (j > i) continue outer;",
        "    hits++;",
        "  }",
        "}",
        "do hits++",
        "while (hits < 12);",
        "console.log('RESULT', hits);",
    ]


def ex_regex_positions(rng):
    return [
        "var out = [];",
        "out.push(/a+/.test('caa'));",
        "if (true) { out.push(/b/.source); }",
        "var div = 100 / 2 / 5;",
        "out.push(div);",
        "out.push(typeof 'x' === 'string' ? /st/.source : 'no');",
        "console.log('RESULT', out.join('|'));",
    ]


def ex_getset(rng):
    return [
        "var log = [];",
        "var o = { _v: 3,",
        "  get v() { log.push('g'); return this._v; },",
        "  set v(x) { log.push('s'); this._v = x * 2; } };",
        "o.v = 4;",
        "log.push(o.v);",
        "console.log('RESULT', log.join(','));",
    ]


def ex_unicode(rng):
    return [
        "var 變量 = 3;",
        "var café = 4;",
        "變量 += café;",
        "console.log('RESULT', 變量, '\\u00e9');",
    ]


def ex_arrow_asi(rng):
    return [
        "var mk = () =>",
        "  ({ v: 11 });",
        "var seq = [1, 2, 3].map(function(n) {",
        "  return n * 2;",
        "})",
        ".filter(function(n) { return n > 2; });",
        "console.log('RESULT', mk().v, seq.join(','));",
    ]


def ex_dynamic_import(rng):
    return [
        "var url = 'data:text/javascript,export default 7';",
        "var m = await import(url);",
        "console.log('RESULT', m.default, import.meta.url.length > 0);",
    ]


EXEC_BUILDERS_SCRIPT = [
    ("asi-break", ex_asi_break),
    ("asi-index", ex_asi_index),
    ("asi-regex-div", ex_asi_regex_div),
    ("asi-pm", ex_asi_pm),
    ("asi-tagged", ex_asi_tagged),
    ("asi-return", ex_asi_return),
    ("postfix", ex_postfix),
    ("nullish", ex_nullish_chain),
    ("spread-destructure", ex_spread_destructure),
    ("template", ex_template),
    ("loops-labels", ex_loops_labels),
    ("regex-positions", ex_regex_positions),
    ("getset", ex_getset),
    ("unicode", ex_unicode),
    ("arrow-asi", ex_arrow_asi),
]

EXEC_BUILDERS_MODULE = [
    ("dynamic-import", ex_dynamic_import),
]


# ---------------------------------------------------------------------------
# Composite / scale specials
# ---------------------------------------------------------------------------

def build_flat(rng, n_lines):
    lines = []
    for k in range(n_lines):
        lines.append("var v%d = %s;" % (k, expr(rng, 1)))
    return lines


def build_deep(rng, depth):
    # Deeply nested but node-parseable: parens + ternaries + arrays.
    # Keep the accumulator linear (never embed `inner` twice per step).
    inner = "1"
    for k in range(depth):
        style = k % 3
        if style == 0:
            inner = "(%s ? 1 : 0)" % inner
        elif style == 1:
            inner = "[%s]" % inner
        else:
            inner = "((%s) + 0)" % inner
    return ["var deep = %s;" % inner]


def build_mix(rng, n_blocks):
    """Compose several random shape blocks into one file."""
    # Families with fixed top-level names (class, labels) would collide when
    # picked twice in one file; they are covered standalone instead.
    pool = [(n, f) for n, f in SHAPE_BUILDERS_SCRIPT
            if n not in ("class-known-K4", "labels")]
    lines = []
    fams = []
    for _ in range(n_blocks):
        name, fn = pick(rng, pool)
        fams.append(name)
        lines.append("")
        lines.extend(fn(rng))
    return lines, fams


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

HEADER = "// synthetic minifier probe (seed %s, file %d) [%s]"


def wrap(lines, seed, idx, tag, crlf=False, bom=False, shebang=False):
    body = "\n".join([HEADER % (seed, idx, tag)] + lines) + "\n"
    if shebang:
        body = "#!/usr/bin/env node\n" + body
    if crlf:
        body = body.replace("\n", "\r\n")
    data = body.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    return data


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--seed", type=int, default=DEFAULT_SEED)
    p.add_argument("--dest", required=True)
    p.add_argument("--shapes", type=int, default=150,
                   help="number of single/mixed shape files")
    p.add_argument("--execs", type=int, default=60,
                   help="number of executable probe files")
    args = p.parse_args(argv)

    os.makedirs(args.dest, exist_ok=True)
    index = []
    idx = 0

    def emit(name, data, kind, module, executable, families):
        nonlocal idx
        idx += 1
        path = os.path.join(args.dest, name)
        with open(path, "wb") as f:
            f.write(data)
        index.append({
            "file": name,
            "kind": kind,
            "module": module,
            "executable": executable,
            "families": families,
            "bytes": len(data),
        })

    # 1) Single-family shape files, cycling through all families.
    script_cycle = (args.shapes * 3) // 5
    module_cycle = args.shapes - script_cycle
    for k in range(script_cycle):
        rng = random.Random("%d:shape-s:%d" % (args.seed, k))
        name, fn = SHAPE_BUILDERS_SCRIPT[k % len(SHAPE_BUILDERS_SCRIPT)]
        crlf = (k % 17 == 16)
        bom = (k % 23 == 22)
        emit("shape_s%03d.js" % k,
             wrap(fn(rng), args.seed, idx, name, crlf=crlf, bom=bom),
             "shape", False, False, [name])
    for k in range(module_cycle):
        rng = random.Random("%d:shape-m:%d" % (args.seed, k))
        name, fn = SHAPE_BUILDERS_MODULE[k % len(SHAPE_BUILDERS_MODULE)]
        emit("shape_m%03d.mjs" % k,
             wrap(fn(rng), args.seed, idx, name),
             "shape", True, False, [name])

    # 2) Mixed composition files.
    for k in range(40):
        rng = random.Random("%d:mix:%d" % (args.seed, k))
        lines, fams = build_mix(rng, 2 + ri(rng, 5))
        emit("mix_%03d.js" % k, wrap(lines, args.seed, idx, "+".join(fams)),
             "mix", False, False, fams)

    # 3) Executable probes.
    for k in range(args.execs):
        rng = random.Random("%d:exec:%d" % (args.seed, k))
        if k % 8 == 7:
            name, fn = EXEC_BUILDERS_MODULE[k % len(EXEC_BUILDERS_MODULE)]
            emit("exec_%03d.mjs" % k, wrap(fn(rng), args.seed, idx, name),
                 "exec", True, True, [name])
        else:
            name, fn = EXEC_BUILDERS_SCRIPT[k % len(EXEC_BUILDERS_SCRIPT)]
            shebang = (k % 31 == 30)
            emit("exec_%03d.js" % k,
                 wrap(fn(rng), args.seed, idx, name, shebang=shebang),
                 "exec", False, True, [name])

    # 4) Scale specials: giant flat files and deep nesting.
    for k, n_lines in enumerate([500, 2000, 8000]):
        rng = random.Random("%d:flat:%d" % (args.seed, k))
        emit("flat_%d.js" % k,
             wrap(build_flat(rng, n_lines), args.seed, idx, "flat-%d" % n_lines),
             "flat", False, False, ["flat"])
    for k, depth in enumerate([100, 500, 900]):
        rng = random.Random("%d:deep:%d" % (args.seed, k))
        emit("deep_%d.js" % k,
             wrap(build_deep(rng, depth), args.seed, idx, "deep-%d" % depth),
             "deep", False, False, ["deep"])

    with open(os.path.join(args.dest, "index.json"), "w") as f:
        json.dump({"seed": args.seed, "files": index}, f, indent=1)
        f.write("\n")
    total = sum(e["bytes"] for e in index)
    print("generated %d files (%d bytes) seed=%d -> %s"
          % (len(index), total, args.seed, args.dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
