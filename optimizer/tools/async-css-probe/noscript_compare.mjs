// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Does the <noscript> removal make a JS-off render show what a
// scripting browser shows? For every case below, the page is rendered as is
// with JavaScript on, and with JavaScript off after strip_noscript has removed
// its <noscript> elements; the hero's box and the body text must match.
// The cases are review cases (c01-c24, n01-n21) and more (e01-e26,
// m01-m20); test/src/worker/noscript_strip_test.cc pins their outputs. A
// case whose removal the product refuses (unreliable, or disagreeing with
// the HtmlScanner) is reported as refused: it fails closed and is never
// rendered.
//
// One known difference, by design: c05's script writes an unclosed
// <noscript> with document.write, which blanks the page with JavaScript on.
// Neither analysis render that gets the stripped document runs scripts.
//
//   bazel build //tools/async-css-probe:strip_noscript
//   npm install --no-save playwright@1.58.2
//   node tools/async-css-probe/noscript_compare.mjs \
//       bazel-bin/tools/async-css-probe/strip_noscript
//   # The product's Chromium instead of Playwright's: CHROME=/usr/bin/chromium

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { chromium } from "playwright";

const HEAD =
  "<!doctype html><html><head><title>T</title>" +
  "<style>.hero{height:200px;background:red}</style></head><body>";
const TAIL = "<div class=hero>HERO</div><p>AFTER</p></body></html>";
const EXPECTED_TO_DIFFER = new Set(["c05_dblescape_script_unclosed_ns"]);

// [name, markup between HEAD and TAIL] or [name, whole page, true].
const CASES = [
  ["c01_baseline", "<noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["c02_sq_attr_noscript_eof", "<noscript class=it's>NOJS</noscript>"],
  ["c03_sq_attr_noscript_later", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><noscript class=it's>NOJS</noscript><div class=hero>HERO</div><p>Don't</p><p>AFTER</p></body></html>", /*whole page*/ true],
  ["c04_doubled_quote_noscript", "<noscript data-a=\"x\"\">NOJS</noscript>"],
  ["c05_dblescape_script_unclosed_ns", "<script><!--\ndocument.write('<script src=a.js></script>');\ndocument.write('<noscript>');\n//--></script>"],
  ["c06_dblescape_script_closed_ns", "<script><!--\ndocument.write('<script src=a.js></script>');\ndocument.write('<noscript><img src=p.gif></noscript>');\n//--></script>"],
  ["c07_bare_comment", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><!--><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript><div class=hero>HERO</div><p>AFTER</p><!-- c --></body></html>", /*whole page*/ true],
  ["c08_dash3_comment", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><!---><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript><div class=hero>HERO</div><p>AFTER</p><!-- c --></body></html>", /*whole page*/ true],
  ["c09_bang_comment_end", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><!-- a --!><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript><div class=hero>HERO</div><p>AFTER</p><!-- c --></body></html>", /*whole page*/ true],
  ["c10_svg_breakout", "<svg width=10 height=10><p>x</p><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></svg>"],
  ["c11_svg_unclosed", "<div><svg width=10 height=10></div><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["c12_foreignobject", "<svg width=100 height=100><foreignObject width=100 height=100><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></foreignObject></svg>"],
  ["c13_desync_into_script", "<p title=it's>Hi</p><script>// don't\nif (a > b) { el.innerHTML = \"<noscript>\"; }</script>"],
  ["c14_script_attr_endtag", "<script data-x=\"</script>\">var a=1;</script><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["c15_select_noscript", "<select><option>a</option><noscript></select><div>X</div></noscript></select>"],
  ["c16_math_annotation", "<math><annotation-xml encoding=\"text/html\"><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></annotation-xml></math>"],
  ["c17_svg_title_rcdata", "<svg width=10 height=10><title><p>x</p></title></svg><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["c18_svg_unquoted_slash", "<svg class=a/><noscript/><rect/></svg>"],
  ["c19_template", "<template><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></template>"],
  ["c20_unquoted_img_apos", "<img alt=Bob's src=x.png><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["c21_gtm", "<noscript><iframe src=\"https://www.googletagmanager.com/ns.html?id=GTM-X\" height=\"0\" width=\"0\" style=\"display:none;visibility:hidden\"></iframe></noscript>"],
  ["c22_dblescape_regex", "<!doctype html><html><head><title>T</title></head><body><script><!--\ndocument.write('<script src=x.js></script>');\nvar tag = \"<noscript>\";\n//--></script><div class=hero>HERO</div><p>AFTER</p></body></html>", /*whole page*/ true],
  ["c23_val_doubled_quote", "<!doctype html><html><head><title>T</title><link rel=stylesheet href=/app.css></head><body><noscript data-a=\"x\"\">NOJS</noscript><div class=hero>HERO</div><p>AFTER</p></body></html>", /*whole page*/ true],
  ["c24_val_desync", "<!doctype html><html><head><title>T</title><link rel=stylesheet href=/app.css></head><body><p title=it's>Hi</p><script>// don't\nif (a > b) { el.innerHTML = \"<noscript>\"; }</script><div class=hero>HERO</div><p>AFTER</p></body></html>", /*whole page*/ true],
  ["e01_nested", "<noscript>a<noscript>b</noscript>c</noscript>"],
  ["e02_unclosed_text", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><div class=hero>HERO</div><p>AFTER</p></body></html><noscript>tail text only", /*whole page*/ true],
  ["e03_upper", "<NOSCRIPT><DIV>NOJS</DIV></NoScript>"],
  ["e04_selfclose", "<noscript/>NOJS</noscript>"],
  ["e05_style_in_ns", "<noscript><style>p{}</noscript>X</style>"],
  ["e06_comment_in_ns", "<noscript><!-- </noscript> -->"],
  ["e07_attr_in_ns", "<noscript><img alt=\"</noscript>\">"],
  ["e08_math_mi", "<math><mi><noscript><b>NOJS</b></noscript></mi></math>"],
  ["e09_font_breakout", "<svg><font color=red>F</font><noscript>NOJS</noscript></svg>"],
  ["e10_div_closes_svg", "<div><svg></div><noscript><b>NOJS</b></noscript>"],
  ["e11_cdata", "<svg><![CDATA[<noscript>]]></svg><noscript>NOJS</noscript>"],
  ["e12_svg_style", "<svg><style><noscript></style></svg>"],
  ["e13_endtag_quote", "<script>var a=1;</script x=\">\"><noscript>NOJS</noscript>"],
  ["e14_escape_reset", "<script><!-- x --></script><noscript>NOJS</noscript>"],
  ["e15_table", "<table><tr><td>c</td></tr><noscript><tr><td>NOJS</td></tr></noscript></table>"],
  ["e16_head", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style><noscript><link rel=stylesheet href=n.css><style>.hero{display:none}</style><meta http-equiv=refresh content=\"9999;url=x\"></noscript></head><body><div class=hero>HERO</div><p>AFTER</p></body></html>", /*whole page*/ true],
  ["e17_text_banner", "<noscript>Please enable JavaScript</noscript>"],
  ["e18_dash3", "<!---><noscript>NOJS</noscript>"],
  ["e19_svg_title_noscript", "<svg><title><noscript>NOJS</noscript></title></svg>"],
  ["e20_svg_desc_breakout", "<svg><desc><span>d</span></desc><noscript>NOJS</noscript></svg>"],
  ["e21_math_svg_annot", "<math><annotation-xml><svg><foreignObject><noscript>NOJS</noscript></foreignObject></svg></annotation-xml></math>"],
  ["e22_textarea", "<textarea><noscript></textarea>"],
  ["e23_bogus", "<?php <noscript> ?><noscript>NOJS</noscript>"],
  ["e24_double_escape_nested", "<script><!--<script></script>--></script><noscript>NOJS</noscript>"],
  ["e25_li", "<ul><li>a<li>b<noscript><li>NOJS</noscript></ul>"],
  ["e26_p_svg", "<p><svg><p>x</p><noscript>NOJS</noscript></svg>"],
  ["n01_span_special_svg", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><span><div><svg width=10 height=10></span><noscript/></svg><div class=f0>FOLD0</div><div class=f1>FOLD1</div><div class=f2>FOLD2</div><div class=hero>HERO</div><p>AFTER</p><footer><noscript><img src=p.gif width=1 height=1></noscript></footer></body></html>", /*whole page*/ true],
  ["n03_heading_endtag", "<h2><svg width=10 height=10></h1><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n04_font_color_breakout", "<svg width=10 height=10><font color=red>f</font><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></svg>"],
  ["n05_font_plain_no_breakout", "<svg width=10 height=10><font>f</font><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></svg>"],
  ["n06_svg_endp", "<svg width=10 height=10></p><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n07_select_svg", "<select><svg><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></svg></select>"],
  ["n08_dblescape_ok", "<script><!--<script></script>a<noscript>x</noscript>--></script><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n09_cdata_html", "<![CDATA[ x > <noscript><style>.hero{display:none}</style><div>NOJS</div></noscript> ]]>"],
  ["n10_svg_style_endsvg", "<svg width=10 height=10><style></svg><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n11_dsd_template", "<div><template shadowrootmode=open><noscript><b>N</b></noscript><p>SHADOW</p></template></div>"],
  ["n12_mi_noscript", "<math><mi><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></mi></math>"],
  ["n13_svg_desc_style", "<svg width=10 height=10><desc><style>.hero{outline:1px solid}</style></desc></svg><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n14_meta_refresh", "<noscript><meta http-equiv=refresh content=\"0;url=/nojs\"></noscript>"],
  ["n15_ul_fo", "<ul><li><svg width=10 height=10><foreignObject></ul><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript></foreignObject></svg>"],
  ["n16_table_noscript", "<table><tr><td>c</td></tr><noscript>NOJSTABLE</noscript></table>"],
  ["n17_a_misnest_svg", "<a href=#><div><svg width=10 height=10></a><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n18_unclosed_noscript_text_only", "<!doctype html><html><head><title>T</title><style>.hero{height:200px;background:red}</style></head><body><div class=hero>HERO</div><p>AFTER</p><noscript>bye", /*whole page*/ true],
  ["n19_comment_short", "<!--><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript><!---><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["n20_template_noscript_svg", "<template><svg><noscript></template><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m01_span_svg", "<span><svg width=10 height=10></span><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m02_div_span_svg", "<div><span><svg width=10 height=10></div><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m03_p_svg", "<p><svg width=10 height=10></p><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m04_li_svg", "<ul><li><svg width=10 height=10></li></ul><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m05_td_svg", "<table><tr><td><svg width=10 height=10></td></tr></table><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m06_button_svg", "<button><svg width=10 height=10></button><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m07_em_div_span_svg", "<em><div><span><svg width=10 height=10></em><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m08_h1_h3", "<h1><svg width=10 height=10></h3><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m09_section", "<section><svg width=10 height=10></section><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m10_b_p_svg", "<b><p><svg width=10 height=10></b><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m11_span_div_svg_div", "<span><div><svg width=10 height=10></div></span><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m12_font_svg", "<font><svg width=10 height=10></font><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m13_object_svg", "<object><svg width=10 height=10></object><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m14_a_b_svg", "<a href=#><b><svg width=10 height=10></a><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m15_custom", "<my-el><svg width=10 height=10></my-el><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m16_li_div_svg", "<ul><li><div><svg width=10 height=10></li></ul><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m17_dd_svg", "<dl><dd><svg width=10 height=10></dd></dl><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m18_form_svg", "<form><svg width=10 height=10></form><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m19_a_div_div_svg", "<a href=#><div><div><svg width=10 height=10></a><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
  ["m20_math_mi_div", "<math><mi><div><svg width=10 height=10></mi></math><noscript><style>.hero{display:none}</style><div>NOJS</div></noscript>"],
];

// Review cases n02 and n21: a misnested <svg> before the fold,
// then hundreds of spans. A removal that misread the misnesting took the
// fold out and stayed within the old count tolerance.
const spans = (sep) =>
  Array.from({ length: 1000 }, (_, i) => `<span>x${i}</span>`).join(sep);
CASES.push([
  "n02_span_special_svg_big",
  "<span><div><svg width=10 height=10></span><noscript/></svg>" +
    "<div class=f0>FOLD0</div><div class=f1>FOLD1</div>" +
    "<div class=f2>FOLD2</div><div class=hero>HERO</div><p>AFTER</p>" +
    "<footer><noscript><img src=p.gif width=1 height=1></noscript>" +
    spans("") + "</footer></body></html>",
  true,
].map((v, i) => (i === 1 ? HEAD + v : v)));
CASES.push([
  "n21_small_misstrip_big_page",
  "<!doctype html><html><head><title>T</title>" +
    "<link rel=stylesheet href=/app.css></head><body>" +
    "<span><div><svg width=10 height=10></span><noscript/></svg>" +
    "<div class=f0>FOLD0</div><div class=f1>FOLD1</div>" +
    "<div class=f2>FOLD2</div><div class=hero>HERO</div>" +
    "<noscript><img src=p.gif width=1 height=1></noscript>" +
    spans(" ") + "</body></html>",
  true,
]);

const strip = process.argv[2];
if (!strip) {
  console.error(
    "usage: noscript_compare.mjs <strip_noscript binary> [case dir]");
  process.exit(2);
}
// More cases: every *.html page in an optional directory (the reviews' case
// directories, a set of fetched sites), compared the same way.
const extraDir = process.argv[3];
if (extraDir) {
  for (const f of fs.readdirSync(extraDir).filter((n) => n.endsWith(".html"))) {
    CASES.push([
      `extra_${f.slice(0, -5)}`,
      fs.readFileSync(path.join(extraDir, f), "utf8"),
      true,
    ]);
  }
}
const dir = fs.mkdtempSync(path.join(os.tmpdir(), "noscript-compare-"));
const inDir = path.join(dir, "in");
const outDir = path.join(dir, "out");
fs.mkdirSync(inDir);
fs.mkdirSync(outDir);
for (const [name, markup, whole] of CASES) {
  fs.writeFileSync(path.join(inDir, `${name}.html`),
                   whole ? markup : HEAD + markup + TAIL);
}
// A removal the product would refuse to render (unreliable, or not agreeing
// with the HtmlScanner) is fail-closed, not compared: nothing is rendered.
const report = execFileSync(strip, [inDir, outDir]).toString();
process.stdout.write(report);
const refused = new Set();
for (const line of report.split("\n")) {
  const m = line.match(/^(\S+)\.html removed=(\d+) reliable=(\d) agrees=(\d)/);
  if (m && (m[3] === "0" || (m[2] !== "0" && m[4] === "0"))) refused.add(m[1]);
}

const browser = await chromium.launch({
  executablePath: process.env.CHROME || undefined,
});
console.log("browser", browser.version());
const viewport = { width: 375, height: 667 };
const on = await browser.newContext({ javaScriptEnabled: true, viewport });
const off = await browser.newContext({ javaScriptEnabled: false, viewport });
const probe = () => {
  const hero = document.querySelector(".hero");
  const r = hero ? hero.getBoundingClientRect() : null;
  return {
    text: document.body
      ? document.body.innerText.replace(/\s+/g, " ").trim()
      : "<no body>",
    hero: r ? `${Math.round(r.y)}/${Math.round(r.height)}` : "none",
  };
};
let failures = 0;
for (const [name] of CASES) {
  if (refused.has(name)) {
    console.log(`ok   refused ${name}  (fails closed: not rendered)`);
    continue;
  }
  const a = await on.newPage();
  await a.goto(`file://${path.join(inDir, name + ".html")}`);
  const scripting = await a.evaluate(probe);
  const b = await off.newPage();
  await b.goto(`file://${path.join(outDir, name + ".html")}`);
  const stripped = await b.evaluate(probe);
  await a.close();
  await b.close();
  const same =
    scripting.text === stripped.text && scripting.hero === stripped.hero;
  const expected =
      same !== EXPECTED_TO_DIFFER.has(name.replace(/^extra_/, ""));
  if (!expected) ++failures;
  console.log(
    `${expected ? "ok  " : "FAIL"} ${same ? "same" : "diff"} ${name}  ` +
      `JS on: ${scripting.hero} "${scripting.text}"  ` +
      `JS off, stripped: ${stripped.hero} "${stripped.text}"`,
  );
}
await browser.close();
fs.rmSync(dir, { recursive: true, force: true });
console.log(failures === 0 ? "all cases as expected" : `${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
