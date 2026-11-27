// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Keep browser translation away from code.
 *
 * Directive names, shell commands and config snippets are not prose: a page
 * translated by the browser would otherwise rewrite `EnableFilters` or
 * `sudo apt-get install` into something that no longer works when copied.
 * Every <pre> and <code> in the built pages gets `translate="no"` (an element
 * that already sets translate is left as it is). Done here, as part of
 * `postbuild`, so Markdown, MDX and hand-written .astro markup are all covered
 * by one rule. Comments and the content of <script>, <style> and <textarea>
 * are not touched.
 */

import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));

const RAW_TEXT = ['script', 'style', 'textarea'];

// A <pre> or <code> start tag, attribute values quoted or bare.
const START_TAG =
  /<(pre|code)((?:\s+[^\s"'=<>`/]+(?:\s*=\s*(?:"[^"]*"|'[^']*'|[^\s"'=<>`]+))?)*)\s*>/gi;
const HAS_TRANSLATE = /\stranslate\s*=/i;

/** @param {string} html @param {string} lower @param {number} at */
function literalEnd(html, lower, at) {
  if (html.startsWith('<!--', at)) {
    const ends = [html.indexOf('-->', at + 4), html.indexOf('--!>', at + 4)].filter((i) => i >= 0);
    if (!ends.length) return html.length;
    const end = Math.min(...ends);
    return end + (html[end + 2] === '>' ? 3 : 4);
  }
  for (const name of RAW_TEXT) {
    if (!lower.startsWith(name, at + 1) || !/[\s/>]/.test(lower.charAt(at + 1 + name.length))) {
      continue;
    }
    const close = lower.indexOf(`</${name}`, at + 1);
    if (close < 0) return html.length;
    const closeEnd = html.indexOf('>', close);
    return closeEnd < 0 ? html.length : closeEnd + 1;
  }
  return -1;
}

/** @param {string} text */
const mark = (text) =>
  text.replace(START_TAG, (tag, name, attrs) =>
    HAS_TRANSLATE.test(attrs) ? tag : `<${name}${attrs} translate="no">`,
  );

/** @param {string} html */
export function markCodeUntranslated(html) {
  // ASCII-only lowercasing, so indexes stay valid in the original string.
  const lower = html.replace(/[A-Z]/g, (c) => c.toLowerCase());
  let out = '';
  let pos = 0;
  for (let at = html.indexOf('<'); at >= 0; at = html.indexOf('<', at + 1)) {
    if (at < pos) continue;
    const end = literalEnd(html, lower, at);
    if (end < 0) continue;
    out += mark(html.slice(pos, at)) + html.slice(at, end);
    pos = end;
  }
  return out + mark(html.slice(pos));
}

/** @param {string} dir @returns {string[]} */
function htmlFiles(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
    const p = path.join(dir, e.name);
    return e.isDirectory() ? htmlFiles(p) : p.endsWith('.html') ? [p] : [];
  });
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  if (!existsSync(DIST)) {
    console.error('mark-code-untranslated: no dist/client; run astro build first');
    process.exit(1);
  }
  let changed = 0;
  for (const file of htmlFiles(DIST)) {
    const before = readFileSync(file, 'utf8');
    const after = markCodeUntranslated(before);
    if (after !== before) {
      writeFileSync(file, after);
      changed++;
    }
  }
  console.log(`mark-code-untranslated: marked code on ${changed} pages`);
}
