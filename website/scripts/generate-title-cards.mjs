// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// generate-title-cards.mjs — build-time programmatic OG/social title cards.
//
// Why this exists:
//   Every blog post historically shared `/og-default.png` for both the
//   BlogPosting JSON-LD `image` field and the `og:image` meta tag. Google
//   Search Console flagged the snippet weakness — same image across the
//   entire blog drops CTR and Discover surfacing. Per-post hand-designed
//   cards do not scale. This script generates one PNG per post at
//   `public/og-cards/{slug}.png` plus the fallback `public/og-default.png`,
//   1200x630 (standard OG aspect), using `satori` to render an SVG tree and
//   `@resvg/resvg-js` to rasterize.
//
// Card design (the site's dark instrument tokens):
//   flat #0b0c0e ground, the capability bit-strip mark from public/logo.svg
//   next to a Plex Mono eyebrow (`blog · <Month YYYY>`; the product name on
//   the default card), a 1px tick-scale rule, and the Inter title
//   bottom-anchored. The default card's title is the home page's title
//   tagline. The .webp/.avif siblings of og-default.png are produced from
//   the PNG by scripts/optimize-rasters.mjs.
//
// Frontmatter contract:
//   - If a post sets `coverImage: /path/to/img.png` in frontmatter, the
//     renderer uses that path verbatim and the generator skips it.
//   - Otherwise the generator synthesizes /og-cards/{slug}.png and the
//     renderer points at that file.
//
// Page cards:
//   PAGE_CARDS below lists the .astro pages that carry a card of their own
//   (public/og-cards/{slug}.png, referenced by the page's ogImage prop).
//   They have no source file to compare mtimes against, so they are always
//   rendered and written only when the bytes differ: the output is
//   deterministic for a given font set and script, so a build on a current
//   tree leaves the directory clean.
//
// Determinism:
//   - Every card is rendered on every run and written only when the bytes
//     differ from the committed file. The output is deterministic for a given
//     font set and script, so a build on a current tree (including a fresh
//     clone, where file mtimes are meaningless) leaves `git status` clean.
//     When a post's title or date changes, the build rewrites its card and
//     the new PNG is committed with the post.
//   - Output is committed-friendly via `public/og-cards/` (already part of
//     Astro's static `public` tree — gets copied into `dist/` verbatim).
//
// Devdep-only:
//   This script is invoked from the `prebuild` npm script and runs only at
//   build time. Neither `satori` nor `@resvg/resvg-js` ever ships in a
//   runtime bundle.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import satori from 'satori';
import { Resvg } from '@resvg/resvg-js';
// satori speaks OpenType (TTF/OTF), not WOFF2. Inter ships in this repo as
// a WOFF2 variable font, so we decompress to a TTF buffer in-memory at
// build time. wawoff2 is a thin WASM wrapper around Google's woff2 tool
// and adds ~140KB of devDeps.
import wawoff from 'wawoff2';
import { PRODUCT_DISPLAY_NAME } from '../src/data/product-facts.mjs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const websiteRoot = path.resolve(__dirname, '..');
const blogDir = path.join(websiteRoot, 'src/content/blog');
// satori's vendored opentype.js cannot parse variable-font fvar tables, so
// we use static-weight Inter from @fontsource/inter (devDep) instead of
// the variable woff2 the site ships at runtime. Plex Mono ships in this
// repo as static-weight woff2 (src/assets/fonts/). Both formats need to be
// decompressed to TTF via wawoff2 before satori can consume them.
const fontsourceDir = path.join(websiteRoot, 'node_modules/@fontsource/inter/files');
const fontPaths = {
  inter400: path.join(fontsourceDir, 'inter-latin-400-normal.woff2'),
  inter700: path.join(fontsourceDir, 'inter-latin-700-normal.woff2'),
  plexMono500: path.join(websiteRoot, 'src/assets/fonts/plex-mono-500.woff2'),
};
const outDir = path.join(websiteRoot, 'public/og-cards');
const defaultCardPath = path.join(websiteRoot, 'public/og-default.png');

const WIDTH = 1200;
const HEIGHT = 630;
const PAD = 80;
const CONTENT_WIDTH = WIDTH - 2 * PAD;

// Card palette = the site's dark instrument tokens (see src/styles/global.css).
const colors = {
  ground: '#0b0c0e', // --color-bg-primary
  title: '#e8eaed', // --color-text-body
  eyebrow: '#a7aeb6', // --color-text-muted
  tick: 'rgba(255, 255, 255, 0.12)', // --color-tick
  tickMajor: '#3a3f47', // --color-border-strong
  bitLit: '#5eb1c9', // --color-bit-lit
  bitClear: '#20242a', // --color-bit-clear
};

// Static pages with a card of their own: slug -> file name under og-cards/,
// eyebrow and title as the card shows them. The eyebrow is the page's own
// eyebrow and the title its H1.
const PAGE_CARDS = [
  {
    slug: 'platform',
    eyebrow: 'transform packs',
    title: 'One interceptor, many packs',
  },
  {
    slug: 'platform-consent',
    eyebrow: 'planned · consent enforcement at the origin',
    title: 'Does your site leak before consent?',
  },
  {
    slug: 'platform-edge-seo',
    eyebrow: 'edge SEO at the origin',
    title: 'Technical SEO fixes at serve time, for sites that are not on a CDN',
  },
  {
    slug: 'ai-readability',
    eyebrow: 'AI readability checker · a free tool',
    title: 'Can AI crawlers read your page?',
  },
  {
    slug: 'ai-readability-hosted-builders',
    eyebrow: 'AI readability · a guide',
    title: 'AI readability on Wix, Squarespace and Shopify: what you can change',
  },
];

// Minimal YAML frontmatter parser. The blog frontmatter shape is
// deliberately simple — string/date scalars + a string array for tags. We
// avoid pulling in `gray-matter` for one job.
function parseFrontmatter(src) {
  if (!src.startsWith('---')) return {};
  const end = src.indexOf('\n---', 3);
  if (end === -1) return {};
  const block = src.slice(3, end).trim();
  const data = {};
  for (const rawLine of block.split('\n')) {
    const line = rawLine.replace(/\s+$/, '');
    if (!line || line.startsWith('#')) continue;
    const colonIdx = line.indexOf(':');
    if (colonIdx === -1) continue;
    const key = line.slice(0, colonIdx).trim();
    let value = line.slice(colonIdx + 1).trim();
    if (!value) {
      data[key] = '';
      continue;
    }
    // Strip surrounding quotes.
    if (
      (value.startsWith("'") && value.endsWith("'")) ||
      (value.startsWith('"') && value.endsWith('"'))
    ) {
      value = value.slice(1, -1);
    }
    data[key] = value;
  }
  return data;
}

function formatMonthYear(isoDate) {
  // Frontmatter date is `YYYY-MM-DD`. Avoid TZ shenanigans by parsing
  // components rather than passing through `new Date(string)`.
  const m = /^(\d{4})-(\d{2})-(\d{2})/.exec(isoDate || '');
  if (!m) return '';
  const months = [
    'January',
    'February',
    'March',
    'April',
    'May',
    'June',
    'July',
    'August',
    'September',
    'October',
    'November',
    'December',
  ];
  const monthName = months[Number(m[2]) - 1] ?? '';
  return monthName ? `${monthName} ${m[1]}` : '';
}

// The capability-register mark from public/logo.svg: 8 cells = a byte of the
// 32-bit capability mask, lit to 0xC9 (a real documented mask value).
function bitStrip() {
  const cell = (lit) => ({
    type: 'div',
    props: {
      style: {
        width: 20,
        height: 20,
        borderRadius: 3,
        flexShrink: 0,
        backgroundColor: lit ? colors.bitLit : colors.bitClear,
      },
    },
  });
  const row = (bits) => ({
    type: 'div',
    props: {
      style: { display: 'flex', gap: 4 },
      children: bits.map(cell),
    },
  });
  // 0xC9 = 1100 1001.
  return {
    type: 'div',
    props: {
      style: { display: 'flex', flexDirection: 'column', gap: 4 },
      children: [row([true, true, false, false]), row([true, false, false, true])],
    },
  };
}

// The 1px chemin-de-fer tick scale (.rule-ticked-scale in global.css):
// minor ticks 1x5 every 8px, major ticks 1x10 every 64px. Satori has no
// repeating-linear-gradient, so the ticks are drawn as flex children.
function tickRule() {
  const count = Math.floor((CONTENT_WIDTH - 1) / 8); // 8px pitch, 1px tick + 7px gap
  return {
    type: 'div',
    props: {
      style: { display: 'flex', alignItems: 'flex-end', gap: 7, height: 10 },
      children: Array.from({ length: count }, (_, i) => ({
        type: 'div',
        props: {
          style: {
            width: 1,
            height: i % 8 === 0 ? 10 : 5,
            flexShrink: 0,
            backgroundColor: i % 8 === 0 ? colors.tickMajor : colors.tick,
          },
        },
      })),
    },
  };
}

// Build the satori VDOM. We hand-author it with plain objects (satori
// supports both JSX and the `{ type, props }` object form; the object form
// keeps this file free of a JSX transform).
function buildTree({ title, eyebrow }) {
  return {
    type: 'div',
    props: {
      style: {
        width: WIDTH,
        height: HEIGHT,
        display: 'flex',
        flexDirection: 'column',
        // Flat instrument-black ground; the tick rule and the bit-strip
        // carry the identity, not gradients or shadows.
        backgroundColor: colors.ground,
        padding: PAD,
        boxSizing: 'border-box',
        fontFamily: 'Inter',
        color: colors.title,
      },
      children: [
        // Top row: the bit-strip mark + the Plex Mono eyebrow.
        {
          type: 'div',
          props: {
            style: {
              display: 'flex',
              alignItems: 'center',
              gap: 24,
            },
            children: [
              bitStrip(),
              {
                type: 'div',
                props: {
                  style: {
                    fontFamily: 'IBM Plex Mono',
                    fontSize: 26,
                    fontWeight: 500,
                    letterSpacing: '0.08em',
                    color: colors.eyebrow,
                  },
                  children: eyebrow,
                },
              },
            ],
          },
        },
        {
          type: 'div',
          props: {
            style: { display: 'flex', marginTop: 40 },
            children: [tickRule()],
          },
        },
        // Title, bottom-anchored. Cap at ~3 lines — satori wraps but does
        // not natively truncate; the layout keeps the column wide enough
        // that 3 lines is rare.
        {
          type: 'div',
          props: {
            style: {
              fontSize: 64,
              fontWeight: 700,
              lineHeight: 1.1,
              letterSpacing: '-0.02em',
              color: colors.title,
              marginTop: 'auto',
            },
            children: title,
          },
        },
      ],
    },
  };
}

async function main() {
  async function loadFont(p) {
    if (!fs.existsSync(p)) throw new Error(`font not found at ${p}`);
    const woff2Buf = fs.readFileSync(p);
    // wawoff.decompress returns a Uint8Array (sfnt/TTF bytes).
    const ttfBytes = await wawoff.decompress(woff2Buf);
    return Buffer.from(ttfBytes);
  }
  const inter400 = await loadFont(fontPaths.inter400);
  const inter700 = await loadFont(fontPaths.inter700);
  const plexMono500 = await loadFont(fontPaths.plexMono500);

  // satori treats `weight` as a discrete lookup, so each weight is declared
  // as a separate static-weight font entry.
  const fonts = [
    { name: 'Inter', data: inter400, weight: 400, style: 'normal' },
    { name: 'Inter', data: inter700, weight: 700, style: 'normal' },
    { name: 'IBM Plex Mono', data: plexMono500, weight: 500, style: 'normal' },
  ];

  // Render, then write only when the bytes differ (page cards: no source
  // mtime to compare against). Returns true when the file changed.
  async function renderCardIfChanged(tree, outPath, label) {
    const svg = await satori(tree, { width: WIDTH, height: HEIGHT, fonts });
    const png = new Resvg(svg, { fitTo: { mode: 'width', value: WIDTH } }).render().asPng();
    if (fs.existsSync(outPath) && Buffer.compare(fs.readFileSync(outPath), png) === 0) {
      console.log(`skipped (unchanged): ${label}`);
      return false;
    }
    fs.writeFileSync(outPath, png);
    console.log(`generated: ${label}`);
    return true;
  }

  fs.mkdirSync(outDir, { recursive: true });

  let generated = 0;
  let skipped = 0;

  // The default card (og-default.png): the product name (PRODUCT_DISPLAY_NAME,
  // no version) as the mono eyebrow
  // over the home page's title tagline (src/pages/index.astro).
  // The eyebrow is the product-name constant, so a name change in either
  // facts file re-renders the card.
  if (
    await renderCardIfChanged(
      buildTree({
        eyebrow: PRODUCT_DISPLAY_NAME,
        title: 'PageSpeed module for nginx and Apache',
      }),
      defaultCardPath,
      'og-default.png',
    )
  )
    generated++;
  else skipped++;

  for (const card of PAGE_CARDS) {
    const outPath = path.join(outDir, `${card.slug}.png`);
    const tree = buildTree({ title: card.title, eyebrow: card.eyebrow });
    if (await renderCardIfChanged(tree, outPath, `og-cards/${card.slug}.png`)) generated++;
    else skipped++;
  }

  const entries = fs
    .readdirSync(blogDir)
    .filter((f) => f.endsWith('.md'))
    .sort();

  for (const filename of entries) {
    const slug = filename.replace(/\.md$/, '');
    const srcPath = path.join(blogDir, filename);
    const outPath = path.join(outDir, `${slug}.png`);

    const raw = fs.readFileSync(srcPath, 'utf8');
    const fm = parseFrontmatter(raw);

    // Posts with an explicit coverImage opt out — the operator wants that
    // path used as-is.
    if (fm.coverImage) {
      console.log(`skipped (explicit coverImage): og-cards/${slug}.png`);
      skipped++;
      continue;
    }

    const tree = buildTree({
      title: fm.title || slug,
      eyebrow: ['blog', formatMonthYear(fm.date)].filter(Boolean).join(' · '),
    });

    if (await renderCardIfChanged(tree, outPath, `og-cards/${slug}.png`)) generated++;
    else skipped++;
  }

  console.log(
    `\ntitle cards: ${generated} generated, ${skipped} skipped, ${entries.length + PAGE_CARDS.length + 1} total`,
  );
}

await main().catch((err) => {
  console.error('generate-title-cards failed:', err);
  process.exit(1);
});
