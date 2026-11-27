// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import type { MiddlewareHandler } from 'astro';
import { basename } from 'node:path';

/**
 * Exact redirect map: old root-level URLs → new 2.0 equivalents.
 * All other /doc/* paths fall through to the catch-all /1.0/doc/* redirect.
 */
const REDIRECTS: Record<string, string> = {
  '/doc': '/docs/',
  '/doc/': '/docs/',
  '/doc/index': '/docs/',
  '/doc/configuration': '/docs/configuration/',
  '/doc/build_ngx_pagespeed_from_source': '/docs/installation-module/',
  '/doc/build_from_source': '/docs/installation-module/',
  '/doc/build_mod_pagespeed_from_source': '/docs/installation-module/',
  '/doc/download': '/docs/installation-docker/',
  '/doc/system': '/docs/deployment/',
  '/doc/faq': '/docs/troubleshooting/',
  '/doc/admin': '/docs/api-reference/',
  '/doc/console': '/docs/api-reference/',
};

/**
 * Middleware: SEO redirects + extensionless /1.0/ URL rewriting.
 *
 * NOTE: In production, Astro 5's @astrojs/node standalone adapter only runs
 * middleware for SSR routes (prerender: false). Legacy doc URLs and SEO
 * redirect source URLs don't match any SSR route, so redirects 1-4 below
 * are handled by nginx instead (see tools/workbench-demo/nginx.conf and
 * deploy/staging/nginx.conf). The rules here serve as documentation and
 * provide correct behavior in the Vite dev server.
 *
 * 1. Exact redirects for old doc URLs with new equivalents.
 * 2. Catch-all: /doc/* → /1.0/doc/* (archive fallback).
 * 3. Legacy /examples/*.html → relevant new pages (the live optimization
 *    gallery now owns extensionless /examples/ routes). /psol/ is a real
 *    landing page now (see note at the rule below); it is not redirected here.
 * 4. Extensionless /1.0/ rewrite (legacy docs use href="configuration"
 *    but files on disk have .html extensions).
 */
export const onRequest: MiddlewareHandler = async (context, next) => {
  const url = new URL(context.request.url);
  const pathname = url.pathname;

  // 0a. Redirect /.well-known/llms.txt → /llms.txt
  if (pathname === '/.well-known/llms.txt') {
    return context.redirect('/llms.txt', 301);
  }

  // 0b. The styled error pages live at the extensionless /404 and /500 routes.
  // A literal request to the trailing-slash variant doesn't match the special
  // error route and falls through to the 404 page; redirect it to the canonical
  // no-slash URL so /500/ serves the real 500 page. The build prerenders the
  // error pages under the trailing-slash path, so the rule must not run then:
  // it would write a redirect stub into 404.html and 500.html instead of the
  // pages, and the built server would answer every miss with that stub. The dev
  // server marks these routes as prerendered too, so check for dev explicitly.
  if (
    (import.meta.env.DEV || !context.isPrerendered) &&
    (pathname === '/500/' || pathname === '/404/')
  ) {
    return context.redirect(pathname.replace(/\/$/, ''), 301);
  }

  // 1. Exact redirects (old → new equivalent)
  const exactTarget = REDIRECTS[pathname];
  if (exactTarget) {
    return context.redirect(exactTarget, 301);
  }

  // 2. Catch-all: /doc/* → /1.0/doc/* (archive fallback)
  if (pathname.startsWith('/doc/') || pathname === '/doc') {
    return context.redirect('/1.0' + pathname, 301);
  }

  // 3. The old open-source site's per-filter demos lived at /examples/<filter>.html.
  // The live optimization gallery now owns /examples/ and /examples/<slug>/, so the
  // legacy .html URLs 301 to the gallery index (keeping their accrued link equity
  // for "mod_pagespeed example" intent) rather than to /features/.
  if (pathname.startsWith('/examples/') && pathname.endsWith('.html')) {
    return context.redirect('/examples/', 301);
  }
  // NOTE: /psol/ is NOT redirected here. It is now a real landing page
  // (src/pages/psol.astro) that explains the PageSpeed Optimization Libraries
  // today and routes to 1.1 / 2.0 / NuGet. A middleware redirect here would
  // shadow that page during prerender. The deep Doxygen leaf URLs
  // (/psol/hierarchy.html, /psol/dir_*.html, …) are 301'd to /psol/ by nginx
  // in prod (the production nginx configuration), not here.

  // 4. Extensionless /1.0/ redirect — legacy docs use extensionless hrefs
  // (e.g. href="configuration") but files have .html extensions.
  // context.rewrite() doesn't serve static files in SSR mode, so redirect
  // to the .html URL which the static file handler serves directly.
  // Dev/preview uses the Vite plugin (rewriteLegacyUrl) instead.
  if (
    pathname.startsWith('/1.0/') &&
    !pathname.endsWith('/') &&
    !basename(pathname).includes('.')
  ) {
    return context.redirect(pathname + '.html', 301);
  }

  const response = await next();

  // 5. Add X-Robots-Tag: noindex to internal API endpoints
  if (pathname.startsWith('/api/') && pathname !== '/api/product.json') {
    response.headers.set('X-Robots-Tag', 'noindex');
  }

  return response;
};
