// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Proxy /v1/* API requests to the worker HTTP API during development.
// In production the static SPA is served by the worker itself, so
// /v1/* routes are handled natively.  During `pnpm dev` SvelteKit's
// dev server intercepts all requests before Vite's server.proxy can
// act, so we proxy here instead.

import type { Handle } from '@sveltejs/kit/hooks';

const API_TARGET = 'http://127.0.0.1:9880';

// Hop-by-hop headers that must not be forwarded through a proxy.
const HOP_BY_HOP = new Set([
  'connection',
  'keep-alive',
  'transfer-encoding',
  'te',
  'trailer',
  'upgrade',
  'proxy-authorization',
  'proxy-authenticate',
]);

export const handle: Handle = async ({ event, resolve }) => {
  const { pathname } = event.url;

  // Proxy API paths to the worker.
  if (pathname.startsWith('/v1/')) {
    const target = new URL(pathname + event.url.search, API_TARGET);

    try {
      const resp = await fetch(target.toString(), {
        method: event.request.method,
        headers: {
          'accept': event.request.headers.get('accept') ?? 'application/json',
          'content-type': event.request.headers.get('content-type') ?? '',
          'authorization': event.request.headers.get('authorization') ?? '',
          // CSRF: forward X-Requested-With from the SPA so the worker's
          // CSRF check on mutating /v1/* requests sees an XHR-marked request.
          // Without this, `pnpm dev` proxy strips the header and every
          // POST/PATCH gets a 400 from the worker.
          'x-requested-with':
            event.request.headers.get('x-requested-with') ?? '',
        },
        body: event.request.method !== 'GET' && event.request.method !== 'HEAD'
          ? event.request.body
          : undefined,
        // @ts-expect-error — duplex required for streaming request bodies
        duplex: 'half',
      });

      // Filter out hop-by-hop headers from upstream response.
      const proxyHeaders = new Headers();
      resp.headers.forEach((value, key) => {
        if (!HOP_BY_HOP.has(key.toLowerCase())) {
          proxyHeaders.set(key, value);
        }
      });

      // Read body as arrayBuffer to avoid streaming issues.
      const body = await resp.arrayBuffer();

      return new Response(body, {
        status: resp.status,
        statusText: resp.statusText,
        headers: proxyHeaders,
      });
    } catch (err) {
      return new Response(
        JSON.stringify({ error: { code: 'PROXY_ERROR', message: String(err) } }),
        { status: 502, headers: { 'content-type': 'application/json' } },
      );
    }
  }

  return resolve(event);
};
