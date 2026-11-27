// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The ids a docs page's markdown SOURCE will expose once rendered, computed
// the way the site's markdown pipeline computes them: an explicit `{#id}`
// (remark-custom-heading-id) is read literally; a heading without one falls
// back to github-slugger, the package @astrojs/markdown-remark's own
// heading-id collector uses, called once per file in document order so its
// dedupe state matches; raw `<a id="…">` tags pass through unchanged.
// Fence-aware: a `#` inside a fenced code block is not a heading.
//
// Shared by anchor-source-gate.test.ts and the reference gates in
// test/reference/.

import GithubSlugger from 'github-slugger';

export function stripFrontmatter(src: string): string {
  return src.replace(/^---\r?\n[\s\S]*?\r?\n---\r?\n?/, '');
}

// Approximates what the rehype heading-id collector concatenates from a
// heading's inline children: code spans, links, and emphasis all reduce to
// their inner text before slugging.
export function cleanHeadingText(text: string): string {
  return text
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1')
    .replace(/\*([^*]*)\*/g, '$1')
    .replace(/_([^_]*)_/g, '$1')
    .trim();
}

export function idsInSource(body: string): Set<string> {
  const ids = new Set<string>();
  const slugger = new GithubSlugger();
  let inFence = false;
  for (const line of body.split('\n')) {
    if (/^\s*(```|~~~)/.test(line)) {
      inFence = !inFence;
      continue;
    }
    if (inFence) continue;
    const heading = /^#{1,6}\s+(.*)$/.exec(line);
    if (!heading) continue;
    const rest = heading[1].trim();
    const explicit = /\{#([a-zA-Z0-9_-]+)\}\s*$/.exec(rest);
    if (explicit) {
      ids.add(explicit[1]);
      continue;
    }
    ids.add(slugger.slug(cleanHeadingText(rest)));
  }
  for (const m of body.matchAll(/<a\s+(?:id|name)=["']([a-zA-Z0-9_-]+)["']/g)) {
    ids.add(m[1]);
  }
  return ids;
}

/** Every `[text](/docs/<slug>/#fragment)` and `[text](#fragment)` link in a markdown body. */
export function fragmentLinks(
  body: string,
): { href: string; slug: string | null; fragment: string }[] {
  const out: { href: string; slug: string | null; fragment: string }[] = [];
  let inFence = false;
  for (const line of body.split('\n')) {
    if (/^\s*(```|~~~)/.test(line)) {
      inFence = !inFence;
      continue;
    }
    if (inFence) continue;
    for (const m of line.matchAll(/\]\(((?:\/docs\/([a-z0-9-]+)\/)?#([A-Za-z0-9_-]+))\)/g)) {
      out.push({ href: m[1], slug: m[2] ?? null, fragment: m[3] });
    }
  }
  return out;
}
