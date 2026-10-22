// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// esbuild configuration for the rewriter client JS assets. Called by
// tools/regenerate-rewriter-js.sh; not meant to be run on its own.
//
//   node tools/js/build-rewriter-js.mjs <src.js> <out_dir> <name>
//
// Writes <out_dir>/<name>_dbg.js (bundled, readable) and
// <out_dir>/<name>_opt.js (bundled, minified). Each asset is one IIFE with
// no comments; public entry points are the explicit window.pagespeed.*
// assignments in the source. The build fails on any esbuild warning or
// error, which covers the syntax esbuild reports as unavailable in ES2015
// (BigInt literals, for example). Regular expressions are the exception:
// esbuild rewrites a literal it cannot lower (lookbehind, the s flag,
// \p{...}) into a `new RegExp(...)` call that throws at run time on an
// ES2015 engine, without a warning, so the build also fails when the output
// has a `new RegExp(` call that none of the inputs has.

import { build } from 'esbuild';
import { readFileSync, writeFileSync } from 'node:fs';
import { basename, dirname, join } from 'node:path';

const [src, outDir, name] = process.argv.slice(2);
if (!src || !outDir || !name) {
  console.error('usage: build-rewriter-js.mjs <src.js> <out_dir> <name>');
  process.exit(2);
}

const common = {
  entryPoints: [basename(src)],
  absWorkingDir: dirname(src),
  bundle: true,
  format: 'iife',
  platform: 'browser',
  target: 'es2015',
  charset: 'ascii',
  legalComments: 'none',
  write: false,
  logLevel: 'silent',
  metafile: true,
};

async function compile(minify) {
  const result = await build({ ...common, minify });
  if (result.warnings.length > 0) {
    throw new Error(`${src}: esbuild warnings:\n` +
        result.warnings.map((w) => w.text).join('\n'));
  }
  let code = result.outputFiles[0].text;
  const inputs = Object.keys(result.metafile.inputs).map(
      (f) => readFileSync(join(common.absWorkingDir, f), 'utf8')).join('\n');
  const regExpCalls = (text) => (text.match(/new RegExp\(/g) || []).length;
  if (regExpCalls(code) > regExpCalls(inputs)) {
    throw new Error(`${src}: a regular expression in ${name} cannot be ` +
        'expressed in ES2015 (esbuild rewrote it into new RegExp)');
  }
  // A non-minified bundle labels each module with a "// <path>" line and
  // marks constructor calls it treats as side-effect free with
  // /* @__PURE__ */; the shipped assets are kept comment-free.
  code = code.replace(/^ *\/\/ [^\n]*\.m?js\n/gm, '')
      .replace(/\/\* @__PURE__ \*\/ /g, '');
  if (/\/\*|^\s*\/\//m.test(code)) {
    throw new Error(`${src}: output for ${name} still contains a comment`);
  }
  return code;
}

try {
  writeFileSync(join(outDir, `${name}_dbg.js`), await compile(false));
  writeFileSync(join(outDir, `${name}_opt.js`), await compile(true));
} catch (e) {
  console.error(e.message || e);
  process.exit(1);
}
