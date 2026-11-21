// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Risk & SEO drill-down panel: four rows (pre-consent leak, script inventory,
// SEO defects, response exposure), each with a state chip and one verdict
// sentence, plus the "what is planned" sentence and link only when the
// lens's gate fires. All text is written with textContent.

import type { PanelContext } from '../app';
import { riskRows, type RiskRow } from '../../../lib/scan/risk-rows';

const CHIP_TONE: Record<RiskRow['state'], string> = {
  attention: 'badge-accent',
  clean: 'badge-neutral',
  none: 'badge-neutral',
};

const DEFAULT_REASON = 'Not measured: the site blocked the scanner.';

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

function rowNode(row: RiskRow): HTMLElement {
  const muted = row.state === 'none';
  const li = el('li', 'scan-risk-row border-t border-border py-4 first:border-t-0 first:pt-0');
  li.dataset.riskRow = row.key;
  li.dataset.state = row.state;

  const head = el('div', 'flex flex-wrap items-center gap-x-3 gap-y-1');
  head.append(
    el('h3', 'text-base font-semibold text-text-body', row.name),
    el('span', `${CHIP_TONE[row.state]} scan-risk-chip`, row.chip),
  );
  li.append(head);

  const sentence = el(
    'p',
    `mt-2 text-sm [overflow-wrap:anywhere] ${muted ? 'text-text-muted' : 'text-text-body'}`,
    row.sentence,
  );
  sentence.dataset.riskSentence = '';
  li.append(sentence);

  if (row.detail) {
    const d = el('p', 'mt-2 text-sm text-text-muted [overflow-wrap:anywhere]', row.detail);
    d.dataset.riskDetail = '';
    li.append(d);
  }
  // On a phone the secondary text sits behind one disclosure per row, closed by
  // default; the panel is hidden when this runs, so it cannot shift the page.
  // The SEO add-on is available today, so its sentence is never under "What is planned".
  const narrow =
    row.building.length > 0 && row.key !== 'seoDefects' && matchMedia('(max-width: 639px)').matches;
  const holder = narrow ? el('details', 'scan-risk-more') : li;
  if (narrow) {
    const summary = el(
      'summary',
      'scan-risk-more-summary flex min-h-11 cursor-pointer items-center justify-between gap-3 text-sm font-medium text-interactive',
    );
    summary.append(el('span', '', 'What is planned'));
    const mark = el('span', 'scan-risk-more-mark', '▾');
    mark.setAttribute('aria-hidden', 'true');
    summary.append(mark);
    holder.addEventListener('toggle', () => {
      mark.textContent = (holder as HTMLDetailsElement).open ? '▴' : '▾';
    });
    holder.append(summary);
    li.append(holder);
  }
  for (const text of row.building) {
    const p = el('p', 'mt-2 text-sm text-text-muted', text);
    p.dataset.riskBuilding = '';
    holder.append(p);
  }
  if (row.link) {
    const a = el(
      'a',
      'mt-2 inline-block text-sm font-medium text-interactive underline',
      row.link.label,
    );
    a.href = row.link.href;
    a.dataset.umamiEvent = row.link.event;
    li.append(a);
  }
  return li;
}

export function renderRisk(panel: HTMLElement, ctx: PanelContext): void {
  panel.replaceChildren();
  const rows = riskRows(ctx.scan?.report ?? null);
  if (rows.length) {
    const list = el('ul', 'scan-risk-rows');
    list.append(...rows.map(rowNode));
    panel.append(list);
  }
  if (ctx.status.state === 'none') {
    // Nothing measured: the scan failed, or every lens was blocked or off.
    if (!rows.length) {
      panel.append(el('p', 'text-sm text-text-muted', ctx.reason || DEFAULT_REASON));
    }
    const retry = el('button', 'btn-secondary mt-3 min-h-11', 'Try again');
    retry.type = 'button';
    retry.addEventListener('click', ctx.rerun);
    panel.append(retry);
  }
}
