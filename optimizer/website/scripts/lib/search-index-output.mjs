// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Check what Pagefind wrote. The search dialog loads these files by name and
 * cannot tell an empty one from a good one: an empty stylesheet "loads" and
 * leaves the dialog unstyled, an empty script or index leaves it without
 * search. `findBadSearchOutput(dir)` returns one line per problem (an empty
 * array when the output is complete), so the build can stop instead of
 * shipping it.
 */
import { existsSync, readdirSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

/** The fixed names the dialog and Pagefind's own loader fetch. */
export const REQUIRED_FILES = [
  'pagefind.js',
  'pagefind-ui.js',
  'pagefind-ui.css',
  'pagefind-entry.json',
  'pagefind-worker.js',
];

/** Hashed or per-language names: at least one of each must exist. */
const REQUIRED_KINDS = [
  { dir: '', pattern: /^wasm\..+\.pagefind$/, label: 'wasm.*.pagefind' },
  { dir: '', pattern: /\.pf_meta$/, label: '*.pf_meta' },
  { dir: 'index', pattern: /\.pf_index$/, label: 'index/*.pf_index' },
  { dir: 'fragment', pattern: /\.pf_fragment$/, label: 'fragment/*.pf_fragment' },
];

function walk(dir) {
  const files = [];
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) files.push(...walk(full));
    else files.push(full);
  }
  return files;
}

export function findBadSearchOutput(outputPath) {
  if (!existsSync(outputPath)) return [`${outputPath}: missing`];
  const bad = [];
  for (const name of REQUIRED_FILES) {
    if (!existsSync(join(outputPath, name))) bad.push(`${name}: missing`);
  }
  for (const { dir, pattern, label } of REQUIRED_KINDS) {
    const where = join(outputPath, dir);
    const names = existsSync(where) ? readdirSync(where) : [];
    if (!names.some((n) => pattern.test(n))) bad.push(`${label}: none written`);
  }
  for (const file of walk(outputPath)) {
    if (statSync(file).size === 0) bad.push(`${relative(outputPath, file)}: 0 bytes`);
  }
  return bad;
}
