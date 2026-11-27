// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// blog-markdown.mjs: the blog collection as Markdown, for agents.
//
// The posts are already Markdown, so the twin is the source with three
// changes: the frontmatter becomes a header (the title as the one H1, the
// description as the lede, the dates and the canonical URL under it); links
// become absolute; and a raw HTML block (an inline SVG figure, a chart wrapper)
// is reduced to its caption or aria-label as one line of text, because an
// agent cannot read the markup of a chart and the markup would swamp the text.
// HTML comments outside code go. Fenced code is copied byte for byte.
//
// The posts carry no MDX, so there is nothing else to resolve. Plain ESM on
// Node built-ins plus the `yaml` dependency, like docs-markdown.mjs: the
// generator and the prerendered route share it and neither can use
// astro:content. Every function here is deterministic.

import { readdirSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { parse as parseYaml } from 'yaml';
import { SITE, absolutizeLinks, websiteRoot } from './docs-markdown.mjs';

/**
 * @typedef {{
 *   title: string,
 *   description: string,
 *   date: string | Date,
 *   lastUpdated?: string | Date,
 *   draft?: boolean,
 * }} PostData
 * @typedef {{ slug: string, file: string, data: PostData, date: string, updated?: string, body: string }} Post
 */

const FRONTMATTER = /^---\r?\n([\s\S]*?)\r?\n---\r?\n?/;
const FENCE = /^(\s*)(`{3,}|~{3,})/;

/** @param {string | Date} value  a frontmatter date as YYYY-MM-DD */
function isoDay(value) {
  return (value instanceof Date ? value.toISOString() : String(value)).slice(0, 10);
}

/**
 * Every published post, newest first (then by slug, for a stable tie-break).
 * A `draft: true` post is left out. The slug is the file name, which is the id
 * of the blog collection entry and so the path of the page.
 * @param {string} [root]
 * @returns {Post[]}
 */
export function loadPosts(root = websiteRoot()) {
  const dir = resolve(root, 'src/content/blog');
  /** @type {Post[]} */
  const posts = [];
  for (const file of readdirSync(dir)) {
    const m = file.match(/^(.+)\.md$/);
    if (!m) continue;
    const raw = readFileSync(resolve(dir, file), 'utf8');
    const fm = raw.match(FRONTMATTER);
    if (!fm) throw new Error(`src/content/blog/${file}: no frontmatter block`);
    /** @type {PostData} */
    const data = parseYaml(fm[1]);
    if (data.draft) continue;
    posts.push({
      slug: m[1],
      file,
      data,
      date: isoDay(data.date),
      updated: data.lastUpdated ? isoDay(data.lastUpdated) : undefined,
      body: raw.slice(fm[0].length),
    });
  }
  posts.sort((a, b) => (a.date < b.date ? 1 : a.date > b.date ? -1 : a.slug < b.slug ? -1 : 1));
  return posts;
}

/** @param {string} slug */
export function postUrl(slug) {
  return `${SITE}/blog/${slug}/`;
}

/** @param {string} slug */
export function postMarkdownUrl(slug) {
  return `${SITE}/blog/${slug}.md`;
}

/** @param {string} html  markup reduced to its text */
function htmlText(html) {
  return html
    .replace(/<(script|style|title|desc)\b[\s\S]*?<\/\1>/g, ' ')
    .replace(/<[^>]*>/g, ' ')
    .replaceAll('&nbsp;', ' ')
    .replaceAll('&lt;', '<')
    .replaceAll('&gt;', '>')
    .replaceAll('&quot;', '"')
    .replaceAll('&amp;', '&')
    .replace(/\s+/g, ' ')
    .trim();
}

/**
 * What an HTML block says in words: the svg's title, else its figcaption, else
 * the first aria-label (minus the interface clause "; scrolls horizontally"),
 * else nothing.
 * @param {string} html
 * @returns {string}
 */
function blockText(html) {
  const title = html.match(/<svg\b[\s\S]*?<title\b[^>]*>([\s\S]*?)<\/title>/);
  if (title && htmlText(title[1])) return htmlText(title[1]);
  const caption = html.match(/<figcaption\b[^>]*>([\s\S]*?)<\/figcaption>/);
  if (caption) return htmlText(caption[1]);
  const label = html.match(/\baria-label="([^"]*)"/);
  return label ? htmlText(label[1].replace(/;\s*scrolls[\s\S]*$/, '')) : '';
}

const BLOCK_OPEN = /^<(figure|svg|div)\b/;

/**
 * The Markdown of one post: the H1, the lede, the dates, the canonical URL and
 * the body. Pure: the same inputs always give the same text.
 * @param {Post} post
 * @returns {string}
 */
export function renderPostMarkdown(post) {
  const { slug, data, body } = post;
  const url = postUrl(slug);
  /** @type {string[]} */
  const out = [
    `# ${data.title}`,
    '',
    data.description,
    '',
    `Published: ${post.date}`,
    ...(post.updated ? [`Updated: ${post.updated}`] : []),
    `Canonical URL: ${url}`,
    '',
  ];
  /** @type {{ marker: string } | null} */
  let fence = null;

  /** @param {string} line */
  const push = (line) => {
    if (!fence && !line && !out[out.length - 1]) return;
    out.push(line);
  };

  const lines = body.replace(/\r\n/g, '\n').split('\n');
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    if (fence) {
      push(raw);
      // CommonMark: a closing fence is the marker (at least as long, same
      // character) followed by whitespace only; "```js" opens, it never closes.
      const f = raw.match(FENCE);
      if (
        f &&
        f[2][0] === fence.marker[0] &&
        f[2].length >= fence.marker.length &&
        raw.slice(f[0].length).trim() === ''
      )
        fence = null;
      continue;
    }
    let line = raw.replace(/\s+$/, '');
    const f = line.match(FENCE);
    if (f) {
      fence = { marker: f[2] };
      push(line);
      continue;
    }
    if (/^<!--/.test(line)) {
      // A comment (the census markers) goes; text after its "-->" on the
      // closing line stays. An unclosed comment ends at the end of its block
      // (the next blank line), not at the end of the file.
      let j = i;
      let close = line.indexOf('-->', 4);
      while (close < 0 && j + 1 < lines.length && lines[j + 1].trim() !== '') {
        j++;
        close = lines[j].indexOf('-->');
      }
      if (close < 0) {
        i = j;
        continue;
      }
      line = lines[j].slice(close + 3).trim();
      i = j;
      if (!line) continue;
    }
    const open = line.match(BLOCK_OPEN);
    if (open) {
      // Collect to the line where the element's own tag balances.
      const tag = open[1];
      const opens = new RegExp(`<${tag}\\b`, 'g');
      const closes = new RegExp(`</${tag}>`, 'g');
      const selfClosed = new RegExp(`<${tag}\\b[^<>]*/>`, 'g');
      const block = [line];
      let depth = 0;
      for (;;) {
        const text = block[block.length - 1];
        depth +=
          (text.match(opens)?.length ?? 0) -
          (text.match(selfClosed)?.length ?? 0) -
          (text.match(closes)?.length ?? 0);
        if (depth <= 0 || i + 1 >= lines.length) break;
        block.push(lines[++i]);
      }
      const text = blockText(block.join('\n'));
      if (text) {
        push(absolutizeLinks(`Figure: ${text}`, url));
        push('');
      }
      continue;
    }
    push(absolutizeLinks(line, url));
  }
  return out.join('\n').replace(/\n+$/, '') + '\n';
}
