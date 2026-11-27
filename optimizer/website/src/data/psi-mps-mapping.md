# PSI → mod_pagespeed mapping table — methodology

This document explains how `psi-mps-mapping.ts` was built and how to keep it
honest as Lighthouse and mod_pagespeed evolve.

## What this is

`psi-mps-mapping.ts` is the data layer behind `modpagespeed.com/analyze`. The
page calls PSI v5 for a customer-supplied URL and uses the table to convert
"here are the failing audits" into "here's the filter config you need."

The mapping is opinionated: it deliberately reports `coverage: "none"` for
audits mod_pagespeed cannot honestly fix, even when there is a tempting
filter that might appear related. Customer trust is more valuable than a
high coverage count.

## Source of truth

- **Audit list:** Lighthouse `core/config/default-config.js`, Performance
  category. The list was extracted from the GoogleChrome/lighthouse
  repository against the tip of `main` (audit IDs are stable; the set
  evolves slowly).
- **Filter inventory:** the filters index, `/docs/filters/`
  (`website/src/pages/docs/filters.astro`, rendered from
  `website/src/data/filters.ts`).
  The 1.15 filter set is the canonical superset; the optimizer worker
  implements a subset implicitly (no `EnableFilters` directive — the worker runs image
  transcoding, CSS/JS minification, and critical CSS inlining whenever
  `pagespeed on` is set).
- **Filter behavior + caveats:** the `Risk` column of the filters index (`/docs/filters/`)
  ("Generally safe" / "Test first" / "Experimental") drives the
  `confidence` field.

## Coverage rubric

| Coverage  | Meaning                                                                              |
| --------- | ------------------------------------------------------------------------------------ |
| `full`    | MPS has at least one filter that directly addresses the audit on every page.         |
| `partial` | MPS reduces the problem but does not eliminate it (e.g., minify but not tree-shake). |
| `none`    | Structural or application-level — MPS cannot help without rewriting authored code.   |

## Confidence rubric

| Confidence     | Trigger                                                                               |
| -------------- | ------------------------------------------------------------------------------------- |
| `high`         | Filter is a CoreFilter or marked "Generally safe" in the reference table.             |
| `medium`       | Filter is marked "Test first" or coverage is partial.                                 |
| `experimental` | Filter is marked "Experimental" in the reference, or has caused customer regressions. |

## Audits deliberately marked `coverage: "none"`

These get a one-line justification in the table; documented here so future
maintainers don't second-guess the call.

| Audit                                           | Why MPS cannot fix it                                                                                                                                                          |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `interaction-to-next-paint`                     | Event-handler JS — rewriting changes behavior.                                                                                                                                 |
| `inp-breakdown-insight`                         | Same root cause as INP.                                                                                                                                                        |
| `font-display-insight`                          | `font-display: swap` is authored in CSS `@font-face` rules.                                                                                                                    |
| `duplicated-javascript-insight`                 | Multiple bundles shipping the same dep — bundler-level fix.                                                                                                                    |
| `legacy-javascript-insight`                     | Polyfills and ES5 transpile — needs module/nomodule build split.                                                                                                               |
| `modern-http-insight`                           | nginx config (`listen 443 ssl http2;`), not content rewriting.                                                                                                                 |
| `third-parties-insight`                         | Cross-origin scripts MPS does not proxy.                                                                                                                                       |
| `document-latency-insight`                      | 2.0 marks this `partial` because Cyclone HTML caching masks origin TTFB on repeat requests. 1.15 cannot — origin TTFB / DNS / TLS sit outside the 1.15 rewriting layer entirely. |
| `dom-size-insight`                              | Structural — only the application can reduce node count.                                                                                                                       |
| `viewport-insight`                              | Missing `<meta name="viewport">` is an authoring concern.                                                                                                                      |
| `forced-reflow-insight`                         | JS reads layout after mutating DOM — code-level fix.                                                                                                                           |
| `slow-css-selector-insight`                     | Selector semantics; `rewrite_css` preserves them deliberately.                                                                                                                 |
| `unused-javascript`                             | Tree-shaking needs static analysis of bundler graphs.                                                                                                                          |
| `non-composited-animations`                     | Animation property choice (transform vs top/left) is authored CSS.                                                                                                             |
| `bf-cache`                                      | Response headers + lifecycle handlers — application controls these.                                                                                                            |
| `user-timings`                                  | Diagnostic only; no optimization expected.                                                                                                                                     |
| `uses-text-compression`                         | gzip/brotli live on nginx itself, not in PageSpeed filters.                                                                                                                    |
| `redirects`                                     | Origin/CDN config concern.                                                                                                                                                     |
| `server-response-time`                          | 2.0 marks this `partial` for the same reason as `document-latency-insight`. 1.15 cannot help (origin TTFB sits inside the application).                                         |
| `network-requests` / `-rtt` / `-server-latency` | Diagnostic listings, not fixable defects.                                                                                                                                      |

## Audits where MPS _might_ hurt PSI score

PSI's scoring is byte-counted; some MPS wins trade one metric for another.
Document these honestly so the /analyze copy can flag them:

- **`prioritize_critical_css`** inlines above-the-fold CSS into the HTML.
  This increases initial HTML byte count and can show up in
  `total-byte-weight` and `network-requests` as a regression even though
  it improves FCP/LCP. Net wins are typical, but PSI's diagnostic numbers
  may look mixed.
- **`inline_css` / `inline_javascript`** trade an extra request for inline
  bytes. On pages with strong HTTP/2 multiplexing the win is marginal and
  the HTML grows.
- **`responsive_images`** generates `srcset`. PSI may flag the additional
  candidate URLs in `network-requests` even though only one is fetched.
- **`lazyload_images`** can cause LCP regressions when the LCP element is
  below the fold or detected as such by the lazy-load heuristic. The
  filter is marked "Test first" for this reason.
- **`defer_javascript`** can move TBT savings into INP regressions — work
  deferred to after load may run when the user is interacting.
- **`hint_preload_subresources`** adds `Link: rel=preload` headers. If the
  hint is wrong (e.g., a resource that the page doesn't use after all),
  PSI's `uses-rel-preload` audit can flip from a positive to a warning.

## 1.15 vs 2.0 coverage

`/analyze` ships two editions of the recommendation, labelled for the two
parts of mod_pagespeed: the optimizer worker (default) and the module.
The `availableIn` field on each mapping declares which
edition(s) actually address the audit; the `snippet11` and `snippet20`
fields hold edition-specific config syntax.

### Why the two are different

- **1.15** exposes a hand-curated list of ~80 named filters that operators
  enable individually via `pagespeed EnableFilters foo,bar;`. The filter
  inventory is documented on the filters index, `/docs/filters/`.
- **2.0** has no named filter list at all. The nginx module surface is two
  required directives (`pagespeed on;` + `pagespeed_cache_path …;`) and a
  handful of optional knobs (`pagespeed_cache_mode`, `pagespeed_disallow`,
  `pagespeed_html_max_age`, `pagespeed_trust_x_forwarded_proto`, the
  `pagespeed_*_max_age` family). All transforms run from the master switch
  by default; each transform has a `--no-…` worker flag to turn it off.
  The `factory_worker` binary exposes the per-transform flags
  (`--no-lazy-load-images`, `--no-image-dimensions`, `--no-lcp-preload`,
  `--no-preconnect-injection`, `--no-async-css`, `--no-script-deferral`,
  `--no-css-import-flattening`). Image transcoding (WebP + AVIF + viewport
  - Save-Data + density variants), CSS/JS minification, and content-hashed
    cache extension are always-on under `pagespeed on` — turn off only via
    top-level switches like `--disable-image`. Source of truth:
    `website/src/content/docs/configuration.md`.

### Classification criteria

Each mapping was classified into one of three buckets:

1. **Both editions (`availableIn: ['1.1', '2.0']`)** — applies to most
   audits. Both products cover image delivery, render-blocking CSS,
   minification, cache extension, image dimensions, and the various
   metric-level (FCP/LCP/CLS) wins. The `snippet11` uses
   `pagespeed EnableFilters …;` form; the `snippet20` uses
   `pagespeed on;` + `pagespeed_cache_path …;` plus any non-default
   directive the audit specifically motivates.
2. **2.0 only (`availableIn: ['2.0']`)** — currently two audits:
   `server-response-time` (TTFB) and `document-latency-insight`. Both rely
   on 2.0's HTML caching in the Cyclone shared-memory cache (1.15 never
   caches HTML at the module layer).
3. **1.15 only (`availableIn: ['1.1']`)** — currently
   `efficient-animated-content`. The 1.15 filter
   `convert_to_webp_animated` is "test first" / experimental in the 1.15
   reference and was not ported to the 2.0 worker.

### 2.0's HTML caching

The load-bearing 1.15-vs-2.0 difference for sites with slow origin TTFB is
that 2.0 caches HTML responses in the shared-memory Cyclone cache when the
origin allows it (`Cache-Control: public, max-age=N, must-revalidate` on
HTML, or set `pagespeed_html_max_age` for origins that send no
Cache-Control). Cache HITs are served zero-copy from the memory-mapped
file with no origin round-trip. HTML output `Cache-Control` is always
`no-cache` so downstream browsers and CDNs revalidate, but conditional
revalidation (`pagespeed_conditional_revalidation on`, default) makes that
cheap. 1.15's behaviour is unchanged: HTML always passes through to the
origin and the module rewrites the response in flight.

The mapping reflects this by promoting `server-response-time` and
`document-latency-insight` from `coverage: 'none'` to `coverage:
'partial'` on 2.0. Customer-facing copy never claims the optimizer worker "fixes slow
TTFB" — only that it can mask it on repeat visits when the origin allows
HTML caching.

### Filters with no documented 2.0 syntax

Most 1.15 filters don't have a separate 2.0 directive because the matching
transform is always-on (and the snippet just emits `pagespeed on;` plus
the Cyclone cache path). One audit (`efficient-animated-content`) is
declared 1.15-only — animated GIF → WebP transcoding doesn't have a
documented 2.0 control surface in the configuration reference.

Two diagnostics show edition-specific behaviour even though both editions
"address" them via infrastructure rather than mod_pagespeed filters:

- `uses-text-compression`: in 1.15 customers set `gzip on;` / `brotli on;`
  on nginx. In 2.0 the worker pre-compresses every cached text resource
  (gzip + brotli alternates next to identity) and nginx serves the
  pre-compressed body — but the audit is still satisfied by nginx-level
  configuration, so coverage stays `none`.

### Refresh cadence (extended)

- **Annual review (Q1):** in addition to checking Lighthouse for new
  audit IDs, diff `website/src/content/docs/configuration.md` against the
  previous year's snapshot. If 2.0 adds a new always-on transform or a
  new directive, update `availableIn` and `snippet20` accordingly.
- **On 2.0 directive renames or new flags:** review the `snippet20`
  fields. The 2.0 surface is small enough that a full audit takes ~15
  minutes.
- **On parity changes:** if a filter currently marked 1.15-only ships in
  2.0 (or vice versa), update `availableIn` and consider whether the
  `coverage` rating changes.

## Refresh cadence

- **Annual review (Q1):** diff against
  `core/config/default-config.js` on the Lighthouse `main` branch. New
  audit IDs get added; deprecated IDs stay in the table with a comment.
- **On filter changes:** if the filters index (`/docs/filters/`) changes (renames, new
  filters, deprecations), update the `filters` array in any affected
  entries.
- **On customer regressions:** if a filter consistently makes a specific
  audit worse, downgrade its `confidence` and add a `caveats` line.

A simple sanity check: run `psi-mps-mapping.ts` through `tsc --noEmit` from
`website/`. The schema is the contract; everything else is editorial.
