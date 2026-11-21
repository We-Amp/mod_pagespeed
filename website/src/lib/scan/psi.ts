// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// PageSpeed Insights logic shared by the analyzer page and the scan
// interface: the proxy call, audit aggregation against the mapping table, the
// configuration snippets and the error wording. Moved out of the analyzer page
// unchanged apart from the edition becoming an argument; nothing here touches
// the DOM, and the mapping table itself stays a lazy chunk.

import type { AuditMapping, Coverage, Edition } from '../../data/psi-mps-mapping';

export type { AuditMapping, Coverage, Edition };

// The audit→filter mapping table is only needed once a run completes, so
// it is split out of the page's initial bundle and imported on the first
// submit. Callers draw results only after loadMapping() has resolved.
let mappingCache: AuditMapping[] | null = null;
// Set when the mapping chunk failed to load (typically deploy skew: a
// deploy replaced the _astro/ hashes after the page was loaded). Once
// set, retrying cannot recover — the browser caches the failed module
// fetch — so the run is blocked before spending PSI calls, and the user
// is told to reload the page.
let mappingFailed = false;
export async function loadMapping(): Promise<AuditMapping[]> {
  if (!mappingCache) {
    mappingCache = (await import('../../data/psi-mps-mapping')).PSI_MPS_MAPPING;
  }
  return mappingCache;
}
export function reloadError(): Error {
  return Object.assign(
    new Error('Part of this page failed to load. Reload the page and run the analysis again.'),
    { reload: true },
  );
}

/** The mapping table once loaded, else null. */
export function getMapping(): AuditMapping[] | null {
  return mappingCache;
}
/** True once the chunk is known to be unloadable; only a page reload recovers. */
export function isMappingFailed(): boolean {
  return mappingFailed && !mappingCache;
}
export function markMappingFailed() {
  mappingFailed = true;
}

export const EDITION_BLURB: Record<Edition, string> = {
  '2.0':
    'The optimizer worker: a separate process behind the module (or an nginx reverse proxy fronting any origin, or the ASP.NET Core middleware). It optimizes off the request path and caches HTML in shared memory, so slow origins serve from RAM on repeat visits.',
  '1.1':
    'The module: in-process inside Apache, nginx or IIS. No extra hop, and the classic named filters on every server.',
};

export const DOCS_HREF: Record<Edition, { href: string; label: string }> = {
  '2.0': { href: '/docs/worker-configuration/', label: 'Read the configuration docs →' },
  '1.1': {
    href: '/docs/configuration/#native-module-configuration-directives',
    label: 'Read the filter reference →',
  },
};

export const CONFIG_DESC: Record<Edition, string> = {
  '2.0':
    'Drop into your nginx server { } or location { } block. Worker-side transforms have no EnableFilters directive; image transcoding, CSS/JS minification and critical-CSS inlining run from the master switch, and per-transform flags live on the factory_worker command line.',
  '1.1':
    'Enables every named filter that targets a flagged audit on this page. nginx syntax; the equivalent Apache and IIS directives are in the configuration reference.',
};
// ---- Named filter → worker transform docs anchor map ----
// The worker collapses the module's per-filter knobs into a smaller set
// of always-on transforms documented as anchored sections under
// /docs/worker-configuration/.
// The slug values below must match the per-transform anchors in
// worker-configuration.md. Named filters with no
// worker equivalent (e.g. animated WebP) are intentionally absent and
// fall back to the named-filter reference.
export const FILTER_TO_2_0_ANCHOR: Record<string, string> = {
  // image pipeline — every image-touching filter collapses into one transform
  rewrite_images: 'image-pipeline',
  convert_jpeg_to_webp: 'image-pipeline',
  convert_to_webp_lossless: 'image-pipeline',
  convert_to_webp_animated: 'image-pipeline',
  recompress_images: 'image-pipeline',
  recompress_jpeg: 'image-pipeline',
  recompress_png: 'image-pipeline',
  recompress_webp: 'image-pipeline',
  resize_images: 'image-pipeline',
  resize_rendered_image_dimensions: 'image-pipeline',
  responsive_images: 'image-pipeline',
  jpeg_sampling: 'image-pipeline',
  strip_image_meta_data: 'image-pipeline',
  // image dimensions (width/height hint injection)
  insert_image_dimensions: 'image-dimensions',
  // lazy-loading
  lazyload_images: 'lazy-load-images',
  // LCP preload hints
  hint_preload_subresources: 'lcp-preload',
  // DNS preconnect / dns-prefetch hint injection
  insert_dns_prefetch: 'preconnect-injection',
  // critical-CSS pipeline (inline above-the-fold CSS).
  // NOTE: 1.1's `inline_css` inlines small EXTERNAL CSS to save
  // round-trips — not critical-CSS extraction. No 2.0 analog; lets
  // it fall through to the 1.1 docs page when clicked.
  prioritize_critical_css: 'critical-css',
  // async CSS loading
  move_css_to_head: 'async-css',
  move_css_above_scripts: 'async-css',
  // @import flattening
  flatten_css_imports: 'css-import-flattening',
  // JS deferral. 1.1's `inline_javascript` inlines small external JS
  // (round-trip saving), not deferral — no 2.0 analog, falls through
  // to the 1.1 docs page.
  defer_javascript: 'script-deferral',
  // minification
  rewrite_css: 'css-minification',
  rewrite_javascript: 'js-minification',
  // cache-control extension
  extend_cache: 'cache-extension',
  extend_cache_pdfs: 'cache-extension',
  extend_cache_css: 'cache-extension',
  extend_cache_images: 'cache-extension',
  extend_cache_scripts: 'cache-extension',
};
// ---- Types (minimal slice of PSI v5 response) ----
export interface PsiAudit {
  id: string;
  title?: string;
  score?: number | null;
  displayValue?: string;
  details?: {
    overallSavingsMs?: number;
    overallSavingsBytes?: number;
    type?: string;
  };
  numericValue?: number;
}
export interface PsiResult {
  lighthouseResult?: {
    categories?: {
      performance?: { score?: number | null };
    };
    audits?: Record<string, PsiAudit>;
  };
  error?: { code?: number; message?: string };
}
export type Strategy = 'mobile' | 'desktop';
// ---- Config ----
// First-party server-side PSI proxy on modpagespeed.com. The Google API
// key lives in /etc/nginx-secrets/psi-api-key.conf on the VM and is
// injected by nginx; responses are cached for 1h per (url, strategy) and
// rate-limited per visitor IP.
export const PSI_ENDPOINT = '/psi/v5/runPagespeed';

export type ScoreBucket = 'good' | 'needs-work' | 'poor';

// PSI reports scores as 0–1; the UI and the lead message use 0–100.
export const toPercent = (s: number | null | undefined): number | null =>
  typeof s === 'number' ? Math.round(s * 100) : null;

export function scoreBucket(score: number): ScoreBucket {
  if (score >= 90) return 'good';
  if (score >= 50) return 'needs-work';
  return 'poor';
}
export const BUCKET_BADGE_CLASS: Record<ScoreBucket, string> = {
  good: 'badge-success',
  'needs-work': 'badge-neutral',
  poor: 'badge-accent',
};
export const BUCKET_LABEL: Record<ScoreBucket, string> = {
  good: 'Good',
  'needs-work': 'Needs work',
  poor: 'Poor',
};
export function formatMs(ms: number): string {
  if (ms < 1000) return `${Math.round(ms)} ms`;
  return `${(ms / 1000).toFixed(2)} s`;
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
}
// ---- PSI call ----
export async function fetchPsi(targetUrl: string, strategy: Strategy): Promise<PsiResult> {
  // Proxy validates `url` + `strategy` server-side and rejects anything
  // else, so we send only those two args; `category` and `key` are added
  // by nginx before the upstream call.
  const params = new URLSearchParams({ url: targetUrl, strategy });

  const res = await fetch(`${PSI_ENDPOINT}?${params.toString()}`, {
    method: 'GET',
    // No credentials, no custom headers — same-origin GET, no preflight.
    // `cache: 'no-store'` makes the browser bypass its HTTP cache for this
    // call. We still get the speedup from nginx's proxy_cache (same key,
    // 1h TTL on 200s); we skip the browser layer because (a) it adds no
    // value over the nginx hit, and (b) it would persistently pin any
    // transient 5xx/4xx the user happened to see during a deploy window.
    cache: 'no-store',
  });
  if (!res.ok) {
    let body: PsiResult | null = null;
    try {
      body = (await res.json()) as PsiResult;
    } catch {
      // ignore
    }
    const message = body?.error?.message ?? `HTTP ${res.status}`;
    const err = new Error(message) as Error & { status?: number };
    err.status = res.status;
    throw err;
  }
  return (await res.json()) as PsiResult;
}

// ---- Audit aggregation ----
export interface AggregatedAudit {
  mapping: AuditMapping;
  overallSavingsMs: number;
  overallSavingsBytes: number;
  mobileScore: number | null;
  desktopScore: number | null;
  // For unmapped-audit logging only.
  psiTitle?: string;
}

export function isFlagged(audit: PsiAudit | undefined): boolean {
  if (!audit) return false;
  // Lighthouse score < 0.9 = audit "failed" or "needs work".
  // Some insight audits have null score and rely on the presence of
  // overallSavingsMs/Bytes; treat those as flagged when savings are
  // non-trivial.
  if (audit.score == null) {
    const d = audit.details ?? {};
    return (d.overallSavingsMs ?? 0) > 0 || (d.overallSavingsBytes ?? 0) > 0;
  }
  return audit.score < 0.9;
}

export function aggregate(
  mobile: PsiResult,
  desktop: PsiResult,
  mappings: readonly AuditMapping[],
): AggregatedAudit[] {
  const mAudits = mobile.lighthouseResult?.audits ?? {};
  const dAudits = desktop.lighthouseResult?.audits ?? {};
  const seen = new Set<string>();
  const out: AggregatedAudit[] = [];

  for (const mapping of mappings) {
    const m = mAudits[mapping.auditId];
    const d = dAudits[mapping.auditId];
    const flagged = isFlagged(m) || isFlagged(d);
    if (!flagged) continue;
    const overallSavingsMs = Math.max(
      m?.details?.overallSavingsMs ?? 0,
      d?.details?.overallSavingsMs ?? 0,
    );
    const overallSavingsBytes = Math.max(
      m?.details?.overallSavingsBytes ?? 0,
      d?.details?.overallSavingsBytes ?? 0,
    );
    seen.add(mapping.auditId);
    out.push({
      mapping,
      overallSavingsMs,
      overallSavingsBytes,
      mobileScore: m?.score ?? null,
      desktopScore: d?.score ?? null,
      psiTitle: m?.title ?? d?.title,
    });
  }

  // Sort: full coverage first (most useful), then by impact (ms savings desc,
  // bytes desc), then alphabetically for stability.
  out.sort((a, b) => {
    const coverageRank: Record<string, number> = { full: 0, partial: 1, none: 2 };
    const cr = coverageRank[a.mapping.coverage] - coverageRank[b.mapping.coverage];
    if (cr !== 0) return cr;
    if (b.overallSavingsMs !== a.overallSavingsMs) return b.overallSavingsMs - a.overallSavingsMs;
    if (b.overallSavingsBytes !== a.overallSavingsBytes)
      return b.overallSavingsBytes - a.overallSavingsBytes;
    return a.mapping.auditTitle.localeCompare(b.mapping.auditTitle);
  });

  return out;
}

export function collectFlaggedAuditIds(mobile: PsiResult, desktop: PsiResult): Set<string> {
  const ids = new Set<string>();
  for (const src of [mobile, desktop]) {
    const audits = src.lighthouseResult?.audits ?? {};
    for (const id of Object.keys(audits)) {
      if (isFlagged(audits[id])) ids.add(id);
    }
  }
  return ids;
}
/** One flagged audit as PageSpeed Insights reported it: no product mapping. */
export interface FlaggedAudit {
  id: string;
  title: string;
  overallSavingsMs: number;
  overallSavingsBytes: number;
}

/**
 * Every flagged audit of the two runs, largest estimated time saving first,
 * with the title PageSpeed Insights gave it and the larger of the two savings
 * estimates.
 */
export function flaggedAudits(mobile: PsiResult, desktop: PsiResult): FlaggedAudit[] {
  const mAudits = mobile.lighthouseResult?.audits ?? {};
  const dAudits = desktop.lighthouseResult?.audits ?? {};
  return [...collectFlaggedAuditIds(mobile, desktop)]
    .map((id) => {
      const m = mAudits[id];
      const d = dAudits[id];
      return {
        id,
        title: m?.title ?? d?.title ?? id,
        overallSavingsMs: Math.max(
          m?.details?.overallSavingsMs ?? 0,
          d?.details?.overallSavingsMs ?? 0,
        ),
        overallSavingsBytes: Math.max(
          m?.details?.overallSavingsBytes ?? 0,
          d?.details?.overallSavingsBytes ?? 0,
        ),
      };
    })
    .sort(
      (a, b) =>
        b.overallSavingsMs - a.overallSavingsMs || b.overallSavingsBytes - a.overallSavingsBytes,
    );
}

/**
 * Filters fall into four buckets derived from token-name prefixes. The
 * grouping mirrors the human-friendly chip categorisation in the 1.1
 * filter reference. Anything that doesn't pattern-match lands in `other`
 * — a deliberate catch-all so new tokens added upstream surface in the
 * snippet instead of silently disappearing.
 */
export type FilterCategory = 'image' | 'css' | 'js' | 'cache' | 'other';

function categorizeFilter(token: string): FilterCategory {
  if (
    token.includes('image') ||
    token.includes('jpeg') ||
    token.includes('png') ||
    token.includes('webp') ||
    token.includes('sprite')
  )
    return 'image';
  if (token.includes('javascript') || token.includes('_js')) return 'js';
  if (token.includes('css')) return 'css';
  if (
    token.includes('cache') ||
    token.includes('preload') ||
    token.includes('dns_prefetch') ||
    token === 'extend_cache'
  )
    return 'cache';
  return 'other';
}

const CATEGORY_ORDER: FilterCategory[] = ['image', 'css', 'js', 'cache', 'other'];
const CATEGORY_COMMENTS: Record<FilterCategory, string> = {
  image: '# Image transcoding (WebP, resize, lazy-load)',
  css: '# CSS rewriting (combine, inline critical, defer)',
  js: '# JavaScript minification + deferral',
  cache: '# Caching, preload, DNS prefetch',
  other: '# Other filters',
};

export function buildConfigSnippet11(items: AggregatedAudit[]): string {
  // Dedupe tokens parsed from each item's hand-authored snippet11.
  // Anything the author deliberately left out of the snippet (e.g.
  // experimental filters listed in mapping.filters for chip display only)
  // stays out of the recommended config.
  const filterTokens = new Set<string>();
  for (const it of items) {
    // Only consider items that 1.1 actually covers.
    if (!it.mapping.availableIn.includes('1.1')) continue;
    const snippet = it.mapping.snippet11;
    if (!snippet) continue;
    const m = /EnableFilters\s+([^;]+);/.exec(snippet);
    if (!m) continue;
    for (const f of m[1].split(',')) {
      const token = f.trim();
      if (token) filterTokens.add(token);
    }
  }
  if (filterTokens.size === 0) return '';
  // Bucket the tokens by category. One EnableFilters line per non-empty
  // bucket so the snippet reads vertically on a 360px viewport instead
  // of horizontally-scrolling as one giant line (the v1 problem).
  const byCategory: Record<FilterCategory, string[]> = {
    image: [],
    css: [],
    js: [],
    cache: [],
    other: [],
  };
  for (const token of filterTokens) {
    byCategory[categorizeFilter(token)].push(token);
  }
  for (const cat of CATEGORY_ORDER) {
    byCategory[cat].sort();
  }

  const lines: string[] = [
    '# mod_pagespeed — the module, nginx syntax.',
    '# Add to your server { } or location { } block.',
    'pagespeed on;',
    '',
  ];
  let first = true;
  for (const cat of CATEGORY_ORDER) {
    const tokens = byCategory[cat];
    if (tokens.length === 0) continue;
    if (!first) lines.push('');
    first = false;
    lines.push(CATEGORY_COMMENTS[cat]);
    lines.push(`pagespeed EnableFilters ${tokens.join(',')};`);
  }
  return lines.join('\n');
}

/**
 * The worker has no `EnableFilters` directive — image transcoding + CSS/JS
 * minification + critical CSS + content-hashed cache extension all run
 * from `pagespeed on;` plus a Cyclone cache file path. The only audit
 * that contributes a non-default directive is `document-latency-insight`
 * / `server-response-time` (set `pagespeed_html_max_age` to cache HTML
 * for some seconds when the origin sends no Cache-Control), or
 * `cache-insight` (opt into `pagespeed_cache_mode aggressive` after
 * validating output).
 *
 * Strategy: collect any extra directives the per-audit `snippet20`
 * contributes beyond the always-on baseline, deduped.
 */
export function buildConfigSnippet20(items: AggregatedAudit[]): string {
  // Always-on baseline. Order matters — `pagespeed on` first, cache
  // path second.
  const baseline = new Set<string>([
    'pagespeed on;',
    'pagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  ]);
  const extras = new Set<string>();
  const unknownAuditTitles: string[] = [];

  for (const it of items) {
    if (!it.mapping.availableIn.includes('2.0')) continue;
    const snippet = it.mapping.snippet20;
    if (!snippet) {
      // The audit IS covered by 2.0 but no concrete directive was
      // documented. Most of these mean "no extra directive needed, the
      // master switch turns it on" — but flag any audit whose snippet
      // we didn't author, so the snippet doesn't silently drop
      // 2.0-only audits.
      if (!it.mapping.availableIn.includes('1.1')) {
        unknownAuditTitles.push(it.mapping.auditTitle);
      }
      continue;
    }
    for (const rawLine of snippet.split('\n')) {
      const line = rawLine.trim();
      if (!line || line.startsWith('#')) continue;
      if (!baseline.has(line)) {
        extras.add(line);
      }
    }
  }

  // If only the always-on baseline applies and no 2.0-only audits were
  // flagged, no snippet is needed — the page is covered by a default
  // install. Render nothing in that case to keep the lead-magnet honest.
  if (extras.size === 0 && unknownAuditTitles.length === 0) {
    // We still emit the baseline so the customer can paste a starting
    // point if they're new to the worker. Skip only when there's
    // literally no flagged worker-covered audit.
    const anyTwoOhCoverage = items.some(
      (it) => it.mapping.availableIn.includes('2.0') && it.mapping.coverage !== 'none',
    );
    if (!anyTwoOhCoverage) return '';
  }

  const lines: string[] = [
    '# mod_pagespeed — module + optimizer worker.',
    '# Add the nginx directives to your server { } or location { } block;',
    '# launch the worker with the listed flags (or use the equivalent',
    '# appsettings.json keys in the ASP.NET Core middleware).',
    '',
    '# nginx — master switch + Cyclone cache file (required).',
    'pagespeed on;',
    'pagespeed_cache_path /var/lib/pagespeed/cache.vol;',
  ];

  if (extras.size > 0) {
    lines.push('');
    lines.push('# Additional directives suggested by this report:');
    for (const directive of extras) {
      lines.push(directive);
    }
  }

  lines.push('');
  lines.push('# factory_worker — image transcoding, CSS/JS minification,');
  lines.push('# critical CSS, lazy-load, LCP preload, preconnect, async');
  lines.push('# CSS, and script deferral are all on by default. Add');
  lines.push('# --no-<transform> flags to disable any of them.');
  lines.push('factory_worker \\');
  lines.push('  --cache-path /var/lib/pagespeed/cache.vol \\');
  lines.push('  --socket /var/lib/pagespeed/pagespeed.sock');

  if (unknownAuditTitles.length > 0) {
    lines.push('');
    lines.push('# These flagged audits are covered by the optimizer worker,');
    lines.push('# but no extra directive was documented — they ride on the');
    lines.push('# master switch above:');
    for (const t of unknownAuditTitles) {
      lines.push(`#   - ${t}`);
    }
  }

  return lines.join('\n');
}

export function buildConfigSnippet(items: AggregatedAudit[], edition: Edition): string {
  return edition === '2.0' ? buildConfigSnippet20(items) : buildConfigSnippet11(items);
}

/** The headline coverage sentence. */
export function coverageSentence(
  totalFlagged: number,
  addressed: number,
  mappedFlagged: number,
): string {
  const unmapped = totalFlagged - mappedFlagged;
  if (totalFlagged === 0) {
    return 'PSI flagged no failing performance audits on this page — your site is already fast.';
  }
  if (addressed === 0) {
    return `PSI flagged ${totalFlagged} performance ${totalFlagged === 1 ? 'audit' : 'audits'}; mod_pagespeed cannot automatically fix any of them on this page.`;
  }
  const tail =
    unmapped > 0
      ? ` (${unmapped} additional ${unmapped === 1 ? 'audit is' : 'audits are'} not yet mapped — see the project mapping data.)`
      : '';
  return `mod_pagespeed addresses ${addressed} of ${totalFlagged} flagged ${totalFlagged === 1 ? 'audit' : 'audits'}.${tail}`;
}

/** The headline savings sentence, over the fixable audits. */
export function savingsSentence(totalMs: number, totalBytes: number): string {
  const parts: string[] = [];
  if (totalMs > 0) parts.push(`${formatMs(totalMs)} of estimated load-time savings`);
  if (totalBytes > 0) parts.push(`${formatBytes(totalBytes)} of byte savings`);
  return parts.length
    ? `Across the fixable audits PSI flagged, mod_pagespeed could recover ${parts.join(' and ')} on this page.`
    : 'PSI did not attach numeric savings estimates to the flagged audits.';
}

export function classifyError(err: unknown): { title: string; body: string } {
  if ((err as { reload?: boolean })?.reload) {
    return {
      title: 'This page needs a reload.',
      body: (err as { message?: string }).message ?? '',
    };
  }
  const status = (err as { status?: number })?.status;
  const message = (err as { message?: string })?.message ?? 'Unknown error.';
  if (status === 429) {
    return {
      title: 'PSI rate limit hit.',
      body: 'You’ve hit the per-visitor rate limit on this page. Wait a moment and try again.',
    };
  }
  if (status === 400 || status === 404) {
    return {
      title: 'PSI couldn’t analyze that URL.',
      body: `${message} Common causes: the page redirected, returned an error, or blocks Googlebot. Try the canonical URL of the page.`,
    };
  }
  if (status && status >= 500) {
    return {
      title: 'PSI is having trouble.',
      body: 'Google’s PageSpeed Insights service returned a server error. This usually clears in under a minute.',
    };
  }
  return {
    title: 'Couldn’t reach PSI.',
    body: `${message} Check your network connection and try again.`,
  };
}

// What a failed PSI request is called, on the tile and in the panel alike. A
// timeout already says what happened; the network advice would be wrong.
export function describePsiError(err: { kind?: string; message: string; status?: number }): {
  title: string;
  body: string;
} {
  if (err.kind === 'timeout')
    return { title: 'PageSpeed Insights did not answer.', body: err.message };
  const c = classifyError(err);
  // The shared network fallback keeps its wording for the original analyzer.
  if (c.title === 'Couldn’t reach PSI.')
    return { ...c, title: 'PageSpeed Insights did not answer.' };
  if (err.status === 429) return { ...c, title: 'Rate limit reached. Try again in a minute.' };
  if (err.status === 400 || err.status === 404)
    return { ...c, title: 'PageSpeed Insights could not analyze that URL.' };
  if (err.status && err.status >= 500)
    return { ...c, title: 'PageSpeed Insights returned an error. Try again later.' };
  return c;
}
