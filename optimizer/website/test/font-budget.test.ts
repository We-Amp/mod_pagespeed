// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// font-budget.test.ts — guards the two font decisions that keep the mobile
// Lighthouse score and the layout stable:
//
//   1. The self-hosted Inter subset stays small. It is preloaded on every page,
//      so its bytes sit on the critical path of the largest contentful paint.
//      The budget is the shipped size plus 10%; a regenerated subset that grows
//      past it (a wider character set, a restored weight range, an extra
//      layout feature) has to be a deliberate decision, made here.
//   2. The web fonts are build assets (src/assets/fonts/), so the build serves
//      them from hashed /_astro/ URLs that are cached for a year. The preloads
//      in the page head and the @font-face rules must name the same file, or
//      the browser fetches each font twice.
//   3. global.css carries metric-matched fallback faces for Inter and IBM Plex
//      Mono, and the font stacks name them right after the web font, so the
//      swap from the local fallback to the web font does not shift text.

import { existsSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, it, expect } from 'vitest';

const path = (rel: string) => fileURLToPath(new URL(rel, import.meta.url));
const css = readFileSync(path('../src/styles/global.css'), 'utf8');

// 41,952 bytes shipped, times 1.1.
const INTER_BUDGET_BYTES = 46_147;

function fontFace(family: string): string {
  const faces = css.match(/@font-face\s*\{[^}]*\}/g) ?? [];
  const face = faces.find((f) => new RegExp(`font-family:\\s*'${family}'`).test(f));
  expect(face, `@font-face for '${family}' in global.css`).toBeDefined();
  return face as string;
}

describe('Inter subset budget', () => {
  it('the woff2 stays under budget', () => {
    const size = statSync(path('../src/assets/fonts/inter-variable-subset.woff2')).size;
    expect(size).toBeLessThanOrEqual(INTER_BUDGET_BYTES);
  });
});

const CSS_FILE = path('../src/styles/global.css');
const HEAD_FILE = path('../src/layouts/parts/HeadMeta.astro');
const FONTS_DIR = path('../src/assets/fonts');

// The file each web-font @font-face loads, resolved the way the build does.
function faceFiles(): string[] {
  const faces = (css.match(/@font-face\s*\{[^}]*\}/g) ?? []).filter((f) => /url\(/.test(f));
  return faces.map((f) => {
    const url = f.match(/url\('([^']+)'\)/)?.[1];
    expect(url, f).toBeDefined();
    return resolve(dirname(CSS_FILE), url as string);
  });
}

describe('hashed font URLs', () => {
  const head = readFileSync(HEAD_FILE, 'utf8');
  // Each preload's href is an imported asset URL: `href={name}` with
  // `import name from '<file>?url'`.
  const preloads = [...head.matchAll(/<link\s+rel="preload"([^>]*)\/>/g)]
    .map((m) => m[1])
    .filter((attrs) => /as="font"/.test(attrs));

  it('every @font-face loads a build asset, not a file under public/', () => {
    const files = faceFiles();
    expect(files.length).toBe(3);
    for (const file of files) {
      expect(dirname(file)).toBe(FONTS_DIR);
      expect(existsSync(file), file).toBe(true);
    }
  });

  it('every font preload names a file an @font-face loads, with crossorigin', () => {
    expect(preloads.length).toBe(2);
    const faces = faceFiles();
    for (const attrs of preloads) {
      expect(attrs).toMatch(/\scrossorigin(\s|$)/);
      expect(attrs).toContain('type="font/woff2"');
      const name = attrs.match(/href=\{(\w+)\}/)?.[1];
      expect(name, `preload href is an imported URL: ${attrs}`).toBeDefined();
      const spec = head.match(new RegExp(`import\\s+${name}\\s+from\\s+'([^']+)\\?url';`))?.[1];
      expect(spec, `import of ${name}`).toBeDefined();
      expect(faces).toContain(resolve(dirname(HEAD_FILE), spec as string));
    }
  });

  // The preloaded pair is a deliberate choice: swapping a file (say Plex Mono
  // 600 for 400) keeps the URLs matched but preloads the wrong face.
  it('the preloads are Inter and IBM Plex Mono 600, exactly', () => {
    const files = preloads.map((attrs) => {
      const name = attrs.match(/href=\{(\w+)\}/)?.[1];
      const spec = head.match(new RegExp(`import\\s+${name}\\s+from\\s+'([^']+)\\?url';`))?.[1];
      return resolve(dirname(HEAD_FILE), spec as string).slice(FONTS_DIR.length + 1);
    });
    expect(files.sort()).toEqual(['inter-variable-subset.woff2', 'plex-mono-600.woff2']);
  });

  // After a build (CI's unit job runs without one, so this skips there): the
  // page and the stylesheet name the same hashed URLs, and nothing asks for
  // the old unhashed /fonts/ paths.
  const distIndex = path('../dist/client/index.html');
  it.skipIf(!existsSync(distIndex))('the built page preloads the URLs its stylesheet loads', () => {
    const html = readFileSync(distIndex, 'utf8');
    const hrefs = [...html.matchAll(/<link rel="preload" href="([^"]+)" as="font"[^>]*>/g)].map(
      (m) => m[1],
    );
    expect(hrefs.length).toBe(2);
    const sheets = [...html.matchAll(/<link rel="stylesheet" href="([^"]+\.css)"/g)].map((m) =>
      readFileSync(path(`../dist/client${m[1]}`), 'utf8'),
    );
    const urls = new Set(
      sheets.flatMap((s) => [...s.matchAll(/url\(([^)]+\.woff2)\)/g)].map((m) => m[1])),
    );
    expect(urls.size).toBe(3);
    for (const url of [...hrefs, ...urls]) {
      expect(url).toMatch(/^\/_astro\/[\w-]+\.[\w-]{8}\.woff2$/);
      expect(existsSync(path(`../dist/client${url}`)), url).toBe(true);
    }
    for (const href of hrefs) expect(urls).toContain(href);
    expect(html + sheets.join('')).not.toMatch(/\/fonts\/[\w-]+\.woff2/);
  });
});

describe('metric-matched font fallbacks', () => {
  for (const family of [
    'Inter Fallback',
    'Inter Fallback Roboto',
    'IBM Plex Mono Fallback',
    'IBM Plex Mono Fallback Liberation',
  ]) {
    it(`${family} carries size-adjust and the vertical metric overrides`, () => {
      const face = fontFace(family);
      expect(face).toMatch(/src:\s*local\(/);
      for (const descriptor of [
        'size-adjust',
        'ascent-override',
        'descent-override',
        'line-gap-override',
      ]) {
        expect(face, descriptor).toMatch(new RegExp(`${descriptor}:\\s*\\d+(\\.\\d+)?%`));
      }
    });
  }

  it('the fallbacks cover macOS, Windows, Linux and Android system fonts', () => {
    const sources = (family: string) => fontFace(family).match(/local\('([^']+)'\)/g) ?? [];
    const inter = [...sources('Inter Fallback'), ...sources('Inter Fallback Roboto')].join(' ');
    for (const local of ['Arial', 'Helvetica', 'Liberation Sans', 'Roboto']) {
      expect(inter).toContain(`local('${local}')`);
    }
    const mono = [
      ...sources('IBM Plex Mono Fallback'),
      ...sources('IBM Plex Mono Fallback Liberation'),
    ].join(' ');
    for (const local of ['Menlo', 'DejaVu Sans Mono', 'Liberation Mono', 'Droid Sans Mono']) {
      expect(mono).toContain(`local('${local}')`);
    }
  });

  it('the font stacks name the fallbacks right after their web font', () => {
    expect(css).toMatch(/--font-sans:\s*'Inter',\s*'Inter Fallback',\s*'Inter Fallback Roboto',/);
    expect(css).toMatch(
      /--font-mono:\s*'IBM Plex Mono',\s*'IBM Plex Mono Fallback',\s*'IBM Plex Mono Fallback Liberation',/,
    );
  });

  it('the Inter license ships next to the font', () => {
    const license = readFileSync(path('../public/fonts/Inter-OFL.txt'), 'utf8');
    expect(license).toContain('The Inter Project Authors');
    expect(license).toContain('SIL OPEN FONT LICENSE Version 1.1');
  });
});

// Every IBM Plex Mono weight is a file the page downloads. The site uses two,
// 400 and 600; a mono element asking for 500 would silently render at 400 (the
// nearest face), so the class lists may not pair font-mono with font-medium.
describe('IBM Plex Mono weights', () => {
  it('global.css declares the 400 and 600 faces only', () => {
    const faces = (css.match(/@font-face\s*\{[^}]*\}/g) ?? []).filter((f) =>
      /font-family:\s*'IBM Plex Mono'/.test(f),
    );
    const weights = faces.map((f) => f.match(/font-weight:\s*(\d+)/)?.[1]).sort();
    expect(weights).toEqual(['400', '600']);
  });

  it('no class list pairs font-mono with font-medium', () => {
    const offenders: string[] = [];
    const walk = (dir: string) => {
      for (const entry of readdirSync(dir, { withFileTypes: true })) {
        const full = `${dir}/${entry.name}`;
        if (entry.isDirectory()) walk(full);
        else if (/\.(astro|ts|mjs)$/.test(entry.name)) {
          const text = readFileSync(full, 'utf8');
          for (const m of text.matchAll(/class(?:Name)?=["'`]([^"'`]*)["'`]/g)) {
            const classes = m[1].split(/\s+/);
            if (classes.includes('font-mono') && classes.includes('font-medium')) {
              offenders.push(`${full}: ${m[1]}`);
            }
          }
        }
      }
    };
    walk(path('../src'));
    expect(offenders).toEqual([]);
  });
});
