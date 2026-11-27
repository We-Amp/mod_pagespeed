// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// docs-markdown.mjs — the docs collection as Markdown, for agents.
//
// One render path shared by the agent-file generator (scripts/generate-llms.mjs,
// which writes public/llms.txt and public/llms-full.txt at prebuild) and the
// per-page Markdown route (src/pages/docs/[slug].md.ts), so the body an agent
// fetches at /docs/<slug>.md is the body that sits in llms-full.txt.
//
// What the render does to a page's source:
//   - frontmatter off; the page's H1 (`h1`, else the title) becomes the one
//     H1, the description the lede, the canonical URL a line under it; the
//     frontmatter `faq` pairs are appended as the page renders them (a
//     "Frequently asked questions" section)
//   - MDX: `import` and `export const relNN = await getRelease(...)` lines go,
//     `{relNN.path}` expressions and the release-aware components (<Version />,
//     <ImageTag />, <NugetCmd />, <XPageSpeedExample />, <VersionMap />) resolve from the release
//     manifests exactly as the components do; a JSX <pre><code> block becomes a
//     fenced code block; a component that renders no content (the release-notes
//     deep-link helper) is dropped
//   - `:::note[Label] … :::` callouts become blockquotes with a bold title
//   - the release-notes HTML scaffolding (collapsed entries, summary lines, alias
//     anchors) becomes headings and plain text; a platform block keeps its label
//   - `{#custom-id}` heading suffixes go; root-relative and fragment links become
//     absolute https://modpagespeed.com URLs
//   - fenced code is copied byte for byte
//
// Plain ESM on Node built-ins plus the `yaml` dependency: the generator runs
// outside Astro and the route is prerendered at build time, so neither can use
// astro:content. Every function here is deterministic — same tree, same bytes.

import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse as parseYaml } from 'yaml';
import { PKG_ASPNETCORE } from '../data/product-facts.mjs';
import { versionMapMarkdown } from '../data/version-map.mjs';

export const SITE = 'https://modpagespeed.com';

/**
 * @typedef {{
 *   title: string,
 *   h1?: string,
 *   description: string,
 *   order: number,
 *   group?: string,
 *   draft?: boolean,
 *   faq?: Array<{ q: string, a: string }>,
 * }} DocData
 * @typedef {{ slug: string, file: string, ext: 'md' | 'mdx', data: DocData, body: string }} Doc
 * @typedef {{ label: string, docs: Doc[] }} DocGroup
 * @typedef {Record<string, any>} Release
 * @typedef {Record<'1.1' | '2.0' | '2.1', Release>} Manifests
 */

/**
 * The website root. Resolved from this module's location when that is the
 * source tree (the generator, the tests, `astro dev`); the Astro build bundles
 * server code under dist/, where the relative path no longer exists, and runs
 * from the website root, so the working directory is the fallback.
 * @returns {string}
 */
export function websiteRoot() {
  const fromModule = fileURLToPath(new URL('../../', import.meta.url));
  if (existsSync(resolve(fromModule, 'src/content/docs'))) return fromModule;
  return process.cwd();
}

// --- Release manifests ------------------------------------------------------

/**
 * The three release manifests, parsed. The substitution helpers below mirror
 * src/lib/release.ts, which cannot be imported here (it reads astro:content).
 * @param {string} [root]
 * @returns {Manifests}
 */
export function loadManifests(root = websiteRoot()) {
  /** @param {string} line */
  const read = (line) =>
    parseYaml(readFileSync(resolve(root, `src/content/releases-${line}/release.yaml`), 'utf8'));
  return { 1.1: read('1.1'), '2.0': read('2.0'), 2.1: read('2.1') };
}

/**
 * %V → release.semver, %R → release.revision ?? 0, %T → release.tag.
 * @param {string} tmpl
 * @param {Release} rel
 */
export function substituteRelease(tmpl, rel) {
  return tmpl
    .replaceAll('%V', rel.release.semver)
    .replaceAll('%R', String(rel.release.revision ?? 0))
    .replaceAll('%T', rel.release.tag);
}

/** @param {Release} rel */
export function dockerTag(rel) {
  return substituteRelease(rel.artifacts.docker?.tag_template ?? '%V', rel);
}

/**
 * @param {Release} rel
 * @param {string} channel
 * @param {string} arch
 * @param {string} [version] replaces %V; defaults to the release (a module
 *   artifact of a module-only update passes MODULE_PACKAGE_VERSION)
 */
export function artifactUrl(rel, channel, arch, version = rel.release.semver) {
  const tmpl = rel.artifacts[channel]?.[arch];
  if (typeof tmpl !== 'string') {
    throw new Error(
      `No artifact template for channel=${channel} arch=${arch} in the ${rel.product.line} manifest`,
    );
  }
  const filename = substituteRelease(tmpl, {
    ...rel,
    release: { ...rel.release, semver: version },
  });
  // An absolute template (the IIS installer on the GitHub release) is
  // rendered as-is, as in src/lib/release.ts.
  if (filename.startsWith('https://')) return filename;
  return rel.urls.archive_base + filename;
}

/** The `dotnet add package …` line NugetCmd.astro renders. @param {Manifests} m */
export function nugetCommand(m) {
  const rel = m['2.0'];
  const pkg = rel.artifacts.nuget?.package ?? PKG_ASPNETCORE;
  const flags = String(rel.display.nuget_install_flags ?? '').trim();
  return `dotnet add package ${pkg}${flags ? ` ${flags}` : ''}`;
}

// --- The docs collection ----------------------------------------------------

const FRONTMATTER = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/;

/**
 * Every published page of the docs collection, in sidebar order: by `order`,
 * then by slug for a stable tie-break (the directory listing order is not
 * guaranteed). A `draft: true` page is left out.
 * @param {string} [root]
 * @returns {Doc[]}
 */
export function loadDocs(root = websiteRoot()) {
  const dir = resolve(root, 'src/content/docs');
  /** @type {Doc[]} */
  const docs = [];
  for (const file of readdirSync(dir)) {
    const m = file.match(/^(.+)\.(mdx?)$/);
    if (!m) continue;
    const raw = readFileSync(resolve(dir, file), 'utf8');
    const fm = raw.match(FRONTMATTER);
    if (!fm) throw new Error(`src/content/docs/${file}: no frontmatter block`);
    /** @type {DocData} */
    const data = parseYaml(fm[1]);
    if (data.draft) continue;
    docs.push({
      slug: m[1],
      file,
      ext: /** @type {'md' | 'mdx'} */ (m[2]),
      data: { ...data, order: data.order ?? 0 },
      body: raw.slice(fm[0].length),
    });
  }
  docs.sort(
    (a, b) => a.data.order - b.data.order || (a.slug < b.slug ? -1 : a.slug > b.slug ? 1 : 0),
  );
  return docs;
}

/**
 * Consecutive pages with the same `group` form a section, in the order the
 * sorted collection presents them — the sidebar's own grouping rule.
 * @param {Doc[]} docs
 * @returns {DocGroup[]}
 */
export function groupDocs(docs) {
  /** @type {DocGroup[]} */
  const groups = [];
  for (const doc of docs) {
    const label = doc.data.group ?? 'Other';
    const last = groups[groups.length - 1];
    if (last && last.label === label) last.docs.push(doc);
    else groups.push({ label, docs: [doc] });
  }
  return groups;
}

/** @param {string} slug */
export function docUrl(slug) {
  return `${SITE}/docs/${slug}/`;
}

/** @param {string} slug */
export function docMarkdownUrl(slug) {
  return `${SITE}/docs/${slug}.md`;
}

// --- MDX components and expressions -----------------------------------------

/**
 * Parse `name="value"` attributes of a JSX tag.
 * @param {string} attrs
 * @returns {Record<string, string>}
 */
function parseAttrs(attrs) {
  /** @type {Record<string, string>} */
  const out = {};
  for (const m of attrs.matchAll(/([\w-]+)="([^"]*)"/g)) out[m[1]] = m[2];
  return out;
}

/**
 * The text a release-aware component renders, resolved from the manifests the
 * same way the component does (src/components/release/*.astro).
 * @param {string} name
 * @param {Record<string, string>} attrs
 * @param {Manifests} m
 * @returns {string}
 */
function componentText(name, attrs, m) {
  /** @param {string} line */
  const manifest = (line) => {
    const rel = m[/** @type {'1.1' | '2.0' | '2.1'} */ (line)];
    if (!rel) throw new Error(`<${name} line="${line}" />: no release manifest for that line`);
    return rel;
  };
  switch (name) {
    case 'Version': {
      const rel = manifest(attrs.line);
      if (attrs.style === 'badge') return rel.display.badge_version;
      if (attrs.style === 'tag') return rel.release.tag;
      return rel.display.header_version;
    }
    case 'ImageTag':
      return dockerTag(manifest(attrs.line));
    case 'NugetCmd':
      return nugetCommand(m);
    case 'XPageSpeedExample': {
      manifest(attrs.line);
      const surface = attrs.surface ?? 'nginx';
      if (surface === 'middleware' || attrs.line === '2.0') {
        return 'X-PageSpeed: MISS';
      }
      if (surface === 'apache') return 'X-Mod-Pagespeed: on';
      return 'X-Page-Speed: on';
    }
    case 'VersionMap':
      // The version map renders the same rows as a Markdown table.
      return versionMapMarkdown();
    case 'ReleaseNotesDeepLink':
      // A progressive enhancement (opens a collapsed entry a deep link points
      // at); it renders nothing a reader sees, so nothing is lost here.
      return '';
    default:
      throw new Error(
        `unknown MDX component <${name} /> — teach src/lib/docs-markdown.mjs its text form`,
      );
  }
}

const COMPONENT = /<([A-Z][A-Za-z0-9]*)\b([^>]*?)\/>/g;
const EXPORT_RELEASE = /^export const (\w+) = await getRelease\('([^']+)'\);?\s*$/gm;

/**
 * Resolve `{rel21.display.badge_version}` (prose) or `${rel20.display.header_version}`
 * (inside a JSX template literal) against the `export const` bindings.
 * @param {string} text
 * @param {Record<string, string>} bindings  binding name → manifest line
 * @param {Manifests} m
 * @param {boolean} templateLiteral
 */
function resolveExpressions(text, bindings, m, templateLiteral) {
  const re = templateLiteral ? /\$\{(\w+)\.([\w.]+)\}/g : /\{(\w+)\.([\w.]+)\}/g;
  return text.replace(re, (whole, name, path) => {
    const line = bindings[name];
    if (!line) return whole;
    /** @type {any} */
    let value = m[/** @type {'1.1' | '2.0' | '2.1'} */ (line)];
    for (const key of path.split('.')) value = value?.[key];
    if (value === undefined) throw new Error(`${whole}: no such field in the ${line} manifest`);
    return String(value);
  });
}

/**
 * Turn the lines between `<pre>` and `</pre>` of a JSX code block into a fenced
 * code block: template-literal segments and components concatenate in order.
 * @param {string[]} lines
 * @param {Record<string, string>} bindings
 * @param {Manifests} m
 */
function jsxPreToFence(lines, bindings, m) {
  let inner = lines.join('\n');
  const lang = inner.match(/<code class="language-([\w-]+)"/)?.[1] ?? '';
  inner = inner.replace(/^\s*<code\b[^>]*>/, '').replace(/<\/code>\s*$/, '');
  let code = '';
  let last = 0;
  const token = /\{`([\s\S]*?)`\}|<([A-Z][A-Za-z0-9]*)\b([^>]*?)\/>/g;
  for (const t of inner.matchAll(token)) {
    const between = inner.slice(last, t.index).trim();
    if (between) code += between; // bare JSX text, kept rather than lost
    if (t[1] !== undefined) {
      code += resolveExpressions(
        t[1].replace(/\\`/g, '`').replace(/\\\$/g, '$'),
        bindings,
        m,
        true,
      );
    } else {
      code += componentText(t[2], parseAttrs(t[3]), m);
    }
    last = t.index + t[0].length;
  }
  const tail = inner.slice(last).trim();
  if (tail) code += tail;
  let fence = '```';
  while (code.includes(fence)) fence += '`';
  return [`${fence}${lang}`, ...code.replace(/\n$/, '').split('\n'), fence];
}

// --- Inline HTML of the release-notes scaffolding ---------------------------

/** `<a href="x">t</a>` → `[t](x)`, then every remaining tag off. @param {string} html */
function inlineText(html) {
  return html
    .replace(/<a\s+href="([^"]*)"[^>]*>([\s\S]*?)<\/a>/g, '[$2]($1)')
    .replace(/<[^>]+>/g, '')
    .replace(/\s+/g, ' ')
    .trim();
}

/**
 * A release entry's summary line, as the page shows it: the version (bold, a
 * link where the source has one), the date and the part in parentheses, then
 * the one-line summary. Falls back to the plain text when the shape is new.
 * @param {string} html  the inner HTML of <summary> or <p class="rn-summary-line">
 */
function summaryText(html) {
  /** @type {Record<string, string>} */
  const spans = {};
  for (const m of html.matchAll(/<span class="(rn-[a-z]+)[^"]*">([\s\S]*?)<\/span>/g)) {
    spans[m[1]] = inlineText(m[2]);
  }
  if (!spans['rn-ver']) return inlineText(html);
  const meta = [spans['rn-date'], spans['rn-badge']].filter(Boolean).join(', ');
  const head = `**${spans['rn-ver']}**${meta ? ` (${meta})` : ''}`;
  return spans['rn-lede'] ? `${head} — ${spans['rn-lede']}` : head;
}

// --- The render -------------------------------------------------------------

const FENCE = /^(\s*)(`{3,}|~{3,})/;
// `:::name[Label]{attributes}`: the attributes (the `after` of a next-release
// marker) are not part of the Markdown copy.
const CALLOUT_OPEN = /^:::([a-z][a-z-]*)(?:\[([^\]]*)\])?(?:\{[^}]*\})?\s*$/;
const CALLOUT_CLOSE = /^:::\s*$/;
const CALLOUT_TITLES = {
  note: 'Note',
  tip: 'Tip',
  caution: 'Caution',
  warning: 'Warning',
  danger: 'Danger',
  important: 'Important',
  'next-release': 'From the next release',
};

/**
 * Rewrite the Markdown links of one prose line to absolute URLs: a
 * root-relative target to the site, a fragment to the page at `pageUrl`.
 * @param {string} line
 * @param {string} pageUrl  absolute URL of the page the line belongs to
 */
export function absolutizeLinks(line, pageUrl) {
  return line
    .replace(/\]\((\/[^)\s]*)\)/g, `](${SITE}$1)`)
    .replace(/\]\((#[^)\s]*)\)/g, `](${pageUrl}$1)`);
}

/**
 * The Markdown of one page: the H1, the lede, the canonical URL, the processed
 * body and the FAQ section. Pure: the same inputs always give the same text.
 * @param {Doc} doc
 * @param {Manifests} m
 * @returns {string}
 */
export function renderDocMarkdown(doc, m) {
  const { slug, data, body } = doc;
  const mdx = doc.ext === 'mdx';
  /** @type {Record<string, string>} */
  const bindings = {};
  if (mdx) for (const e of body.matchAll(EXPORT_RELEASE)) bindings[e[1]] = e[2];

  /** @type {string[]} */
  const out = [];
  let fence = null; // { indent, marker } while inside a fenced code block
  let inCallout = false;
  /** @type {string[] | null} */
  let pre = null; // the lines of a JSX <pre> block being collected
  /** @type {string[] | null} */
  let img = null; // the lines of a multi-line <img …/> tag being collected

  /**
   * An HTML <img> (the screenshots some pages carry, written one attribute per
   * line) becomes a Markdown image with an absolute URL.
   * @param {string} tag
   */
  const imageMarkdown = (tag) => {
    const src = tag.match(/\bsrc="([^"]*)"/)?.[1] ?? '';
    const alt = tag.match(/\balt="([^"]*)"/)?.[1] ?? '';
    return absolutizeLinks(`![${alt}](${src})`, docUrl(slug));
  };

  /**
   * Emit a line, inside a blockquote while in a callout. Outside fenced code,
   * runs of blank lines collapse to one.
   * @param {string} line
   */
  const push = (line) => {
    if (!fence && !line && (!out.length || !out[out.length - 1])) return;
    out.push(inCallout ? (line ? `> ${line}` : '>') : line);
  };

  for (const raw of body.replace(/\r\n/g, '\n').split('\n')) {
    if (fence) {
      push(raw);
      const f = raw.match(FENCE);
      if (f && f[2][0] === fence.marker[0] && f[2].length >= fence.marker.length) fence = null;
      continue;
    }
    const line = raw.replace(/\s+$/, '');

    if (pre) {
      if (/^<\/pre>\s*$/.test(line)) {
        for (const l of jsxPreToFence(pre, bindings, m)) push(l);
        pre = null;
      } else pre.push(line);
      continue;
    }
    if (img) {
      img.push(line);
      if (/\/?>\s*$/.test(line)) {
        push(imageMarkdown(img.join(' ')));
        img = null;
      }
      continue;
    }
    if (/^<img\s*$/.test(line)) {
      img = [line];
      continue;
    }
    const inlineImg = line.match(/^<img\s[^>]*\/?>\s*$/);
    if (inlineImg) {
      push(imageMarkdown(line));
      continue;
    }
    const f = line.match(FENCE);
    if (f) {
      fence = { indent: f[1], marker: f[2] };
      push(line);
      continue;
    }
    if (mdx && /^<pre>\s*$/.test(line)) {
      pre = [];
      continue;
    }
    const open = line.match(CALLOUT_OPEN);
    if (open && !inCallout && open[1] in CALLOUT_TITLES) {
      inCallout = true;
      const title =
        open[2]?.trim() || CALLOUT_TITLES[/** @type {keyof typeof CALLOUT_TITLES} */ (open[1])];
      push(`**${title}**`);
      push('');
      continue;
    }
    if (inCallout && CALLOUT_CLOSE.test(line)) {
      inCallout = false;
      out.push('');
      continue;
    }

    let text = line;
    if (mdx) {
      if (/^import\s[^;]*\sfrom\s+['"][^'"]+['"];?\s*$/.test(text)) continue;
      if (/^export const \w+ = await getRelease\('[^']+'\);?\s*$/.test(text)) continue;
      if (/^\{\/\*[\s\S]*\*\/\}\s*$/.test(text)) continue;
      text = text.replace(COMPONENT, (_, name, attrs) => componentText(name, parseAttrs(attrs), m));
      text = resolveExpressions(text, bindings, m, false);
      if (!text.trim() && line.trim()) continue;
    }

    // Release-notes scaffolding and other presentational HTML.
    text = text
      .replace(/<span id="[^"]*" class="rn-alias"[^>]*><\/span>/g, '')
      .replace(/<a id="[^"]*"><\/a>/g, '');
    if (!text.trim() && line.trim()) continue;
    const platform = text.match(
      /^<div data-platform="([^"]*)"(?: data-platform-label="([^"]*)")?>\s*$/,
    );
    if (platform) {
      push(`**${platform[2] || platform[1]}**`);
      continue;
    }
    if (/^<\/?(?:div|details)\b[^>]*>\s*$/.test(text)) continue;
    const summary = text.match(/^<summary>([\s\S]*)<\/summary>\s*$/);
    if (summary) {
      push(absolutizeLinks(`### ${summaryText(summary[1])}`, docUrl(slug)));
      continue;
    }
    const summaryLine = text.match(/^<p class="rn-summary-line">([\s\S]*)<\/p>\s*$/);
    if (summaryLine) {
      push(absolutizeLinks(summaryText(summaryLine[1]), docUrl(slug)));
      continue;
    }

    // Headings: the custom id suffix off; a second H1 demoted so the title stays
    // the only one.
    const heading = text.match(/^(#{1,6})(\s.*?)\s*(?:\{#[^}]+\})?\s*$/);
    if (heading) text = `${heading[1] === '#' ? '##' : heading[1]}${heading[2]}`;

    push(absolutizeLinks(text, docUrl(slug)));
  }

  while (out.length && !out[out.length - 1]) out.pop();
  /** @type {string[]} */
  // The H1 is the one the HTML page shows: `h1` when the page sets one (the
  // sidebar keeps the shorter `title`), else the title.
  const parts = [
    `# ${data.h1 ?? data.title}`,
    '',
    data.description,
    '',
    `Canonical URL: ${docUrl(slug)}`,
    '',
  ];
  parts.push(...out);
  if (data.faq?.length) {
    parts.push('', '## Frequently asked questions', '');
    for (const { q, a } of data.faq)
      parts.push(`**${q}**`, '', absolutizeLinks(a, docUrl(slug)), '');
  }
  // No trailing blank lines, one final newline (blank runs inside the body were
  // already collapsed by push(), fenced code excepted).
  return parts.join('\n').replace(/\s+$/, '').concat('\n');
}
