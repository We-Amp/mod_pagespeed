// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Unit tests for the pure parts of scripts/visual-diff.mjs (the opt-in
// before/after tool): what the build-output normalization strips and keeps,
// the rule-level CSS split, the pixel comparison, and the preconditions that
// make the tool refuse to run. No browser and no server is involved.

import { cpSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterAll, describe, expect, it, vi } from 'vitest';
import {
  DEFAULT_PATHS,
  NOISE_MAX_DELTA,
  NOISE_MAX_PIXELS,
  comparePixels,
  distDiff,
  formatPercent,
  freezeSvg,
  isRasterNoise,
  main,
  multisetDiff,
  normalizeText,
  shouldReshoot,
  slugForPath,
  splitCssRules,
  summarizeRounds,
} from '../scripts/visual-diff.mjs';

const image = (width: number, height: number, fill = 0) => ({
  data: Buffer.alloc(width * height * 4, fill),
  width,
  height,
});

describe('normalizeText', () => {
  it('strips scoped-style ids, bundle hashes and the build-id tag', () => {
    const a =
      '<meta name="build-id" content="base-1"><link href="/_astro/BaseLayout.CRGEekDt.css">' +
      '<p data-astro-cid-airhgkwu class="x">hi</p><style>.x[data-astro-cid-airhgkwu]{color:red}</style>' +
      '<script src="/_astro/page.B1x_y-Zq.js"></script>';
    const b =
      '<meta name="build-id" content="head-2"><link href="/_astro/BaseLayout.Zz019aQw.css">' +
      '<p data-astro-cid-jnnmztss class="x">hi</p><style>.x[data-astro-cid-jnnmztss]{color:red}</style>' +
      '<script src="/_astro/page.Qq7_abcd.js"></script>';
    expect(normalizeText(a)).toBe(normalizeText(b));
    expect(normalizeText(a)).toContain('/_astro/BaseLayout.css');
    expect(normalizeText(a)).not.toContain('build-id');
  });

  it('keeps real differences and content-hashed images', () => {
    expect(normalizeText('<p class="x">hi</p>')).not.toBe(normalizeText('<p class="y">hi</p>'));
    const img = '<img src="/_astro/hero.D4f5g6h7.webp">';
    expect(normalizeText(img)).toBe(img);
  });
});

describe('splitCssRules', () => {
  it('descends into grouping at-rules and keeps other blocks whole', () => {
    const rules = splitCssRules(
      '@layer a,b;.a{color:red}@media (min-width:1px){.b{color:blue}@supports (x:y){.c{d:e}}}' +
        '@keyframes k{0%{o:0}to{o:1}}.d:after{content:"}"}',
    );
    expect(rules).toEqual([
      ' | @layer a,b;',
      ' | .a{color:red}',
      '@media (min-width:1px) | .b{color:blue}',
      '@media (min-width:1px) @supports (x:y) | .c{d:e}',
      ' | @keyframes k{0%{o:0}to{o:1}}',
      ' | .d:after{content:"}"}',
    ]);
  });

  it('does not read an escaped quote in a selector as the start of a string', () => {
    expect(splitCssRules(String.raw`.a\:content-\[\'\'\]:after{content:""}.b{color:red}`)).toEqual([
      String.raw` | .a\:content-\[\'\'\]:after{content:""}`,
      ' | .b{color:red}',
    ]);
    expect(
      splitCssRules(
        String.raw`@media (min-width:1px){.a\:content-\[\"\"\]:after{x:y}.b{c:d}}.e{f:g}`,
      ),
    ).toEqual([
      String.raw`@media (min-width:1px) | .a\:content-\[\"\"\]:after{x:y}`,
      '@media (min-width:1px) | .b{c:d}',
      ' | .e{f:g}',
    ]);
  });

  it('does not read an escaped brace or semicolon as syntax', () => {
    expect(splitCssRules(String.raw`.a\{b\}{color:red}.c\;d{top:0}.e{f:g}`)).toEqual([
      String.raw` | .a\{b\}{color:red}`,
      String.raw` | .c\;d{top:0}`,
      ' | .e{f:g}',
    ]);
    expect(splitCssRules(String.raw`@media (x){.a\}b{c:d}.e{f:"\"}"}}.g{h:i}`)).toEqual([
      String.raw`@media (x) | .a\}b{c:d}`,
      String.raw`@media (x) | .e{f:"\"}"}`,
      ' | .g{h:i}',
    ]);
  });

  it('feeds a multiset diff that ignores order and reports each side', () => {
    const base = splitCssRules('.a{x:1}.b{x:2}.b{x:2}');
    const head = splitCssRules('.b{x:2}.a{x:1}.c{x:3}');
    expect(multisetDiff(base, head)).toEqual({ onlyA: [' | .b{x:2}'], onlyB: [' | .c{x:3}'] });
    expect(multisetDiff(base, [...base].reverse())).toEqual({ onlyA: [], onlyB: [] });
  });
});

describe('distDiff', () => {
  const root = mkdtempSync(join(tmpdir(), 'visual-diff-'));
  afterAll(() => rmSync(root, { recursive: true, force: true }));
  const build = (name: string, hash: string, cid: string, body: string, css: string) => {
    const client = join(root, name, 'dist', 'client');
    mkdirSync(join(client, '_astro'), { recursive: true });
    mkdirSync(join(client, 'about'), { recursive: true });
    const page =
      `<meta name="build-id" content="${name}"><link href="/_astro/site.${hash}.css">` +
      `<main data-astro-cid-${cid}>${body}</main>`;
    writeFileSync(join(client, 'index.html'), page);
    writeFileSync(join(client, 'about', 'index.html'), page);
    writeFileSync(join(client, '_astro', `site.${hash}.css`), css);
    return join(root, name);
  };

  it('is empty for two builds that differ only in generated names', () => {
    const a = build('a', 'AAAAaaaa', 'abc123', 'same', '.x[data-astro-cid-abc123]{color:red}');
    const b = build('b', 'BBBBbbbb', 'def456', 'same', '.x[data-astro-cid-def456]{color:red}');
    const result = distDiff(a, b);
    expect(result.empty).toBe(true);
    expect(result.html.compared).toBe(2);
  });

  it('reports a changed page and a changed rule', () => {
    const a = build('c', 'AAAAaaaa', 'abc123', 'before', '.x{color:red}.y{top:0}');
    const b = build('d', 'BBBBbbbb', 'def456', 'after', '.x{color:blue}.y{top:0}');
    const result = distDiff(a, b);
    expect(result.empty).toBe(false);
    expect(result.html.changed.map((c) => c.file).sort()).toEqual([
      'about/index.html',
      'index.html',
    ]);
    expect(result.css.onlyBase).toEqual([' | .x{color:red}']);
    expect(result.css.onlyHead).toEqual([' | .x{color:blue}']);
  });

  it('reports both build ids', () => {
    const a = build('e', 'AAAAaaaa', 'abc123', 'same', '.x{color:red}');
    const b = build('f', 'BBBBbbbb', 'def456', 'same', '.x{color:red}');
    expect(distDiff(a, b)).toMatchObject({ baseId: 'e', headId: 'f', sameBuild: false });
  });

  it('refuses the same directory given twice, however it is spelled', () => {
    const a = build('g', 'AAAAaaaa', 'abc123', 'same', '.x{color:red}');
    expect(() => distDiff(a, a)).toThrow(/same build output/);
    expect(() => distDiff(join(a, 'dist'), join(a, 'dist', 'client'))).toThrow(/same build output/);
  });

  it('refuses two builds with equal ids and a build without an id', () => {
    const a = build('h', 'AAAAaaaa', 'abc123', 'same', '.x{color:red}');
    const twin = join(root, 'twin', 'h');
    mkdirSync(join(root, 'twin'), { recursive: true });
    cpSync(a, twin, { recursive: true });
    expect(() => distDiff(a, twin)).toThrow(/build ids are equal/);

    const b = build('i', 'BBBBbbbb', 'def456', 'same', '.x{color:red}');
    writeFileSync(join(b, 'dist', 'client', 'index.html'), '<main>same</main>');
    expect(() => distDiff(a, b)).toThrow(/no build-id tag/);
  });

  it('compares a build with itself only on request, and marks the result', () => {
    const a = build('j', 'AAAAaaaa', 'abc123', 'same', '.x{color:red}');
    const result = distDiff(a, a, { allowSameBuild: true });
    expect(result).toMatchObject({ empty: true, sameBuild: true });
  });
});

describe('comparePixels', () => {
  it('reports zero for equal images', () => {
    expect(comparePixels(image(4, 3, 7), image(4, 3, 7))).toMatchObject({ changed: 0, total: 12 });
  });

  it('counts changed pixels, their rows and the largest channel difference', () => {
    const a = image(4, 3);
    const b = image(4, 3);
    b.data[(1 * 4 + 2) * 4] = 9;
    b.data[(2 * 4 + 0) * 4 + 1] = 2;
    expect(comparePixels(a, b)).toMatchObject({ changed: 2, maxDelta: 9, top: 1, bottom: 2 });
    expect(comparePixels(a, b, 2)).toMatchObject({ changed: 1, top: 1, bottom: 1 });
  });

  it('counts the pixels only one image has as changed', () => {
    const cmp = comparePixels(image(4, 3), image(4, 5));
    expect(cmp).toMatchObject({ width: 4, height: 5, total: 20, changed: 8, top: 3, bottom: 4 });
  });
});

describe('re-shoot decision', () => {
  const noise = { changed: 24, maxDelta: 2, sameSize: true };
  const equal = { changed: 0, maxDelta: 0, sameSize: true };
  const shift = { changed: 57600, maxDelta: 158, sameSize: true };

  it('treats only a small, faint, same-size difference as raster noise', () => {
    expect(isRasterNoise(noise)).toBe(true);
    expect(isRasterNoise(equal)).toBe(false);
    expect(isRasterNoise(shift)).toBe(false);
    expect(isRasterNoise({ ...noise, changed: NOISE_MAX_PIXELS + 1 })).toBe(false);
    expect(isRasterNoise({ ...noise, maxDelta: NOISE_MAX_DELTA + 1 })).toBe(false);
    expect(isRasterNoise({ ...noise, sameSize: false })).toBe(false);
  });

  it('shoots again only after noise, never after a real change, three rounds at most', () => {
    expect(shouldReshoot([noise])).toBe(true);
    expect(shouldReshoot([noise, noise])).toBe(true);
    expect(shouldReshoot([noise, noise, noise])).toBe(false);
    expect(shouldReshoot([equal])).toBe(false);
    expect(shouldReshoot([shift])).toBe(false);
    expect(shouldReshoot([noise, equal])).toBe(false);
    expect(shouldReshoot([noise, shift])).toBe(false);
  });

  it('reports a discarded round that differed, and flags one that was not noise', () => {
    expect(summarizeRounds([equal], 0)).toEqual({ reshot: false, flaky: false, note: null });
    expect(summarizeRounds([shift], 0)).toEqual({ reshot: false, flaky: false, note: null });
    expect(summarizeRounds([noise, equal], 1)).toEqual({
      reshot: true,
      flaky: false,
      note: 're-shot: an earlier pair differed by 24 px, max channel delta 2',
    });
    expect(summarizeRounds([noise, shift], 0)).toEqual({
      reshot: true,
      flaky: true,
      note: 'FLAKY: a discarded pair differed by 57600 px, max channel delta 158',
    });
  });
});

describe('helpers', () => {
  it('formats the changed share', () => {
    expect(formatPercent(0, 100)).toBe('0%');
    expect(formatPercent(1, 1_000_000)).toBe('<0.01%');
    expect(formatPercent(125, 1000)).toBe('12.50%');
  });

  it('names a capture after its path', () => {
    expect(slugForPath('/')).toBe('home');
    expect(slugForPath('/docs/getting-started/')).toBe('docs_getting-started');
    expect(new Set(DEFAULT_PATHS.map(slugForPath)).size).toBe(DEFAULT_PATHS.length);
  });

  it('includes the error pages in the default path list', () => {
    expect(DEFAULT_PATHS).toContain('/error/404/');
    expect(DEFAULT_PATHS).toContain('/error/500/');
  });

  it('switches off animations inside an svg image and leaves a still one alone', () => {
    const still = '<svg><rect/></svg>';
    expect(freezeSvg(still)).toBe(still);
    const moving =
      '<svg><style>@keyframes b{to{opacity:0}}.c{animation:b 1s 4}</style>' +
      '<rect class="c"><animate attributeName="x" to="1"/></rect></svg>';
    const frozen = freezeSvg(moving);
    expect(frozen).not.toContain('<animate');
    expect(frozen).toContain('animation:none !important');
  });
});

describe('preconditions', () => {
  const run = async (argv: string[]) => {
    const err = vi.spyOn(console, 'error').mockImplementation(() => {});
    try {
      const code = await main(argv);
      return { code, message: err.mock.calls.map((c) => String(c[0])).join('\n') };
    } finally {
      err.mockRestore();
    }
  };
  const shots = ['shots', '--base', 'http://127.0.0.1:9', '--head', 'http://127.0.0.1:9'];

  it('refuses two equal build ids', async () => {
    const { code, message } = await run([
      ...shots,
      '--base-id',
      'x',
      '--head-id',
      'x',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('build ids are equal');
  });

  it('refuses an origin that is not local', async () => {
    const { code, message } = await run([
      'shots',
      '--base',
      'https://example.com',
      '--head',
      'http://127.0.0.1:9',
      '--base-id',
      'a',
      '--head-id',
      'b',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('not a loopback address');
  });

  it('refuses an origin where nothing answers', async () => {
    const { code, message } = await run([
      ...shots,
      '--base-id',
      'a',
      '--head-id',
      'b',
      '--out',
      'o',
    ]);
    expect(code).toBe(2);
    expect(message).toContain('nothing answers');
  });

  it('refuses a build-output diff of one directory against itself', async () => {
    const { code, message } = await run(['dist', '--base', 'dist', '--head', 'dist/client']);
    expect(code).toBe(2);
    expect(message).toMatch(/same build output|no build output found/);
  });

  it('rejects a capture time limit that is not a positive number', async () => {
    const { code, message } = await run([...shots, '--timeout', '0']);
    expect(code).toBe(2);
    expect(message).toContain('--timeout');
  });

  it('rejects an unknown mode', async () => {
    expect((await run(['nope'])).code).toBe(2);
  });
});
