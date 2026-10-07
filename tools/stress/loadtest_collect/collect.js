/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Headless-Chrome slurp driver for load-test corpus collection.
//
// Replaces the abandonware phantomjs script.js. For each page URL it drives a
// real headless Chrome through the mod_pagespeed slurp proxy and waits for the
// network to go idle, so every sub-resource is fetched (and recorded by the
// slurp proxy) before moving on. Unlike phantomjs's load-event callback,
// `networkidle2` captures lazily/async-loaded resources too — a more complete
// slurp.
//
// Usage:
//   node collect.js <pages.txt> [proxyHost:port]
// where pages.txt has one page URL (including http://) per line.
//
// Chrome binary: set CHROME_PATH to override; otherwise common Linux/macOS
// locations are auto-detected. Uses puppeteer-core (no bundled Chromium) to
// keep the dependency small and to reuse the system browser.
//
// MODERN-CHROME / HTTPS NOTES (the slurp proxy speaks plaintext HTTP):
//   * Feed pages.txt http:// URLs. The slurp proxy can still record an https
//     origin: map it in the record vhost with
//       ModPagespeedMapOriginDomain https://example.com http://example.com
//     so the fetch is upgraded while the browser stays on plaintext http.
//   * Modern Chrome auto-upgrades http->https (HTTPS-Upgrades, and learns HSTS
//     from a 'Strict-Transport-Security' response header) which would defeat the
//     plaintext proxy. We disable the upgrade heuristics below; the record vhost
//     must ALSO strip the STS header (`Header always unset
//     Strict-Transport-Security`) so Chrome never learns to upgrade mid-run.
//   * HSTS-*preloaded* domains (baked into the Chrome build) cannot be kept on
//     http by any runtime flag — Chrome upgrades them a priori. Such domains
//     are not capturable through the plaintext slurp proxy; collect them from a
//     non-preloaded hostname or exclude them.
//   * We scope requests to the page hostnames (see allowedHosts) and abort
//     everything else, so third-party assets (analytics, fonts, absolute-https
//     references) are neither hammered nor left hanging the networkidle wait.

'use strict';

const fs = require('fs');
const puppeteer = require('puppeteer-core');

function detectChrome() {
  if (process.env.CHROME_PATH) {
    return process.env.CHROME_PATH;
  }
  const candidates = [
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/snap/bin/chromium',
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
  ];
  for (const candidate of candidates) {
    if (fs.existsSync(candidate)) {
      return candidate;
    }
  }
  throw new Error(
      'No Chrome/Chromium binary found. Set CHROME_PATH to its location.');
}

async function main() {
  const pagesFile = process.argv[2];
  const proxy = process.argv[3];  // optional "host:port"
  if (!pagesFile) {
    console.error('Usage: node collect.js <pages.txt> [proxyHost:port]');
    process.exit(1);
  }

  const urls = fs.readFileSync(pagesFile, 'utf8')
                   .split('\n')
                   .map((s) => s.trim())
                   .filter((s) => s && !s.startsWith('#'));
  if (urls.length === 0) {
    console.error('No URLs in ' + pagesFile);
    process.exit(1);
  }

  // Scope capture to the hostnames that appear in pages.txt. When proxying, we
  // abort any request to a host outside this set (third-party scripts, fonts,
  // analytics, absolute-https references) so the slurp stays first-party and the
  // networkidle wait doesn't stall on un-tunnelable CONNECTs.
  const allowedHosts = new Set();
  for (const u of urls) {
    try {
      allowedHosts.add(new URL(u).hostname);
    } catch (e) { /* skip malformed */ }
  }

  const args = [
    '--no-sandbox',
    '--disable-gpu',
    '--disable-dev-shm-usage',
    // The slurp proxy serves self-signed / rewritten content; don't let cert
    // or HSTS errors abort the page load.
    '--ignore-certificate-errors',
    // Keep the browser on plaintext http: disable the auto-upgrade heuristics
    // that would turn our http page loads into https (and bypass the proxy).
    '--disable-features=HttpsUpgrades,HttpsFirstBalancedMode,HttpsFirstModeV2,' +
        'AutoupgradeHttp,AutofillServerCommunication,OptimizationHints',
    // Silence Chrome's background phone-home so it doesn't clutter the slurp.
    '--disable-background-networking',
    '--no-first-run',
    '--no-default-browser-check',
    '--disable-component-update',
    '--disable-client-side-phishing-detection',
    '--disable-sync',
    '--metrics-recording-only',
    '--disable-default-apps',
    '--disable-domain-reliability',
    '--disable-breakpad',
    '--no-pings',
  ];
  if (proxy) {
    args.push('--proxy-server=' + proxy);
  }

  const browser = await puppeteer.launch({
    headless: 'new',
    executablePath: detectChrome(),
    args,
  });

  let ok = 0;
  let failed = 0;
  for (const url of urls) {
    const page = await browser.newPage();
    if (proxy) {
      await page.setRequestInterception(true);
      page.on('request', (req) => {
        let host = '';
        try { host = new URL(req.url()).hostname; } catch (e) { /* */ }
        // First-party http only; abort third-party + https (un-tunnelable).
        if (req.url().startsWith('http://') && allowedHosts.has(host)) {
          req.continue();
        } else {
          req.abort();
        }
      });
    }
    try {
      console.log('[collect] ' + url);
      // networkidle2 = no more than 2 in-flight requests for 500ms => the page
      // (and its rewrite-eligible sub-resources) has settled.
      await page.goto(url, {waitUntil: 'networkidle2', timeout: 60000});
      // Brief grace for any trailing lazy-loaded resources.
      await new Promise((r) => setTimeout(r, 1500));
      ok++;
    } catch (e) {
      console.error('[collect] WARN ' + url + ': ' + e.message);
      failed++;
    } finally {
      await page.close();
    }
  }

  await browser.close();
  console.log(
      '[collect] done: ' + ok + ' ok, ' + failed + ' failed, ' + urls.length +
      ' total');
}

main().catch((e) => {
  console.error('[collect] FATAL: ' + (e && e.stack ? e.stack : e));
  process.exit(1);
});
