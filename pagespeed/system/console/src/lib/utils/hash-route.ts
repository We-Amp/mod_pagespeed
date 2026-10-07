// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** Hash routes: "#/page" optionally followed by "?name=value" parameters. */

export interface HashRoute {
  path: string;
  params: URLSearchParams;
}

export const DEFAULT_PATH = "#/overview";

export function parseHash(hash: string): HashRoute {
  const raw = hash && hash !== "#" ? hash : DEFAULT_PATH;
  const q = raw.indexOf("?");
  return q < 0
    ? { path: raw, params: new URLSearchParams() }
    : { path: raw.slice(0, q), params: new URLSearchParams(raw.slice(q + 1)) };
}

export type Severity = "fatal" | "error" | "warning" | "info";

const RANK: Record<Severity, number> = { fatal: 0, error: 1, warning: 2, info: 3 };

/** Whether `level` is one of the four severities, not an arbitrary or absent value. */
export function isKnownSeverity(level: string | null): level is Severity {
  return level !== null && Object.prototype.hasOwnProperty.call(RANK, level);
}

/** A `level=` link shows that severity and everything more severe; absent or unknown shows all. */
export function severityFilter(level: string | null): Record<Severity, boolean> {
  const known = isKnownSeverity(level);
  const max = known ? RANK[level as Severity] : RANK.info;
  return {
    fatal: RANK.fatal <= max,
    error: RANK.error <= max,
    warning: RANK.warning <= max,
    info: RANK.info <= max,
  };
}

/** The browser tab title for a page. */
export function pageTitle(label: string, consoleTitle: string): string {
  return `${label} — ${consoleTitle}`;
}

/** Lets only the most recently started load apply its result. */
export class LatestGate {
  #current = 0;

  begin(): number {
    return ++this.#current;
  }

  isCurrent(ticket: number): boolean {
    return ticket === this.#current;
  }
}
