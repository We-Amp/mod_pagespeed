// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// render-references.mjs — renders the generated reference data
// (src/data/reference/*.json, written by generate-references.mjs) into the
// marked regions of the two configuration pages:
//
//   src/content/docs/configuration.md
//     <!-- generated:begin module-directives --> … <!-- generated:end module-directives -->
//   src/content/docs/worker-configuration.md
//     nginx-compat, thin-module-directives, worker-flags
//
// Each directive or flag becomes one fixed block (heading, Syntax, Default,
// Context, description), in the shape nginx.org uses for its directive
// reference. Prose the source does not carry — operator notes, examples,
// caveats — lives in src/data/reference/notes/<region>.md under a `## <name>`
// heading per directive and is appended to that block; a note whose name no
// longer exists in the data is an error, so a removed flag cannot leave a
// stale note behind.
//
// An entry that is merged but not in a release yet is listed in
// src/data/reference/next-release.json (region -> name -> the last released
// version when it was written). Its block below the heading is wrapped in a
// `:::next-release{after="X.Y.Z"}` marker, the same marker docs pages use; the
// generated JSON cannot carry it, because the drift gate regenerates that from
// source. test/next-release-markers.test.ts fails once a newer version is
// released, and on a name the data no longer has.
//
// The rendered regions are deterministic; test/reference/rendered-regions.test.ts
// re-renders them and fails when a page's region differs (data or notes
// changed without `npm run gen:references`, or a region was hand-edited).
//
// Usage:
//   node scripts/render-references.mjs            # rewrite the regions in place
//   node scripts/render-references.mjs --check    # exit 1 if a page is stale

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import {
  NGINX_APT_DISTROS,
  STOCK_NGINX_ALMA,
  SIDECAR_NGINX_VERSION,
} from '../src/data/product-facts.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const WEBSITE_ROOT = resolve(__dirname, '..');
export const REFERENCE_DIR = resolve(WEBSITE_ROOT, 'src/data/reference');
export const NOTES_DIR = resolve(REFERENCE_DIR, 'notes');
export const DOCS_DIR = resolve(WEBSITE_ROOT, 'src/content/docs');
export const DOCKER_NGINX_VERSION_FILE = resolve(WEBSITE_ROOT, '..', 'docker/nginx-version.txt');

/** Which regions each page carries. */
export const PAGE_REGIONS = {
  'configuration.md': ['module-directives'],
  'worker-configuration.md': ['nginx-compat', 'thin-module-directives', 'worker-flags'],
};

const readJson = (name) => JSON.parse(readFileSync(resolve(REFERENCE_DIR, name), 'utf8'));

// ---------------------------------------------------------------------------
// Notes: `## <name>` sections of src/data/reference/notes/<region>.md
// ---------------------------------------------------------------------------

export function loadNotes(region) {
  const path = resolve(NOTES_DIR, `${region}.md`);
  const notes = new Map();
  if (!existsSync(path)) return notes;
  const src = readFileSync(path, 'utf8');
  let current = null;
  for (const line of src.split('\n')) {
    const h = /^## (\S.*)$/.exec(line);
    if (h) {
      current = h[1].trim();
      notes.set(current, []);
      continue;
    }
    if (current !== null) notes.get(current).push(line);
  }
  for (const [k, lines] of notes) notes.set(k, lines.join('\n').trim());
  return notes;
}

// ---------------------------------------------------------------------------
// Markdown helpers
// ---------------------------------------------------------------------------

/** Source help text → page prose: copy rules and markdown safety. */
export function prose(text) {
  // "daemon" is "worker" in customer-facing copy, except inside a path or
  // identifier such as the /v1/daemon/* API routes.
  return text
    .replace(/(?<![/\w.-])daemon's(?![\w/-])/g, "worker's")
    .replace(/(?<![/\w.-])daemons(?![\w/-])/g, 'workers')
    .replace(/(?<![/\w.-])daemon(?![\w/-])/g, 'worker')
    .replace(/\s*See docs\/[\w./-]+\.md\.?/g, '')
    .split(/(`[^`]*`)/)
    .map((part, i) =>
      // Odd parts are code spans: markdown shows them verbatim, so an escape
      // there would appear literally (`&lt;Directory&gt;`).
      i % 2
        ? part
        : part
            .replace(/\\/g, '\\\\')
            .replace(/</g, '&lt;')
            .replace(/>/g, '&gt;')
            .replace(/\*/g, '\\*'),
    )
    .join('')
    .trim();
}

function fmtDefault(value) {
  if (value === null || value === undefined) return '—';
  if (value === '') return '`(empty)`';
  return `\`${value}\``;
}

export function headingId(name) {
  return name.replace(/[^A-Za-z0-9_-]/g, '').toLowerCase();
}

const areaId = (area) => `area-${area.toLowerCase().replace(/[^a-z0-9]+/g, '-')}`;

/** A Prettier-shaped markdown table (padded cells, dashed separator). */
export function table(header, rows) {
  const all = [header, ...rows];
  const widths = header.map((_, i) => Math.max(...all.map((r) => r[i].length)));
  const line = (cells) => `| ${cells.map((c, i) => c.padEnd(widths[i])).join(' | ')} |`;
  return [
    line(header),
    `| ${widths.map((w) => '-'.repeat(w)).join(' | ')} |`,
    ...rows.map(line),
  ].join('\n');
}

function withNotes(block, notes, key) {
  const note = notes.get(key);
  return note ? `${block}\n\n${note}` : block;
}

/** src/data/reference/next-release.json: { region: { name: after } }. */
export function loadNextRelease() {
  const path = resolve(REFERENCE_DIR, 'next-release.json');
  if (!existsSync(path)) return {};
  const regions = JSON.parse(readFileSync(path, 'utf8'));
  delete regions._comment;
  return regions;
}

/**
 * Wrap a block, its `####` heading included, in a next-release marker when the
 * entry is in `marks` (one region of next-release.json).
 */
function withNextRelease(block, marks, key) {
  const after = marks[key];
  if (!after) return block;
  return `:::next-release{after="${after}"}\n${block.trim()}\n:::`;
}

/**
 * The comma-separated links that index a list of entries, and their count.
 * Entries in `marks` are not released yet, so they stay out of both; their
 * block is still rendered, inside its marker.
 */
const released = (list, marks) => list.filter((e) => !marks[e.name]);
const indexLinks = (list, marks) =>
  released(list, marks)
    .map((e) => `[\`${e.name}\`](#${headingId(e.name)})`)
    .join(', ');

/**
 * Regions whose entries cannot be marked. The module-directives region is
 * flat on purpose: rehype-directive-blocks and the directive finder treat
 * each `h4` and the siblings after it as one directive, and a marker would put
 * the heading inside an aside, out of that run.
 */
const UNMARKABLE_REGIONS = new Set(['module-directives']);

/**
 * The next-release entries of one region; a name the data lacks, or an entry
 * in a region that cannot be marked, is an error.
 */
export function nextReleaseMarks(region, names, regions = loadNextRelease()) {
  const marks = regions[region] ?? {};
  if (UNMARKABLE_REGIONS.has(region) && Object.keys(marks).length) {
    throw new Error(
      `next-release.json lists ${region} entries (${Object.keys(marks).join(', ')}), but that region cannot be marked: the directive finder needs its flat headings. Describe the unreleased directive in a marker on a docs page instead.`,
    );
  }
  const known = new Set(names);
  const orphans = Object.keys(marks).filter((k) => !known.has(k));
  if (orphans.length) {
    throw new Error(`next-release.json names unknown ${region} entries: ${orphans.join(', ')}`);
  }
  return marks;
}

function assertNoOrphanNotes(region, notes, names) {
  const known = new Set(names);
  const orphans = [...notes.keys()].filter((k) => !known.has(k));
  if (orphans.length) {
    throw new Error(`notes/${region}.md has notes for unknown entries: ${orphans.join(', ')}`);
  }
}

// ---------------------------------------------------------------------------
// Regions
// ---------------------------------------------------------------------------

export function renderModuleDirectives() {
  const data = readJson('module-directives.json');
  const notes = loadNotes('module-directives');
  assertNoOrphanNotes(
    'module-directives',
    notes,
    data.directives.map((d) => d.name),
  );
  const marks = nextReleaseMarks(
    'module-directives',
    data.directives.map((d) => d.name),
  );
  const byArea = new Map(data.areas.map((a) => [a, []]));
  for (const d of data.directives) {
    if (!byArea.has(d.area)) byArea.set(d.area, []);
    byArea.get(d.area).push(d);
  }
  const out = [];
  const nav = [...byArea.entries()]
    .filter(([, list]) => list.length)
    .map(([area, list]) => `[${area}](#${areaId(area)}) (${released(list, marks).length})`);
  const { repo, tag } = data.source;
  out.push(
    `Generated from the mod_pagespeed source at tag [\`${tag}\`](${repo}/tree/${tag}): ${released(data.directives, marks).length} directives.`,
    '',
  );
  out.push(`Areas: ${nav.join(' · ')}.`, '');
  for (const [area, list] of byArea) {
    if (!list.length) continue;
    out.push(`### ${area} {#${areaId(area)}}`, '');
    out.push(indexLinks(list, marks), '');
    for (const d of list) {
      out.push(withNextRelease(withNotes(directiveBlock(d), notes, d.name), marks, d.name), '');
    }
  }
  return out.join('\n').trim();
}

const PLATFORM_ONLY = { apache: 'Apache only', nginx: 'nginx only', iis: 'IIS only' };

function directiveBlock(d) {
  const lines = [];
  const title = d.name === 'ModPagespeed' ? 'pagespeed / ModPagespeed' : d.name;
  lines.push(`#### ${title} {#${headingId(d.name)}}`, '');
  const syn = [];
  if (d.syntax.apache) syn.push(`\`${d.syntax.apache}\` (Apache)`);
  if (d.syntax.nginx) syn.push(`\`${d.syntax.nginx}\` (nginx)`);
  if (d.syntax.iis) syn.push(`\`${d.syntax.iis}\` (IIS)`);
  lines.push(`- **Syntax:** ${syn.join(' · ')}`);
  lines.push(
    `- **Default:** ${fmtDefault(d.default)}${
      d.defaultIis !== undefined ? `; IIS: ${fmtDefault(d.defaultIis)}` : ''
    }`,
  );
  const ctx = [];
  if (d.context.apache) ctx.push(`Apache: ${d.context.apache.join(', ')}`);
  if (d.context.nginx) ctx.push(`nginx: ${d.context.nginx.join(', ')}`);
  if (d.context.iis) ctx.push(`IIS: ${d.context.iis.join(', ')}`);
  if (d.context.query) ctx.push('also per request (query parameter)');
  lines.push(`- **Context:** ${ctx.join(' · ')}`);
  if (d.platforms.length === 1) {
    lines.push(`- **Platform:** ${PLATFORM_ONLY[d.platforms[0]]}`);
  }
  if (d.since) lines.push(`- **Since:** ${d.since}`);
  lines.push('');
  if (d.description) {
    lines.push(prose(d.description));
  } else {
    lines.push('_The module source carries no help text for this option._');
  }
  if (d.descriptionByPlatform?.nginx) {
    lines.push('', `On nginx: ${prose(d.descriptionByPlatform.nginx)}`);
  }
  return lines.join('\n');
}

const FLAG_SECTION_TITLES = {
  Options: 'General options',
  'Windows service': 'Windows service',
  'Cache Key Normalization': 'Cache key normalization',
  'SVG Auto-Vectorization': 'SVG auto-vectorization',
  'Agent Optimize (EXPERIMENTAL — off by default)': 'Agent optimize (experimental, off by default)',
  'Web Bot Auth (observe-only — off by default)': 'Web Bot Auth (observe-only, off by default)',
  'RSL-CAP Enforcement (EXPERIMENTAL — off by default)':
    'RSL-CAP enforcement (experimental, off by default)',
};

export function renderWorkerFlags() {
  const data = readJson('worker-flags.json');
  const notes = loadNotes('worker-flags');
  assertNoOrphanNotes(
    'worker-flags',
    notes,
    data.flags.map((f) => f.name),
  );
  const marks = nextReleaseMarks(
    'worker-flags',
    data.flags.map((f) => f.name),
  );
  const sections = new Map();
  for (const f of data.flags) {
    if (!sections.has(f.section)) sections.set(f.section, []);
    sections.get(f.section).push(f);
  }
  const out = [];
  const sectionId = (s) =>
    `flags-${(FLAG_SECTION_TITLES[s] ?? s)
      .toLowerCase()
      .replace(/[^a-z0-9]+/g, '-')
      .replace(/-+$/, '')}`;
  out.push(
    `Sections: ${[...sections.entries()]
      .map(
        ([s, list]) =>
          `[${FLAG_SECTION_TITLES[s] ?? s}](#${sectionId(s)}) (${released(list, marks).length})`,
      )
      .join(' · ')}.`,
    '',
  );
  for (const [section, list] of sections) {
    out.push(`### ${FLAG_SECTION_TITLES[section] ?? section} {#${sectionId(section)}}`, '');
    out.push(indexLinks(list, marks), '');
    for (const f of list) {
      out.push(withNextRelease(withNotes(flagBlock(f), notes, f.name), marks, f.name), '');
    }
  }
  return out.join('\n').trim();
}

function flagBlock(f) {
  const lines = [];
  lines.push(`#### ${f.name} {#${headingId(f.name)}}`, '');
  lines.push(`- **Syntax:** \`${f.name}${f.arg ? ` ${f.arg}` : ''}\``);
  lines.push(`- **Default:** ${fmtDefault(f.default)}`);
  if (f.platform === 'windows') lines.push('- **Platform:** Windows service only');
  lines.push('', prose(f.description));
  return lines.join('\n');
}

const THIN_PLACEHOLDERS = {
  pagespeed_cache_path: 'path',
  pagespeed_disallow: 'pattern',
  pagespeed_hot_threshold: 'count',
  pagespeed_agent_content_signal: 'value | off',
};

export function renderThinModuleDirectives() {
  const data = readJson('thin-module-directives.json');
  const notes = loadNotes('thin-module-directives');
  assertNoOrphanNotes(
    'thin-module-directives',
    notes,
    data.directives.map((d) => d.name),
  );
  const marks = nextReleaseMarks(
    'thin-module-directives',
    data.directives.map((d) => d.name),
  );
  const out = [];
  out.push(indexLinks(data.directives, marks), '');
  for (const d of data.directives) {
    const arg =
      THIN_PLACEHOLDERS[d.name] ??
      (d.type === 'flag'
        ? 'on | off'
        : d.type === 'enum'
          ? d.values.join(' | ')
          : d.type === 'number'
            ? 'seconds'
            : 'value');
    const lines = [];
    lines.push(`#### ${d.name} {#${headingId(d.name)}}`, '');
    lines.push(`- **Syntax:** \`${d.name} ${arg};\``);
    lines.push(
      `- **Default:** ${fmtDefault(d.default)}${d.defaultNote ? ` (${d.defaultNote})` : ''}`,
    );
    lines.push(`- **Context:** ${d.context.join(', ')}`);
    out.push(withNextRelease(withNotes(lines.join('\n'), notes, d.name), marks, d.name), '');
  }
  return out.join('\n').trim();
}

export function renderNginxCompat() {
  const dockerNginx = readFileSync(DOCKER_NGINX_VERSION_FILE, 'utf8').trim();
  const rows = [
    [
      'Docker / reverse proxy (`ghcr.io/we-amp/pagespeed-nginx`, the combined image, Helm)',
      `nginx ${dockerNginx}, bundled in the image`,
      'Thin module + worker',
    ],
    ...NGINX_APT_DISTROS.map((d) => [
      `Native module package, ${d.distro}`,
      `nginx ${d.nginx} (the distribution's stock nginx; exact-version pin)`,
      'Native module',
    ]),
    [
      'Native module package, AlmaLinux 9 (yum)',
      `nginx ${STOCK_NGINX_ALMA} (stock nginx; exact-version pin)`,
      'Native module',
    ],
    [
      'NuGet sidecar `WeAmp.PageSpeed.Sidecar`',
      `nginx ${SIDECAR_NGINX_VERSION}, bundled in the package`,
      'Thin module + worker',
    ],
  ];
  return table(['Shape', 'nginx version', 'Configuration surface'], rows);
}

export function renderRegions() {
  return {
    'module-directives': renderModuleDirectives(),
    'worker-flags': renderWorkerFlags(),
    'thin-module-directives': renderThinModuleDirectives(),
    'nginx-compat': renderNginxCompat(),
  };
}

export const REGION_RE = /<!-- generated:begin ([a-z-]+) -->\n[\s\S]*?<!-- generated:end \1 -->/g;

/** Replace every marked region in `markdown` with its rendered content. */
export function applyRegions(markdown, regions) {
  const seen = [];
  const out = markdown.replace(REGION_RE, (whole, name) => {
    seen.push(name);
    if (!(name in regions)) throw new Error(`unknown generated region: ${name}`);
    // Blank lines around the content keep the page Prettier-stable.
    return `<!-- generated:begin ${name} -->\n\n${regions[name]}\n\n<!-- generated:end ${name} -->`;
  });
  return { out, seen };
}

export function renderPage(file, regions = renderRegions()) {
  const path = resolve(DOCS_DIR, file);
  const current = readFileSync(path, 'utf8');
  const { out, seen } = applyRegions(current, regions);
  const expected = PAGE_REGIONS[file];
  for (const r of expected) {
    if (!seen.includes(r)) throw new Error(`${file} is missing the generated region ${r}`);
  }
  return { path, current, out };
}

function main(argv) {
  const check = argv.includes('--check');
  const regions = renderRegions();
  let stale = 0;
  for (const file of Object.keys(PAGE_REGIONS)) {
    const { path, current, out } = renderPage(file, regions);
    if (current === out) continue;
    if (check) {
      console.error(`stale: ${file}`);
      stale++;
    } else {
      writeFileSync(path, out);
      console.log(`rendered ${file}`);
    }
  }
  if (check) {
    if (stale) process.exit(1);
    console.log('rendered regions are current');
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
