// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Indexable pages served as hand-written HTML from `public/` rather than as
 * Astro routes. The sitemap integration only sees Astro routes, so these are
 * passed to it as custom pages. A page in `public/` with a canonical link and
 * no robots noindex must be listed here (test/static-html-pages.test.ts).
 */
export const STATIC_INDEXABLE_PAGES = ['/cyclone-benchmark/'];
