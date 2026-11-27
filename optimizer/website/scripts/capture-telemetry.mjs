// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// capture-telemetry.mjs — capture the site's real response measurements and
// write src/data/telemetry-capture.json, the dated data file behind the
// telemetry strip's server-rendered fallback values.
//
// For each path one GET with `accept-encoding: gzip` is issued over
// node:https; the wire bytes are the raw (compressed) response body, the
// decoded bytes come from zlib.gunzipSync. ttfbMs is the wire-level time to
// the first response byte on a warm connection: a keep-alive agent holds the
// socket open from a warm-up GET, and the measured GET is timed from its
// request leaving the client to the first response bytes — DNS/TCP/TLS
// excluded, the server-side equivalent of the strip's live definition (the
// HEAD probe's responseStart − requestStart). The strip upgrades to live
// values in the browser; this file is what no-JS readers and first paint see,
// always labelled with its capture date.
//
// Run before each site deploy and at least monthly:
//   npm run capture:telemetry                      # against production
//   node scripts/capture-telemetry.mjs <origin>    # against another origin

import { get, Agent } from 'node:https';
import { gunzipSync } from 'node:zlib';
import { writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const ORIGIN = process.argv[2] ?? 'https://modpagespeed.com';
const PATHS = ['/', '/docs/', '/download/', '/pricing/', '/features/', '/demo/'];
const HEADER_KEYS = [
  'x-mod-pagespeed',
  'vary',
  'content-encoding',
  'content-type',
  'cache-control',
  'content-length',
];
const MAX_REDIRECTS = 3;
const TIMEOUT_MS = 15000;
// One keep-alive connection to the origin: the warm-up request below pays the
// DNS/TCP/TLS cost once, and every measured request then reuses the connected
// socket, so ttfbMs excludes connection setup by construction.
const agent = new Agent({ keepAlive: true, maxSockets: 1 });

const outFile = resolve(
  dirname(fileURLToPath(import.meta.url)),
  '../src/data/telemetry-capture.json',
);

function fetchPath(url, redirectsLeft, ttfbSoFar = 0) {
  return new Promise((resolvePromise, reject) => {
    let sentAt = 0;
    const req = get(
      url,
      {
        agent,
        headers: {
          'accept-encoding': 'gzip',
          accept: 'text/html,application/xhtml+xml',
          'user-agent': 'modpagespeed.com telemetry capture (build tooling)',
        },
      },
      (res) => {
        const status = res.statusCode ?? 0;
        // The response headers have arrived: that is the first byte of this
        // leg, so the leg's time to first byte is settled here.
        const legTtfbMs = Math.max(0, Math.round(performance.now() - sentAt));
        if (status >= 300 && status < 400 && res.headers.location) {
          res.resume();
          if (redirectsLeft <= 0) {
            reject(new Error(`too many redirects for ${url}`));
            return;
          }
          resolvePromise(
            fetchPath(new URL(res.headers.location, url), redirectsLeft - 1, ttfbSoFar + legTtfbMs),
          );
          return;
        }
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => {
          const body = Buffer.concat(chunks);
          const headers = Object.fromEntries(
            HEADER_KEYS.map((key) => [key, res.headers[key] ?? null]),
          );
          let decodedBytes = body.length;
          if ((res.headers['content-encoding'] ?? '').includes('gzip')) {
            try {
              decodedBytes = gunzipSync(body).length;
            } catch (err) {
              reject(new Error(`gunzip failed for ${url}: ${err.message}`));
              return;
            }
          }
          resolvePromise({
            status,
            wireBytes: body.length,
            decodedBytes,
            ttfbMs: ttfbSoFar + legTtfbMs,
            headers,
          });
        });
        res.on('error', reject);
      },
    );
    // 'finish' is the request being flushed to the socket: on the warm socket
    // the measured request rides, that moment is the wire-level requestStart.
    req.on('finish', () => {
      sentAt = performance.now();
    });
    req.setTimeout(TIMEOUT_MS, () => {
      req.destroy(new Error(`timeout after ${TIMEOUT_MS} ms for ${url}`));
    });
    req.on('error', reject);
  });
}

const origin = new URL(ORIGIN).origin;
const paths = {};
for (const path of PATHS) {
  const target = new URL(path, origin);
  // Warm the connection, then measure on the reuse.
  await fetchPath(target, MAX_REDIRECTS);
  const result = await fetchPath(target, MAX_REDIRECTS);
  paths[path] = result;
  console.log(
    `${path}: ${result.status} wire=${result.wireBytes} decoded=${result.decodedBytes} ` +
      `ttfb=${result.ttfbMs}ms x-mod-pagespeed=${result.headers['x-mod-pagespeed'] ?? 'not present'}`,
  );
}
agent.destroy();

const capture = {
  origin,
  capturedAt: new Date().toISOString(),
  paths,
};
writeFileSync(outFile, JSON.stringify(capture, null, 2) + '\n');
console.log(`wrote ${outFile}`);
