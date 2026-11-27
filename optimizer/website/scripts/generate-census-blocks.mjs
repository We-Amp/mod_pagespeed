// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.
//
// Rewrites the generated blocks of the installed-base census post from the census
// dataset, so no number in the post is typed by hand. Blocks are delimited by
//   <!-- census:begin NAME -->  …  <!-- census:end NAME -->
// Usage: node scripts/generate-census-blocks.mjs --date 2026-09-01 [--check]
//
// The rewritten post is passed through Prettier with the site's own config, so the
// output is already what `npm run format` produces and the two never fight.
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import * as prettier from 'prettier';
import { LICENSE_CLAUSE_CAP } from '../src/data/product-facts.mjs';

const args = process.argv.slice(2);
const dateIdx = args.indexOf('--date');
const date = dateIdx >= 0 ? args[dateIdx + 1] : undefined;
const check = args.includes('--check');
if (!date || date.startsWith('--')) throw new Error('--date YYYY-MM-01 is required');

const root = resolve(import.meta.dirname, '..');
const post = resolve(root, 'src/content/blog/pagespeed-installed-base-2026.md');
const census = JSON.parse(readFileSync(resolve(root, `src/data/census/${date}.json`), 'utf8'));
const svg = (name) =>
  readFileSync(resolve(root, `src/data/census/charts/${date}/${name}.svg`), 'utf8').trim();
// A <figure> line opens a CommonMark HTML block that runs to the next blank line, so the
// whole SVG (which has no blank lines) reaches the page as raw HTML, untouched by Markdown.
// At phone width the 720-unit charts would scale their 12px labels down to about 6px, so
// the figure scrolls sideways instead and the chart never renders narrower than 640px.
// The scrolling figure takes keyboard focus, so it carries a role and a name of its own.
const chartLabel = {
  series: 'origins per crawl by build era',
  distribution: 'origins per reported version',
  ranks: 'PageSpeed share per rank band',
  networks: 'ten largest networks by share',
};
const figure = (name) => {
  if (!(name in chartLabel)) throw new Error(`chart "${name}" has no label`);
  const label = `Chart: ${chartLabel[name]}; scrolls horizontally on small screens`;
  return `<figure class="census-chart" role="group" aria-label="${label}" style="overflow-x:auto" tabindex="0">\n<div style="min-width:640px">\n${svg(name)}\n</div>\n</figure>`;
};

const n = (v) => Number(v).toLocaleString('en-US');
const pct = (v, digits = 1) => `${(Number(v) * 100).toFixed(digits)}%`;
const month = (d) =>
  new Date(`${d}T00:00:00Z`).toLocaleString('en-US', {
    month: 'long',
    year: 'numeric',
    timeZone: 'UTC',
  });
const h = census.headline;
const t = census.tables;
const bucketName = {
  'google-era': 'Google-era',
  'incubator-era': 'Incubator-era',
  current: 'Current line',
  hidden: 'Version hidden',
};
const engineName = {
  'apache-module': 'Apache module (`X-Mod-Pagespeed`)',
  'nginx-module': 'nginx module (`X-Page-Speed`)',
};
const familyName = {
  apache: 'Apache',
  nginx: 'nginx',
  cloudflare: 'Cloudflare',
  litespeed: 'LiteSpeed',
  iis: 'IIS',
  other: 'Other',
  hidden: 'Not sent',
};
// The dataset labels rank buckets by their upper bound ("top 5k"), but the buckets are
// exclusive bands: each origin falls in exactly one and the rows sum to the total.
const rankBand = {
  'top 1k': 'top 1k',
  'top 5k': '1k–5k',
  'top 10k': '5k–10k',
  'top 50k': '10k–50k',
  'top 100k': '50k–100k',
  'top 500k': '100k–500k',
  'top 1M': '500k–1M',
  'top 5M': '1M–5M',
  'top 10M': '5M–10M',
  'top 50M': '10M–50M',
};
const band = (label) => {
  if (!(label in rankBand)) throw new Error(`unknown rank bucket "${label}"`);
  return rankBand[label];
};
const signalName = {
  both: 'Header and technology detection',
  header_only: 'Header only',
  wappalyzer_only: 'Technology detection only',
};
const table = (header, rows) =>
  [
    `| ${header.join(' | ')} |`,
    `| ${header.map(() => '---').join(' | ')} |`,
    ...rows.map((r) => `| ${r.join(' | ')} |`),
  ].join('\n');

// Series: one row per crawl that is the first, a January crawl, or the latest.
const seriesByDate = new Map();
for (const r of t.series) {
  const row = seriesByDate.get(r.date) ?? {};
  row[r.bucket] = Number(r.origins);
  seriesByDate.set(r.date, row);
}
const seriesDates = [...seriesByDate.keys()].sort();
const seriesRows = seriesDates
  .filter((d, i) => i === 0 || i === seriesDates.length - 1 || d.slice(5, 7) === '01')
  .map((d) => {
    const r = seriesByDate.get(d);
    const buckets = ['google-era', 'incubator-era', 'current', 'hidden'].map((b) => r[b] ?? 0);
    return [month(d), ...buckets.map(n), n(buckets.reduce((a, b) => a + b, 0))];
  });

// Servers: per-engine totals and the two front-server pairings the prose cites.
const serverOrigins = (engine, family) =>
  t.servers
    .filter((r) => r.engine === engine && (family === undefined || r.server_family === family))
    .reduce((a, r) => a + Number(r.origins), 0);

// Detection: header-bearing pages are "both" + "header_only"; detected are "both" + "wappalyzer_only".
const signal = (s) => Number(t.detection.find((r) => r.signal === s)?.origins ?? 0);
const headerPages = signal('both') + signal('header_only');
const detectedPages = signal('both') + signal('wappalyzer_only');

// Crawl size (root pages, both clients) for the two crawls around HTTP Archive's 2022 growth.
const crawlSize = (d) => {
  const row = t.series.find((r) => r.date === d);
  if (!row) throw new Error(`series has no crawl ${d}`);
  return n(row.all_origins);
};

// The site's canonical license sentence, verbatim from product-facts.mjs.
const licenseLine = LICENSE_CLAUSE_CAP.endsWith('.')
  ? LICENSE_CLAUSE_CAP
  : `${LICENSE_CLAUSE_CAP}.`;

const blocks = {
  headline: `In the ${month(h.crawl_date)} crawl, HTTP Archive saw PageSpeed answering on **${n(h.latest_total)} origins**. ${n(h.versioned_total)} of them report a version; ${n(h.hidden_total)} send the header with the version hidden. PageSpeed-serving origins are ${pct(h.latest_share, 3)} of the root pages in the crawl, ${Number(h.latest_share) < Number(h.first_share) ? 'down' : 'up'} from ${pct(h.first_share, 3)} in ${month(h.first_date)}, the first crawl in this census.`,
  'security-numbers': `${pct(h.share_out_of_fixes)} of the origins that report a version run a build that no longer receives fixes: ${pct(h.share_google_of_versioned)} a Google-era build last updated in 2018, ${pct(h.share_incubator_of_versioned)} an incubator-era build last updated in 2020. ${n(h.current)} origins report the current line. Update recommended.`,
  'series-chart': figure('series'),
  'crawl-size-note': `HTTP Archive's crawl grew from ${crawlSize('2022-06-01')} root pages in ${month('2022-06-01')} to ${crawlSize('2022-08-01')} in ${month('2022-08-01')}; the jump in the chart that year follows it.`,
  'license-line': licenseLine,
  'series-table': table(
    ['Crawl', 'Google-era', 'Incubator-era', 'Current line', 'Version hidden', 'Total'],
    seriesRows,
  ),
  'distribution-chart': figure('distribution'),
  'ranks-chart': figure('ranks'),
  'networks-chart': figure('networks'),
  'distribution-table': table(
    ['Reported version', 'Era', 'Origins'],
    t.distribution.slice(0, 12).map((r) => [r.version, bucketName[r.bucket], n(r.origins)]),
  ),
  // The latest crawl has no unranked origins; an empty row is omitted rather than shown as 0 of 0.
  'ranks-table': table(
    ['Rank band', 'PageSpeed origins', 'All origins', 'Share'],
    t.ranks
      .filter((r) => Number(r.all_origins) > 0)
      .map((r) => [band(r.rank), n(r.pagespeed_origins), n(r.all_origins), pct(r.share, 2)]),
  ),
  'networks-table': table(
    ['Network', 'Share of header-bearing desktop pages'],
    t.networks_top10.map((r) => [r.network, `${r.share_pct}%`]),
  ),
  'servers-numbers': `On the desktop crawl, ${n(serverOrigins('apache-module'))} origins send \`X-Mod-Pagespeed\`, the Apache module's header, and ${n(serverOrigins('nginx-module'))} send \`X-Page-Speed\`, the nginx module's header. The Apache module answers behind an nginx front on ${n(serverOrigins('apache-module', 'nginx'))} origins and behind Cloudflare on ${n(serverOrigins('apache-module', 'cloudflare'))}.`,
  'servers-table': table(
    ['Module header', 'Server header family', 'Origins'],
    t.servers.map((r) => [
      engineName[r.engine] ?? r.engine,
      familyName[r.server_family] ?? r.server_family,
      n(r.origins),
    ]),
  ),
  'detection-numbers': `In the ${month(h.crawl_date)} desktop crawl, ${n(headerPages)} root pages send a PageSpeed header. Technology detection flags ${n(detectedPages)} pages, ${signal('wappalyzer_only') === 1 ? 'one' : n(signal('wappalyzer_only'))} of them without the header.`,
  'detection-table': table(
    ['Signal', 'Root pages'],
    t.detection.map((r) => [signalName[r.signal] ?? r.signal, n(r.origins)]),
  ),
  'cooccurrence-table': table(
    ['Segment', 'WordPress share'],
    t.cooccurrence.map((r) => [
      r.segment === 'pagespeed' ? 'PageSpeed-serving origins' : 'All origins',
      pct(r.wordpress_share),
    ]),
  ),
};

// A missing or renamed field in the dataset renders as NaN or undefined; refuse to write it.
for (const [name, body] of Object.entries(blocks)) {
  if (/\bNaN\b|\bundefined\b/.test(body)) {
    throw new Error(`block "${name}" contains NaN or undefined; check the census data shape`);
  }
}

let text = readFileSync(post, 'utf8');
const original = text;
for (const [name, body] of Object.entries(blocks)) {
  const re = new RegExp(`(<!-- census:begin ${name} -->)[\\s\\S]*?(<!-- census:end ${name} -->)`);
  if (!re.test(text)) throw new Error(`post has no block "${name}"`);
  text = text.replace(re, (_, begin, end) => `${begin}\n${body}\n${end}`);
}
text = await prettier.format(text, {
  ...(await prettier.resolveConfig(post)),
  filepath: post,
});
if (check) {
  if (text !== original) {
    console.error(
      'census blocks are stale; run: node scripts/generate-census-blocks.mjs --date ' + date,
    );
    process.exit(1);
  }
  console.log('census blocks are current');
} else {
  writeFileSync(post, text);
  console.log(`rewrote ${Object.keys(blocks).length} blocks in ${post}`);
}
