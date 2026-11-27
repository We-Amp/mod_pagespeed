// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// visual-diff.mjs — before/after evidence for a site change. Opt-in, local,
// never part of the test suites and never a CI gate. Two modes:
//
//   shots  Full-page screenshots of the same paths on two locally served
//          builds (a base and a head), a per-pixel diff of each pair, a
//          `page | viewport | changed %` table and an HTML contact sheet.
//   dist   The normalized diff of two build outputs: prerendered HTML and
//          built CSS compared after the names that change on every build
//          (scoped-style ids, hashed asset names, the build-id tag) are
//          stripped. On-demand routes are not in dist/; shots cover those.
//
// The script never builds and never starts or stops a server. Build each
// side with its own SITE_BUILD_ID and serve it on an explicit free port:
//
//   SITE_BUILD_ID=base-<short sha> PRICING_ALLOW_STALE=1 npm run build
//   HOST=127.0.0.1 PORT=<free port> node dist/server/entry.mjs
//
// Usage:
//   node scripts/visual-diff.mjs shots --base <origin> --head <origin> \
//     --base-id <id> --head-id <id> --out <dir> [--paths <file>] [path...]
//   node scripts/visual-diff.mjs dist --base <dist dir> --head <dist dir>
//
// shots refuses to run unless each origin is a loopback address and serves
// the expected <meta name="build-id">, and unless the two ids differ, so a
// wrong port or a wrong dist/ cannot pass as "0 changed pixels".
// --allow-same-build lifts the last rule for one purpose only: shooting one
// build twice to measure render noise.
//
// Determinism (shots): dark colour scheme, reduced motion, animations
// disabled, device scale factor 1, fixed Date, UTC, a fresh browser context
// (empty storage) per page, every request to another origin refused, fonts
// awaited, lazy images forced to load, animations inside SVG images switched
// off, the page captured unscrolled, each page shot until two consecutive
// captures are byte-equal. The telemetry strip (live timing values) and the
// footer copyright year are masked; --mask <selector> adds more. --timeout
// <ms> (default 45000) limits the navigation and each capture of a page;
// raise it for a very tall page or a busy machine.
//
// dist applies the same identity rule to the two directories: it reads the
// build-id tag from each side's index.html and refuses a side without one,
// two equal ids, or the same directory given twice. Any run made with
// --allow-same-build is marked "SAME-BUILD SELF-TEST, NOT EVIDENCE" in every
// output.
//
// What dist does not cover: client JavaScript bundles, images, fonts and
// other static files are not compared, and the CSS comparison ignores rule
// order (a rule that moves between sheets is not a difference, and neither
// is a reordering of two rules). "empty" therefore means "same prerendered
// markup and the same set of CSS rules", not "identical build".
//
// Exit codes: 0 done (shots: whatever the diff; dist: no difference),
// 1 a difference was found (dist always; shots only with --fail-on-diff,
// which also fails on an unstable capture and on a discarded round that
// differed by more than raster noise), 2 usage error or a failed
// precondition.

/* eslint-disable no-console */

import {
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  realpathSync,
  renameSync,
  rmSync,
  statSync,
  writeFileSync,
} from 'node:fs';
import { join, relative, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export const VIEWPORTS = [
  { name: 'desktop', width: 1440, height: 900 },
  { name: 'tablet', width: 820, height: 1180 },
  { name: 'phone', width: 390, height: 844 },
];

// The default path set: the main journeys plus the two error pages. The node
// server answers an unknown URL with a redirect stub, so the designed error
// pages are reviewed at /error/404/ and /error/500/.
export const DEFAULT_PATHS = [
  '/',
  '/download/',
  '/pricing/',
  '/features/',
  '/docs/',
  '/docs/getting-started/',
  '/support/',
  '/contact/',
  '/about/',
  '/blog/',
  '/error/404/',
  '/error/500/',
];

export const DEFAULT_MASKS = ['[data-ui="telemetry-strip"]', 'footer p:has-text("©")'];

const FIXED_DATE = '2026-01-15T12:00:00Z';
const MASK_COLOR = '#ff00ff';

class UsageError extends Error {}

// ---------------------------------------------------------------------------
// Normalization (dist mode)

/** Strip what differs between two builds of the same source. */
export function normalizeText(text) {
  return (
    text
      // <meta name="build-id" content="...">
      .replace(/<meta\s+name="build-id"\s+content="[^"]*"\s*\/?>/g, '')
      // scoped-style ids: the attribute on elements and the selector in CSS
      .replace(/\[data-astro-cid-[a-z0-9]+\]/g, '')
      .replace(/\sdata-astro-cid-[a-z0-9]+(?:="")?/g, '')
      // hashed bundle names: _astro/<name>.<hash>.css|js
      .replace(/(_astro\/[A-Za-z0-9_.@-]+?)\.[A-Za-z0-9_-]{6,12}\.(css|js)\b/g, '$1.$2')
  );
}

/**
 * Split a stylesheet into rules. Grouping at-rules (@media, @supports,
 * @layer, @container, @scope, @starting-style) are descended into and their
 * prelude becomes the context of the rules inside; everything else (style
 * rules, @font-face, @keyframes, @property, block-less statements) is one
 * entry. Returns strings of the form `<context> | <prelude>{<body>}`.
 */
export function splitCssRules(css, context = '') {
  const out = [];
  const grouping = /^@(media|supports|layer|container|scope|starting-style)\b/;
  let i = 0;
  let start = 0;
  const n = css.length;
  const skipString = (quote) => {
    i++;
    while (i < n && css[i] !== quote) i += css[i] === '\\' ? 2 : 1;
    i++;
  };
  while (i < n) {
    const c = css[i];
    if (c === '\\') {
      // An escaped character (a quote, brace or semicolon inside an
      // identifier, as in `.after\:content-\[\'\'\]`) is never syntax.
      i += 2;
    } else if (c === '"' || c === "'") {
      skipString(c);
    } else if (c === '/' && css[i + 1] === '*') {
      const end = css.indexOf('*/', i + 2);
      i = end === -1 ? n : end + 2;
    } else if (c === ';') {
      const statement = css.slice(start, i).trim();
      if (statement) out.push(`${context} | ${statement};`);
      i++;
      start = i;
    } else if (c === '{') {
      const prelude = css.slice(start, i).trim();
      let depth = 1;
      const bodyStart = ++i;
      while (i < n && depth > 0) {
        const d = css[i];
        if (d === '\\') {
          i += 2;
          continue;
        }
        if (d === '"' || d === "'") {
          skipString(d);
          continue;
        }
        if (d === '/' && css[i + 1] === '*') {
          const end = css.indexOf('*/', i + 2);
          i = end === -1 ? n : end + 2;
          continue;
        }
        if (d === '{') depth++;
        else if (d === '}') depth--;
        i++;
      }
      const body = css.slice(bodyStart, i - 1);
      if (grouping.test(prelude)) {
        out.push(...splitCssRules(body, context ? `${context} ${prelude}` : prelude));
      } else {
        out.push(`${context} | ${prelude}{${body.trim()}}`);
      }
      start = i;
    } else {
      i++;
    }
  }
  const tail = css.slice(start).trim();
  if (tail) out.push(`${context} | ${tail}`);
  return out;
}

/** Multiset difference of two string lists: what only a has, what only b has. */
export function multisetDiff(a, b) {
  const count = new Map();
  for (const s of a) count.set(s, (count.get(s) ?? 0) + 1);
  const onlyB = [];
  for (const s of b) {
    const c = count.get(s) ?? 0;
    if (c > 0) count.set(s, c - 1);
    else onlyB.push(s);
  }
  const onlyA = [];
  for (const [s, c] of count) for (let k = 0; k < c; k++) onlyA.push(s);
  return { onlyA, onlyB };
}

function walk(dir, ext, base = dir, out = []) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full, ext, base, out);
    else if (name.endsWith(ext)) out.push(relative(base, full));
  }
  return out;
}

function resolveClientDir(input) {
  const root = resolve(input);
  for (const candidate of [join(root, 'dist', 'client'), join(root, 'client'), root]) {
    if (existsSync(join(candidate, '_astro'))) return candidate;
  }
  throw new UsageError(`no build output found at ${input} (expected dist/client/ with _astro/)`);
}

/** First position where two strings differ, with some context from each. */
function firstDifference(a, b) {
  let i = 0;
  const n = Math.min(a.length, b.length);
  while (i < n && a[i] === b[i]) i++;
  const from = Math.max(0, i - 80);
  const clip = (s) => s.slice(from, i + 120).replace(/\s+/g, ' ');
  return { offset: i, base: clip(a), head: clip(b) };
}

const BUILD_ID_TAG = /<meta\s+name="build-id"\s+content="([^"]*)"/;
export const SAME_BUILD_MARKER = 'SAME-BUILD SELF-TEST, NOT EVIDENCE';

function distBuildId(clientDir) {
  const index = join(clientDir, 'index.html');
  if (!existsSync(index)) return null;
  const m = BUILD_ID_TAG.exec(readFileSync(index, 'utf8'));
  return m ? m[1] : null;
}

export function distDiff(baseInput, headInput, { allowSameBuild = false } = {}) {
  const baseDir = resolveClientDir(baseInput);
  const headDir = resolveClientDir(headInput);
  const baseId = distBuildId(baseDir);
  const headId = distBuildId(headDir);
  const sameDir = realpathSync(baseDir) === realpathSync(headDir);
  const sameBuild = sameDir || baseId === null || headId === null || baseId === headId;
  if (!allowSameBuild) {
    if (sameDir) {
      throw new UsageError(
        `base and head are the same build output (${baseDir}): refusing to compare a build with itself.`,
      );
    }
    for (const [label, id, dir] of [
      ['base', baseId, baseDir],
      ['head', headId, headDir],
    ]) {
      if (id === null) {
        throw new UsageError(
          `${label} ${dir}: no build-id tag in index.html. Build with SITE_BUILD_ID set.`,
        );
      }
    }
    if (baseId === headId) {
      throw new UsageError(
        `base and head build ids are equal (${baseId}): refusing to compare a build with itself. ` +
          'Build the two sides with distinct SITE_BUILD_ID values.',
      );
    }
  }

  const baseHtml = walk(baseDir, '.html');
  const headHtml = walk(headDir, '.html');
  const headSet = new Set(headHtml);
  const baseSet = new Set(baseHtml);
  const html = {
    compared: 0,
    onlyBase: baseHtml.filter((f) => !headSet.has(f)),
    onlyHead: headHtml.filter((f) => !baseSet.has(f)),
    changed: [],
  };
  for (const file of baseHtml) {
    if (!headSet.has(file)) continue;
    html.compared++;
    const a = normalizeText(readFileSync(join(baseDir, file), 'utf8'));
    const b = normalizeText(readFileSync(join(headDir, file), 'utf8'));
    if (a !== b) html.changed.push({ file, ...firstDifference(a, b) });
  }

  // CSS is compared as one multiset of rules over all built sheets, so a
  // rule that moves between sheets is not a difference; which sheet a page
  // links is covered by the HTML comparison above.
  const sheetRules = (dir) => {
    const files = walk(join(dir, '_astro'), '.css');
    const rules = [];
    for (const f of files) {
      rules.push(...splitCssRules(normalizeText(readFileSync(join(dir, '_astro', f), 'utf8'))));
    }
    return { files: files.map((f) => normalizeText(`_astro/${f}`)).sort(), rules };
  };
  const baseCss = sheetRules(baseDir);
  const headCss = sheetRules(headDir);
  const ruleDiff = multisetDiff(baseCss.rules, headCss.rules);
  const sheetDiff = multisetDiff(baseCss.files, headCss.files);
  const css = {
    baseRules: baseCss.rules.length,
    headRules: headCss.rules.length,
    onlyBase: ruleDiff.onlyA,
    onlyHead: ruleDiff.onlyB,
    sheetsOnlyBase: sheetDiff.onlyA,
    sheetsOnlyHead: sheetDiff.onlyB,
  };

  const empty =
    html.onlyBase.length === 0 &&
    html.onlyHead.length === 0 &&
    html.changed.length === 0 &&
    css.onlyBase.length === 0 &&
    css.onlyHead.length === 0 &&
    css.sheetsOnlyBase.length === 0 &&
    css.sheetsOnlyHead.length === 0;
  return { empty, baseId, headId, sameBuild, html, css };
}

function printDistDiff(result, limit) {
  const { html, css } = result;
  const lines = [];
  if (result.sameBuild)
    lines.push(`${SAME_BUILD_MARKER} (the two sides are not two distinct builds)`);
  lines.push(`base build-id ${result.baseId ?? 'none'}, head build-id ${result.headId ?? 'none'}`);
  const list = (title, items, fmt = (x) => x) => {
    if (items.length === 0) return;
    lines.push(`${title} (${items.length}):`);
    for (const item of items.slice(0, limit)) lines.push(`  ${fmt(item)}`);
    if (items.length > limit) lines.push(`  ... ${items.length - limit} more (raise --limit)`);
  };
  lines.push(
    `html: ${html.compared} files compared, ${html.changed.length} differ, ` +
      `${html.onlyBase.length} only in base, ${html.onlyHead.length} only in head`,
  );
  list('html only in base', html.onlyBase);
  list('html only in head', html.onlyHead);
  list(
    'html differs',
    html.changed,
    (c) => `${c.file} @${c.offset}\n    base: ${c.base}\n    head: ${c.head}`,
  );
  lines.push(
    `css: ${css.baseRules} rules in base, ${css.headRules} in head, ` +
      `${css.onlyBase.length} only in base, ${css.onlyHead.length} only in head`,
  );
  const clip = (s) => (s.length > 240 ? `${s.slice(0, 240)}...` : s);
  list('css sheets only in base', css.sheetsOnlyBase);
  list('css sheets only in head', css.sheetsOnlyHead);
  list('css rules only in base', css.onlyBase, clip);
  list('css rules only in head', css.onlyHead, clip);
  lines.push(
    'not compared: client JavaScript, images and other static files; CSS rule order is ignored',
  );
  lines.push(
    `normalized dist diff: ${result.empty ? 'empty' : 'NOT empty'}` +
      `${result.sameBuild ? ` (${SAME_BUILD_MARKER})` : ''}`,
  );
  console.log(lines.join('\n'));
}

// ---------------------------------------------------------------------------
// Pixel comparison (shots mode)

/**
 * Compare two RGBA images ({ data, width, height }). The canvas is the larger
 * of the two in each dimension; a pixel that exists in only one image counts
 * as changed. threshold is the largest per-channel difference still treated
 * as equal (0: exact); maxDelta is the largest per-channel difference seen
 * where both images have the pixel.
 */
export function comparePixels(a, b, threshold = 0) {
  const width = Math.max(a.width, b.width);
  const height = Math.max(a.height, b.height);
  const total = width * height;
  if (a.width === b.width && a.height === b.height && a.data.equals(b.data)) {
    return {
      width,
      height,
      total,
      sameSize: true,
      changed: 0,
      maxDelta: 0,
      top: null,
      bottom: null,
      mask: null,
    };
  }
  const mask = new Uint8Array(total);
  let changed = 0;
  let maxDelta = 0;
  let top = null;
  let bottom = null;
  const sameWidth = a.width === b.width;
  for (let y = 0; y < height; y++) {
    const inA = y < a.height;
    const inB = y < b.height;
    if (inA && inB && sameWidth) {
      const ra = a.data.subarray(y * width * 4, (y + 1) * width * 4);
      const rb = b.data.subarray(y * width * 4, (y + 1) * width * 4);
      if (ra.equals(rb)) continue;
    }
    let rowChanged = false;
    for (let x = 0; x < width; x++) {
      let differs = !inA || !inB || x >= a.width || x >= b.width;
      if (!differs) {
        const ia = (y * a.width + x) * 4;
        const ib = (y * b.width + x) * 4;
        for (let ch = 0; ch < 4; ch++) {
          const delta = Math.abs(a.data[ia + ch] - b.data[ib + ch]);
          if (delta > maxDelta) maxDelta = delta;
          if (delta > threshold) differs = true;
        }
      }
      if (differs) {
        mask[y * width + x] = 1;
        changed++;
        rowChanged = true;
      }
    }
    if (rowChanged) {
      if (top === null) top = y;
      bottom = y;
    }
  }
  const sameSize = a.width === b.width && a.height === b.height;
  return {
    width,
    height,
    total,
    sameSize,
    changed,
    maxDelta,
    top,
    bottom,
    mask: changed ? mask : null,
  };
}

// Raster noise, as measured on two captures of one build: a few antialiased
// edge pixels that round differently, one or two steps per channel, with no
// change in page size. Only a difference of this shape is worth a second
// capture; anything larger is a result.
export const NOISE_MAX_PIXELS = 200;
export const NOISE_MAX_DELTA = 2;

/** True when a comparison differs, and only in the way raster noise does. */
export function isRasterNoise(cmp) {
  return (
    cmp.changed > 0 &&
    cmp.sameSize === true &&
    cmp.changed <= NOISE_MAX_PIXELS &&
    cmp.maxDelta <= NOISE_MAX_DELTA
  );
}

/**
 * Whether to capture a pair again, given the comparisons of the rounds so
 * far (oldest first). Only while every round differed and every difference
 * was noise-shaped, and three rounds at most: a real change is shot once.
 */
export function shouldReshoot(rounds, maxRounds = 3) {
  return rounds.length < maxRounds && rounds.every(isRasterNoise);
}

/**
 * What the discarded rounds say about a capture. rounds: every round's
 * comparison, oldest first; kept: the index of the round whose pair was kept.
 */
export function summarizeRounds(rounds, kept) {
  const discarded = rounds.filter((r, i) => i !== kept && r.changed > 0);
  const flaky = discarded.filter((r) => !isRasterNoise(r));
  const worst = (list) => list.reduce((w, r) => (r.changed > w.changed ? r : w), list[0]);
  let note = null;
  if (flaky.length > 0) {
    const w = worst(flaky);
    note = `FLAKY: a discarded pair differed by ${w.changed} px, max channel delta ${w.maxDelta}`;
  } else if (discarded.length > 0) {
    const w = worst(discarded);
    note = `re-shot: an earlier pair differed by ${w.changed} px, max channel delta ${w.maxDelta}`;
  }
  return { reshot: discarded.length > 0, flaky: flaky.length > 0, note };
}

/** The diff picture: the base image dimmed to grey, changed pixels in magenta. */
function renderDiff(a, cmp) {
  const { width, height, mask } = cmp;
  const out = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const o = (y * width + x) * 4;
      if (mask[y * width + x]) {
        out[o] = 255;
        out[o + 1] = 0;
        out[o + 2] = 255;
      } else if (x < a.width && y < a.height) {
        const i = (y * a.width + x) * 4;
        const grey = Math.round(
          (a.data[i] * 0.3 + a.data[i + 1] * 0.59 + a.data[i + 2] * 0.11) / 3,
        );
        out[o] = out[o + 1] = out[o + 2] = grey;
      }
      out[o + 3] = 255;
    }
  }
  return out;
}

export const formatPercent = (changed, total) => {
  if (changed === 0) return '0%';
  const pct = (changed / total) * 100;
  return pct < 0.01 ? '<0.01%' : `${pct.toFixed(2)}%`;
};

export const slugForPath = (path) =>
  path
    .replace(/^\/|\/$/g, '')
    .replace(/[^A-Za-z0-9._-]+/g, '_')
    .replace(/^_|_$/g, '') || 'home';

// ---------------------------------------------------------------------------
// shots mode

const LOOPBACK = new Set(['127.0.0.1', 'localhost', '[::1]']);

function parseOrigin(value, label) {
  let url;
  try {
    url = new URL(value);
  } catch {
    throw new UsageError(`--${label}: not a URL: ${value}`);
  }
  if (!LOOPBACK.has(url.hostname)) {
    throw new UsageError(
      `--${label}: ${url.hostname} is not a loopback address; compare two locally served builds`,
    );
  }
  return url.origin;
}

async function servedBuildId(origin) {
  let res;
  try {
    res = await fetch(`${origin}/`, { redirect: 'manual' });
  } catch (err) {
    throw new UsageError(`nothing answers at ${origin}/ (${err.cause?.code ?? err.message})`);
  }
  const html = await res.text();
  const m = /<meta\s+name="build-id"\s+content="([^"]*)"/.exec(html);
  return m ? m[1] : null;
}

async function assertBuildId(origin, expected, label) {
  const served = await servedBuildId(origin);
  if (served !== expected) {
    throw new UsageError(
      `${label} ${origin}: expected build-id ${expected}, got ${served ?? 'none'}. ` +
        'Build with SITE_BUILD_ID set and check the port.',
    );
  }
}

// Runs in the page: load what a full-page capture would otherwise miss.
async function settlePage() {
  await document.fonts.ready;
  for (const el of document.querySelectorAll('img[loading="lazy"], iframe[loading="lazy"]')) {
    el.loading = 'eager';
  }
  const pending = [...document.images]
    .filter((img) => !img.complete)
    .map(
      (img) =>
        new Promise((done) => {
          img.addEventListener('load', done, { once: true });
          img.addEventListener('error', done, { once: true });
        }),
    );
  await Promise.race([Promise.all(pending), new Promise((done) => setTimeout(done, 10000))]);
  await Promise.all([...document.images].map((img) => img.decode().catch(() => {})));
  // No scroll-through: scroll-driven state (the active entry of an "on this
  // page" list, sticky bars) would depend on observer timing. The page is
  // captured as loaded, at scroll position 0.
  const frame = () => new Promise((done) => requestAnimationFrame(() => done()));
  await frame();
  await frame();
}

// An SVG used as an image runs its own animations, out of reach of the
// reduced-motion emulation and of the screenshot's animation freeze. Serve
// such files with their animations switched off.
export function freezeSvg(svg) {
  if (!/@keyframes|<animate|<set\b/.test(svg)) return svg;
  return svg
    .replace(/<(animate\w*|set)\b[^>]*\/>/g, '')
    .replace(/<(animate\w*|set)\b[^>]*>[\s\S]*?<\/\1>/g, '')
    .replace(/<\/svg>\s*$/, '<style>*{animation:none !important}</style></svg>');
}

async function shootOne(browser, origin, path, viewport, masks, file, timeout) {
  const context = await browser.newContext({
    viewport: { width: viewport.width, height: viewport.height },
    deviceScaleFactor: 1,
    colorScheme: 'dark',
    reducedMotion: 'reduce',
    locale: 'en-US',
    timezoneId: 'UTC',
    serviceWorkers: 'block',
  });
  const info = { status: null, blocked: 0, attempts: 0, stable: false };
  try {
    // One limit for the navigation and for each capture: a very tall page
    // (a hundred screens and more) needs longer than a short one.
    context.setDefaultTimeout(timeout);
    context.setDefaultNavigationTimeout(timeout);
    await context.clock.setFixedTime(new Date(FIXED_DATE));
    await context.route('**/*', async (route) => {
      const url = route.request().url();
      if (/^(data|blob|about):/.test(url)) return route.continue();
      if (!url.startsWith(`${origin}/`)) {
        info.blocked++;
        return route.abort();
      }
      if (!/\.svg(\?|$)/.test(url)) return route.continue();
      const response = await route.fetch();
      const body = await response.text();
      return route.fulfill({ response, body: freezeSvg(body) });
    });
    const page = await context.newPage();
    const response = await page.goto(new URL(path, origin).href, {
      waitUntil: 'load',
      timeout,
    });
    info.status = response?.status() ?? null;
    await page.addStyleTag({ content: 'html{scroll-behavior:auto !important}' });
    await page.evaluate(settlePage);
    await page.waitForLoadState('networkidle', { timeout: 5000 }).catch(() => {});
    const capture = () =>
      page.screenshot({
        fullPage: true,
        animations: 'disabled',
        caret: 'hide',
        scale: 'css',
        mask: masks.map((selector) => page.locator(selector)),
        maskColor: MASK_COLOR,
      });
    let previous = null;
    let current = null;
    while (info.attempts < 5) {
      current = await capture();
      info.attempts++;
      if (previous && previous.equals(current)) {
        info.stable = true;
        break;
      }
      previous = current;
      await page.waitForTimeout(150);
    }
    writeFileSync(file, current);
  } finally {
    await context.close();
  }
  return info;
}

const escapeHtml = (s) =>
  String(s).replace(
    /[&<>"]/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c],
  );

function contactSheet(report) {
  const rows = report.results
    .map((r) => {
      const cell = (side) =>
        r.files?.[side]
          ? `<td><a href="${r.files[side]}"><img loading="lazy" src="${r.files[side]}" alt="${side}"></a></td>`
          : `<td class="none">${side === 'diff' && r.changed === 0 ? 'identical' : 'n/a'}</td>`;
      const state = r.error || r.flaky ? 'error' : r.changed === 0 ? 'same' : 'changed';
      return (
        `<tr class="${state}"><th>${escapeHtml(r.path)}<br><small>${r.viewport}` +
        `${r.width ? ` · ${r.width}x${r.height}` : ''}</small><br><b>${escapeHtml(r.percent)}</b>` +
        `${r.changed ? `<br><small>rows ${r.top}-${r.bottom}</small>` : ''}` +
        `${r.note ? `<br><small>${escapeHtml(r.note)}</small>` : ''}</th>` +
        `${cell('base')}${cell('head')}${cell('diff')}</tr>`
      );
    })
    .join('\n');
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><title>Visual diff</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{background:#111;color:#ddd;font:14px/1.4 system-ui,sans-serif;margin:16px}
table{border-collapse:collapse;width:100%}
th,td{border:1px solid #333;padding:8px;vertical-align:top;text-align:left}
th{width:16em;font-weight:400}
img{width:100%;max-width:420px;max-height:640px;object-fit:cover;object-position:top;display:block}
tr.changed b{color:#ff6ad5}tr.error b{color:#ff6a6a}tr.same b{color:#7c7}
.none{color:#777}
.banner{background:#ff00ff;color:#000;font-weight:700;padding:12px;font-size:18px}
</style></head><body>
<h1>Visual diff</h1>
${report.sameBuild ? `<p class="banner">${SAME_BUILD_MARKER}: the same build is on both sides, so this shows render noise only and is not before/after evidence.</p>` : ''}
<p>base <code>${escapeHtml(report.baseId)}</code> vs head <code>${escapeHtml(report.headId)}</code>
 · ${report.results.length} captures · ${report.summary.changed} changed · ${report.summary.errors} errors
 · ${report.summary.reshot} re-shot · ${report.summary.flaky} flaky${report.threshold ? ` · threshold ${report.threshold}` : ''}
 · masked: <code>${escapeHtml(report.masks.join(', '))}</code></p>
<table><tr><th>page</th><th>base</th><th>head</th><th>diff</th></tr>
${rows}
</table></body></html>
`;
}

function markdownTable(report) {
  const lines = [];
  if (report.sameBuild) {
    lines.push(
      `**${SAME_BUILD_MARKER}**: the same build is on both sides, so this table shows render noise only.`,
      '',
    );
  }
  if (report.threshold > 0) {
    lines.push(`Per-channel differences up to ${report.threshold} are treated as equal.`, '');
  }
  lines.push('| page | viewport | changed % |', '|---|---|---|');
  for (const r of report.results) {
    lines.push(`| \`${r.path}\` | ${r.viewport} | ${r.percent}${r.note ? ` (${r.note})` : ''} |`);
  }
  const s = report.summary;
  if (s.reshot > 0) {
    lines.push(
      '',
      `${s.reshot} capture(s) were shot again after an earlier pair differed` +
        `${s.flaky > 0 ? `; ${s.flaky} of them by more than raster noise (FLAKY)` : ''}.`,
    );
  }
  return `${lines.join('\n')}\n`;
}

async function runShots(opts) {
  const baseOrigin = parseOrigin(opts.base, 'base');
  const headOrigin = parseOrigin(opts.head, 'head');
  if (!opts.baseId || !opts.headId) throw new UsageError('--base-id and --head-id are required');
  if (!opts.out) throw new UsageError('--out is required');
  if (opts.baseId === opts.headId && !opts.allowSameBuild) {
    throw new UsageError(
      `base and head build ids are equal (${opts.baseId}): refusing to compare a build with itself. ` +
        'Build the two sides with distinct SITE_BUILD_ID values.',
    );
  }
  await assertBuildId(baseOrigin, opts.baseId, 'base');
  await assertBuildId(headOrigin, opts.headId, 'head');

  const paths = [...opts.paths];
  if (opts.pathsFile) {
    for (const line of readFileSync(opts.pathsFile, 'utf8').split('\n')) {
      const p = line.trim();
      if (p && !p.startsWith('#')) paths.push(p);
    }
  }
  if (paths.length === 0) paths.push(...DEFAULT_PATHS);
  for (const p of paths) {
    if (!p.startsWith('/')) throw new UsageError(`path must start with "/": ${p}`);
  }
  const viewports = opts.viewports
    ? VIEWPORTS.filter((v) => opts.viewports.includes(v.name))
    : VIEWPORTS;
  if (viewports.length === 0) throw new UsageError('--viewports: use desktop, tablet, phone');
  const masks = [...DEFAULT_MASKS, ...opts.masks];

  const out = resolve(opts.out);
  for (const sub of ['base', 'head', 'diff']) mkdirSync(join(out, sub), { recursive: true });

  const { chromium } = await import('playwright');
  const { default: sharp } = await import('sharp');
  const decode = async (file) => {
    const { data, info } = await sharp(file)
      .ensureAlpha()
      .raw()
      .toBuffer({ resolveWithObject: true });
    return { data, width: info.width, height: info.height };
  };

  const jobs = [];
  for (const path of paths) for (const viewport of viewports) jobs.push({ path, viewport });
  const results = new Array(jobs.length);
  const browser = await chromium.launch();
  let next = 0;
  const worker = async () => {
    while (next < jobs.length) {
      const index = next++;
      const { path, viewport } = jobs[index];
      const name = `${slugForPath(path)}--${viewport.name}.png`;
      const result = { path, viewport: viewport.name };
      try {
        const baseFile = join(out, 'base', name);
        const headFile = join(out, 'head', name);
        // A handful of antialiased edge pixels can differ by a rounding step
        // between two captures of the same page. Only a pair that differs in
        // that way is shot again (twice at most) and the closest pair kept.
        // Every round is recorded, and a discarded round that differed is
        // reported with the result, so an intermittent render cannot hide.
        let best = null;
        const roundLog = [];
        do {
          const tmpBase = join(out, 'base', `.tmp-${name}`);
          const tmpHead = join(out, 'head', `.tmp-${name}`);
          const b = await shootOne(
            browser,
            baseOrigin,
            path,
            viewport,
            masks,
            tmpBase,
            opts.timeout,
          );
          const h = await shootOne(
            browser,
            headOrigin,
            path,
            viewport,
            masks,
            tmpHead,
            opts.timeout,
          );
          const imgA = await decode(tmpBase);
          const imgB = await decode(tmpHead);
          const cmp = comparePixels(imgA, imgB, opts.threshold);
          roundLog.push({
            changed: cmp.changed,
            maxDelta: cmp.maxDelta,
            sameSize: cmp.sameSize,
            size: { base: `${imgA.width}x${imgA.height}`, head: `${imgB.width}x${imgB.height}` },
            noise: isRasterNoise(cmp),
          });
          if (!best || cmp.changed < best.cmp.changed) {
            renameSync(tmpBase, baseFile);
            renameSync(tmpHead, headFile);
            best = { b, h, imgA, imgB, cmp, index: roundLog.length - 1 };
          } else {
            rmSync(tmpBase);
            rmSync(tmpHead);
          }
        } while (shouldReshoot(roundLog));
        const { b, h, imgA, imgB, cmp } = best;
        const rounds = roundLog.length;
        const discarded = summarizeRounds(roundLog, best.index);
        roundLog.forEach((r, i) => (r.kept = i === best.index));
        Object.assign(result, {
          status: { base: b.status, head: h.status },
          blockedRequests: { base: b.blocked, head: h.blocked },
          stable: b.stable && h.stable,
          size: {
            base: `${imgA.width}x${imgA.height}`,
            head: `${imgB.width}x${imgB.height}`,
          },
          width: cmp.width,
          height: cmp.height,
          changed: cmp.changed,
          total: cmp.total,
          maxDelta: cmp.maxDelta,
          rounds,
          roundLog,
          reshot: discarded.reshot,
          flaky: discarded.flaky,
          percent: formatPercent(cmp.changed, cmp.total),
          top: cmp.top,
          bottom: cmp.bottom,
          files: { base: `base/${name}`, head: `head/${name}` },
        });
        const notes = [];
        if (b.status !== h.status) notes.push(`status ${b.status} vs ${h.status}`);
        if (imgA.height !== imgB.height) notes.push(`height ${imgA.height} -> ${imgB.height}`);
        if (!result.stable) notes.push('capture not stable');
        if (cmp.changed > 0) notes.push(`${cmp.changed} px, max channel delta ${cmp.maxDelta}`);
        if (discarded.note) notes.push(discarded.note);
        if (notes.length) result.note = notes.join('; ');
        if (cmp.changed > 0) {
          await sharp(renderDiff(imgA, cmp), {
            raw: { width: cmp.width, height: cmp.height, channels: 4 },
          })
            .png()
            .toFile(join(out, 'diff', name));
          result.files.diff = `diff/${name}`;
        }
      } catch (err) {
        Object.assign(result, {
          error: String(err.message ?? err).split('\n')[0],
          changed: null,
          percent: 'error',
          note: String(err.message ?? err).split('\n')[0],
        });
      }
      results[index] = result;
      console.error(
        `${result.percent.padStart(7)}  ${viewport.name.padEnd(7)} ${path}` +
          `${result.reshot ? `  [${result.note}]` : ''}`,
      );
    }
  };
  try {
    await Promise.all(Array.from({ length: Math.max(1, opts.jobs) }, worker));
  } finally {
    await browser.close();
  }

  const report = {
    baseId: opts.baseId,
    headId: opts.headId,
    sameBuild: opts.baseId === opts.headId,
    threshold: opts.threshold,
    masks,
    viewports: viewports.map((v) => `${v.name} ${v.width}x${v.height}`),
    summary: {
      captures: results.length,
      changed: results.filter((r) => r.changed > 0).length,
      errors: results.filter((r) => r.error).length,
      unstable: results.filter((r) => r.stable === false).length,
      reshot: results.filter((r) => r.reshot).length,
      flaky: results.filter((r) => r.flaky).length,
    },
    results,
  };
  writeFileSync(join(out, 'report.json'), `${JSON.stringify(report, null, 2)}\n`);
  writeFileSync(join(out, 'table.md'), markdownTable(report));
  writeFileSync(join(out, 'index.html'), contactSheet(report));

  process.stdout.write(markdownTable(report));
  const s = report.summary;
  console.log(
    `\n${s.captures} captures, ${s.changed} changed, ${s.errors} errors, ${s.unstable} unstable, ` +
      `${s.reshot} re-shot, ${s.flaky} flaky` +
      `${report.sameBuild ? `\n${SAME_BUILD_MARKER}: same build on both sides, this measures render noise only` : ''}`,
  );
  console.log(`contact sheet: ${join(out, 'index.html')}`);
  if (s.errors > 0) return 2;
  if (opts.failOnDiff && (s.changed > 0 || s.unstable > 0 || s.flaky > 0)) return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// CLI

const USAGE = `usage:
  node scripts/visual-diff.mjs shots --base <origin> --head <origin> --base-id <id> --head-id <id>
       --out <dir> [--paths <file>] [--viewports desktop,tablet,phone] [--mask <selector>]...
       [--threshold <0-255>] [--jobs <n>] [--timeout <ms>] [--fail-on-diff] [--allow-same-build] [path...]
  node scripts/visual-diff.mjs dist --base <dist dir> --head <dist dir> [--limit <n>] [--json <file>]
       [--allow-same-build]`;

function parseArgs(argv) {
  const [mode, ...rest] = argv;
  const opts = {
    mode,
    paths: [],
    masks: [],
    threshold: 0,
    jobs: 3,
    timeout: 45000,
    limit: 20,
    failOnDiff: false,
    allowSameBuild: false,
  };
  const value = (i, flag) => {
    if (i >= rest.length) throw new UsageError(`${flag} needs a value`);
    return rest[i];
  };
  for (let i = 0; i < rest.length; i++) {
    const arg = rest[i];
    if (arg === '--base') opts.base = value(++i, arg);
    else if (arg === '--head') opts.head = value(++i, arg);
    else if (arg === '--base-id') opts.baseId = value(++i, arg);
    else if (arg === '--head-id') opts.headId = value(++i, arg);
    else if (arg === '--out') opts.out = value(++i, arg);
    else if (arg === '--paths') opts.pathsFile = value(++i, arg);
    else if (arg === '--viewports') opts.viewports = value(++i, arg).split(',');
    else if (arg === '--mask') opts.masks.push(value(++i, arg));
    else if (arg === '--threshold') opts.threshold = Number(value(++i, arg));
    else if (arg === '--jobs') opts.jobs = Number(value(++i, arg));
    else if (arg === '--timeout') opts.timeout = Number(value(++i, arg));
    else if (arg === '--limit') opts.limit = Number(value(++i, arg));
    else if (arg === '--json') opts.json = value(++i, arg);
    else if (arg === '--fail-on-diff') opts.failOnDiff = true;
    else if (arg === '--allow-same-build') opts.allowSameBuild = true;
    else if (arg.startsWith('--')) throw new UsageError(`unknown option ${arg}`);
    else opts.paths.push(arg);
  }
  if (!Number.isInteger(opts.threshold) || opts.threshold < 0 || opts.threshold > 255) {
    throw new UsageError('--threshold: an integer from 0 to 255');
  }
  if (!Number.isInteger(opts.jobs) || opts.jobs < 1)
    throw new UsageError('--jobs: a positive integer');
  if (!Number.isInteger(opts.timeout) || opts.timeout < 1) {
    throw new UsageError('--timeout: a positive number of milliseconds');
  }
  return opts;
}

export async function main(argv) {
  try {
    const opts = parseArgs(argv);
    if (opts.mode === 'shots') {
      if (!opts.base || !opts.head) throw new UsageError('--base and --head are required');
      return await runShots(opts);
    }
    if (opts.mode === 'dist') {
      if (!opts.base || !opts.head) throw new UsageError('--base and --head are required');
      const result = distDiff(opts.base, opts.head, { allowSameBuild: opts.allowSameBuild });
      if (opts.json) writeFileSync(opts.json, `${JSON.stringify(result, null, 2)}\n`);
      printDistDiff(result, opts.limit);
      return result.empty ? 0 : 1;
    }
    throw new UsageError(mode(opts.mode));
  } catch (err) {
    if (err instanceof UsageError) {
      console.error(`visual-diff: ${err.message}\n\n${USAGE}`);
      return 2;
    }
    throw err;
  }
}

const mode = (m) => (m ? `unknown mode "${m}"` : 'a mode is required: shots or dist');

if (process.argv[1] && import.meta.url === pathToFileURL(realpathSync(process.argv[1])).href) {
  process.exitCode = await main(process.argv.slice(2));
}
