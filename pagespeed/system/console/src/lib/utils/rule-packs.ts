// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Rule packs page's model: the module's JSON status (schema 1) read
 * defensively, the pack states in words, and the decision tail's client-side
 * filters. Everything in the answer is text from the server's configuration
 * or from the decision log (visitor-controlled paths and hosts): it is
 * shown as text, never as markup or links.
 */

import type { RulePackDecisionRow, RulePackEntry, RulePackLicense, RulePacksResponse } from "$lib/api/types";

/** The only JSON schema this page reads. */
export const RULE_PACKS_SCHEMA = 1;

export type PackTone = "success" | "warning" | "error" | "neutral";

export interface PackStateInfo {
  label: string;
  tone: PackTone;
  explain: string;
}

/** A pack file's load state in words; an unknown state is shown as it came. */
export function packStateInfo(state: string | undefined, hasId: boolean): PackStateInfo {
  switch (state) {
    case "loaded":
      return { label: "loaded", tone: "success", explain: "The pack is loaded and in use." };
    case "failed-keeping-earlier":
      return {
        label: "failed, keeping earlier",
        tone: "warning",
        explain: "The last reload failed; the previously loaded version stays in use.",
      };
    case "refused":
      return { label: "refused", tone: "error", explain: "The pack was refused and is not in use." };
    case "duplicate-id":
      return {
        label: "duplicate id",
        tone: "warning",
        explain: "An earlier file of this scope already holds a pack with this id, so this file is not used.",
      };
    case "none":
      return hasId
        ? { label: "not loaded", tone: "neutral", explain: "No pack is loaded from this file." }
        : { label: "not read yet", tone: "neutral", explain: "This server process has not read the pack file yet." };
    default:
      return { label: state || "unknown", tone: "neutral", explain: "" };
  }
}

export interface RulePacksView {
  scope: string;
  host: string;
  generatedAt: string;
  filterEnabled: boolean;
  files: string[];
  mode: string;
  requireSigned: boolean;
  packs: PackView[];
  overlapWarnings: string[];
  counters: Array<{ name: string; value: number }> | null;
  log: {
    date: string;
    pack: string | null;
    limit: number;
    status: string;
    error: string | null;
    rows: RulePackDecisionRow[];
  };
  /** Files are configured but this server process has not read any of them yet: poll again. */
  notLoadedYet: boolean;
  /** No pack file is configured at all. */
  noneConfigured: boolean;
}

export interface PackView {
  position: number;
  file: string;
  state: PackStateInfo;
  rawState: string;
  id: string | null;
  version: string | null;
  publisher: string | null;
  keyId: string | null;
  sha256: string | null;
  lastEventAt: string | null;
  lastError: string | null;
  warnings: string[];
  license: { label: string; expires: string | null; terms: string | null } | null;
}

const str = (v: unknown): string | null => (typeof v === "string" ? v : null);
const strings = (v: unknown): string[] => (Array.isArray(v) ? v.filter((x): x is string => typeof x === "string") : []);

function licenseView(l: RulePackLicense | null | undefined): PackView["license"] {
  if (l === null || l === undefined || typeof l !== "object") return null;
  return { label: str(l.label) ?? "", expires: str(l.expires), terms: str(l.terms) };
}

function packView(p: RulePackEntry, index: number): PackView {
  const id = str(p.id);
  return {
    position: typeof p.position === "number" ? p.position : index,
    file: str(p.file) ?? "",
    state: packStateInfo(str(p.state) ?? undefined, id !== null),
    rawState: str(p.state) ?? "",
    id,
    version: str(p.version),
    publisher: str(p.publisher),
    keyId: str(p.key_id),
    sha256: str(p.sha256),
    lastEventAt: str(p.last_event_at),
    lastError: str(p.last_error),
    warnings: strings(p.warnings),
    license: licenseView(p.license),
  };
}

/** The status as the page shows it; throws on a schema this page does not know. */
export function rulePacksView(raw: RulePacksResponse): RulePacksView {
  if (raw.schema !== RULE_PACKS_SCHEMA) {
    throw new Error(`unsupported Rule packs status schema ${String(raw.schema)}`);
  }
  const cfg = raw.configuration ?? {};
  const files = strings(cfg.rule_pack_files);
  const packs = (Array.isArray(raw.packs) ? raw.packs : []).map(packView);
  const counters =
    raw.counters !== null && typeof raw.counters === "object"
      ? Object.entries(raw.counters)
          .filter((e): e is [string, number] => typeof e[1] === "number")
          .sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))
          .map(([name, value]) => ({ name, value }))
      : null;
  const d = raw.decisions ?? {};
  return {
    scope: str(raw.scope) ?? "",
    host: str(raw.host) ?? "",
    generatedAt: str(raw.generated_at) ?? "",
    filterEnabled: cfg.filter_enabled === true,
    files,
    mode: str(cfg.rule_pack_mode) ?? "",
    requireSigned: cfg.rule_pack_require_signed === true,
    packs,
    overlapWarnings: strings(raw.overlap_warnings),
    counters,
    log: {
      date: str(d.date) ?? "",
      pack: str(d.pack),
      limit: typeof d.limit === "number" ? d.limit : 0,
      status: str(d.status) ?? "ok",
      error: str(d.error),
      rows: Array.isArray(d.rows) ? d.rows : [],
    },
    notLoadedYet: files.length > 0 && packs.length > 0 && packs.every((p) => p.rawState === "none" && p.id === null),
    noneConfigured: files.length === 0,
  };
}

/** The pack ids the tail can be read from, once each, in load order. */
export function tailPackOptions(packs: readonly PackView[]): string[] {
  const out: string[] = [];
  for (const p of packs) if (p.id !== null && !out.includes(p.id)) out.push(p.id);
  return out;
}

export interface TailFilters {
  /** Rows of this pack id; "" matches every row. */
  pack: string;
  action: string;
  reason: string;
  /** A substring of the rule name, case-insensitive; "" matches every row. */
  rule: string;
  /** Rows of this host (the host lens); null matches every row. */
  host: string | null;
}

export const NO_FILTERS: TailFilters = { pack: "", action: "", reason: "", rule: "", host: null };

export function filterRows(rows: readonly RulePackDecisionRow[], f: TailFilters): RulePackDecisionRow[] {
  const rule = f.rule.trim().toLowerCase();
  return rows.filter(
    (r) =>
      (f.pack === "" || r.pack_id === f.pack) &&
      (f.action === "" || r.action === f.action) &&
      (f.reason === "" || r.reason === f.reason) &&
      (rule === "" || (r.rule ?? "").toLowerCase().includes(rule)) &&
      (f.host === null || (r.host ?? "").toLowerCase() === f.host),
  );
}

/** The distinct non-empty values of one row field, sorted, for a filter's choices. */
export function distinctValues(rows: readonly RulePackDecisionRow[], key: "pack_id" | "action" | "reason"): string[] {
  const set = new Set<string>();
  for (const r of rows) {
    const v = r[key];
    if (typeof v === "string" && v !== "") set.add(v);
  }
  return [...set].sort();
}

/** Why the tail has no rows, in words; null when the log was read. */
export function tailStatusText(status: string, error: string | null): string | null {
  switch (status) {
    case "ok":
      return null;
    case "missing":
      return "No decision log exists for this day yet.";
    case "log_dir_not_set":
      return "The decision log is off: LogDir is not set.";
    case "no_pack_file":
      return "No rule pack file is configured, so there is no decision log.";
    case "error":
      return `The decision log could not be read${error ? `: ${error}` : "."}`;
    default:
      return `Decision log status: ${status}.`;
  }
}

/** A server-rendered page link (`href` from serverPageHref) with the given query parameters; empty ones are left out. */
export function withQuery(href: string, params: Record<string, string | null | undefined>): string {
  const q = new URLSearchParams();
  for (const [k, v] of Object.entries(params)) if (v) q.set(k, v);
  const text = q.toString();
  return text === "" ? href : `${href}?${text}`;
}
