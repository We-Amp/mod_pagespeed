// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Pure tile-status rules for the scan summary strip. A tile always carries a
// status word (never colour alone) and one value line.

export type TileState = 'checking' | 'good' | 'needs-work' | 'poor' | 'clean' | 'flagged' | 'none';

export interface TileStatus {
  state: TileState;
  /** The status word shown on the tile. */
  word: string;
  /** The single value line; empty while checking or not measured. */
  value: string;
}

export const CHECKING: TileStatus = { state: 'checking', word: 'Checking…', value: '' };
export const NOT_MEASURED: TileStatus = { state: 'none', word: 'Not measured', value: '' };

// Mobile score decides (PSI convention); desktop when mobile is missing.
export function speedStatus(
  mobile: number | null | undefined,
  desktop: number | null | undefined,
): TileStatus {
  const m = typeof mobile === 'number' ? mobile : null;
  const d = typeof desktop === 'number' ? desktop : null;
  const basis = m ?? d;
  if (basis === null) return NOT_MEASURED;
  const state: TileState = basis >= 90 ? 'good' : basis >= 50 ? 'needs-work' : 'poor';
  const word = state === 'good' ? 'Good' : state === 'needs-work' ? 'Needs work' : 'Poor';
  const part = (label: string, v: number | null) => `${label} ${v === null ? '—' : v}`;
  return { state, word, value: `${part('Mobile', m)} · ${part('Desktop', d)}` };
}

interface ReportLike {
  grade?: string;
  score?: number;
  [lens: string]: unknown;
}

export function aireadStatus(report: ReportLike | null | undefined): TileStatus {
  const grade = report?.grade;
  if (!report || typeof grade !== 'string' || typeof report.score !== 'number') {
    return NOT_MEASURED;
  }
  const state: TileState =
    grade === 'A' || grade === 'B' ? 'good' : grade === 'C' ? 'needs-work' : 'poor';
  const word = state === 'good' ? 'Good' : state === 'needs-work' ? 'Needs work' : 'Poor';
  return { state, word, value: `Grade ${grade} · ${report.score}/100` };
}

// The four Risk & SEO lenses, in panel order, and the verdict that counts as
// flagged for each.
export const RISK_LENSES = [
  { key: 'preConsentLeak', flagged: 'leaks' },
  { key: 'scriptInventory', flagged: 'attention' },
  { key: 'seoDefects', flagged: 'attention' },
  { key: 'responseExposure', flagged: 'attention' },
] as const;

interface LensLike {
  status?: string;
  verdict?: string;
}

/** A lens counts toward k only when it ran (status ok) and reached a verdict. */
export function lensMeasured(v: LensLike | null | undefined): boolean {
  return !!v && v.status === 'ok' && typeof v.verdict === 'string' && v.verdict !== 'unknown';
}

export function riskStatus(report: ReportLike | null | undefined): TileStatus {
  if (!report) return NOT_MEASURED;
  let k = 0;
  let n = 0;
  for (const lens of RISK_LENSES) {
    const v = report[lens.key] as LensLike | null | undefined;
    if (!lensMeasured(v)) continue;
    k += 1;
    if (v!.verdict === lens.flagged) n += 1;
  }
  if (k === 0) return NOT_MEASURED;
  return {
    state: n === 0 ? 'clean' : 'flagged',
    word: n === 0 ? 'Nothing flagged' : `${n} flagged`,
    value:
      n === 0
        ? `0 of ${k} checks flagged`
        : n === 1
          ? '1 check needs attention'
          : `${n} checks need attention`,
  };
}

const NEEDS_ATTENTION: ReadonlySet<TileState> = new Set(['needs-work', 'poor', 'flagged']);

export const UNREADABLE_LINE = 'We could read almost nothing on this page.';
const COUNT: Record<number, string> = { 1: 'one', 2: 'two', 3: 'three' };

/**
 * The line above the tiles. `improve` is how many next steps the result has:
 * with any, the line counts them and points at the list, so it never says
 * nothing needs attention above a list of things to do.
 */
export function healthLine(
  statuses: readonly TileStatus[],
  improve = 0,
  opts: { shown?: number; unreadable?: boolean } = {},
): string {
  const pending = statuses.filter((s) => s.state === 'checking').length;
  if (pending > 0) return `Checking ${pending} of 3 areas…`;
  // The scan read next to nothing: a count of things to improve would be about an empty page.
  if (opts.unreadable) return UNREADABLE_LINE;
  if (improve === 1) return '1 thing to improve. Start with the step below.';
  if (improve > 1 && improve > (opts.shown ?? improve)) {
    return `${improve} things to improve. The first ${COUNT[opts.shown ?? 0] ?? opts.shown} are below.`;
  }
  if (improve > 1) return `${improve} things to improve. Start with the steps below.`;
  const measured = statuses.filter((s) => s.state !== 'none');
  if (measured.length === 0) return 'We could not check this page.';
  const n = measured.filter((s) => NEEDS_ATTENTION.has(s.state)).length;
  if (n === 0) {
    return measured.length === 1
      ? 'The one area checked needs no attention.'
      : `None of the ${measured.length} areas checked needs attention.`;
  }
  if (measured.length === 1) return 'The one area checked needs attention.';
  const k = measured.length;
  if (n === 1) return `1 of ${k} areas checked needs attention.`;
  return `${n} of ${k} areas checked need attention.`;
}
