// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Scan controller for the scan pages. Loaded lazily by the results view. It
// starts the three requests at once (scanner, PageSpeed Insights mobile,
// PageSpeed Insights desktop), sets each tile's state as its source settles
// and hands the results to the drill-down panels and the lead form.

import {
  CHECKING,
  NOT_MEASURED,
  aireadStatus,
  healthLine,
  riskStatus,
  speedStatus,
  type TileStatus,
} from '../../lib/scan/status';
import { renderAireadPanel } from './panels/airead';
import { initLeadForm } from './lead-form';
import { renderRisk } from './panels/risk';
import {
  classifyError,
  describePsiError,
  isMappingFailed,
  loadMapping,
  markMappingFailed,
  reloadError,
} from '../../lib/scan/psi';
import { resolveApiBase } from '../../lib/scan/api-base';
import { fitFor, type Fit } from '../../lib/scan/fit';
import { proxyErrorText, scanErrorCopy } from '../../lib/scan/scan-errors';
import { renderSpeedPanel } from './panels/speed';

type Pillar = 'speed' | 'airead' | 'risk';
type Strategy = 'mobile' | 'desktop';

const PSI_ENDPOINT = '/psi/v5/runPagespeed';
const PSI_TIMEOUT_MS = 60_000;
const SCAN_TIMEOUT_MS = 45_000;
const PILLARS: Pillar[] = ['speed', 'airead', 'risk'];
const PILLAR_NAME: Record<Pillar, string> = {
  speed: 'Speed',
  airead: 'AI readability',
  risk: 'Risk & SEO',
};
const TONE: Record<TileStatus['state'], string> = {
  checking: 'badge-neutral',
  good: 'badge-success',
  'needs-work': 'badge-accent',
  poor: 'badge-accent',
  clean: 'badge-neutral',
  flagged: 'badge-accent',
  none: 'badge-neutral',
};

const SCAN_TIMEOUT_REASON = 'The scanner did not answer in time.';
const PSI_TIMEOUT_REASON = 'PageSpeed Insights did not answer within a minute.';
// The one wait-time figure; the scan box states the same duration.
const WAIT_HINT = 'Usually takes 20 to 40 seconds.';
// How long after the results section is shown its live region is first filled.
const ANNOUNCE_DELAY_MS = 100;
// The origin-control readings a scan can report.
const ORIGIN_CONTROL = ['none', 'limited', 'full', 'unknown'];

export interface PsiBody {
  lighthouseResult?: { categories?: { performance?: { score?: number | null } } };
}
export interface ScanReport extends Record<string, unknown> {
  grade?: string;
  score?: number;
}
interface ScanBody {
  error?: string;
  report?: ScanReport;
  permalink?: string;
}

export class RequestError extends Error {
  constructor(
    message: string,
    readonly kind: 'timeout' | 'network' | 'http',
    /** The HTTP status for kind 'http'; the panel renderers classify on it. */
    readonly status?: number,
  ) {
    super(message);
  }
}

/** What the two PageSpeed Insights requests returned, kept after the tile is set. */
export interface PsiResults {
  mobile: PsiBody | null;
  desktop: PsiBody | null;
  errors: { mobile: RequestError | null; desktop: RequestError | null };
}
/** What the scanner returned, kept after the tiles are set. */
export interface ScanOutcome {
  report: ScanReport | null;
  error: RequestError | null;
  /** The reason shown when the scan did not produce a report. */
  reason: string;
  /** The scanner's shareable link for this result, when it returned one. */
  permalink?: string;
}

/** Everything a panel renderer needs; a renderer fills only its own panel. */
export interface PanelContext {
  pillar: Pillar;
  url: string;
  status: TileStatus;
  /** Why the pillar was not measured; empty otherwise. */
  reason: string;
  psi: PsiResults | null;
  scan: ScanOutcome | null;
  /** Re-run only this pillar's source (the per-tile "Try again"). */
  rerun: () => void;
  /** Change the tile after the panel was handed over (a late failure). */
  setStatus: (status: TileStatus, reason?: string) => void;
}
export type PanelRenderer = (panel: HTMLElement, ctx: PanelContext) => void;

// The shell renders only the not-measured reason and its retry. Each pillar's
// task replaces its own entry here; nothing else needs to change.
function renderNotMeasured(panel: HTMLElement, ctx: PanelContext) {
  panel.replaceChildren();
  if (ctx.status.state !== 'none') return;
  const reason = document.createElement('p');
  reason.className = 'text-sm text-text-muted';
  reason.textContent = ctx.reason;
  const retry = document.createElement('button');
  retry.type = 'button';
  retry.className = 'btn-secondary mt-3 min-h-11';
  retry.textContent = 'Try again';
  retry.addEventListener('click', ctx.rerun);
  panel.append(reason, retry);
}
export const renderPanel: Record<Pillar, PanelRenderer> = {
  speed: renderSpeedPanel,
  airead: renderAireadPanel,
  risk: renderNotMeasured,
};
renderPanel.risk = renderRisk;

/** The latest run's raw results, for the renderers and for tests. */
export const state: {
  url: string;
  lastPsi: PsiResults | null;
  lastScan: ScanOutcome | null;
  lastReport: ScanReport | null;
} = { url: '', lastPsi: null, lastScan: null, lastReport: null };

let controller: { rerun: (pillar: Pillar) => void } | null = null;
/** Re-run one pillar's source: speed runs PageSpeed Insights, the others the scanner. */
export function rerun(pillar: Pillar) {
  controller?.rerun(pillar);
}

// A host the backends can resolve: dotted DNS labels (letters, digits, hyphens;
// punycode is already applied by URL), and a final label that contains a letter,
// so IP literals and "1.2"-style numeric hosts are out. A trailing dot is allowed.
export function isPublicDnsName(hostname: string): boolean {
  const host = hostname.replace(/\.$/, '');
  if (host.length > 253) return false;
  const labels = host.split('.');
  if (labels.length < 2) return false;
  if (!labels.every((l) => /^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$/i.test(l))) return false;
  return /[a-z]/i.test(labels[labels.length - 1]);
}

// Normalise what the visitor typed: a bare host gets https://, then it must
// parse as an http(s) URL with a public-looking hostname.
export function normalizeUrl(
  raw: string,
): { ok: true; url: string } | { ok: false; reason: string } {
  let text = (raw || '').trim();
  if (!text) return { ok: false, reason: 'Enter a URL to check.' };
  if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(text)) text = 'https://' + text;
  let parsed: URL;
  try {
    parsed = new URL(text);
  } catch {
    return { ok: false, reason: 'That does not look like a valid URL (try https://example.com).' };
  }
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
    return { ok: false, reason: 'Only http:// and https:// URLs are supported.' };
  }
  if (!parsed.hostname || parsed.hostname === 'localhost') {
    return { ok: false, reason: 'We need a publicly reachable hostname. Localhost won’t work.' };
  }
  if (parsed.username || parsed.password) {
    return { ok: false, reason: 'Leave out the username and password; enter just the page URL.' };
  }
  if (/^\[|^[0-9.]+$/.test(parsed.hostname)) {
    return { ok: false, reason: 'Enter the site’s domain name, not an IP address.' };
  }
  if (!isPublicDnsName(parsed.hostname)) {
    return {
      ok: false,
      reason: 'That does not look like a real website address. Check for typos (try example.com).',
    };
  }
  return { ok: true, url: parsed.toString() };
}

async function getJson<T>(url: string, timeoutMs: number, timeoutReason: string): Promise<T> {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(url, { method: 'GET', cache: 'no-store', signal: ctl.signal });
    let body: unknown = null;
    try {
      body = await res.json();
    } catch {
      /* non-JSON error body */
    }
    if (!res.ok) {
      const detail = (body as { error?: unknown } | null)?.error;
      const message =
        typeof detail === 'string'
          ? detail
          : ((detail as { message?: string } | undefined)?.message ?? `HTTP ${res.status}`);
      throw new RequestError(message, 'http', res.status);
    }
    return body as T;
  } catch (err) {
    if (err instanceof RequestError) throw err;
    if (ctl.signal.aborted) throw new RequestError(timeoutReason, 'timeout');
    throw new RequestError('The request failed.', 'network');
  } finally {
    clearTimeout(timer);
  }
}

const perfScore = (r: PsiBody): number | null => {
  const s = r.lighthouseResult?.categories?.performance?.score;
  return typeof s === 'number' ? Math.round(s * 100) : null;
};

function track(name: string, data?: Record<string, unknown>) {
  try {
    (window as unknown as { umami?: { track: (n: string, d?: unknown) => void } }).umami?.track(
      name,
      data,
    );
  } catch {
    /* analytics must never break a run */
  }
}

export function init() {
  const root = document.querySelector<HTMLElement>('[data-scan-results]');
  const form = document.querySelector<HTMLFormElement>('form[data-scan-box]');
  if (!root || !form) return;
  const surface = form.dataset.scanSurface === 'airead' ? 'airead' : 'analyze';
  const input = form.querySelector<HTMLInputElement>('input[name="url"]')!;
  const submit = form.querySelector<HTMLButtonElement>('button[type="submit"]')!;
  const submitLabel = form.querySelector<HTMLElement>('[data-scan-submit-label]')!;
  const errorEl = form.querySelector<HTMLElement>('[role="alert"]')!;
  const live = root.querySelector<HTMLElement>('[data-scan-live]')!;
  const heading = root.querySelector<HTMLElement>('[data-scan-heading]')!;
  const hostEl = root.querySelector<HTMLElement>('[data-scan-host]')!;
  const healthEl = root.querySelector<HTMLElement>('[data-scan-health]')!;
  const hintEl = root.querySelector<HTMLElement>('[data-scan-hint]');
  const fitLine = root.querySelector<HTMLElement>('[data-scan-fit-line]');
  const fitText = root.querySelector<HTMLElement>('[data-scan-fit-text]');
  const fitActions = root.querySelector<HTMLElement>('[data-scan-fit-actions]');
  const tiles = new Map<Pillar, HTMLButtonElement>();
  const panels = new Map<Pillar, HTMLElement>();
  for (const p of PILLARS) {
    tiles.set(p, root.querySelector<HTMLButtonElement>(`[data-scan-tile="${p}"]`)!);
    panels.set(p, root.querySelector<HTMLElement>(`[data-scan-panel="${p}"]`)!);
  }

  const statuses: Record<Pillar, TileStatus> = {
    speed: CHECKING,
    airead: CHECKING,
    risk: CHECKING,
  };
  let runId = 0;
  let running = false;
  let announcedFirst = false;

  function announce(text: string) {
    live.textContent = text;
  }

  // The line under the heading: for a hosted platform a notice with what the
  // owner can do there, otherwise one muted line. `data-server-offers` on the
  // section decides what is shown of everything that needs the visitor's own
  // server: "on" shows it, "off" (a hosted platform) hides it, and "pending"
  // (null: a scan is on its way) keeps it out of sight until the result says
  // which of the two it is.
  function paintFit(fit: Fit | null) {
    root!.dataset.serverOffers = !fit ? 'pending' : fit.offers ? 'on' : 'off';
    if (fitLine) fitLine.textContent = fit && fit.offers ? fit.text : '';
    if (fitText) fitText.textContent = fit && !fit.offers ? fit.text : '';
    if (fitActions) {
      fitActions.replaceChildren();
      for (const action of fit && !fit.offers ? fit.actions : []) {
        const li = document.createElement('li');
        li.textContent = action.text;
        if (action.source && action.sourceLabel) {
          const a = document.createElement('a');
          a.className = 'font-medium text-interactive underline';
          a.href = action.source;
          a.target = '_blank';
          a.rel = 'noopener noreferrer';
          a.textContent = action.sourceLabel;
          a.setAttribute('aria-label', `${action.sourceLabel} (opens in new tab)`);
          a.title = `${action.sourceLabel} (opens in new tab)`;
          li.append(' ', a);
        }
        fitActions.append(li);
      }
      fitActions.hidden = fitActions.childElementCount === 0;
    }
  }

  // The hosted-platform sentence is read out once, with the last of the two
  // results to land, so a later result never replaces it unread.
  let heldNote = '';
  function withNote(text: string): string {
    const note = heldNote;
    heldNote = '';
    return note ? `${text} ${note}` : text;
  }

  // The tile value stays one line in every state, so a failing source never
  // changes the tile's height; the reason goes in the panel.
  function paintTile(pillar: Pillar, status: TileStatus, reason = '') {
    statuses[pillar] = status;
    const tile = tiles.get(pillar)!;
    tile.dataset.state = status.state;
    const word = tile.querySelector<HTMLElement>('[data-scan-status]')!;
    word.textContent = status.word;
    word.className = `scan-tile-status ${TONE[status.state]}`;
    const value = tile.querySelector<HTMLElement>('[data-scan-value]')!;
    if (status.state === 'checking') {
      const bar = document.createElement('span');
      bar.className = 'scan-skeleton';
      bar.setAttribute('aria-hidden', 'true');
      value.replaceChildren(bar);
      value.removeAttribute('title');
    } else if (status.state === 'none' && reason) {
      // One line, truncated: the tile keeps its height; the panel has the full text.
      const line = document.createElement('span');
      line.className = 'scan-tile-reason';
      line.textContent = reason;
      value.replaceChildren(line);
      value.title = reason;
    } else {
      value.textContent = status.value || '—';
      value.removeAttribute('title');
    }
    healthEl.textContent = healthLine(PILLARS.map((p) => statuses[p]));
  }

  // Paint the tile, then hand the panel to the pillar's renderer.
  function settle(pillar: Pillar, status: TileStatus, reason = '') {
    const settledRun = runId;
    paintTile(pillar, status, reason);
    renderPanel[pillar](panels.get(pillar)!, {
      pillar,
      url: state.url,
      status,
      reason,
      psi: state.lastPsi,
      scan: state.lastScan,
      rerun: () => rerun(pillar),
      setStatus: (s, r = '') => {
        if (settledRun !== runId) return;
        paintTile(pillar, s, r);
        if (s.state === 'none') announce(`${PILLAR_NAME[pillar]} not measured. ${r}`);
      },
    });
  }

  function setOpen(open: Pillar | null) {
    for (const p of PILLARS) {
      const isOpen = p === open;
      const tile = tiles.get(p)!;
      tile.setAttribute('aria-expanded', String(isOpen));
      tile.setAttribute('aria-label', `${isOpen ? 'Hide' : 'Show'} ${PILLAR_NAME[p]} details`);
      panels.get(p)!.hidden = !isOpen;
    }
    if (open) track('scan-tile-open', { pillar: open });
  }

  for (const p of PILLARS) {
    tiles.get(p)!.addEventListener('click', () => {
      const wasOpen = tiles.get(p)!.getAttribute('aria-expanded') === 'true';
      setOpen(wasOpen ? null : p);
    });
  }

  // A result landed: say so. Focus went to the heading when the scan started;
  // by now the visitor may be reading a panel, typing in the form or back on
  // the scan button, so the first result moves focus to the heading only when
  // focus is on nothing.
  function arrival(text: string) {
    announce(text);
    if (!announcedFirst) {
      announcedFirst = true;
      const active = document.activeElement;
      if (!active || active === document.body) heading.focus({ preventScroll: true });
    }
  }

  // The mapping table could not load: a reload is the only way out, so say so
  // instead of spending PSI calls (the proxy limits requests per visitor).
  function speedNeedsReload() {
    const { title } = classifyError(reloadError());
    state.lastPsi = null;
    settle('speed', NOT_MEASURED, title);
    arrival(withNote(`Speed not measured. ${title}`));
  }

  async function runSpeed(id: number, url: string) {
    if (isMappingFailed()) {
      speedNeedsReload();
      return;
    }
    const mappingP = loadMapping();
    mappingP.catch(() => {});
    const [m, d] = await Promise.allSettled([
      getJson<PsiBody>(psiUrl(url, 'mobile'), PSI_TIMEOUT_MS, PSI_TIMEOUT_REASON),
      getJson<PsiBody>(psiUrl(url, 'desktop'), PSI_TIMEOUT_MS, PSI_TIMEOUT_REASON),
    ]);
    if (id !== runId) return;
    const asError = (r: PromiseSettledResult<PsiBody>) =>
      r.status === 'rejected' && r.reason instanceof RequestError ? r.reason : null;
    state.lastPsi = {
      mobile: m.status === 'fulfilled' ? m.value : null,
      desktop: d.status === 'fulfilled' ? d.value : null,
      errors: { mobile: asError(m), desktop: asError(d) },
    };
    const mobile = state.lastPsi.mobile ? perfScore(state.lastPsi.mobile) : null;
    const desktop = state.lastPsi.desktop ? perfScore(state.lastPsi.desktop) : null;
    if (mobile !== null || desktop !== null) {
      track('psi-analyze-result', {
        url,
        hostname: hostOf(url),
        mobile_score: mobile,
        desktop_score: desktop,
      });
    }
    const status = speedStatus(mobile, desktop);
    if (status.state !== 'none') {
      try {
        await mappingP;
      } catch {
        markMappingFailed();
        if (id === runId) speedNeedsReload();
        return;
      }
      if (id !== runId) return;
    }
    if (status.state === 'none') {
      const err = state.lastPsi.errors.mobile ?? state.lastPsi.errors.desktop;
      const reason = err ? describePsiError(err).title : 'No result.';
      settle('speed', status, reason);
      arrival(withNote(`Speed not measured. ${reason}`));
    } else {
      settle('speed', status);
      arrival(withNote(`Speed ready: ${status.word.toLowerCase()}. ${status.value}.`));
    }
  }

  async function runScan(id: number, url: string) {
    let report: ScanReport | null = null;
    let error: RequestError | null = null;
    let reason = '';
    let permalink: string | undefined;
    try {
      const body = await getJson<ScanBody>(
        `${apiBase()}/scan?url=${encodeURIComponent(url)}`,
        SCAN_TIMEOUT_MS,
        SCAN_TIMEOUT_REASON,
      );
      if (body.error) reason = scanErrorCopy(body.error);
      else {
        report = body.report ?? null;
        permalink = typeof body.permalink === 'string' ? body.permalink : undefined;
      }
    } catch (err) {
      error = err instanceof RequestError ? err : null;
      // A bare "HTTP 502" (a proxy's non-JSON answer) is not a service message.
      const proxyStatus =
        error && error.kind === 'http' && /^HTTP \d+$/.test(error.message)
          ? error.status
          : undefined;
      if (error && error.kind === 'http' && /^HTTP \d+$/.test(error.message)) {
        error = new RequestError(error.message, 'network');
      }
      reason =
        error && error.kind !== 'network'
          ? error.kind === 'http'
            ? scanErrorCopy(error.message)
            : error.message
          : proxyErrorText(proxyStatus);
    }
    if (id !== runId) return;
    if (!report && !reason) reason = 'The scanner returned no result.';
    state.lastReport = report;
    state.lastScan = { report, error, reason: report ? '' : reason, permalink };
    const fit = fitFor(report);
    paintFit(fit);
    if (report) {
      const platform = report.platform as { id?: unknown; originControl?: unknown } | undefined;
      const id = platform?.id;
      const control = platform?.originControl;
      track('ai-scan', {
        domain: hostOf(url),
        grade: report.grade,
        score: report.score,
        csr_gap:
          (report.categories as Record<string, { flag?: string }> | undefined)?.['Content fidelity']
            ?.flag === 'csr-gap',
        // Only a short id and the known words leave the page.
        platform: typeof id === 'string' && /^[a-z0-9-]{1,40}$/.test(id) ? id : 'unknown',
        origin_control:
          typeof control === 'string' && ORIGIN_CONTROL.includes(control) ? control : 'unknown',
      });
    }
    const ai = report ? aireadStatus(report) : NOT_MEASURED;
    const risk = report ? riskStatus(report) : NOT_MEASURED;
    if (ai.state === 'none' && !reason) reason = 'The scanner returned no result.';
    settle('airead', ai, ai.state === 'none' ? reason : '');
    settle('risk', risk, risk.state === 'none' ? reason : '');
    // While Speed is still on its way the sentence waits for it.
    heldNote = fit.spoken;
    const text =
      ai.state === 'none'
        ? `AI readability not measured. ${reason}`
        : `AI readability ready: grade ${report!.grade}, ${report!.score} of 100.`;
    arrival(statuses.speed.state === 'checking' ? text : withNote(text));
  }

  function hostOf(url: string) {
    try {
      return new URL(url).hostname;
    } catch {
      return url;
    }
  }

  function psiUrl(url: string, strategy: Strategy) {
    return `${PSI_ENDPOINT}?${new URLSearchParams({ url, strategy }).toString()}`;
  }

  function apiBase() {
    return resolveApiBase(location.search, location.hostname);
  }

  function setBusy(busy: boolean) {
    running = busy;
    submit.setAttribute('aria-disabled', String(busy));
    submit.setAttribute('aria-busy', String(busy));
    submitLabel.textContent = busy ? 'Checking…' : 'Check page';
    // The line keeps its height (CSS min-height) so its text coming and going
    // never moves what is below it.
    if (hintEl) {
      hintEl.textContent = busy ? WAIT_HINT : '';
    }
  }

  function showError(reason: string | null) {
    errorEl.textContent = reason ?? '';
    errorEl.classList.toggle('hidden', !reason);
    input.setAttribute('aria-invalid', reason ? 'true' : 'false');
  }

  function start(url: string) {
    const id = ++runId;
    announcedFirst = false;
    setBusy(true);
    hostEl.textContent = hostOf(url);
    root!.hidden = false;
    // The results are now on the page: put focus on their heading and say
    // what is happening, so the wait is not silent for a screen reader.
    heading.focus({ preventScroll: true });
    // The live region may have been hidden until this moment, and a region
    // that appears already filled is often not read out. Empty it now and
    // fill it a moment later, unless a result got there first.
    announce('');
    setTimeout(() => {
      if (id === runId && !announcedFirst) announce(`Checking ${hostOf(url)}. ${WAIT_HINT}`);
    }, ANNOUNCE_DELAY_MS);
    state.url = url;
    state.lastPsi = null;
    state.lastScan = null;
    state.lastReport = null;
    heldNote = '';
    paintFit(null);
    for (const p of PILLARS) {
      paintTile(p, CHECKING);
      panels.get(p)!.replaceChildren();
    }
    setOpen(null);
    track('psi-analyze-submit');
    const done = Promise.allSettled([runSpeed(id, url), runScan(id, url)]);
    void done.then(() => {
      if (id === runId) setBusy(false);
    });
    root!.scrollIntoView({
      behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth',
      block: 'start',
    });
  }

  controller = {
    rerun(pillar) {
      if (!state.url) return;
      const redo: Pillar[] = pillar === 'speed' ? ['speed'] : ['airead', 'risk'];
      if (pillar !== 'speed') paintFit(null);
      for (const p of redo) {
        paintTile(p, CHECKING);
        panels.get(p)!.replaceChildren();
      }
      void (pillar === 'speed' ? runSpeed(runId, state.url) : runScan(runId, state.url));
    },
  };

  initLeadForm(state);

  form.addEventListener('submit', (ev) => {
    ev.preventDefault();
    if (running) return;
    const result = normalizeUrl(input.value);
    if (!result.ok) {
      showError(result.reason);
      input.focus();
      return;
    }
    showError(null);
    // Keep the address bar shareable: the checked URL goes into ?url=.
    try {
      const next = new URLSearchParams(location.search);
      next.set('url', result.url);
      history.replaceState(null, '', `${location.pathname}?${next.toString()}${location.hash}`);
    } catch {
      /* a blocked history API must not stop the run */
    }
    start(result.url);
  });

  // ?url= deep links run on both pages. A submit before this script loaded
  // arrives as a plain GET with the field's value, possibly empty.
  const presetParams = new URLSearchParams(location.search);
  if (presetParams.has('url')) {
    const preset = presetParams.get('url') ?? '';
    input.value = preset;
    const result = normalizeUrl(preset);
    if (result.ok) start(result.url);
    else showError(result.reason);
  }

  // Development only: ?fixture=full paints every tile from canned data so the
  // layout can be reviewed without a network.
  if (import.meta.env.DEV && new URLSearchParams(location.search).get('fixture') === 'full') {
    showFixture();
  }

  function showFixture() {
    hostEl.textContent = 'example.com';
    root!.hidden = false;
    paintTile('speed', speedStatus(58, 91));
    paintTile('airead', aireadStatus({ grade: 'C', score: 64 }));
    paintTile(
      'risk',
      riskStatus({
        preConsentLeak: { status: 'ok', verdict: 'leaks' },
        scriptInventory: { status: 'ok', verdict: 'clean' },
        seoDefects: { status: 'ok', verdict: 'attention' },
        responseExposure: { status: 'blocked' },
      }),
    );
  }
}
