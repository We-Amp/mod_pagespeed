// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Keep punctuation attached to the inline link or inline code it follows.
 *
 * The formatter puts every closing </a> on its own line and the sentence
 * punctuation after it on the next, so the HTML source holds
 * `</a>\n  .` and the page shows "Privacy policy ." with a gap, and the
 * underline running into it. The source cannot hold the adjacent form: the
 * formatter rewrites it on the next run. So the built pages are normalized
 * here, as part of `postbuild` and before the search index is built: the
 * whitespace between a link's end and a following . , ; : ) ! ? is removed, as
 * is trailing whitespace inside the link, which would be underlined.
 *
 * Inline <code> is rewritten the same way; the content of <pre>, <script>,
 * <style> and <textarea> is not.
 */

import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const DIST = fileURLToPath(new URL('../dist/client', import.meta.url));

// ASCII whitespace only: a non-breaking space before punctuation is deliberate.
// The lookbehind makes a match start at the beginning of a whitespace run, so
// long runs stay linear.
const GAP = /(?<![ \t\n\r\f])[ \t\n\r\f]*<\/(a|code)>[ \t\n\r\f]+(?=[.,;:)!?])/g;

const RAW_TEXT = ['pre', 'script', 'style', 'textarea'];

/** @param {string} c */
const isTagBoundary = (c) => c === '>' || c === '/' || c === ' ' || /[\t\n\r\f]/.test(c);

/** ASCII-only lowercasing, so indexes stay valid in the original string. */
const asciiLower = (/** @type {string} */ s) =>
  s.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32));

/**
 * End of the literal region that starts at `at`: a comment (up to --> or --!>)
 * or a raw-text element (through its closing tag). Returns -1 when `at` starts
 * neither. A region that is never closed runs to the end of the document.
 * Plain string scanning, no tag or comment regular expressions.
 * @param {string} html @param {string} lower @param {number} at
 */
function literalEnd(html, lower, at) {
  if (html.startsWith('<!--', at)) {
    const ends = [html.indexOf('-->', at + 4), html.indexOf('--!>', at + 4)].filter((i) => i >= 0);
    return ends.length
      ? Math.min(...ends) + (html[Math.min(...ends) + 2] === '>' ? 3 : 4)
      : html.length;
  }
  for (const name of RAW_TEXT) {
    if (!lower.startsWith(name, at + 1) || !isTagBoundary(html.charAt(at + 1 + name.length))) {
      continue;
    }
    const openEnd = html.indexOf('>', at);
    if (openEnd < 0) return html.length;
    for (
      let i = lower.indexOf(`</${name}`, openEnd);
      i >= 0;
      i = lower.indexOf(`</${name}`, i + 1)
    ) {
      if (isTagBoundary(html.charAt(i + 2 + name.length))) {
        const closeEnd = html.indexOf('>', i);
        return closeEnd < 0 ? html.length : closeEnd + 1;
      }
    }
    return html.length;
  }
  return -1;
}

/** @param {string} html */
export function attachLinkPunctuation(html) {
  const fix = (/** @type {string} */ text) => text.replace(GAP, '</$1>');
  const lower = asciiLower(html);
  let out = '';
  let pos = 0;
  for (let at = html.indexOf('<'); at >= 0; at = html.indexOf('<', at + 1)) {
    if (at < pos) continue;
    const end = literalEnd(html, lower, at);
    if (end < 0) continue;
    out += fix(html.slice(pos, at)) + html.slice(at, end);
    pos = end;
  }
  return out + fix(html.slice(pos));
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
    console.error('attach-link-punctuation: no dist/client; run astro build first');
    process.exit(1);
  }
  let changed = 0;
  for (const file of htmlFiles(DIST)) {
    const before = readFileSync(file, 'utf8');
    const after = attachLinkPunctuation(before);
    if (after !== before) {
      writeFileSync(file, after);
      changed++;
    }
  }
  console.log(`attach-link-punctuation: normalized ${changed} pages`);
}
