// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// visible-text.mjs — dump the normalized visible text of a built page for
// copy-identity diffs. Reads the prerendered HTML from dist/client (no server
// needed), extracts the text of <body> (script, style, template and noscript
// content excluded), and prints one trimmed text node per line. Attribute
// copy is included too: alt, aria-label and title values print with a
// "@name: " marker prefix, in document order among the text lines.
// Run it on the same path before and after a change and diff the two dumps:
// the diff is the exact set of visible-string changes.
//
// Usage: node scripts/visible-text.mjs <path> [--root <dir>]
//   node scripts/visible-text.mjs /
//   node scripts/visible-text.mjs /docs/ --root dist/client

import { readFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { parse } from 'parse5';

const args = process.argv.slice(2);
let pagePath = null;
let root = 'dist/client';
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--root') root = args[++i];
  else if (!pagePath) pagePath = args[i];
  else {
    console.error(`unexpected argument: ${args[i]}`);
    process.exit(2);
  }
}
if (!pagePath) {
  console.error('usage: node scripts/visible-text.mjs <path> [--root <dir>]');
  process.exit(2);
}

// "/" -> dist/client/index.html; "/docs/" -> dist/client/docs/index.html.
// File-like paths (with an extension) map directly.
const file = /\.[a-z0-9]+$/i.test(pagePath)
  ? join(root, pagePath)
  : join(root, pagePath, 'index.html');

let html;
try {
  html = readFileSync(resolve(file), 'utf8');
} catch {
  console.error(`cannot read ${file} — is the site built?`);
  process.exit(2);
}

const doc = parse(html);

const SKIP = new Set(['script', 'style', 'template', 'noscript']);
const WANT = new Set(['body']);
const ATTRS = ['alt', 'aria-label', 'title'];

function textOf(node, out) {
  if (node.nodeName === '#text') {
    const t = node.value.replace(/\s+/g, ' ').trim();
    if (t) out.push(t);
    return;
  }
  if (!node.tagName || SKIP.has(node.tagName)) return;
  for (const name of ATTRS) {
    const attr = (node.attrs ?? []).find((a) => a.name === name);
    const v = attr?.value.replace(/\s+/g, ' ').trim();
    if (v) out.push(`@${name}: ${v}`);
  }
  for (const child of node.childNodes ?? []) textOf(child, out);
}

function* walk(node) {
  if (node.tagName && WANT.has(node.tagName)) {
    yield node;
    return; // <body> occurs once; textOf below descends into it
  }
  for (const child of node.childNodes ?? []) yield* walk(child);
}

const lines = [];
for (const region of walk(doc)) {
  const inner = [];
  textOf(region, inner);
  lines.push(`--- <${region.tagName}> ---`, ...inner);
}
process.stdout.write(lines.join('\n') + '\n');
