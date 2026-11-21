// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The Speed panel of the scan interface: two score plates, the coverage and
// savings sentences, the top fixes, the audits outside an optimizer's reach
// and the configuration. Every result string is written with textContent.

import {
  BUCKET_BADGE_CLASS,
  BUCKET_LABEL,
  CONFIG_DESC,
  FILTER_TO_2_0_ANCHOR,
  aggregate,
  buildConfigSnippet,
  classifyError,
  describePsiError,
  collectFlaggedAuditIds,
  coverageSentence,
  flaggedAudits,
  formatBytes,
  formatMs,
  isMappingFailed,
  loadMapping,
  markMappingFailed,
  reloadError,
  savingsSentence,
  scoreBucket,
  toPercent,
  type AggregatedAudit,
  type AuditMapping,
  type Edition,
  type PsiResult,
} from '../../../lib/scan/psi';
import { NOT_MEASURED } from '../../../lib/scan/status';
import type { PanelContext, RequestError } from '../app';

const TOP_N = 5;
const ALL_CLEAN = 'PSI flagged no failing performance audits on this page.';

// What the visitor has opened. It survives the edition toggle, which redraws
// the panel; a new run starts closed.
interface View {
  edition: Edition;
  showAll: boolean;
  unfixableOpen: boolean;
  configOpen: boolean;
}
const views = new WeakMap<HTMLElement, View>();
const tokens = new WeakMap<HTMLElement, number>();

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function button(className: string, text: string): HTMLButtonElement {
  const b = el('button', className, text);
  b.type = 'button';
  return b;
}

const scoreColor = (n: number) =>
  scoreBucket(n) === 'good'
    ? 'text-success'
    : scoreBucket(n) === 'needs-work'
      ? 'text-text-body'
      : 'text-accent';

function plate(label: string, score: number | null, err: RequestError | null): HTMLElement {
  const box = el('div', 'readout readout-plate p-4');
  box.dataset.speedPlate = label.toLowerCase();
  box.appendChild(el('p', 'readout-label text-sm text-text-muted', label));
  const value = el('p', 'readout-value display-num mt-1');
  const num = el('span', score === null ? 'text-text-body' : scoreColor(score));
  num.textContent = score === null ? '—' : String(score);
  value.appendChild(num);
  box.appendChild(value);
  const gauge = el('div', 'readout-gauge');
  gauge.setAttribute('aria-hidden', 'true');
  const bar = el('i', 'after');
  const bucket = score === null ? null : scoreBucket(score);
  bar.classList.add(
    bucket === 'good' ? 'after-improved' : bucket === 'poor' ? 'after-accent' : 'after-neutral',
  );
  bar.style.inlineSize = `${score ?? 0}%`;
  gauge.append(el('i', 'track'), bar);
  box.appendChild(gauge);
  const chip = el(
    'p',
    `mt-2 ${bucket ? (bucket === 'needs-work' ? 'badge-accent' : BUCKET_BADGE_CLASS[bucket]) : 'badge-neutral'}`,
    bucket ? BUCKET_LABEL[bucket] : 'Not measured',
  );
  chip.dataset.speedBucket = '';
  box.appendChild(chip);
  if (score === null && err) {
    box.appendChild(el('p', 'mt-2 text-xs text-text-muted', describePsiError(err).title));
  }
  return box;
}

function savingsText(
  item: Pick<AggregatedAudit, 'overallSavingsMs' | 'overallSavingsBytes'>,
): string {
  const parts: string[] = [];
  if (item.overallSavingsMs > 0) parts.push(formatMs(item.overallSavingsMs));
  if (item.overallSavingsBytes > 0) parts.push(formatBytes(item.overallSavingsBytes));
  return parts.join(' · ');
}

// A filter name as inline code. It opens the matching documentation page; on
// the worker the page is the transform the filter belongs to.
function filterCode(name: string, edition: Edition): HTMLButtonElement {
  const chip = button(
    'inline-flex items-center rounded px-1 py-1 text-left text-xs font-mono text-interactive underline-offset-2 hover:underline min-h-11 sm:min-h-0 [overflow-wrap:anywhere]',
    name,
  );
  chip.setAttribute('data-umami-event', 'psi-result-filter-click');
  chip.setAttribute('data-umami-event-filter', name);
  const anchor: string | undefined = FILTER_TO_2_0_ANCHOR[name];
  const toWorker = edition === '2.0' && anchor !== undefined;
  chip.setAttribute(
    'aria-label',
    toWorker
      ? `Open ${anchor.replace(/-/g, ' ')} transform documentation in a new tab`
      : `Open ${name} filter documentation in a new tab`,
  );
  chip.addEventListener('click', () => {
    window.open(
      toWorker ? `/docs/worker-configuration/#${anchor}` : `/docs/filters/#${name}`,
      '_blank',
      'noopener',
    );
  });
  return chip;
}

function fixRow(item: AggregatedAudit, edition: Edition): HTMLLIElement {
  const li = el('li', 'flex flex-wrap items-center gap-x-3 gap-y-2 py-3');
  li.dataset.speedFix = item.mapping.auditId;
  li.appendChild(el('span', 'text-sm font-semibold text-text-body', item.mapping.auditTitle));
  const full = item.mapping.coverage === 'full';
  li.appendChild(
    el('span', full ? 'badge-success' : 'badge-accent', full ? 'Fully addressed' : 'Partial fix'),
  );
  if (item.mapping.confidence !== 'high') li.appendChild(el('span', 'badge-neutral', 'Test first'));
  const saved = savingsText(item);
  if (saved) li.appendChild(el('span', 'text-xs text-text-annotation', `est. ${saved}`));
  if (item.mapping.filters.length > 0) {
    const codes = el('span', 'flex flex-wrap gap-1.5');
    for (const f of item.mapping.filters) codes.appendChild(filterCode(f, edition));
    li.appendChild(codes);
  }
  return li;
}

function disclosure(
  label: string,
  open: boolean,
  controls: string,
  onToggle: () => void,
): HTMLButtonElement {
  const b = button('btn-secondary min-h-11 text-sm', label);
  b.setAttribute('aria-expanded', String(open));
  b.setAttribute('aria-controls', controls);
  b.addEventListener('click', onToggle);
  return b;
}

function editionToggle(edition: Edition, set: (e: Edition) => void): HTMLElement {
  const group = el('div', 'inline-flex shrink-0 gap-1 rounded-lg bg-bg-elevated p-1');
  group.setAttribute('role', 'group');
  group.setAttribute('aria-label', 'Which part you configure');
  for (const [value, label] of [
    ['2.0', 'Optimizer worker'],
    ['1.1', 'Module'],
  ] as const) {
    const b = button(
      'product-tab rounded-md px-3 py-1.5 text-xs font-medium transition-colors min-h-11',
      label,
    );
    b.dataset.edition = value;
    b.setAttribute('aria-pressed', String(edition === value));
    b.setAttribute('data-umami-event', 'psi-edition-toggle');
    b.setAttribute('data-umami-event-edition', value);
    b.addEventListener('click', () => set(value));
    group.appendChild(b);
  }
  return group;
}

function configBlock(snippet: string, view: View, redraw: (focus?: string) => void): HTMLElement {
  const wrap = el('div', 'mt-4');
  const id = 'scan-speed-config';
  const toggle = disclosure(
    view.configOpen ? 'Hide the configuration' : 'Show the configuration',
    view.configOpen,
    id,
    () => {
      view.configOpen = !view.configOpen;
      redraw('config');
    },
  );
  toggle.dataset.speedFocus = 'config';
  wrap.appendChild(toggle);
  const body = el('div', 'mt-3');
  body.id = id;
  body.hidden = !view.configOpen;
  if (view.configOpen) {
    body.appendChild(
      editionToggle(view.edition, (e) => {
        view.edition = e;
        redraw(`edition-${e}`);
      }),
    );
    body.appendChild(el('p', 'mt-3 text-xs text-text-muted', CONFIG_DESC[view.edition]));
    const pre = el(
      'pre',
      'mt-3 max-h-80 overflow-auto rounded-lg bg-bg-recessed p-4 text-xs font-mono text-text-body',
    );
    pre.tabIndex = 0;
    pre.setAttribute('aria-label', 'Recommended configuration');
    pre.dataset.speedSnippet = '';
    pre.appendChild(el('code', '', snippet));
    body.appendChild(pre);
    const copy = button('btn-secondary mt-3 min-h-11 text-xs', 'Copy');
    // The accessible name stays "Copy" while the visible text says "Copied".
    copy.setAttribute('aria-label', 'Copy');
    copy.setAttribute('data-umami-event', 'psi-result-config-copy');
    // Spoken through the layout's one status writer (window.siteAnnounce),
    // like the code-block buttons; the button's own name never changes.
    const announceCopy = (ok: boolean) =>
      (window as { siteAnnounce?: (_message: string) => void }).siteAnnounce?.(
        ok ? 'Copied to clipboard' : 'Copy failed, select the text to copy',
      );
    copy.addEventListener('click', () => {
      if (!navigator.clipboard?.writeText) return;
      void navigator.clipboard.writeText(snippet).then(
        () => {
          announceCopy(true);
          copy.textContent = 'Copied';
          setTimeout(() => {
            copy.textContent = 'Copy';
          }, 1500);
        },
        () => announceCopy(false),
      );
    });
    body.appendChild(copy);
  }
  wrap.appendChild(body);
  return wrap;
}

function problem(title: string, body: string, action: HTMLElement): HTMLElement {
  const box = el('div', '');
  box.dataset.speedProblem = '';
  box.append(
    el('p', 'text-sm font-semibold text-text-body', title),
    el('p', 'mt-1 text-sm text-text-muted', body),
    action,
  );
  return box;
}

function retry(ctx: PanelContext): HTMLButtonElement {
  const b = button('btn-secondary mt-3 min-h-11', 'Try again');
  b.addEventListener('click', ctx.rerun);
  return b;
}

function reloadProblem(): HTMLElement {
  const reload = button('btn-secondary mt-3 min-h-11', 'Reload the page');
  reload.addEventListener('click', () => location.reload());
  const { title, body } = classifyError(reloadError());
  return problem(title, body, reload);
}

export function renderSpeedPanel(panel: HTMLElement, ctx: PanelContext) {
  panel.replaceChildren();
  const token = (tokens.get(panel) ?? 0) + 1;
  tokens.set(panel, token);

  // Nothing came back from PSI: the title and body of the first error.
  if (ctx.status.state === 'none') {
    const err = ctx.psi?.errors.mobile ?? ctx.psi?.errors.desktop ?? null;
    if (!err) {
      // No PSI error: the mapping table could not load, so only a reload helps.
      panel.appendChild(reloadProblem());
      return;
    }
    const { title, body } = describePsiError(err);
    panel.appendChild(problem(title, body, retry(ctx)));
    return;
  }

  const psi = ctx.psi;
  if (!psi) return;
  const mobile = psi.mobile as PsiResult | null;
  const desktop = psi.desktop as PsiResult | null;
  const view: View = { edition: '2.0', showAll: false, unfixableOpen: false, configOpen: false };
  views.set(panel, view);

  const plates = el('div', 'grid grid-cols-2 gap-4 max-w-md');
  plates.append(
    plate(
      'Mobile',
      toPercent(mobile?.lighthouseResult?.categories?.performance?.score),
      psi.errors.mobile,
    ),
    plate(
      'Desktop',
      toPercent(desktop?.lighthouseResult?.categories?.performance?.score),
      psi.errors.desktop,
    ),
  );
  panel.appendChild(plates);
  const rest = el('div', 'mt-5');
  panel.appendChild(rest);

  const partial = !mobile || !desktop;

  const table = isMappingFailed() ? Promise.reject(new Error('chunk')) : loadMapping();
  void table.then(
    (loaded) => {
      if (tokens.get(panel) !== token) return;
      const redraw = (focus?: string) => {
        drawResults(rest, loaded, mobile, desktop, view, partial, ctx, redraw);
        if (focus) {
          const target =
            rest.querySelector<HTMLElement>(`[data-speed-focus="${focus}"]`) ??
            rest.querySelector<HTMLElement>(`[data-edition="${focus.replace('edition-', '')}"]`);
          target?.focus();
        }
      };
      redraw();
    },
    () => {
      if (tokens.get(panel) !== token) return;
      markMappingFailed();
      ctx.setStatus(NOT_MEASURED, classifyError(reloadError()).title);
      panel.replaceChildren(reloadProblem());
    },
  );
}

function drawResults(
  rest: HTMLElement,
  table: readonly AuditMapping[],
  mobile: PsiResult | null,
  desktop: PsiResult | null,
  view: View,
  partial: boolean,
  ctx: PanelContext,
  redraw: (focus?: string) => void,
) {
  rest.replaceChildren();
  const m = mobile ?? {};
  const d = desktop ?? {};
  const items = aggregate(m, d, table);
  const fixable = items.filter((i) => i.mapping.coverage !== 'none');
  const unfixable = items.filter((i) => i.mapping.coverage === 'none');
  const totalFlagged = collectFlaggedAuditIds(m, d).size;

  if (partial) {
    const missing = !mobile ? 'Mobile' : 'Desktop';
    const err = (!mobile ? ctx.psi?.errors.mobile : ctx.psi?.errors.desktop) ?? null;
    const note = el(
      'p',
      'mb-3 text-sm text-text-muted',
      `${missing} not measured${err ? `: ${describePsiError(err).title}` : '.'} The results below come from the other profile.`,
    );
    note.dataset.speedPartial = '';
    const again = retry(ctx);
    again.classList.add('mb-4');
    rest.append(note, again);
  }

  if (totalFlagged === 0) {
    rest.appendChild(el('p', 'text-sm text-text-body', ALL_CLEAN));
    return;
  }

  // Two versions of the same result. What mod_pagespeed would do about the
  // audits needs a server the visitor runs, so it is marked data-server-offer;
  // the plain version, marked data-hosted-only, lists what PageSpeed Insights
  // flagged and nothing else. The results section shows one of the two.
  const plain = el('div', '');
  plain.dataset.hostedOnly = '';
  plain.appendChild(
    el(
      'p',
      'text-sm text-text-body',
      `PageSpeed Insights flagged ${totalFlagged} performance ${totalFlagged === 1 ? 'audit' : 'audits'} on this page.`,
    ),
  );
  plain.appendChild(el('h3', 'mt-6 text-base font-semibold text-text-body', 'Flagged audits'));
  const all = el('ul', 'mt-1 divide-y divide-border');
  all.dataset.speedFlagged = '';
  for (const audit of flaggedAudits(m, d)) {
    const li = el('li', 'flex flex-wrap items-center gap-x-3 gap-y-2 py-3');
    li.appendChild(el('span', 'text-sm font-semibold text-text-body', audit.title));
    const saved = savingsText(audit);
    if (saved) li.appendChild(el('span', 'text-xs text-text-annotation', `est. ${saved}`));
    all.appendChild(li);
  }
  plain.appendChild(all);
  const outer = rest;
  outer.appendChild(plain);
  rest = el('div', '');
  rest.dataset.serverOffer = '';
  outer.appendChild(rest);
  rest.appendChild(
    el('p', 'text-sm text-text-body', coverageSentence(totalFlagged, fixable.length, items.length)),
  );
  if (fixable.length > 0) {
    const ms = fixable.reduce((s, i) => s + i.overallSavingsMs, 0);
    const bytes = fixable.reduce((s, i) => s + i.overallSavingsBytes, 0);
    rest.appendChild(el('p', 'mt-2 text-sm text-text-muted', savingsSentence(ms, bytes)));

    rest.appendChild(
      el('h3', 'mt-6 text-base font-semibold text-text-body', 'Fixes on your server'),
    );
    const list = el('ul', 'mt-1 divide-y divide-border');
    list.id = 'scan-speed-fixes';
    const shown = view.showAll ? fixable : fixable.slice(0, TOP_N);
    for (const item of shown) list.appendChild(fixRow(item, view.edition));
    rest.appendChild(list);
    if (fixable.length > TOP_N) {
      const more = disclosure(
        view.showAll ? 'Show fewer' : `Show all ${fixable.length}`,
        view.showAll,
        'scan-speed-fixes',
        () => {
          view.showAll = !view.showAll;
          redraw('all');
        },
      );
      more.classList.add('mt-3');
      more.dataset.speedFocus = 'all';
      rest.appendChild(more);
    }
  }

  if (unfixable.length > 0) {
    const row = el('div', 'mt-6 flex flex-wrap items-center gap-3');
    const count = unfixable.length;
    row.appendChild(
      el(
        'p',
        'text-sm text-text-muted',
        `${count} ${count === 1 ? 'audit' : 'audits'} mod_pagespeed does not address`,
      ),
    );
    const id = 'scan-speed-unfixable';
    const toggle = disclosure(
      view.unfixableOpen ? 'Hide them' : 'Show them',
      view.unfixableOpen,
      id,
      () => {
        view.unfixableOpen = !view.unfixableOpen;
        redraw('unfixable');
      },
    );
    toggle.dataset.speedFocus = 'unfixable';
    row.appendChild(toggle);
    rest.appendChild(row);
    const titles = el('ul', 'mt-2 list-disc pl-5 text-sm text-text-muted');
    titles.id = id;
    titles.hidden = !view.unfixableOpen;
    for (const u of unfixable) titles.appendChild(el('li', '', u.mapping.auditTitle));
    rest.appendChild(titles);
  }

  const snippet = buildConfigSnippet(fixable, view.edition);
  if (snippet) rest.appendChild(configBlock(snippet, view, redraw));
}
