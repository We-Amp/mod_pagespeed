// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// scroll-offset-gate.test.ts — one offset for the sticky header, in one place.
// The page scroller carries `scroll-padding-top` (src/styles/global.css), which
// keeps both anchor targets and keyboard focus clear of the header (WCAG
// 2.4.11 Focus Not Obscured). A scroll margin on a target adds to that
// padding, so an element that sets one lands lower than every other anchor on
// the site. This gate fails on any scroll margin in src/: the Tailwind
// `scroll-m*` utilities, the CSS `scroll-margin*` properties and the
// `scrollMargin*` style properties.

import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

const websiteRoot = fileURLToPath(new URL('..', import.meta.url));

const SCAN_EXTENSIONS = new Set([
  '.astro',
  '.css',
  '.ts',
  '.tsx',
  '.js',
  '.mjs',
  '.cjs',
  '.md',
  '.mdx',
]);

function scanFiles(dir: string): string[] {
  const out: string[] = [];
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    if (statSync(full).isDirectory()) {
      if (entry === 'node_modules' || entry.startsWith('.')) continue;
      out.push(...scanFiles(full));
    } else if (SCAN_EXTENSIONS.has(entry.slice(entry.lastIndexOf('.')))) {
      out.push(full);
    }
  }
  return out;
}

// The Tailwind utilities: every margin side and logical variant (mt, mb, my,
// mx, ms, me, ml, mr, mbs, mbe, ...) followed by a value, a bracketed value or
// a parenthesised variable, with or without a leading minus or variant prefix.
const UTILITY = /(?<![\w-])-?scroll-m[a-z]{0,2}-[\w[(]/;
// The CSS property in any position: a declaration, a quoted object key, an
// arbitrary-property class, or the name passed to style.setProperty().
const PROPERTY = /scroll-margin/;
// The style properties: el.style.scrollMarginTop, scrollMarginBlockStart, ...
const STYLE_PROPERTY = /scrollMargin/;

function violationsIn(text: string): { line: number; match: string }[] {
  const found: { line: number; match: string }[] = [];
  text.split('\n').forEach((line, i) => {
    for (const re of [UTILITY, PROPERTY, STYLE_PROPERTY]) {
      const m = re.exec(line);
      if (m) found.push({ line: i + 1, match: m[0] });
    }
  });
  return found;
}

describe('scroll offset for the sticky header', () => {
  it('the detector recognises every spelling of a scroll margin', () => {
    // Names are assembled from parts here, never written out: Tailwind scans
    // this file for class candidates too and would emit a utility for any
    // class-shaped token it finds.
    const u = (...parts: string[]) => ['scroll', ...parts].join('-');
    const samples = [
      `<section id="tiers" class="px-4 ${u('mt', '20')}">`,
      `<h2 class="${u('mt', '[5rem]')}">`,
      `<div class="lg:${u('my', '8')}">`,
      `<div class="-${u('mt', '4')}">`,
      `  ${u('margin', 'top')}: 5rem;`,
      `  ${u('margin')}: 5rem 0;`,
      `  ${u('margin', 'block', 'start')}: 5rem;`,
      `<h2 class="${u('mt', '(--header-h)')}">`,
      `<h2 class="${u('mbs', '20')}">`,
      `<h2 class="${u('mbe', '20')}">`,
      `<div class="${u('mr', '4')}">`,
      `<div class="md:!${u('mt', '20')}">`,
      `<div class="[${u('margin', 'top')}:5rem]">`,
      `  @apply ${u('mt', '20')};`,
      `el.style.setProperty('${u('margin', 'top')}', '5rem');`,
      `const style = { '${u('margin', 'block', 'start')}': '5rem' };`,
      "el.style.scrollMarginTop = '5rem';",
      'el.style.scrollMarginBlockStart = "5rem";',
      'el.style.scrollMarginInlineStart = "1rem";',
      '<div style={{ scrollMarginTop: 80 }}>',
    ];
    for (const sample of samples) {
      expect(violationsIn(sample), sample).not.toHaveLength(0);
    }
    const clean = [
      `  ${u('padding', 'top')}: 5rem;`,
      `<html class="${u('smooth')} dark">`,
      `<div class="${u('pt', '20')} overflow-y-auto">`,
      `  ${u('behavior')}: auto !important;`,
    ];
    for (const sample of clean) {
      expect(violationsIn(sample), sample).toHaveLength(0);
    }
  });

  it('no element in src/ sets a scroll margin', () => {
    const violations: string[] = [];
    for (const file of scanFiles(join(websiteRoot, 'src'))) {
      for (const v of violationsIn(readFileSync(file, 'utf8'))) {
        violations.push(`${relative(websiteRoot, file)}:${v.line}  ${v.match}`);
      }
    }
    expect(
      violations,
      'A scroll margin adds to the scroll padding on <html>; remove it and rely on the padding:\n' +
        violations.join('\n'),
    ).toHaveLength(0);
  });

  it('the page scroller keeps the sticky header clear with scroll padding', () => {
    const css = readFileSync(join(websiteRoot, 'src/styles/global.css'), 'utf8');
    expect(css).toMatch(/\bhtml\s*\{\s*scroll-padding-top:\s*5rem;/);
  });
});
