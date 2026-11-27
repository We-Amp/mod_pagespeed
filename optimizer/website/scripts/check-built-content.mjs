#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Built-output content lint over the production build.
 *
 * Runs after `npm run build` (npm run check:content) over every .html file
 * under dist/ — the pages the node adapter wrote to dist/client/ plus
 * 404/500 — and checks what a search engine sees, not what the source
 * intends:
 *
 *   title-count          exactly one document <title> per page (every page;
 *                        a <title> inside <svg>/<math> is not a document title)
 *   title-length         15–65 chars after entity decoding (indexable pages)
 *   description-present  <meta name="description"> exists (indexable pages)
 *   description-length   50–165 chars after entity decoding (indexable pages)
 *   description-duplicate  no two indexable pages share one description
 *   h1-count             exactly one <h1> per page (every page)
 *   document-structure   the raw token stream is one well-formed document:
 *                        one <html>/<head>/<body> pair in order, nothing but
 *                        whitespace and comments between them and after
 *                        </html>, the <title> inside <head>, and the
 *                        head-only tags (ld+json scripts, stylesheet and
 *                        canonical links, description/robots/viewport metas,
 *                        charset) where a crawler or parser expects them
 *                        (every page)
 *   term-drift-daemon    "daemon"/"daemons" absent from what a search result
 *                        shows — visible text, <title>, description,
 *                        og:title and og:description (indexable pages); the
 *                        process is the worker. <script>, <style>, <code>
 *                        and <pre> contents are stripped from visible text
 *                        first, so code samples may name anything.
 *   product-naming       spellings that never named a product —
 *                        "mod_pagespeed 2.0"/"mod pagespeed 2.0" in any case
 *                        (the 2.0 predecessor is CamelCase "ModPageSpeed
 *                        2.0") and CamelCase "ModPageSpeed 2.1"+ (the
 *                        current line is lowercase mod_pagespeed) — absent
 *                        from visible text, <title> and description (every
 *                        page). The real predecessor names are legitimate
 *                        history and stay allowed.
 *   retired-product-label  the retired "2.1" product label: "mod_pagespeed
 *                        2.1" (any case, underscore or space) used as the
 *                        product's name (every page). Every match is
 *                        counted, on every surface a reader, a search
 *                        engine or a social card shows: visible text,
 *                        <title>, description, og:title, og:description,
 *                        twitter:* metas, alt and aria-label values, and the
 *                        string values of application/ld+json blocks (URL
 *                        values skipped). The product is plain
 *                        "mod_pagespeed"; an exact version ("mod_pagespeed
 *                        2.1.0", "2.5.0") is a release fact and passes, as
 *                        do version-scoped phrases that do not put the
 *                        label after the name ("since 2.1.0", "before
 *                        2.1"), and code (<code>/<pre> are stripped).
 *                        Historical pages that keep the label on purpose are
 *                        exempted in content-lint-allow.json. Pages the
 *                        rename has not reached yet are listed with their
 *                        exact count in retired-label-pending.json: that
 *                        count prints as a warning, and any other count is
 *                        an error (more: new text; fewer: lower the entry),
 *                        so the list only ever shrinks and the sweep
 *                        empties it.
 *   canonical            exactly one <link rel="canonical"> with an absolute
 *                        https://modpagespeed.com URL ending in "/" or a file
 *                        extension (indexable pages)
 *
 * Scope: dist/client/1.0/ is the frozen archive of the old site and is
 * skipped, like the other build-output gates skip it. Pages marked
 * <meta name="robots" content="noindex..."> skip only the rules marked
 * "indexable" above.
 *
 * Redirect stubs — noindex pages whose only job is a <meta http-equiv
 * ="refresh"> to another URL, like Astro's 404/500 stubs and the /go/
 * click-through redirects — skip h1-count and document-structure: they
 * have no content to structure. Other noindex pages (e.g. /error/404/)
 * still get every rule.
 *
 * Justified exceptions live in scripts/content-lint-allow.json as
 * {urlPath: {rule: reason}}: only the named rule is skipped on that page,
 * every other rule still applies. An entry that names an unknown rule, or
 * matches no built page, is an error, so entries cannot outlive the rules
 * and pages they were written for.
 *
 * Every failure prints `<url>  <rule>  <detail>`. Error-level failures exit
 * 1; warn-level rules only print (a rule calibrated to more noise than it is
 * worth is set to warn in RULES below rather than mass-editing copy).
 */
import { readFileSync, readdirSync, existsSync, statSync } from 'node:fs';
import { join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const WEBSITE_ROOT = fileURLToPath(new URL('..', import.meta.url));
const DIST = resolve(WEBSITE_ROOT, 'dist');
const SITE_ORIGIN = 'https://modpagespeed.com';

const TITLE_MIN = 15;
const TITLE_MAX = 65;
const DESCRIPTION_MIN = 50;
const DESCRIPTION_MAX = 165;

/** Rule table. `indexableOnly` rules skip noindex pages. */
export const RULES = {
  'title-count': { level: 'error', indexableOnly: false },
  'title-length': { level: 'error', indexableOnly: true },
  'description-present': { level: 'error', indexableOnly: true },
  'description-length': { level: 'error', indexableOnly: true },
  'description-duplicate': { level: 'error', indexableOnly: true },
  'h1-count': { level: 'error', indexableOnly: false },
  'document-structure': { level: 'error', indexableOnly: false },
  'term-drift-daemon': { level: 'error', indexableOnly: true },
  // Error-level on purpose: both patterns had zero hits when the rule was
  // narrowed (2026-10), because they match only spellings that never named
  // a product line. The CamelCase predecessor "ModPageSpeed 2.0" — 79 hits
  // in visible text at the time, all legitimate history — stays allowed.
  'product-naming': { level: 'error', indexableOnly: false },
  // Error-level for new text; the pages the rename has not reached yet are
  // downgraded to warnings by retired-label-pending.json (see the header).
  'retired-product-label': { level: 'error', indexableOnly: false },
  canonical: { level: 'error', indexableOnly: true },
};

const NAMED_ENTITIES = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  nbsp: ' ',
  mdash: '—',
  ndash: '–',
  hellip: '…',
  rsquo: '’',
  lsquo: '‘',
  ldquo: '“',
  rdquo: '”',
  copy: '©',
  reg: '®',
  trade: '™',
  middot: '·',
};

/** Decode the entities Astro emits into title/description/body text. */
export function decodeEntities(text) {
  return text.replace(/&(#x?[0-9a-f]+|[a-z][a-z0-9]*);/gi, (whole, name) => {
    if (name[0] === '#') {
      const code =
        name[1] === 'x' || name[1] === 'X'
          ? parseInt(name.slice(2), 16)
          : parseInt(name.slice(1), 10);
      return Number.isFinite(code) && code > 0 ? String.fromCodePoint(code) : whole;
    }
    return NAMED_ENTITIES[name.toLowerCase()] ?? whole;
  });
}

/** Collapse whitespace runs to one space and trim — the SERP-visible form. */
export function collapseWhitespace(text) {
  return text.replace(/\s+/g, ' ').trim();
}

const length = (s) => [...s].length;

/**
 * Attribute value of a tag, honoring whichever quote delimiter opened it
 * (Astro leaves a literal ' inside a double-quoted attribute, so a character
 * class of "both quotes" would cut the value short).
 */
function attrValue(tag, name) {
  return new RegExp(`\\b${name}=(["'])([\\s\\S]*?)\\1`, 'i').exec(tag)?.[2] ?? '';
}

/**
 * Elements whose content is raw text, never markup: the scan jumps straight
 * to their matching close tag so markup inside them is never tokenized.
 */
const RAW_TEXT_ELEMENTS = new Set([
  'script',
  'style',
  'textarea',
  'title',
  'xmp',
  'noscript',
  'iframe',
  'noembed',
  'noframes',
]);

/**
 * Forward scan of raw HTML into open/close/comment/doctype tokens, no DOM.
 * The tag alternative is quote-aware — a '>' or '<' inside a quoted
 * attribute value does not end the tag — and everything from an opening
 * raw-text tag to its `</name>` is skipped as markup-free text.
 */
export function tokens(html) {
  const out = [];
  const re =
    /<!--[\s\S]*?-->|<!doctype[^>]*>|<(\/?)([a-zA-Z][a-zA-Z0-9:-]*)((?:[^>"']|"[^"]*"|'[^']*')*)>/gi;
  let match;
  while ((match = re.exec(html))) {
    if (!match[2]) {
      out.push({
        kind: match[0].startsWith('<!--') ? 'comment' : 'doctype',
        name: '',
        attrs: '',
        start: match.index,
        end: re.lastIndex,
      });
      continue;
    }
    const name = match[2].toLowerCase();
    const close = match[1] === '/';
    out.push({
      kind: close ? 'close' : 'open',
      name,
      attrs: match[3],
      start: match.index,
      end: re.lastIndex,
    });
    if (!close && RAW_TEXT_ELEMENTS.has(name) && !/\/\s*$/.test(match[3])) {
      const endRe = new RegExp(`</${name}\\s*>`, 'ig');
      endRe.lastIndex = re.lastIndex;
      const end = endRe.exec(html);
      if (!end) {
        out[out.length - 1].unterminated = true;
        break;
      }
      out.push({ kind: 'close', name, attrs: '', start: end.index, end: endRe.lastIndex });
      re.lastIndex = endRe.lastIndex;
    }
  }
  return out;
}

/**
 * Open tokens of the document's <title> elements — a <title> inside
 * <svg>/<math> names a graphic or equation, not the document.
 */
function documentTitleTokens(tk) {
  let foreign = 0;
  const titles = [];
  for (const t of tk) {
    const isForeign = t.name === 'svg' || t.name === 'math';
    if (t.kind === 'open' && isForeign && !/\/\s*$/.test(t.attrs)) foreign += 1;
    else if (t.kind === 'close' && isForeign) foreign -= 1;
    else if (t.kind === 'open' && t.name === 'title' && foreign === 0) titles.push(t);
  }
  return titles;
}

/** Open tokens of one tag name — quote-aware, unlike a `<tag[^>]*>` scan. */
function openTokens(html, name) {
  return tokens(html).filter((t) => t.kind === 'open' && t.name === name);
}

/** Title count and decoded text of the first document <title>. */
export function titleInfo(html) {
  const tk = tokens(html);
  const titles = documentTitleTokens(tk);
  let raw = '';
  if (titles.length) {
    const close = tk.find(
      (t) => t.kind === 'close' && t.name === 'title' && t.start >= titles[0].end,
    );
    raw = close ? html.slice(titles[0].end, close.start) : '';
  }
  return {
    count: titles.length,
    text: decodeEntities(collapseWhitespace(raw)),
  };
}

/** Description tag count and decoded content of the first one. */
export function descriptionInfo(html) {
  const tags = openTokens(html, 'meta').filter(
    (t) => attrValue(t.attrs, 'name').toLowerCase() === 'description',
  );
  return {
    count: tags.length,
    text: decodeEntities(collapseWhitespace(tags[0] ? attrValue(tags[0].attrs, 'content') : '')),
  };
}

/** Decoded content of the first <meta property="…"> ('' when absent). */
function metaProperty(html, property) {
  const tag = openTokens(html, 'meta').find(
    (t) => attrValue(t.attrs, 'property').toLowerCase() === property,
  );
  return decodeEntities(collapseWhitespace(tag ? attrValue(tag.attrs, 'content') : ''));
}

/** A page is non-indexable when robots says so, however the value is ordered. */
export function isNoindex(html) {
  return openTokens(html, 'meta').some(
    (t) =>
      attrValue(t.attrs, 'name').toLowerCase() === 'robots' &&
      /\bnoindex\b/i.test(attrValue(t.attrs, 'content')),
  );
}

/**
 * A redirect stub: a noindex page whose only job is a meta refresh to
 * another URL. It has no content, so h1-count and document-structure do
 * not apply to it.
 */
export function isRedirectStub(html) {
  return (
    isNoindex(html) &&
    openTokens(html, 'meta').some(
      (t) => attrValue(t.attrs, 'http-equiv').toLowerCase() === 'refresh',
    )
  );
}

/** Remove script/style/pre/code contents and comments: not visible text. */
export function stripNonVisible(html) {
  return html
    .replace(/<script\b[^>]*>[\s\S]*?<\/script\s*>/gi, ' ')
    .replace(/<style\b[^>]*>[\s\S]*?<\/style\s*>/gi, ' ')
    .replace(/<pre\b[^>]*>[\s\S]*?<\/pre\s*>/gi, ' ')
    .replace(/<code\b[^>]*>[\s\S]*?<\/code\s*>/gi, ' ')
    .replace(/<!--[\s\S]*?-->/g, ' ');
}

/** The text a visitor reads: body, tags stripped, entities decoded. */
export function visibleText(html) {
  const body = /<body\b[^>]*>([\s\S]*?)<\/body\s*>/i.exec(html)?.[1] ?? html;
  return collapseWhitespace(
    decodeEntities(stripNonVisible(body).replace(/<(?:[^>"']|"[^"]*"|'[^']*')*>/g, ' ')),
  );
}

/** Count <h1> opening tags outside script/style/comments. */
export function h1Count(html) {
  return (stripNonVisible(html).match(/<h1(?=[\s/>])/gi) ?? []).length;
}

/**
 * Document-structure problems as messages (one per check that fails):
 * one <html>/<head>/<body> pair each, in order; nothing but whitespace and
 * comments between </head> and <body>, between </body> and </html>, and
 * after </html>; exactly one document <title>, inside <head>; and the
 * head-only tags (ld+json scripts, stylesheet/canonical links, the
 * description/robots/viewport metas and charset) inside <head>. Every
 * message names the element and its byte offset.
 */
export function documentStructureProblems(html) {
  const problems = [];
  const tk = tokens(html);

  const unterminated = tk.find((t) => t.unterminated);
  if (unterminated) problems.push(`unterminated <${unterminated.name}>`);

  const opens = (name) => tk.filter((t) => t.kind === 'open' && t.name === name);
  const closes = (name) => tk.filter((t) => t.kind === 'close' && t.name === name);
  for (const name of ['html', 'head', 'body']) {
    if (opens(name).length !== 1)
      problems.push(`${opens(name).length} <${name}> start tags (want 1)`);
    if (closes(name).length !== 1)
      problems.push(`${closes(name).length} </${name}> end tags (want 1)`);
  }
  if (problems.length) return problems;

  const [htmlOpen, headOpen, headClose, bodyOpen, bodyClose, htmlClose] = [
    opens('html')[0],
    opens('head')[0],
    closes('head')[0],
    opens('body')[0],
    closes('body')[0],
    closes('html')[0],
  ];
  if (!(
    htmlOpen.start < headOpen.start &&
    headOpen.end <= headClose.start &&
    headClose.end <= bodyOpen.start &&
    bodyClose.end <= htmlClose.start
  )) {
    problems.push('html/head/body tags out of order');
  }

  const gap = (from, to) =>
    html
      .slice(from, to)
      .replace(/<!--[\s\S]*?-->/g, '')
      .trim();
  const betweenHeadAndBody = gap(headClose.end, bodyOpen.start);
  if (betweenHeadAndBody)
    problems.push(`content between </head> and <body>: "${betweenHeadAndBody.slice(0, 80)}"`);
  const betweenBodyAndHtml = gap(bodyClose.end, htmlClose.start);
  if (betweenBodyAndHtml)
    problems.push(`content between </body> and </html>: "${betweenBodyAndHtml.slice(0, 80)}"`);
  const afterHtml = gap(htmlClose.end, html.length);
  if (afterHtml) problems.push(`content after </html>: "${afterHtml.slice(0, 80)}"`);

  const inHead = (t) => t.start >= headOpen.end && t.end <= headClose.start;

  const titles = documentTitleTokens(tk);
  if (titles.length !== 1) problems.push(`${titles.length} document <title> elements (want 1)`);
  else if (!inHead(titles[0]))
    problems.push(`<title> is outside <head> at offset ${titles[0].start}`);

  for (const t of tk) {
    if (t.kind !== 'open') continue;
    const where = `at offset ${t.start}`;
    if (
      t.name === 'script' &&
      attrValue(t.attrs, 'type').trim().toLowerCase() === 'application/ld+json' &&
      !inHead(t)
    ) {
      problems.push(`application/ld+json <script> outside <head> ${where}`);
    }
    if (t.name === 'link') {
      const rel = attrValue(t.attrs, 'rel');
      if (/(^|\s)stylesheet(\s|$)/i.test(rel) && !inHead(t))
        problems.push(`<link rel="stylesheet"> outside <head> ${where}`);
      if (/(^|\s)canonical(\s|$)/i.test(rel) && !inHead(t))
        problems.push(`<link rel="canonical"> outside <head> ${where}`);
    }
    if (t.name === 'meta') {
      const name = attrValue(t.attrs, 'name').toLowerCase();
      if (['description', 'robots', 'viewport'].includes(name) && !inHead(t))
        problems.push(`<meta name="${name}"> outside <head> ${where}`);
      if (/(^|\s)charset\s*=/i.test(t.attrs) && !inHead(t))
        problems.push(`<meta charset> outside <head> ${where}`);
    }
  }
  return problems;
}

/** hrefs of every <link rel="canonical"> ('' when the tag has no href). */
export function canonicalHrefs(html) {
  return openTokens(html, 'link')
    .filter((t) => /\bcanonical\b/i.test(attrValue(t.attrs, 'rel')))
    .map((t) => attrValue(t.attrs, 'href'));
}

/** null when the canonical URL is well-formed, else why it is not. */
export function canonicalProblem(href) {
  let url;
  try {
    url = new URL(href);
  } catch {
    return `not an absolute URL: "${href}"`;
  }
  if (url.origin !== SITE_ORIGIN) return `not on ${SITE_ORIGIN}: "${href}"`;
  if (!url.pathname.endsWith('/') && !/\.[a-z0-9]+$/i.test(url.pathname)) {
    return `path does not end in "/" or a file extension: "${href}"`;
  }
  return null;
}

const DAEMON_RE = /\bdaemons?\b/i;
// Spellings that never named a product line: the 2.0 predecessor is the
// CamelCase "ModPageSpeed 2.0" (legitimate history, allowed), and the
// current line is lowercase mod_pagespeed — so "mod_pagespeed 2.0" in any
// case, and CamelCase "ModPageSpeed 2.1"+, are always drift.
const NAMING_RES = [/\bmod[_ ]pagespeed\s*2\.0\b/i, /\bModPageSpeed\s*2\.[1-9]\b/];

// The retired product label: the name followed by "2.1" that is not the
// start of a longer version ("2.1.0", "2.10"). Case-insensitive, like the
// naming rule above, and the space form too: both read as the label.
const RETIRED_LABEL_RE = /\bmod[_ ]pagespeed\s+2\.1(?!\d|\.\d)/gi;

/** Every string value in a JSON document, URL values left out. */
function jsonStrings(value, out = []) {
  if (typeof value === 'string') {
    if (!/^(?:https?:)?\/\//.test(value) && !value.startsWith('/')) out.push(value);
  } else if (Array.isArray(value)) {
    for (const v of value) jsonStrings(v, out);
  } else if (value && typeof value === 'object') {
    for (const v of Object.values(value)) jsonStrings(v, out);
  }
  return out;
}

/**
 * The surfaces the retired-label rule reads besides visible text, title and
 * description: social metas, alt and aria-label values, and the strings of
 * every application/ld+json block. [where, text] pairs.
 */
export function labelSurfaces(html) {
  const tk = tokens(html);
  const surfaces = [];
  for (const t of tk) {
    if (t.kind !== 'open') continue;
    if (t.name === 'meta') {
      const key = (attrValue(t.attrs, 'property') || attrValue(t.attrs, 'name')).toLowerCase();
      if (key === 'og:title' || key === 'og:description' || key.startsWith('twitter:')) {
        const content = attrValue(t.attrs, 'content');
        if (!/^https?:\/\//.test(content)) {
          surfaces.push([key, decodeEntities(collapseWhitespace(content))]);
        }
      }
    }
    for (const attr of ['alt', 'aria-label']) {
      const value = attrValue(t.attrs, attr);
      if (value) surfaces.push([attr, decodeEntities(collapseWhitespace(value))]);
    }
    if (t.name === 'script' && /application\/ld\+json/i.test(attrValue(t.attrs, 'type'))) {
      const close = tk.find((c) => c.kind === 'close' && c.name === 'script' && c.start >= t.end);
      const raw = html.slice(t.end, close ? close.start : html.length);
      let strings;
      try {
        strings = jsonStrings(JSON.parse(raw));
      } catch {
        strings = [raw];
      }
      for (const text of strings) surfaces.push(['JSON-LD', text]);
    }
  }
  return surfaces;
}

/** A ~100-char window around the match, so the failure names the copy. */
function around(text, match) {
  const start = Math.max(0, match.index - 45);
  return text.slice(start, Math.min(text.length, match.index + match[0].length + 55)).trim();
}

/**
 * Lint one page. Returns [{rule, message}] without the url (the caller
 * knows it); duplicate descriptions need the whole page set and are
 * computed by lintPages.
 */
export function lintPage(html) {
  const failures = [];
  const noindex = isNoindex(html);
  const indexable = !noindex;
  const fails = (rule, message) => failures.push({ rule, message });

  const title = titleInfo(html);
  if (title.count !== 1) {
    fails('title-count', title.count === 0 ? 'no <title>' : `${title.count} <title> elements`);
  } else if (indexable) {
    const n = length(title.text);
    if (n < TITLE_MIN || n > TITLE_MAX) {
      fails('title-length', `${n} chars (want ${TITLE_MIN}-${TITLE_MAX}): "${title.text}"`);
    }
  }

  const description = descriptionInfo(html);
  if (indexable) {
    if (description.count === 0 || description.text === '') {
      fails('description-present', 'no <meta name="description"> content');
    } else if (description.count > 1) {
      fails('description-present', `${description.count} <meta name="description"> elements`);
    } else {
      const n = length(description.text);
      if (n < DESCRIPTION_MIN || n > DESCRIPTION_MAX) {
        fails(
          'description-length',
          `${n} chars (want ${DESCRIPTION_MIN}-${DESCRIPTION_MAX}): "${description.text}"`,
        );
      }
    }
  }

  if (!isRedirectStub(html)) {
    const h1s = h1Count(html);
    if (h1s !== 1) fails('h1-count', h1s === 0 ? 'no <h1>' : `${h1s} <h1> elements`);

    for (const problem of documentStructureProblems(html)) fails('document-structure', problem);
  }

  if (indexable) {
    const daemonSurfaces = [
      ['visible text', visibleText(html)],
      ['<title>', title.text],
      ['<meta name="description">', description.text],
      ['og:title', metaProperty(html, 'og:title')],
      ['og:description', metaProperty(html, 'og:description')],
    ];
    for (const [where, text] of daemonSurfaces) {
      const daemon = DAEMON_RE.exec(text);
      if (daemon) {
        fails('term-drift-daemon', `${where}: …${around(text, daemon)}…`);
        break;
      }
    }
  }

  for (const text of [visibleText(html), title.text, description.text]) {
    const naming = NAMING_RES.map((re) => re.exec(text)).find(Boolean);
    if (naming) {
      fails(
        'product-naming',
        `"${naming[0]}" never named a product line: …${around(text, naming)}…`,
      );
      break;
    }
  }

  // Every match on every surface counts: the pending list holds exact counts.
  const labelHits = [];
  for (const [where, text] of [
    ['visible text', visibleText(html)],
    ['<title>', title.text],
    ['<meta name="description">', description.text],
    ...labelSurfaces(html),
  ]) {
    for (const m of text.matchAll(RETIRED_LABEL_RE))
      labelHits.push(`${where}: …${around(text, m)}…`);
  }
  if (labelHits.length) {
    failures.push({
      rule: 'retired-product-label',
      count: labelHits.length,
      message: `${labelHits.length} use(s) of the retired "mod_pagespeed 2.1" label; the product is "mod_pagespeed" (an exact version is three parts, from the release manifest). First: ${labelHits[0]}`,
    });
  }

  if (indexable) {
    const hrefs = canonicalHrefs(html);
    if (hrefs.length !== 1) {
      fails(
        'canonical',
        hrefs.length === 0 ? 'no <link rel="canonical">' : `${hrefs.length} <link rel="canonical">`,
      );
    } else {
      const problem = canonicalProblem(hrefs[0]);
      if (problem) fails('canonical', problem);
    }
  }

  return failures;
}

/**
 * Lint a whole build. pages: [{url, html}]; allowlist:
 * {urlPath: {rule: reason}} — only the named rule is skipped on that page.
 * Returns {failures: [{url, rule, level, message}], warnings: [...]}.
 */
export function lintPages(pages, allowlist = {}, pending = {}) {
  const failures = [];
  const warnings = [];
  const stillPending = new Set();

  const known = new Set(pages.map((p) => p.url));
  for (const url of Object.keys(allowlist)) {
    if (!known.has(url)) {
      failures.push({
        url,
        rule: 'allowlist',
        level: 'error',
        message: 'allowlist entry matches no built page',
      });
    }
  }

  const byDescription = new Map();
  for (const page of pages) {
    const exempt = allowlist[page.url] ?? {};
    for (const { rule, message, count } of lintPage(page.html)) {
      if (exempt[rule] !== undefined) {
        warnings.push({
          url: page.url,
          rule: 'allowlist',
          level: 'warn',
          message: `exempted ${rule}: ${exempt[rule]}`,
        });
        continue;
      }
      if (rule === 'retired-product-label' && pending[page.url] !== undefined) {
        stillPending.add(page.url);
        const allowed = pending[page.url];
        if (count === allowed) {
          warnings.push({
            url: page.url,
            rule,
            level: 'warn',
            message: `pending the rename (retired-label-pending.json): ${message}`,
          });
        } else {
          failures.push({
            url: page.url,
            rule,
            level: 'error',
            message:
              count > allowed
                ? `${count} uses, ${allowed} pending: new text must not add the retired label. ${message}`
                : `${count} uses, ${allowed} pending: lower its count in scripts/retired-label-pending.json to ${count}`,
          });
        }
        continue;
      }
      const level = RULES[rule]?.level ?? 'error';
      const failure = { url: page.url, rule, level, message };
      if (level === 'warn') warnings.push(failure);
      else failures.push(failure);
    }
    if (!isNoindex(page.html)) {
      const d = descriptionInfo(page.html).text;
      if (d) {
        const group = byDescription.get(d) ?? [];
        group.push(page.url);
        byDescription.set(d, group);
      }
    }
  }
  for (const url of Object.keys(pending)) {
    if (stillPending.has(url)) continue;
    failures.push({
      url,
      rule: 'retired-product-label',
      level: 'error',
      message: known.has(url)
        ? 'no longer carries the retired label: remove it from scripts/retired-label-pending.json'
        : 'retired-label-pending.json entry matches no built page',
    });
  }

  for (const [d, urls] of byDescription) {
    const live = urls.filter(
      (url) => (allowlist[url] ?? {})['description-duplicate'] === undefined,
    );
    if (live.length > 1) {
      const failure = {
        url: live[0],
        rule: 'description-duplicate',
        level: RULES['description-duplicate'].level,
        message: `identical description on ${live.length} pages (${live.join(', ')}): "${d.slice(0, 120)}"`,
      };
      if (failure.level === 'warn') warnings.push(failure);
      else failures.push(failure);
    }
  }

  return { failures, warnings };
}

/** URL path of a built file: dist/client/docs/a/index.html -> /docs/a/. */
export function urlOf(distRoot, file) {
  const segments = relative(distRoot, file).split(/[\\/]/);
  if (segments[0] === 'client') segments.shift();
  let directory = false;
  if (segments[segments.length - 1] === 'index.html') {
    segments.pop();
    directory = true;
  }
  const url = `/${segments.map(encodeURIComponent).join('/')}`;
  return directory && !url.endsWith('/') ? `${url}/` : url;
}

/** Collect every *.html under distRoot except the frozen /1.0/ archive. */
export function collectPages(distRoot) {
  const out = [];
  const walk = (dir) => {
    for (const name of readdirSync(dir)) {
      const p = join(dir, name);
      if (statSync(p).isDirectory()) {
        if (name === '1.0' && relative(distRoot, dir) === 'client') continue;
        walk(p);
      } else if (name.endsWith('.html')) {
        out.push(p);
      }
    }
  };
  walk(distRoot);
  return out;
}

export function loadAllowlist(file) {
  if (!existsSync(file)) return {};
  const parsed = JSON.parse(readFileSync(file, 'utf8'));
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error(`${file}: expected a JSON object of {urlPath: {rule: reason}}`);
  }
  for (const [url, rules] of Object.entries(parsed)) {
    if (typeof rules !== 'object' || rules === null || Array.isArray(rules)) {
      throw new Error(`${file}: ${url}: expected {rule: reason}, got ${JSON.stringify(rules)}`);
    }
    for (const [rule, reason] of Object.entries(rules)) {
      if (!(rule in RULES)) throw new Error(`${file}: ${url}: unknown rule "${rule}"`);
      if (typeof reason !== 'string' || reason.trim() === '') {
        throw new Error(`${file}: ${url}: ${rule}: reason must be a non-empty string`);
      }
    }
  }
  return parsed;
}

/**
 * The pages the retired-label rename has not reached yet: the `pages` map
 * {urlPath: count} of scripts/retired-label-pending.json (its `sources` half
 * is read by test/content-accuracy.test.ts). A missing file means nothing is
 * pending.
 */
export function loadPending(file) {
  if (!existsSync(file)) return {};
  const pages = JSON.parse(readFileSync(file, 'utf8'))?.pages;
  if (
    typeof pages !== 'object' ||
    pages === null ||
    Array.isArray(pages) ||
    Object.entries(pages).some(([u, n]) => !u.startsWith('/') || !Number.isInteger(n) || n < 1)
  ) {
    throw new Error(`${file}: expected "pages" to be {urlPath: positive count}`);
  }
  return pages;
}

function summary(failures, warnings, pageCount) {
  const byRule = new Map();
  for (const list of [failures, warnings]) {
    for (const f of list) {
      const entry = byRule.get(f.rule) ?? { error: 0, warn: 0 };
      if (list === failures) entry.error += 1;
      else entry.warn += 1;
      byRule.set(f.rule, entry);
    }
  }
  const lines = [`content-lint: ${pageCount} pages checked`];
  for (const [rule, { level }] of Object.entries(RULES)) {
    const n = byRule.get(rule) ?? { error: 0, warn: 0 };
    lines.push(
      `  ${rule.padEnd(22)} ${String(n.error).padStart(3)} errors  ${String(n.warn).padStart(3)} warnings  (${level})`,
    );
  }
  const allow = byRule.get('allowlist') ?? { error: 0, warn: 0 };
  if (allow.error || allow.warn) {
    lines.push(
      `  ${'allowlist'.padEnd(22)} ${String(allow.error).padStart(3)} errors  ${String(allow.warn).padStart(3)} exemptions`,
    );
  }
  return lines.join('\n');
}

async function main() {
  if (!existsSync(DIST)) {
    console.error(`check-built-content: ${DIST} does not exist; run \`npm run build\` first.`);
    process.exit(1);
  }
  const files = collectPages(DIST);
  if (files.length === 0) {
    console.error(`check-built-content: no .html files under ${DIST}.`);
    process.exit(1);
  }
  const allowlist = loadAllowlist(join(WEBSITE_ROOT, 'scripts', 'content-lint-allow.json'));
  const pages = files.map((file) => ({
    url: urlOf(DIST, file),
    html: readFileSync(file, 'utf8'),
  }));

  const pending = loadPending(join(WEBSITE_ROOT, 'scripts', 'retired-label-pending.json'));
  const { failures, warnings } = lintPages(pages, allowlist, pending);

  const print = (f) => console.log(`  ${f.url}  ${f.rule}  ${f.message}`);
  if (warnings.length) {
    console.log('warnings (do not fail the build):');
    warnings.forEach(print);
  }
  if (failures.length) {
    console.log('failures:');
    failures.forEach(print);
  }
  console.log(summary(failures, warnings, pages.length));
  if (failures.length > 0) {
    console.error(`content-lint: ${failures.length} error(s).`);
    process.exit(1);
  }
  console.log('content-lint: OK');
}

// Run as a CLI only when invoked directly (the vitest suite imports the rest).
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  await main();
}
