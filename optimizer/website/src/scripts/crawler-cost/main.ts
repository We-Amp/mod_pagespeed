// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// DOM glue for the AI crawler cost meter. Parsing happens in the Web Worker;
// this file only forwards files, renders results and reads the rate fields.
// Log-derived values are written with textContent, never as markup.
import {
  bytesState,
  estimateCost,
  formatBytes,
  formatMoney,
  formatRatio,
  formatSummary,
  headlineRatio,
  operatorRatios,
  parseRate,
  periodDays,
  projectTo30Days,
} from '../../lib/crawler-cost/summary';
import type { Aggregate, FileReport, Rates } from '../../lib/crawler-cost/types';

type Outbound =
  | { type: 'ready' }
  | { type: 'progress'; bytesRead: number; bytesTotal: number }
  | { type: 'file'; report: FileReport }
  | { type: 'result'; result: Aggregate }
  | { type: 'error'; message: string };

function el<T extends HTMLElement>(id: string): T {
  const node = document.getElementById(id);
  if (!node) throw new Error(`missing #${id}`);
  return node as T;
}

function trackAnalysis(): void {
  const umami = (window as unknown as { umami?: { track?: (name: string) => void } }).umami;
  if (umami && typeof umami.track === 'function') umami.track('crawler-cost');
}

export function initCrawlerCostMeter(): void {
  if (!document.getElementById('cc-file')) return;
  const input = el<HTMLInputElement>('cc-file');
  const drop = el<HTMLElement>('cc-drop');
  const status = el<HTMLElement>('cc-status');
  const progress = el<HTMLProgressElement>('cc-progress');
  const results = el<HTMLElement>('cc-results');
  // Created at page load, not on first use, so the page keeps working with the
  // network off once it has loaded.
  let worker: Worker | null = null;
  try {
    worker = new Worker(new URL('./worker.ts', import.meta.url), { type: 'module' });
  } catch {
    status.textContent =
      'This tool could not start in this browser. Reload the page and try again.';
    return;
  }
  const w: Worker = worker;
  let last: Aggregate | null = null;
  let busy = false;
  let failed = false;
  let lastDecile = -1;

  function rates(): Rates | null {
    const perGb = parseRate(el<HTMLInputElement>('cc-rate-gb').value);
    const perMillionRequests = parseRate(el<HTMLInputElement>('cc-rate-req').value);
    const currency =
      el<HTMLInputElement>('cc-currency').value.trim().toUpperCase().slice(0, 3) || 'USD';
    if (perGb === null || perMillionRequests === null) return null;
    return { perGb, perMillionRequests, currency };
  }

  function text(id: string, value: string): void {
    el(id).textContent = value;
  }

  function cell(row: HTMLTableRowElement, value: string, numeric = false): void {
    const td = document.createElement('td');
    td.textContent = value;
    if (numeric) td.className = 'text-right tabular-nums';
    row.appendChild(td);
  }

  function render(a: Aggregate): void {
    const r = rates();
    const state = bytesState(a);
    const missing = state === 'missing';
    const NOT_IN_LOG = 'not in this log';
    const money = (bytes: number, requests: number) => {
      if (missing) return NOT_IN_LOG;
      if (!r) return 'enter a rate';
      const c = estimateCost(bytes, requests, r);
      return c === null ? 'enter a rate' : formatMoney(c, r.currency);
    };
    const days = periodDays(a);
    text(
      'cc-period',
      a.firstTime !== null && a.lastTime !== null
        ? `${a.requests.toLocaleString('en-US')} requests from ${new Date(a.firstTime).toISOString().slice(0, 10)} to ${new Date(a.lastTime).toISOString().slice(0, 10)}`
        : `${a.requests.toLocaleString('en-US')} requests (no timestamps found)`,
    );
    const ratio = headlineRatio(a);
    text(
      'cc-ratio',
      ratio === null
        ? `No visits from AI assistants in this log (${a.crawlerPageRequests.toLocaleString('en-US')} crawler page requests).`
        : `${formatRatio(ratio)} crawler page requests per visit an AI assistant sent back`,
    );
    text(
      'cc-ratio-note',
      ratio === null
        ? ''
        : `${a.crawlerPageRequests.toLocaleString('en-US')} crawler page requests, ${a.referredPageRequests.toLocaleString('en-US')} visits sent back (landing requests). Fetches made on a person's behalf are listed apart below.`,
    );

    let totalCost = money(a.crawlerBytes, a.crawlerRequests);
    if (missing) {
      const reqCost = r && r.perMillionRequests > 0 ? estimateCost(0, a.crawlerRequests, r) : null;
      totalCost =
        r && reqCost !== null
          ? `${formatMoney(reqCost, r.currency)} (from requests only)`
          : NOT_IN_LOG;
    }
    const totals = el<HTMLElement>('cc-totals');
    totals.replaceChildren();
    const share = (part: number, whole: number) =>
      whole === 0 ? '0%' : `${((part / whole) * 100).toFixed(1)}%`;
    for (const [label, value] of [
      [
        'AI crawler requests',
        `${a.crawlerRequests.toLocaleString('en-US')} (${share(a.crawlerRequests, a.requests)} of all requests)`,
      ],
      [
        'AI crawler bytes',
        missing
          ? NOT_IN_LOG
          : `${formatBytes(a.crawlerBytes)} (${share(a.crawlerBytes, a.bytes)} of all bytes)${state === 'partial' ? ' (partial)' : ''}`,
      ],
      ['Estimated cost', totalCost],
      [
        'Estimated at',
        r
          ? `${r.perGb} ${r.currency} per GB${r.perMillionRequests > 0 ? `, ${r.perMillionRequests} ${r.currency} per million requests` : ''}`
          : 'enter a rate',
      ],
    ]) {
      const dt = document.createElement('dt');
      dt.textContent = label;
      dt.className = 'text-xs uppercase tracking-wider text-text-annotation';
      const dd = document.createElement('dd');
      dd.textContent = value;
      dd.className = 'mt-1 text-base font-semibold text-text-body';
      const wrap = document.createElement('div');
      wrap.append(dt, dd);
      totals.appendChild(wrap);
    }
    const cost = r && !missing ? estimateCost(a.crawlerBytes, a.crawlerRequests, r) : null;
    const projected = cost === null ? null : projectTo30Days(cost, days);
    text(
      'cc-projection',
      projected === null || !r
        ? ''
        : `Projected to 30 days at this rate: ${formatMoney(projected, r.currency)} (a projection from ${days?.toFixed(1)} days of log, not a bill).`,
    );

    const tbody = el<HTMLTableSectionElement>('cc-table').querySelector(
      'tbody',
    ) as HTMLTableSectionElement;
    tbody.replaceChildren();
    for (const row of a.crawlers) {
      const tr = document.createElement('tr');
      cell(tr, row.token);
      cell(tr, row.operator);
      cell(tr, row.kind);
      cell(tr, row.requests.toLocaleString('en-US'), true);
      cell(tr, share(row.requests, a.requests), true);
      cell(tr, row.pageRequests.toLocaleString('en-US'), true);
      cell(tr, missing ? NOT_IN_LOG : formatBytes(row.bytes), true);
      cell(tr, missing ? NOT_IN_LOG : share(row.bytes, a.bytes), true);
      cell(tr, money(row.bytes, row.requests), true);
      tbody.appendChild(tr);
    }
    text(
      'cc-fetchers',
      a.fetcherPageRequests > 0
        ? `Fetches made on a person's behalf (kind "fetcher"): ${a.fetcherPageRequests.toLocaleString('en-US')} page ${a.fetcherPageRequests === 1 ? 'request' : 'requests'}, listed above and left out of the headline ratio.`
        : '',
    );

    const ops = el<HTMLTableSectionElement>('cc-ops').querySelector(
      'tbody',
    ) as HTMLTableSectionElement;
    ops.replaceChildren();
    for (const o of operatorRatios(a)) {
      const tr = document.createElement('tr');
      cell(tr, o.operator);
      cell(tr, o.requests.toLocaleString('en-US'), true);
      cell(tr, missing ? NOT_IN_LOG : formatBytes(o.bytes), true);
      cell(tr, o.crawlerPageRequests.toLocaleString('en-US'), true);
      cell(tr, o.referred.toLocaleString('en-US'), true);
      cell(tr, o.ratio === null ? 'no visits' : formatRatio(o.ratio), true);
      ops.appendChild(tr);
    }

    const notes: string[] = [];
    if (a.unparsed > 0)
      notes.push(
        `${a.unparsed.toLocaleString('en-US')} unparsed ${a.unparsed === 1 ? 'line' : 'lines'} skipped. First: ${a.unparsedSamples.join(' | ')}`,
      );
    if (a.bytesMissing > 0)
      notes.push(
        `${a.bytesMissing.toLocaleString('en-US')} ${a.bytesMissing === 1 ? 'line carries' : 'lines carry'} no bytes field. In IIS Manager, open Logging, Select Fields, and tick Bytes Sent (sc-bytes) to log them.`,
      );
    if (a.agentlessLines === a.requests && a.requests > 0)
      notes.push('This log carries no user agents, so no crawler can be recognised in it.');
    for (const f of a.files) {
      notes.push(
        `${f.name}: ${f.format === 'unknown' ? 'format not recognised' : f.format}${f.gzip ? ', gzip' : ''}, ${f.lines.toLocaleString('en-US')} ${f.lines === 1 ? 'line' : 'lines'}.`,
      );
      if (f.lines === 0 && f.sample === null && !f.error)
        notes.push(`No log lines found in ${f.name}.`);
      if (f.format === 'common')
        notes.push(
          `${f.name}: no user agent field in this log, so nothing in it can be classified.`,
        );
      if (f.givenUp)
        notes.push(
          `${f.name}: not a recognised access log (nginx or Apache combined, Apache common, IIS W3C extended). First line: ${f.sample ?? ''}`,
        );
      if (f.error) notes.push(`${f.name}: ${f.error}`);
    }
    el<HTMLElement>('cc-notes').replaceChildren(
      ...notes.map((n) => {
        const p = document.createElement('p');
        p.textContent = n;
        return p;
      }),
    );
    results.hidden = false;
  }

  w.onmessage = (event: MessageEvent<Outbound>) => {
    const msg = event.data;
    if (msg.type === 'ready') {
      drop.dataset.ready = '1';
    } else if (msg.type === 'progress') {
      progress.hidden = false;
      const pct = msg.bytesTotal === 0 ? 0 : Math.round((msg.bytesRead / msg.bytesTotal) * 100);
      progress.value = pct;
      const decile = Math.floor(pct / 10);
      if (decile !== lastDecile) {
        lastDecile = decile;
        status.textContent = `Reading: ${formatBytes(msg.bytesRead)} of ${formatBytes(msg.bytesTotal)}`;
      }
    } else if (msg.type === 'file') {
      status.textContent = `${msg.report.name}: ${msg.report.format === 'unknown' ? 'format not recognised' : msg.report.format}${msg.report.gzip ? ', gzip' : ''}`;
    } else if (msg.type === 'result') {
      last = msg.result;
      busy = false;
      progress.hidden = true;
      status.textContent = 'Done. Nothing from the log left your browser.';
      render(last);
      if (msg.result.requests > 0) trackAnalysis();
    } else if (msg.type === 'error') {
      busy = false;
      progress.hidden = true;
      status.textContent = `Could not analyse these files: ${msg.message}`;
    }
  };
  w.onerror = () => {
    failed = true;
    busy = false;
    progress.hidden = true;
    status.textContent = 'The analysis stopped unexpectedly. Reload the page and try again.';
  };

  function start(files: File[]): void {
    if (files.length === 0) return;
    if (failed) {
      status.textContent =
        'This tool could not start in this browser. Reload the page and try again.';
      return;
    }
    if (busy) {
      status.textContent = 'Still reading the previous files. Drop again when it is done.';
      return;
    }
    busy = true;
    lastDecile = -1;
    results.hidden = true;
    el<HTMLElement>('cc-summary').hidden = true;
    text('cc-copied', '');
    progress.hidden = false;
    progress.value = 0;
    status.textContent = `Reading ${files.length} ${files.length === 1 ? 'file' : 'files'} in your browser`;
    w.postMessage({ type: 'analyze', files });
  }

  input.addEventListener('change', () => start(Array.from(input.files ?? [])));
  for (const type of ['dragenter', 'dragover'])
    drop.addEventListener(type, (e) => {
      e.preventDefault();
      drop.classList.add('border-interactive');
    });
  drop.addEventListener('dragleave', (e) => {
    if (drop.contains((e as DragEvent).relatedTarget as Node | null)) return;
    drop.classList.remove('border-interactive');
  });
  // A file dropped outside the zone must not navigate the tab away.
  document.addEventListener('dragover', (e) => e.preventDefault());
  document.addEventListener('drop', (e) => e.preventDefault());
  drop.addEventListener('drop', (e) => {
    e.preventDefault();
    drop.classList.remove('border-interactive');
    start(Array.from((e as DragEvent).dataTransfer?.files ?? []));
  });
  for (const id of ['cc-rate-gb', 'cc-currency', 'cc-rate-req'])
    el(id).addEventListener('input', () => {
      el<HTMLElement>('cc-summary').hidden = true;
      text('cc-copied', '');
      if (last) render(last);
    });

  el('cc-copy').addEventListener('click', async () => {
    if (!last) return;
    const summary = formatSummary(
      last,
      rates(),
      `${window.location.origin}${window.location.pathname}`,
    );
    const pre = el<HTMLElement>('cc-summary');
    pre.textContent = summary;
    pre.hidden = false;
    try {
      await navigator.clipboard.writeText(summary);
      text('cc-copied', 'Copied to your clipboard.');
    } catch {
      text('cc-copied', 'Copy the text below.');
    }
  });
}
