// Rollup fixture builds for the js-minify corpus:
//
//   rollup-iife-banner.js  single-scope IIFE bundle + legal banner comment.
//   rollup-esm.js          bundled ES module output (module-goal corpus file).
//
// Deterministic (fixed output files, no hashing). Output directory comes from
// JSM_BUNDLE_DIR (set by build_bundles.py). Nothing emitted is committed.
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DEST = process.env.JSM_BUNDLE_DIR || path.join(HERE, "dist");

const LEGAL = [
  "js-minify-corpus bundle fixture (rollup)",
  "Copyright (c) 2026 We-Amp B.V. Synthetic fixture, not a product build.",
  "Bundled from first-party sources in tools/js-minify-corpus/bundle-fixtures/src/.",
].join("\n");

export default [
  {
    input: path.join(HERE, "src", "entry.js"),
    output: {
      file: path.join(DEST, "rollup-iife-banner.js"),
      format: "iife",
      name: "JsMinifyCorpusFixture",
      banner: `/*\n${LEGAL}\n*/`,
      inlineDynamicImports: true,
    },
  },
  {
    input: path.join(HERE, "src", "entry.js"),
    output: {
      file: path.join(DEST, "rollup-esm.js"),
      format: "es",
      inlineDynamicImports: true,
    },
  },
];
