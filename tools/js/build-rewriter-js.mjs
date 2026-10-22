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
// assignments in the source. The build fails on any warning and on any
// syntax esbuild cannot lower to ES2015, so the browser floor cannot move
// silently.

import { build } from 'esbuild';
import { writeFileSync } from 'node:fs';
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
};

async function compile(minify) {
  const result = await build({ ...common, minify });
  if (result.warnings.length > 0) {
    throw new Error(`${src}: esbuild warnings:\n` +
        result.warnings.map((w) => w.text).join('\n'));
  }
  let code = result.outputFiles[0].text;
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
