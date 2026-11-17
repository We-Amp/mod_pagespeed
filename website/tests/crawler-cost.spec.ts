// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The AI crawler cost meter: golden numbers in the browser, the one worker,
// the no-upload guarantee (a sentinel string from the fixture never leaves),
// hostile input rendered as text, no storage writes, gzip, offline, noscript,
// the analytics event and the link slots.
import { readFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { expect, test } from '@playwright/test';
import { stubUmami, trackedEvents } from './helpers/umami';

const fixture = (name: string) =>
  new URL(`../test/fixtures/crawler-cost/${name}`, import.meta.url).pathname;
const SENTINEL = 'SentinelBot-9f3a7c';

async function storageSnapshot(page: import('@playwright/test').Page) {
  return page.evaluate(async () => ({
    local: Object.entries(localStorage),
    session: Object.entries(sessionStorage),
    cookie: document.cookie,
    idb: ((await indexedDB.databases()) ?? []).map((d) => d.name),
    caches: await caches.keys(),
  }));
}

test.describe('AI crawler cost meter', () => {
  // The real tracker still loads and posts, but the endpoint is answered
  // locally so no test traffic reaches production analytics.
  test.beforeEach(async ({ page }) => {
    await page.route('**/api/send', (route) => route.fulfill({ status: 200, body: '{}' }));
  });

  test('the nginx fixture gives the golden numbers', async ({ page }) => {
    await page.goto('/ai-crawler-cost/');
    await expect(page.locator('#cc-drop[data-ready="1"]')).toBeAttached();
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-ratio')).toHaveText(
      '1.5 crawler page requests per visit an AI assistant sent back',
    );
    await expect(page.locator('#cc-ratio-note')).toContainText(
      '3 crawler page requests, 2 visits sent back',
    );
    const rows = page.locator('#cc-table tbody tr');
    await expect(rows).toHaveCount(3);
    await expect(rows.nth(0)).toContainText('GPTBot');
    await expect(rows.nth(0).locator('td').nth(3)).toHaveText('3');
    await expect(page.locator('#cc-totals')).toContainText('5 (62.5% of all requests)');
    await expect(page.locator('#cc-totals')).toContainText('33.9 KB');
    await expect(page.locator('#cc-notes')).toContainText('1 unparsed line skipped');
    await expect(page.locator('#cc-fetchers')).toContainText('(kind "fetcher"): 1 page request,');
    await expect(page.locator('#cc-ops tbody tr')).toHaveCount(2);
    await expect(page.locator('#cc-status')).toHaveText(
      'Done. Nothing from the log left your browser.',
    );
  });

  test('exactly one worker exists after load and no second one appears during an analysis', async ({
    page,
  }) => {
    await page.goto('/ai-crawler-cost/');
    await expect.poll(() => page.workers().length).toBe(1);
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    expect(page.workers().length).toBe(1);
  });

  test('nothing from the log leaves the browser and no third party is contacted', async ({
    page,
    baseURL,
  }) => {
    // The tracker only runs on the production hostnames, so it never posts from
    // the local server. The in-page recorder stands in for it: the analysis
    // must still report its one event, and that event must carry nothing.
    await stubUmami(page);
    await page.goto('/ai-crawler-cost/');
    const seen: Array<{ url: string; body: string | null }> = [];
    page.on('request', (r) => seen.push({ url: r.url(), body: r.postData() }));
    await page.setInputFiles('#cc-file', fixture('cc-hostile.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    await expect
      .poll(async () => (await trackedEvents(page)).filter((e) => e.name === 'crawler-cost'))
      .toHaveLength(1);
    expect(JSON.stringify(await trackedEvents(page))).not.toContain(SENTINEL);
    const own = new URL(baseURL ?? 'http://localhost').hostname;
    for (const r of seen) {
      expect(r.url).not.toContain(SENTINEL);
      expect(r.body ?? '').not.toContain(SENTINEL);
      const host = new URL(r.url).hostname;
      expect([own, 'modpagespeed.com', 'umami.we-amp.com'], r.url).toContain(host);
    }
  });

  test('no request of any kind carries log content, for every fixture and a gzip upload', async ({
    context,
    page,
  }) => {
    // Observe every request the context makes (page, worker, subframes), without altering any and
    // record URL, headers and body. None may contain a distinctive piece of
    // any uploaded log line: addresses, paths, agents, referrers.
    // Warm the dev server first so a cold start's reload is not in the picture.
    await page.goto('/ai-crawler-cost/');
    await page.waitForLoadState('networkidle');
    const seen: Array<{ url: string; headers: string; body: string }> = [];
    context.on('request', (r) => {
      void r.allHeaders().then((h) =>
        seen.push({
          url: r.url(),
          headers: JSON.stringify(h),
          body: r.postDataBuffer()?.toString('latin1') ?? '',
        }),
      );
    });
    const names = [
      'cc-nginx-combined.log',
      'cc-apache-combined.log',
      'cc-iis-w3c.log',
      'cc-hostile.log',
    ];
    const needles = new Set<string>([SENTINEL]);
    for (const n of names) {
      for (const line of readFileSync(fixture(n), 'utf8').split('\n')) {
        for (const m of line.matchAll(
          /\b\d{1,3}(?:\.\d{1,3}){3}\b|(?:GET|POST|HEAD) (\S{4,})|[A-Za-z]+Bot\/[\d.]+/g,
        )) {
          needles.add(m[1] ?? m[0]);
        }
      }
    }
    expect(needles.size).toBeGreaterThan(10);
    const before = seen.length;
    for (const n of names) {
      await page.goto('/ai-crawler-cost/');
      await page.setInputFiles('#cc-file', fixture(n));
      await expect(page.locator('#cc-results'), n).toBeVisible();
    }
    await page.goto('/ai-crawler-cost/');
    await page.setInputFiles('#cc-file', {
      name: 'cc.log.gz',
      mimeType: 'application/gzip',
      buffer: gzipSync(readFileSync(fixture('cc-nginx-combined.log'))),
    });
    await expect(page.locator('#cc-results')).toBeVisible();
    // Let any trailing analytics post settle.
    await page.waitForTimeout(500);
    expect(seen.length).toBeGreaterThan(before);
    for (const r of seen) {
      // The path of an own-site asset (for example /guide/) may coincide with a
      // log path, so a URL is checked by its query string and fragment, plus
      // its host when it is not this site; headers and bodies are checked whole.
      const u = new URL(r.url);
      const own = u.origin === new URL(page.url()).origin;
      // The browser's own user-agent header is not log content.
      const headers = r.headers.replace(/"user-agent":"[^"]*"/, '');
      const haystack = [u.search, u.hash, own ? '' : u.host + u.pathname, headers, r.body].join(
        '\n',
      );
      for (const needle of needles) {
        expect(decodeURIComponent(haystack), `${r.url} must not contain ${needle}`).not.toContain(
          needle,
        );
      }
    }
  });

  test('hostile fields render as text and inject nothing', async ({ page }) => {
    await page.goto('/ai-crawler-cost/');
    const title = await page.title();
    // The fixture's hostile fields sit on parsed lines and are never displayed;
    // the one place log text is echoed is the first unparsed line, so put
    // hostile markup there too.
    const junk = '<img src=x onerror=window.__pwned=1> <script>window.__pwned=1</script>\n';
    await page.setInputFiles('#cc-file', {
      name: 'hostile.log',
      mimeType: 'text/plain',
      buffer: Buffer.concat([Buffer.from(junk), readFileSync(fixture('cc-hostile.log'))]),
    });
    await expect(page.locator('#cc-results')).toBeVisible();
    expect(
      await page.evaluate(() => (window as unknown as { __pwned?: number }).__pwned),
    ).toBeUndefined();
    expect(await page.title()).toBe(title);
    await expect(page.locator('#cc-notes')).toContainText('<img src=x onerror=window.__pwned=1>');
    await expect(
      page.locator(
        '#cc-table img, #cc-table svg, #cc-table script, #cc-notes img, #cc-notes svg, #cc-notes script',
      ),
    ).toHaveCount(0);
  });

  test('an analysis writes nothing to browser storage', async ({ page }) => {
    await page.goto('/ai-crawler-cost/');
    const before = await storageSnapshot(page);
    expect(before.local).toEqual([]);
    expect(before.session).toEqual([]);
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    expect(await storageSnapshot(page)).toEqual(before);
  });

  test('a gzip file parses to the same numbers', async ({ page }) => {
    await page.goto('/ai-crawler-cost/');
    const buffer = gzipSync(readFileSync(fixture('cc-nginx-combined.log')));
    await page.setInputFiles('#cc-file', {
      name: 'access.log.gz',
      mimeType: 'application/gzip',
      buffer,
    });
    await expect(page.locator('#cc-ratio')).toContainText('1.5 crawler page requests');
    await expect(page.locator('#cc-table tbody tr')).toHaveCount(3);
  });

  test('works with the network off once loaded', async ({ page, context }) => {
    await page.goto('/ai-crawler-cost/');
    // The worker posts "ready" once its handler is installed; the page marks
    // the drop zone then. Going offline after that point is a cold start: no
    // analysis has run online first.
    await expect(page.locator('#cc-drop[data-ready="1"]')).toBeAttached();
    await context.setOffline(true);
    await page.setInputFiles('#cc-file', fixture('cc-apache-combined.log'));
    await expect(page.locator('#cc-ratio')).toContainText('1.0 crawler page requests');
    await context.setOffline(false);
  });

  test('fires the crawler-cost event once per analysis, with no properties', async ({ page }) => {
    await stubUmami(page);
    await page.goto('/ai-crawler-cost/');
    // The worker is ready once its handler is installed.
    await expect(page.locator('#cc-drop[data-ready="1"]')).toBeAttached();
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    const events = (await trackedEvents(page)).filter((e) => e.name === 'crawler-cost');
    expect(events).toHaveLength(1);
    expect(events[0].data).toBeUndefined();
  });

  test('rates: a comma decimal works and an empty rate asks for one', async ({ page }) => {
    await page.goto('/ai-crawler-cost/');
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    await page.fill('#cc-rate-gb', '0,09');
    await expect(page.locator('#cc-totals')).toContainText('less than 0.01 USD');
    await page.fill('#cc-rate-gb', '');
    await expect(page.locator('#cc-totals')).toContainText('enter a rate');
  });

  test('the what-to-do links carry fixed event names and the copy button yields a summary', async ({
    page,
  }) => {
    await page.goto('/ai-crawler-cost/');
    for (const t of ['recipes', 'markdown', 'signed-agents', 'pay-per-crawl', 'consulting']) {
      await expect(page.locator(`a[data-umami-event="crawler-cost-next-${t}"]`)).toHaveCount(1);
    }
    await page.setInputFiles('#cc-file', fixture('cc-nginx-combined.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    await page.click('#cc-copy');
    await expect(page.locator('#cc-summary')).toContainText(
      '1.5 crawler page requests per visit an AI assistant sent back (3 crawler page requests, 2 visits)',
    );
    await expect(page.locator('#cc-summary')).not.toContainText('203.0.113');
    await expect(page.locator('#cc-summary')).not.toContainText('/guide');
  });

  test('an IIS log with some lines lacking sc-bytes shows a partial total and the notes', async ({
    page,
  }) => {
    await page.goto('/ai-crawler-cost/');
    await page.setInputFiles('#cc-file', fixture('cc-iis-w3c.log'));
    await expect(page.locator('#cc-results')).toBeVisible();
    await expect(page.locator('#cc-totals')).toContainText('(partial)');
    await expect(page.locator('#cc-notes')).toContainText('2 lines carry no bytes field');
    await expect(page.locator('#cc-table tbody tr')).toHaveCount(2);
    await expect(page.locator('#cc-ratio')).toContainText('1.0 crawler page requests');
  });

  test('a one-line JSON file is reported as not an access log and fires no event', async ({
    page,
  }) => {
    await stubUmami(page);
    await page.goto('/ai-crawler-cost/');
    await expect(page.locator('#cc-drop[data-ready="1"]')).toBeAttached();
    await page.setInputFiles('#cc-file', {
      name: 'data.json',
      mimeType: 'application/json',
      buffer: Buffer.from('{"a":1}'),
    });
    await expect(page.locator('#cc-results')).toBeVisible();
    await expect(page.locator('#cc-notes')).toContainText('not a recognised access log');
    expect((await trackedEvents(page)).filter((e) => e.name === 'crawler-cost')).toHaveLength(0);
  });

  test('without JavaScript the page explains itself and keeps its links', async ({ browser }) => {
    const context = await browser.newContext({ javaScriptEnabled: false });
    const page = await context.newPage();
    await page.goto('/ai-crawler-cost/');
    await expect(
      page.locator('noscript p', {
        hasText: 'This tool runs in your browser and needs JavaScript.',
      }),
    ).toBeVisible();
    await expect(page.locator('a[data-umami-event="crawler-cost-next-recipes"]')).toBeVisible();
    await context.close();
  });

  test('the blog aside and the scanner slot link to the page', async ({ page }) => {
    await page.goto('/blog/agentic-web-at-the-origin/');
    await expect(
      page.locator('a[href="/ai-crawler-cost/"][data-umami-event="blog-tool-crawler-cost"]'),
    ).toBeVisible();
    await page.goto('/blog/pagespeed-installed-base-2026/');
    await expect(page.locator('a[data-umami-event="blog-tool-crawler-cost"]')).toHaveCount(1);
  });
});
