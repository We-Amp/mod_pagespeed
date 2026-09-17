// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Webpack fixture builds for the js-minify corpus. Three production-mode
// shapes, all deterministic (fixed filenames, no content hashing):
//
//   webpack-unminified.js       runtime boilerplate + eval-free module
//                               wrappers, unminified (module-path comments).
//   webpack-minified-banner.js  terser-minified bundle + multi-line legal
//                               banner (comments kept IN the bundle).
//   webpack-sourcemap.js        unminified bundle with a trailing
//                               `//# sourceMappingURL=` comment (+ .map).
//
// Each build also emits its lazy chunk (dynamic import in src/entry.js),
// which carries webpack's chunk-loading runtime shape.
//
// Output directory comes from JSM_BUNDLE_DIR (set by build_bundles.py);
// defaults to ./dist for ad-hoc local runs. Nothing emitted is committed.
const path = require("path");
const webpack = require("webpack");
const TerserPlugin = require("terser-webpack-plugin");

const DEST = process.env.JSM_BUNDLE_DIR || path.resolve(__dirname, "dist");

const LEGAL = [
  "js-minify-corpus bundle fixture (webpack)",
  "Copyright (c) 2026 We-Amp B.V. Synthetic fixture, not a product build.",
  "Bundled from first-party sources in tools/js-minify-corpus/bundle-fixtures/src/.",
].join("\n");

const base = {
  mode: "production",
  context: __dirname,
  entry: "./src/entry.js",
  output: { path: DEST, clean: false },
};

module.exports = [
  {
    ...base,
    name: "webpack-unminified",
    devtool: false,
    optimization: { minimize: false },
    output: {
      ...base.output,
      filename: "webpack-unminified.js",
      chunkFilename: "webpack-unminified.lazy-chunk.js",
    },
  },
  {
    ...base,
    name: "webpack-minified-banner",
    devtool: false,
    optimization: {
      minimize: true,
      minimizer: [
        new TerserPlugin({
          extractComments: false,
          terserOptions: { format: { comments: false } },
        }),
      ],
    },
    plugins: [
      // BannerPlugin normally runs BEFORE terser (PROCESS_ASSETS_STAGE_ADDITIONS
      // precedes OPTIMIZE_SIZE), so a comments:false terser would strip it again.
      // Pin it to the final REPORT stage so the legal banner survives minification.
      new webpack.BannerPlugin({
        banner: LEGAL,
        stage: webpack.Compilation.PROCESS_ASSETS_STAGE_REPORT,
      }),
    ],
    output: {
      ...base.output,
      filename: "webpack-minified-banner.js",
      chunkFilename: "webpack-minified-banner.lazy-chunk.js",
    },
  },
  {
    ...base,
    name: "webpack-sourcemap",
    devtool: "source-map",
    optimization: { minimize: false },
    output: {
      ...base.output,
      filename: "webpack-sourcemap.js",
      chunkFilename: "webpack-sourcemap.lazy-chunk.js",
    },
  },
];
