// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Fixture entry for the js-minify corpus bundle builds. First-party toy code
// that deliberately exercises a module graph with a few modern-syntax shapes
// so the bundled output carries them into the corpus: arrow functions,
// template literals, optional chaining, nullish coalescing, spread, and a
// lazy dynamic-import chunk.
import { clamp, movingAverage } from "./math.js";
import { shout, slugify } from "./strings.js";
import { createPalette, DEFAULTS } from "./palette.js";

const samples = [12, 7, 42, 3.14, 100];
const smoothed = movingAverage(samples, 3).map((v) => clamp(v, 0, 50));
const palette = createPalette(DEFAULTS.accent);

export function describe() {
  const label = slugify(shout(`Smoothed ${smoothed.length} samples`));
  return `${label}:${palette.mix("#003399")?.hex ?? "none"}`;
}

console.log(describe());

// Lazy chunk: webpack emits a separate chunk + loading runtime for this;
// rollup/esbuild inline it under the single-file configs in this package.
import("./lazy.js").then((m) => console.log(m.lazySummary()));
