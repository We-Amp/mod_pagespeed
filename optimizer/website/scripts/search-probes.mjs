// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// search-probes.mjs — where does the page a query names land in site search?
// Opt-in and local: the dev server has no search index, so result order is
// not covered by the test suites. This drives the real search dialog on a
// BUILT site (the same Pagefind bundle, index and options a visitor gets) and
// prints the position of the expected page for each probe query.
//
//   PRICING_ALLOW_STALE=1 npm run build
//   HOST=127.0.0.1 PORT=<free port> node dist/server/entry.mjs
//   node scripts/search-probes.mjs http://127.0.0.1:<port> [more origins...]
//
// With several origins (a base build and a changed build, say) the table has
// one rank column per origin. Exit code 1 when, on the LAST origin, a probe
// does not put its page first or the nonsense query finds something.

/* eslint-disable no-console */

import { chromium } from '@playwright/test';

/** [query, path of the page the query names] */
const PROBES = [
  ['pricing', '/pricing/'],
  ['download', '/download/'],
  ['contact', '/contact/'],
  ['RewriteLevel', '/docs/configuration/'],
  ['upgrade from 1.13', '/docs/upgrade/'],
];
const NONSENSE = 'zzqqxx';

const origins = process.argv.slice(2);
if (origins.length === 0) {
  console.error('usage: node scripts/search-probes.mjs <origin> [origin...]');
  process.exit(2);
}

async function search(page, query) {
  const input = page.locator('#site-search input.pagefind-ui__search-input');
  await input.fill('');
  await input.fill(query);
  // The message line settles on "N results for <query>" or the zero-results
  // text once the search for this exact query has rendered.
  const message = page.locator('#site-search .pagefind-ui__message');
  await message.filter({ hasText: query }).waitFor({ timeout: 15000 });
  await page.waitForTimeout(400);
  const results = await page.locator('#site-search .pagefind-ui__result').evaluateAll((nodes) =>
    nodes.map((n) => {
      const a = n.querySelector('.pagefind-ui__result-title a, a.pagefind-ui__result-link');
      return {
        path: a ? new URL(a.href).pathname : '',
        title: a ? a.textContent.trim() : '',
      };
    }),
  );
  return { message: (await message.textContent()).trim(), results };
}

async function probeOrigin(browser, origin) {
  const context = await browser.newContext();
  // Same-origin only: nothing a probe measures depends on a third party.
  await context.route('**/*', (route) =>
    route.request().url().startsWith(origin) ? route.continue() : route.abort(),
  );
  const page = await context.newPage();
  await page.goto(`${origin}/`);
  await page.locator('header button[data-search-open]').first().click();
  await page.locator('#site-search input.pagefind-ui__search-input').waitFor({ timeout: 15000 });
  const rows = [];
  for (const [query, expected] of PROBES) {
    const { results } = await search(page, query);
    const index = results.findIndex((r) => r.path === expected);
    rows.push({
      query,
      expected,
      rank: index === -1 ? `not in top ${results.length}` : String(index + 1),
      first: results[0] ? `${results[0].title} (${results[0].path})` : '(none)',
      expectedTitle: index === -1 ? '' : results[index].title,
    });
  }
  const nonsense = await search(page, NONSENSE);
  await context.close();
  return { rows, nonsense };
}

const browser = await chromium.launch();
const runs = [];
for (const origin of origins) {
  runs.push({ origin, ...(await probeOrigin(browser, origin.replace(/\/$/, ''))) });
}
await browser.close();

const head = runs.map((_, i) => `rank @${i + 1}`).join(' | ');
console.log(runs.map((r, i) => `@${i + 1} = ${r.origin}`).join('\n'));
console.log('');
console.log(`| query | expected page | ${head} | shown as | first result @${runs.length} |`);
console.log(`|---|---|${runs.map(() => '---|').join('')}---|---|`);
PROBES.forEach(([query, expected], i) => {
  const last = runs[runs.length - 1].rows[i];
  const ranks = runs.map((r) => r.rows[i].rank).join(' | ');
  console.log(
    `| ${query} | ${expected} | ${ranks} | ${last.expectedTitle || '-'} | ${last.first} |`,
  );
});
console.log(
  `| ${NONSENSE} | (no results) | ${runs.map((r) => `${r.nonsense.results.length} results`).join(' | ')} | - | ${runs[runs.length - 1].nonsense.message} |`,
);

const final = runs[runs.length - 1];
const failed = final.rows.some((r) => r.rank !== '1') || final.nonsense.results.length > 0;
process.exit(failed ? 1 : 0);
