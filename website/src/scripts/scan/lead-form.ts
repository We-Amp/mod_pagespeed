// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The one lead form of the v2 interface, plus the next-steps row and the
// share row under it. The shell renders the static form; this module fills
// the topic chips once the scan result is in, adds the speed-help chip when
// the speed result settles, and sends the whole form as one request (a general
// question when no topic is ticked). Messages go through the shell's single
// live region.

import { resolveApiBase } from '../../lib/scan/api-base';
import { speedStatus, type TileState } from '../../lib/scan/status';
import {
  CONSULTING,
  FOUND_MARKER,
  MAX_TOPICS,
  chipsFor,
  emailOk,
  newSubmissionId,
  payloadForSubmission,
  type Chip,
  type LeadPayload,
} from '../../lib/scan/lead';
import { buildReportMarkdown } from '../../lib/scan/report';
import type { PsiBody, PsiResults, ScanOutcome, ScanReport } from './app';

interface ScanState {
  url: string;
  lastPsi: PsiResults | null;
  lastScan: ScanOutcome | null;
  lastReport: ScanReport | null;
}

function track(name: string, data?: Record<string, unknown>) {
  try {
    (window as unknown as { umami?: { track: (n: string, d?: unknown) => void } }).umami?.track(
      name,
      data,
    );
  } catch {
    /* analytics must never break the form */
  }
}

const perfScore = (r: PsiBody | null): number | null => {
  const s = r?.lighthouseResult?.categories?.performance?.score;
  return typeof s === 'number' ? Math.round(s * 100) : null;
};

const CHIP_CLASS =
  'inline-flex min-h-11 cursor-pointer items-center gap-2 rounded-full border border-border bg-bg-secondary px-4 py-2 text-sm text-text-body has-[:checked]:border-interactive has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-interactive has-[:disabled]:cursor-default has-[:disabled]:opacity-60';

export function initLeadForm(state: ScanState) {
  const root = document.querySelector<HTMLElement>('[data-scan-results]');
  const box = document.querySelector<HTMLFormElement>('form[data-scan-box]');
  if (!root) return;
  const found = root.querySelector<HTMLFormElement>('form[data-scan-lead]');
  if (!found) return;
  const form: HTMLFormElement = found;
  const surface = box?.dataset.scanSurface === 'airead' ? 'airead' : 'analyze';
  const live = root.querySelector<HTMLElement>('[data-scan-live]')!;
  const chipsEl = form.querySelector<HTMLElement>('[data-scan-chips]')!;
  const emailEl = form.querySelector<HTMLInputElement>('input[name="email"]')!;
  const emailErr = form.querySelector<HTMLElement>('[data-scan-lead-email-error]')!;
  const sitesEl = form.querySelector<HTMLInputElement>('[data-scan-sites]')!;
  const noteEl = form.querySelector<HTMLTextAreaElement>('textarea[name="note"]')!;
  const submitBtn = form.querySelector<HTMLButtonElement>('button[type="submit"]')!;
  const msgEl = form.querySelector<HTMLElement>('[data-scan-lead-msg]')!;
  const auditEl = root.querySelector<HTMLElement>('[data-scan-audit]')!;
  const nextRow = root.querySelector<HTMLElement>('[data-scan-next]')!;
  const shareRow = root.querySelector<HTMLElement>('[data-scan-share]')!;
  const linkRow = shareRow.querySelector<HTMLElement>('[data-scan-permalink-row]')!;
  const linkEl = shareRow.querySelector<HTMLAnchorElement>('[data-scan-permalink]')!;
  const copyBtn = shareRow.querySelector<HTMLButtonElement>('[data-scan-copy-link]')!;
  const downloadBtn = shareRow.querySelector<HTMLButtonElement>('[data-scan-download]')!;

  emailEl.required = true;
  const loadedAt = Date.now();

  let rendered: ScanOutcome | null = null;
  let renderedSpeed: TileState | null = null;
  let chips: Chip[] = [];
  let busy = false;
  let pending = false;
  let permalink = '';
  let thanks: HTMLElement | null = null;
  // True once the visitor has changed a topic by hand: their choices are then left alone.
  let touched = false;
  // The id of the submit attempt in progress; a retry after a failure reuses it.
  let submissionId = '';

  function announce(text: string) {
    live.textContent = '';
    live.textContent = text;
  }
  function message(text: string, spoken = true) {
    msgEl.textContent = text;
    if (spoken && text) announce(text);
  }
  // The email error stays under the field until the address is valid.
  function emailError(text: string) {
    emailErr.textContent = text;
    emailErr.hidden = !text;
    emailEl.setAttribute('aria-invalid', text ? 'true' : 'false');
  }
  emailEl.addEventListener('input', () => {
    if (!emailErr.hidden && emailOk(emailEl.value)) emailError('');
  });

  function setPermalink(value: string) {
    let url: URL;
    try {
      url = new URL(value);
    } catch {
      return;
    }
    if (url.protocol !== 'https:' && url.protocol !== 'http:') return;
    permalink = url.toString();
    linkEl.href = permalink;
    linkEl.textContent = permalink;
    linkRow.hidden = false;
    copyBtn.hidden = false;
  }

  function addChip(chip: Chip) {
    const label = document.createElement('label');
    label.className = CHIP_CLASS;
    label.dataset.scanChip = chip.id;
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.name = 'topic';
    input.value = chip.id;
    input.checked = chip.selected;
    input.className = 'h-4 w-4 accent-[var(--color-interactive)]';
    input.addEventListener('change', () => {
      touched = true;
      onTopicsChange();
    });
    const text = document.createElement('span');
    text.textContent = chip.label;
    label.append(input, text);
    // A chip whose check fired says so; the text is inside the label, so it
    // is part of the checkbox's accessible name.
    if (chip.gated) {
      const found = document.createElement('span');
      found.className = 'text-xs text-text-muted';
      found.dataset.scanChipFound = '';
      found.textContent = FOUND_MARKER;
      label.append(' ', found);
    }
    // Keep display order when a chip arrives late.
    const at = chips.findIndex((c) => c.id === chip.id);
    const next = chips
      .slice(at + 1)
      .map((c) => chipsEl.querySelector<HTMLElement>(`[data-scan-chip="${c.id}"]`))
      .find(Boolean);
    chipsEl.insertBefore(label, next ?? null);
  }

  function inputOf(id: string) {
    return chipsEl.querySelector<HTMLInputElement>(`[data-scan-chip="${id}"] input`);
  }

  function selected(): Chip[] {
    return chips.filter((c) => inputOf(c.id)?.checked);
  }

  function onTopicsChange() {
    if (selected().length > MAX_TOPICS) message('Pick up to four topics.');
    else if (msgEl.textContent === 'Pick up to four topics.') message('', false);
  }

  function psiNumbers() {
    return {
      mobile: perfScore(state.lastPsi?.mobile ?? null),
      desktop: perfScore(state.lastPsi?.desktop ?? null),
    };
  }

  function speedState(): TileState | null {
    if (!state.lastPsi) return null;
    const { mobile, desktop } = psiNumbers();
    return speedStatus(mobile, desktop).state;
  }

  // While the scan runs the form is already on the page, with a placeholder
  // where the chips will go, so the content below does not jump when they
  // arrive. The submit button waits for the chips.
  function showPending() {
    rendered = null;
    renderedSpeed = null;
    chips = [];
    permalink = '';
    touched = false;
    submissionId = '';
    pending = true;
    auditEl.classList.add('invisible');
    downloadBtn.hidden = true;
    const bar = document.createElement('span');
    bar.className = 'scan-skeleton';
    bar.setAttribute('aria-hidden', 'true');
    chipsEl.replaceChildren(bar);
    form.hidden = false;
    nextRow.hidden = false;
    shareRow.hidden = false;
    linkRow.hidden = true;
    copyBtn.hidden = true;
    thanks?.remove();
    thanks = null;
    message('', false);
    submitBtn.disabled = true;
    setBusy(false);
  }

  function sync() {
    const scan = state.lastScan;
    if (scan || state.lastPsi) downloadBtn.hidden = false;
    if (!scan && !state.lastPsi) {
      if (!pending && !root!.hidden) showPending();
      return;
    }
    if (!scan) return;
    const speed = speedState();
    if (scan !== rendered) {
      rendered = scan;
      renderedSpeed = speed;
      if (scan.permalink) setPermalink(scan.permalink);
      pending = false;
      thanks?.remove();
      thanks = null;
      touched = false;
      submissionId = '';
      message('', false);
      chipsEl.replaceChildren();
      chips = chipsFor(scan.report, speed);
      chips.forEach(addChip);
      auditEl.classList.toggle('invisible', !scan.report);
      form.hidden = false;
      submitBtn.disabled = false;
      onTopicsChange();
      return;
    }
    if (speed !== renderedSpeed && !thanks) {
      renderedSpeed = speed;
      // Add the speed-help chip without touching the visitor's choices.
      const next = chipsFor(scan.report, speed);
      // Until the visitor touches a topic, the speed-help chip follows the
      // newest speed result. No other chip is ever ticked for the visitor.
      if (!touched) {
        const input = inputOf(CONSULTING.id);
        const chip = next.find((c) => c.id === CONSULTING.id);
        if (input && chip) input.checked = chip.selected;
      }
      for (const chip of next) {
        if (!chips.some((c) => c.id === chip.id)) {
          // A late speed-help chip is pre-selected only while the visitor has
          // not touched the topics (and there is room under the cap).
          if (chip.selected && (touched || selected().length >= MAX_TOPICS)) chip.selected = false;
          chips = next.map((c) => chips.find((o) => o.id === c.id) ?? c);
          addChip(chip);
        }
      }
      onTopicsChange();
    }
  }

  new MutationObserver(sync).observe(root, {
    subtree: true,
    childList: true,
    attributes: true,
    attributeFilter: ['data-state', 'hidden'],
  });

  const apiBase = () => resolveApiBase(location.search, location.hostname);
  // The HTTP status of the answer; 0 when the request did not get one.
  async function post(payload: LeadPayload): Promise<number> {
    try {
      const res = await fetch(`${apiBase()}/contact`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(payload),
      });
      return res.status;
    } catch {
      return 0;
    }
  }

  // While a request is on its way the button says so but stays focusable, so
  // focus is still on it when the answer comes back.
  function setBusy(on: boolean) {
    busy = on;
    if (on) {
      submitBtn.setAttribute('aria-disabled', 'true');
      submitBtn.setAttribute('aria-busy', 'true');
    } else {
      submitBtn.removeAttribute('aria-disabled');
      submitBtn.removeAttribute('aria-busy');
    }
  }

  form.addEventListener('submit', async (ev) => {
    ev.preventDefault();
    // A second click while the request is on its way sends nothing.
    if (busy) return;
    const ticked = selected();
    if (ticked.length > MAX_TOPICS) {
      message('Pick up to four topics.');
      return;
    }
    const email = emailEl.value.trim();
    if (!emailOk(email)) {
      const text = email
        ? 'Enter a valid email address, like name@example.com.'
        : 'Enter your email address.';
      emailError(text);
      // Moving focus to the field reads its error out. When focus is already
      // there (Enter in the field) nothing moves, so the error is announced.
      if (document.activeElement === emailEl) announce(text);
      else emailEl.focus();
      return;
    }
    emailError('');
    message('', false);
    setBusy(true);
    submissionId ||= newSubmissionId();
    const { mobile, desktop } = psiNumbers();
    const payload = payloadForSubmission(
      chips,
      ticked.map((c) => c.id),
      {
        email,
        sites: sitesEl.value.trim(),
        note: noteEl.value,
        url: state.url,
        mobile,
        desktop,
        t: loadedAt,
        submissionId,
      },
      (state.lastScan?.report as ScanReport | null) ?? null,
    );
    const status = await post(payload);
    setBusy(false);
    if (status < 200 || status > 299) {
      // Everything the visitor filled in stays as it is, ready to send again.
      message(
        status === 400
          ? 'We could not send your request. Check the email address and try again.'
          : status === 429
            ? 'Too many requests from this connection. Try again in a minute.'
            : 'We could not send your request. Try again.',
      );
      return;
    }
    submissionId = '';
    const chosen = payload.topics.filter((t) => t.chosen);
    track('lead_submit', {
      channel: 'scan',
      topic: payload.topic,
      wedge: payload.wedge,
      source_path: location.pathname,
      topics_n: chosen.length,
    });
    for (const topic of chosen) {
      if (topic.wedge) track(`airead-next-${topic.wedge}`);
    }
    showThanks(
      email,
      ticked.map((c) => c.label),
    );
  });

  function showThanks(email: string, labels: string[]) {
    form.hidden = true;
    thanks = document.createElement('div');
    thanks.className = 'mt-8 rounded-2xl border border-border bg-bg-elevated p-5 sm:p-6';
    thanks.tabIndex = -1;
    const line = document.createElement('p');
    line.className = 'text-text-body [overflow-wrap:anywhere]';
    line.textContent = `Sent. An engineer at We-Amp replies to ${email} within one business day.`;
    const topics = document.createElement('p');
    topics.className = 'mt-1 text-sm text-text-muted';
    // A general question names no topic: the visitor ticked none.
    topics.textContent =
      labels.length === 0 ? 'Your question was sent.' : `Topics sent: ${labels.join(', ')}.`;
    thanks.append(line, topics);
    form.after(thanks);
    announce(`${line.textContent} ${topics.textContent}`);
    thanks.focus({ preventScroll: true });
  }

  // Next steps: the surface decides which event each link carries.
  const download = nextRow.querySelector<HTMLElement>('[data-scan-cta="download"]');
  const support = nextRow.querySelector<HTMLElement>('[data-scan-cta="support"]');
  root
    .querySelector<HTMLElement>('[data-scan-cta="audit"]')
    ?.setAttribute('data-umami-event-surface', surface);
  if (surface === 'airead') {
    download?.setAttribute('data-umami-event', 'airead-cta-optimize');
    support?.setAttribute('data-umami-event', 'airead-cta-buy');
  } else {
    download?.setAttribute('data-umami-event', 'psi-result-install-cta');
    support?.setAttribute('data-umami-event', 'cta_commercial');
    support?.setAttribute('data-umami-event-offer', 'support');
    support?.setAttribute('data-umami-event-surface', 'analyze');
    support?.setAttribute('data-umami-event-position', 'psi_result');
  }

  copyBtn.addEventListener('click', async () => {
    if (!permalink) return;
    let copied = false;
    try {
      await navigator.clipboard.writeText(permalink);
      copied = true;
    } catch {
      const range = document.createRange();
      range.selectNodeContents(linkEl);
      const sel = window.getSelection();
      sel?.removeAllRanges();
      sel?.addRange(range);
    }
    announce(copied ? 'Link copied' : 'Link selected — press Ctrl/Cmd+C to copy it');
    if (copied) track('airead-copy-link');
  });

  downloadBtn.addEventListener('click', () => {
    track('analyze_report_download');
    const { mobile, desktop } = psiNumbers();
    const body = buildReportMarkdown({
      url: state.url,
      when: new Date().toISOString(),
      mobile,
      desktop,
      report: (state.lastScan?.report as ScanReport | null) ?? null,
    });
    let host = 'site';
    try {
      host = new URL(state.url).hostname.replace(/[^a-z0-9.-]+/gi, '-');
    } catch {
      /* the URL was validated before the run */
    }
    const objectUrl = URL.createObjectURL(new Blob([body], { type: 'text/markdown' }));
    const link = document.createElement('a');
    link.href = objectUrl;
    link.download = `pagespeed-report-${host}.md`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(objectUrl), 1000);
    announce(`Saved as ${link.download}.`);
  });
}
