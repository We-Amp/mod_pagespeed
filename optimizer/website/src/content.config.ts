// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { defineCollection, z } from 'astro:content';
import { glob, file } from 'astro/loaders';
import { parse as parseYaml } from 'yaml';

const blog = defineCollection({
  loader: glob({ pattern: '**/*.md', base: './src/content/blog' }),
  schema: z.object({
    // Search engines cut a title past roughly 60 characters and a snippet past
    // roughly 155, so a longer value loses its last words in the results page.
    // The page appends the product suffix only when the total still fits.
    title: z.string().max(60, 'blog title must be 60 characters or fewer'),
    description: z.string().max(155, 'blog description must be 155 characters or fewer'),
    date: z.coerce.date(),
    author: z.string().default('Otto van der Schaaf'),
    tags: z.array(z.string()).default([]),
    draft: z.boolean().default(false),
    // Optional dateModified for BlogPosting JSON-LD. When unset, no
    // dateModified is emitted — emitting date == datePublished is
    // misleading to search engines and aggregators.
    lastUpdated: z.coerce.date().optional(),
    // Optional explicit cover-image path used by BlogPosting JSON-LD `image`
    // and the page's og:image / twitter:image meta. When unset (the default
    // for the current posts), the build-time generator at
    // `scripts/generate-title-cards.mjs` synthesizes a per-post title card at
    // /og-cards/{slug}.png so each post advertises a DISTINCT social image —
    // fixing the GSC weakness of every post sharing /og-default.png. Kept as
    // a plain string (not Astro's image() helper) so the renderer can decide
    // absolute vs relative URL composition.
    coverImage: z.string().optional(),
    // Product line a post belongs to. Drives the per-post CTA aside and the
    // BaseLayout title suffix in [slug].astro. Defaults to '2.0' so existing
    // posts keep their current ModPageSpeed 2.0 framing; 1.1-era posts set
    // product: '1.1' explicitly.
    product: z.enum(['1.1', '2.0']).default('2.0'),
    // Optional tool pointer rendered as a one-line aside above "Read next":
    // the post's body stays as published, the aside carries the link.
    tool: z.enum(['crawler-cost']).optional(),
    // Optional pin-to-top order on the blog index. Lower number = higher
    // position (1 sorts above 2). Posts without `pinned` sort by date as
    // before. Optional so existing posts keep validating without changes.
    pinned: z.number().int().optional(),
    // Optional HowTo steps surfaced as a second JSON-LD block (HowTo) by
    // blog/[slug].astro, alongside the always-present BlogPosting. Mirrors the
    // docs collection's HowTo pattern (docs/[slug].astro). The how-to spokes
    // (fix-{lcp,cls,inp}-*) are step-based fix guides; this makes their ordered
    // remediation steps machine-readable for answer engines. Steps MUST reflect
    // the visible on-page content — HowTo structured data is content-checked
    // (do not invent steps the article doesn't show). Keep `text` concise.
    howTo: z
      .object({
        name: z.string(),
        description: z.string().optional(),
        tools: z.array(z.string()).optional(),
        steps: z.array(z.object({ name: z.string(), text: z.string() })).min(2),
      })
      .optional(),
    // Optional FAQ pairs surfaced as a FAQPage JSON-LD block by
    // blog/[slug].astro for People-Also-Ask / featured-snippet capture.
    // Like HowTo, FAQPage structured data is content-checked: each q/a MUST
    // also appear verbatim in the visible article body (e.g. a
    // "Frequently asked questions" section), or Google may flag a mismatch.
    // Keep answers concise and factual.
    faq: z
      .array(z.object({ q: z.string(), a: z.string() }))
      .min(2)
      .optional(),
  }),
});

const docs = defineCollection({
  loader: glob({ pattern: '**/*.{md,mdx}', base: './src/content/docs' }),
  schema: z.object({
    // The sidebar label, breadcrumb and (unless `h1` is set) the heading.
    // The <title> appends " — mod_pagespeed" only when the total stays
    // within 60 characters, the point where search engines truncate.
    title: z.string().max(60, 'docs title must be 60 characters or fewer'),
    // Optional <title> override for a landing page whose search title should
    // lead with the searcher's words rather than the short sidebar label.
    seoTitle: z.string().max(60, 'docs seoTitle must be 60 characters or fewer').optional(),
    // Optional page heading override; defaults to `title`.
    h1: z.string().optional(),
    // Search engines truncate snippets past roughly 155 characters, so a
    // longer description is cut mid-claim. The build fails on a regression.
    description: z.string().max(155, 'docs description must be 155 characters or fewer'),
    order: z.number().default(0),
    group: z.string().optional(),
    // Optional dateModified for TechArticle JSON-LD (parity with docs-1.1).
    lastUpdated: z.coerce.date().optional(),
    // Optional datePublished for TechArticle JSON-LD. When unset, the field
    // is omitted from the rendered JSON-LD — we do not invent dates.
    datePublished: z.coerce.date().optional(),
    // Optional FAQ pairs surfaced as FAQPage JSON-LD by /docs/[slug].astro.
    // Keep answers short (1–2 sentences) and grounded in the page body —
    // FAQ rich results are content-checked by Google.
    faq: z
      .array(
        z.object({
          q: z.string(),
          a: z.string(),
        }),
      )
      .optional(),
  }),
});

// Evergreen "How it works" architecture explainers (wiki-republish cycle).
// Transformative, source-grounded deep-dives on the optimization techniques —
// distinct from docs (reference/config) and blog (task/news).
const howItWorks = defineCollection({
  loader: glob({ pattern: '**/*.md', base: './src/content/how-it-works' }),
  schema: z.object({
    title: z.string().max(60, 'how-it-works title must be 60 characters or fewer'),
    description: z.string().max(155, 'how-it-works description must be 155 characters or fewer'),
    // Hub ordering (lower = earlier).
    order: z.number().default(0),
    // Short blurb for the /how-it-works/ hub card. Falls back to description.
    summary: z.string().optional(),
    lastUpdated: z.coerce.date().optional(),
    datePublished: z.coerce.date().optional(),
    // Optional FAQ pairs → FAQPage JSON-LD + visible accordion (content-checked).
    faq: z.array(z.object({ q: z.string(), a: z.string() })).optional(),
  }),
});

// Release manifest collections, synced from the release manifests maintained
// outside this repository (§7). The 2.1 manifest (the
// converged line) is seeded in-tree — the sync workflow does not yet
// copy it. The schema mirrors the release manifest schema but is
// intentionally loose on nested compat/artifacts (we render via the helper in
// src/lib/release.ts).
const releaseSchema = z.object({
  // schema_version 2 (D1/D7) adds optional status/eol; the object is
  // non-strict so they would pass anyway, but model them so consumers get
  // typed access (e.g. a future frozen-line notice on the download page).
  schema_version: z.union([z.literal(1), z.literal(2)]),
  status: z.enum(['active', 'frozen']).optional(),
  eol: z.record(z.string(), z.string()).optional(),
  product: z.object({
    line: z.enum(['1.1', '2.0', '2.1']),
    display_name: z.string(),
  }),
  release: z.object({
    semver: z.string(),
    revision: z.number().int().nonnegative().nullable(),
    tag: z.string(),
    released_on: z.string(),
  }),
  artifacts: z.record(z.string(), z.record(z.string(), z.union([z.string(), z.array(z.string())]))),
  urls: z.object({
    archive_base: z.string().url(),
    latest_base: z.string().url(),
    gpg_key: z.string().url(),
  }),
  // Intentionally loose: rendering uses individual keys per consumer.
  compat: z.record(z.string(), z.any()),
  display: z.record(z.string(), z.string()),
});

// Astro's `file()` loader is designed for a single data file producing an
// array of entries OR (with a parser) a flat object. Our manifests are flat
// objects keyed by `release` (single entry per file), so we use a custom
// parser that returns an array with a stable id.
const yamlSingleton = (id: string) => (text: string) => {
  const data = parseYaml(text);
  return [{ id, ...data }];
};

const releases11 = defineCollection({
  loader: file('src/content/releases-1.1/release.yaml', {
    parser: yamlSingleton('release'),
  }),
  schema: releaseSchema,
});

// The 2.0 line derives every customer-facing version string from
// release.semver (display.header_version === semver, display.badge_version ===
// "v"+semver). Enforce that invariant here so the 2.0.11-vs-2.0.16 drift that
// shipped a header version the docs didn't install (F59/F133) can't recur — a
// bump tool that touches release.semver without re-deriving display.* now fails
// the build instead of silently disagreeing with itself. The 2.1 line (the
// converged line) carries the same invariant.
const displayMatchesSemver = (line: string) =>
  releaseSchema.superRefine((data, ctx) => {
    const { semver } = data.release;
    if (data.display.header_version !== semver) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['display', 'header_version'],
        message: `display.header_version (${data.display.header_version}) must equal release.semver (${semver}) for the ${line} line`,
      });
    }
    if (data.display.badge_version !== `v${semver}`) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        path: ['display', 'badge_version'],
        message: `display.badge_version (${data.display.badge_version}) must equal "v"+release.semver (v${semver}) for the ${line} line`,
      });
    }
  });

const releaseSchema20 = displayMatchesSemver('2.0');
const releaseSchema21 = displayMatchesSemver('2.1');

const releases20 = defineCollection({
  loader: file('src/content/releases-2.0/release.yaml', {
    parser: yamlSingleton('release'),
  }),
  schema: releaseSchema20,
});

const releases21 = defineCollection({
  loader: file('src/content/releases-2.1/release.yaml', {
    parser: yamlSingleton('release'),
  }),
  schema: releaseSchema21,
});

export const collections = {
  blog,
  docs,
  'how-it-works': howItWorks,
  'releases-1.1': releases11,
  'releases-2.0': releases20,
  'releases-2.1': releases21,
};
