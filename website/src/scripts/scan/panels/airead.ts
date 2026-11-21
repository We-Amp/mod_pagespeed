// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The AI readability drill-down panel. Every string reaches the DOM through
// textContent; the only dynamic style is the bar width, a number.

import { formatAiread, TOKEN_CAPTION } from '../../../lib/scan/airead';
import type { PanelContext, ScanReport } from '../app';

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

export function notMeasured(panel: HTMLElement, ctx: PanelContext) {
  const err = ctx.scan?.error;
  // A scanner answer (an error body or an HTTP error) gets the title; a
  // timeout or an unreachable service has its own sentence.
  const serviceMessage = !err || err.kind === 'http';
  if (serviceMessage)
    panel.append(el('p', 'text-sm font-semibold text-text-body', 'Couldn’t scan that.'));
  panel.append(el('p', 'text-sm text-text-muted', ctx.reason || 'The scanner returned no result.'));
  const retry = el('button', 'btn-secondary mt-3 min-h-11', 'Try again');
  retry.type = 'button';
  retry.addEventListener('click', ctx.rerun);
  panel.append(retry);
}

export function renderAireadPanel(panel: HTMLElement, ctx: PanelContext) {
  panel.replaceChildren();
  const report = ctx.scan?.report as ScanReport | null | undefined;
  if (ctx.status.state === 'none' || !report) {
    if (ctx.status.state === 'none') notMeasured(panel, ctx);
    return;
  }
  const m = formatAiread(report);

  const head = el('div', 'flex items-center gap-4');
  const badge = el(
    'div',
    'flex h-14 w-14 shrink-0 items-center justify-center rounded-xl border border-border text-3xl font-bold text-text-body',
    m.grade,
  );
  badge.setAttribute('role', 'img');
  badge.setAttribute('aria-label', `Grade ${m.grade}, ${m.score} out of 100`);
  const headText = el('div', 'min-w-0');
  headText.append(
    el('div', 'text-xl font-semibold text-text-body', `${m.score}/100`),
    el('div', 'text-sm text-text-muted', `Grade ${m.grade} — ${m.word}`),
  );
  head.append(badge, headText);
  panel.append(head);

  const cats = el('div', 'mt-4 space-y-4');
  for (const c of m.categories) {
    const row = el('div', 'scan-airead-cat');
    const top = el('div', 'flex justify-between gap-3 text-sm font-medium text-text-body');
    const name = el('span', 'min-w-0', c.name);
    if (c.flag) name.append(' ', el('span', 'badge-accent ml-2', c.flag));
    top.append(name, el('span', 'shrink-0', `${c.score}/${c.max}`));
    const bar = el('div', 'mt-1 h-2 overflow-hidden rounded bg-bg-secondary');
    const fill = el('i', 'block h-full bg-interactive');
    fill.style.width = `${c.pct}%`;
    bar.append(fill);
    const verdict = el('div', 'mt-1 text-sm text-text-muted', c.verdict);
    if (c.fromRules) verdict.dataset.scanCrawlerRules = '';
    row.append(top, bar, verdict);
    if (c.note) {
      const note = el('p', 'mt-1 text-sm text-text-muted', c.note);
      note.dataset.scanCrawlerRules = '';
      row.append(note);
    }
    cats.append(row);
  }
  panel.append(cats);

  const tokens = el('div', 'mt-6');
  tokens.append(
    el('p', 'text-sm font-semibold text-text-body', m.tokenLine),
    el('p', 'mt-1 text-sm text-text-muted', TOKEN_CAPTION),
  );
  const details = el('details', 'mt-3');
  const summary = el(
    'summary',
    'flex min-h-11 cursor-pointer items-center text-sm font-medium text-interactive',
    'Show what a crawler reads',
  );
  summary.id = 'scan-airead-crawler-label';
  details.append(summary);
  if (m.rawMarkdown) {
    const pre = el(
      'pre',
      'overflow-auto rounded-md border border-border bg-bg-secondary p-3 text-xs text-text-body',
      m.rawMarkdown,
    );
    pre.style.maxHeight = 'calc(16 * 1.5em)';
    pre.style.lineHeight = '1.5';
    pre.style.whiteSpace = 'pre-wrap';
    pre.tabIndex = 0;
    pre.setAttribute('role', 'region');
    pre.setAttribute('aria-labelledby', summary.id);
    pre.setAttribute('data-scan-crawler-text', '');
    details.append(pre);
  } else {
    details.append(el('p', 'text-sm text-text-muted', '— almost nothing —'));
  }
  tokens.append(details);
  panel.append(tokens);

  if (m.alsoFound.length) {
    const found = el('div', 'mt-6');
    found.setAttribute('data-scan-also-found', '');
    found.append(el('h3', 'text-sm font-semibold text-text-body', 'Also found'));
    const list = el('ul', 'mt-2 list-disc space-y-2 pl-5 text-sm text-text-muted');
    for (const f of m.alsoFound) {
      const li = el('li', '', f.text);
      li.dataset.wedge = f.wedge;
      if (f.link) {
        const a = el('a', 'ml-1 font-medium text-interactive underline', f.link.label);
        a.href = f.link.href;
        li.append(a);
      }
      list.append(li);
    }
    found.append(list);
    panel.append(found);
  }
}
