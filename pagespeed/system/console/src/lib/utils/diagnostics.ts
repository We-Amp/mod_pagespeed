// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The diagnostics bundle on the Support page: a plain-text summary of this
 * console's server to paste into a support request or an issue report. It
 * is assembled from one read of each of this console's own admin pages and
 * goes only to the clipboard, a downloaded file or the text box on the page:
 * nothing is sent anywhere.
 *
 * The bundle is written to be pasted somewhere public. Query strings are
 * removed unless the operator keeps them; the configuration text is left out
 * unless the operator adds it, and then the values of options whose names
 * suggest a secret are replaced; client addresses in log lines are masked;
 * cookie and authorization header values and passwords inside addresses are
 * replaced everywhere. It ends with a list of what it left out.
 */

import type {
  CacheStructureResponse,
  ConfigResponse,
  DaemonHealthResponse,
  DaemonStatsResponse,
  MessagesResponse,
  PurgeSetResponse,
  StatsResponse,
} from "$lib/api/types";
import { ALERT_RULES, AlertTracker, failedChecks, optimizerStartMs, type AlertSeverity } from "$lib/alerts";
import { PRODUCT_NAME } from "$lib/data/product-facts-console";
import { DIALECTS, type Dialect } from "$lib/utils/dialect";
import { formatDuration } from "$lib/utils/format";
import { daemonAvailability, sampleOverview, toSnapshot, variablesOf } from "$lib/utils/overview";
import { sha256Hex } from "$lib/utils/sha256";
import { optimizerVersions } from "$lib/utils/versions";

/** How many warnings and errors the bundle lists. */
export const DIAGNOSTICS_MESSAGES = 50;

/**
 * The module counters the bundle lists: the ones a support reply asks for
 * first (traffic, the optimizer's serves and fall-throughs, fetches,
 * rewrites, the module cache).
 */
export const KEY_COUNTERS: readonly string[] = [
  "page_load_count",
  "num_flushes",
  "ipro_daemon_served",
  "ipro_daemon_served_original",
  "ipro_daemon_fallthrough",
  "ipro_not_in_cache",
  "num_resource_fetch_successes",
  "num_resource_fetch_failures",
  "num_rewrites_executed",
  "num_rewrites_dropped",
  "cache_hits",
  "cache_misses",
];

/** At most this many lines of the module cache's own statistics. */
const CACHE_BACKEND_LINES = 20;
/** One cache summary is cut at this many characters. */
const CACHE_SUMMARY_CHARS = 300;

export interface DiagnosticsOptions {
  /** Keep query strings in addresses (off by default). */
  queryStrings: boolean;
  /** Add the configuration text, with secret-looking values replaced (off by default). */
  configText: boolean;
}

export const DEFAULT_DIAGNOSTICS_OPTIONS: DiagnosticsOptions = { queryStrings: false, configText: false };

export interface DiagnosticsFinding {
  severity: AlertSeverity;
  label: string;
  message: string;
  fix: string;
}

export interface DiagnosticsCheck {
  name: string;
  pass: boolean | null;
}

export interface DiagnosticsCache {
  caches: Array<{ name: string; summary: string }>;
  backend: string[];
  purgeEnabled: boolean | null;
}

export interface DiagnosticsPurge {
  /** Epoch ms of the last whole-cache invalidation; null for none. */
  globalMs: number | null;
  /** URLs in the purge set (their addresses are left out). */
  urls: number;
}

/** What one collection read: everything the text is written from. */
export interface DiagnosticsData {
  generatedAt: number;
  /** The build stamp: the module and the console are built together. */
  build: string;
  /** The browser's user agent, as it reports itself. */
  userAgent: string;
  /** The web server the console's own responses named; null when none did. */
  server: Dialect | null;
  /** Epoch ms; null when unknown. */
  moduleStartMs: number | null;
  optimizerStartMs: number | null;
  optimizer: string;
  /** The optimizer's health checks; null when it did not answer. */
  checks: DiagnosticsCheck[] | null;
  /** Findings of one reading; null when the module statistics could not be read. */
  findings: DiagnosticsFinding[] | null;
  /** Module counters; null when the statistics could not be read. */
  counters: Record<string, number> | null;
  cache: DiagnosticsCache | null;
  purge: DiagnosticsPurge | null;
  configSha256: string | null;
  /** The configuration text this console's scope runs with; null when it could not be read. */
  configText: string | null;
  /** The message_history list (oldest first); anything else lists nothing. */
  messages: unknown;
}

/** The reads of one collection; the message log is read once, as a plain list. */
export interface DiagnosticsApi {
  getConfig(): Promise<ConfigResponse>;
  getStats(): Promise<StatsResponse>;
  daemonHealth(): Promise<DaemonHealthResponse>;
  daemonStats(): Promise<DaemonStatsResponse>;
  getMessages(): Promise<MessagesResponse>;
  getCacheStructure(): Promise<CacheStructureResponse>;
  getPurgeSet(): Promise<PurgeSetResponse>;
}

export interface Clipboardish {
  writeText(text: string): Promise<void>;
}

const SERIOUS: ReadonlySet<string> = new Set(["warning", "error", "fatal"]);

/** The last `n` warnings and worse of a message list (oldest first), newest first. */
export function lastWarnings(messages: unknown, n: number = DIAGNOSTICS_MESSAGES): Array<{ severity: string; message: string }> {
  if (!Array.isArray(messages)) return [];
  const out: Array<{ severity: string; message: string }> = [];
  for (const entry of messages) {
    if (entry === null || typeof entry !== "object") continue;
    const { severity, message } = entry as { severity?: unknown; message?: unknown };
    if (typeof severity === "string" && SERIOUS.has(severity) && typeof message === "string") {
      out.push({ severity, message });
    }
  }
  return out.slice(-n).reverse();
}

/** One line: every control character (line breaks included, tab excepted) becomes a space. */
function oneLine(s: string): string {
  return s.replace(/[\u0000-\u0008\u000a-\u001f\u007f]/g, " ");
}

function cap(s: string, max: number): string {
  return s.length > max ? `${s.slice(0, max)}…` : s;
}

// ── What the bundle never carries ──────────────────────────────────────

/** Replaces a query string that was removed. */
export const QUERY_REMOVED = "?[query removed]";
/** Replaces a value that was removed. */
export const REDACTED = "[redacted]";

/**
 * Remove the query string of every address in `text`: an absolute URL or a
 * path, up to its fragment or the end of the address. Punctuation that ends
 * a sentence after the address stays.
 */
export function stripQueryStrings(text: string): string {
  return text
    .replace(
      /((?:\b[a-z][a-z0-9+.-]*:\/\/|\/)[^\s?#"'<>]*)\?[^\s#"'<>]*?(?=[.,;:!)\]]*(?:[\s#"'<>]|$))/gi,
      `$1${QUERY_REMOVED}`,
    )
    .replace(/;jsessionid=[^\s?#/;"'<>]+/gi, `;jsessionid=${REDACTED}`);
}

/** Replaces a client address that was masked. */
export const ADDRESS_REMOVED = "[address removed]";

/** Loopback, private, link-local and unspecified IPv4: not personal, kept. */
function keptV4(a: number, b: number): boolean {
  return a === 0 || a === 10 || a === 127 || (a === 169 && b === 254) || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168);
}

/** Loopback, unspecified, unique-local (fc00::/7) and link-local (fe80::/10) IPv6: kept. */
function keptV6(normalized: string): boolean {
  if (normalized === "::1" || normalized === "::") return true;
  const first = parseInt(normalized.split(":")[0] || "0", 16);
  return (first & 0xfe00) === 0xfc00 || (first & 0xffc0) === 0xfe80;
}

/** The canonical form of an IPv6 address, or null when `c` is not one. */
function ipv6(c: string): string | null {
  if ((c.match(/:/g) ?? []).length < 2) return null;
  try {
    const host = new URL(`http://[${c}]/`).hostname;
    return host.slice(1, -1);
  } catch {
    return null;
  }
}

/**
 * Mask the public IPv4 and IPv6 addresses in a log line: the module writes
 * the requesting client's address into some warnings. Loopback and private
 * ranges stay (they help map an origin); times, versions and hashes are not
 * addresses and stay as they are.
 */
export function maskClientAddresses(text: string): string {
  return text
    .replace(/(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\w]|\.\d)/g, (m, a, b, c, d) => {
      const o = [a, b, c, d].map(Number);
      if (o.some((n) => n > 255)) return m;
      return keptV4(o[0], o[1]) ? m : ADDRESS_REMOVED;
    })
    .replace(/(?<![\w:.])(?:[0-9a-f]{0,4}:){2,7}[0-9a-f]{0,4}(?![\w:]|\.\w)/gi, (m) => {
      const v6 = ipv6(m);
      return v6 === null || keptV6(v6) ? m : ADDRESS_REMOVED;
    });
}

/** Option names that suggest a secret value. */
const SECRET_NAME = /key|secret|passw(?:or)?d|passphrase|token|credential/i;

/** Header names whose values are secrets wherever they appear. */
const SECRET_HEADER = /\b((?:set-)?cookie|(?:proxy-)?authorization|x-api-key|x-auth-token)(\s*[:=]\s*)\S[^\n]*/gi;

/**
 * Replace what is secret-looking anywhere in the bundle: cookie and
 * authorization header values, bearer tokens and the password inside an
 * address (`scheme://user:password@host`, `//user:password@host` or
 * `user:password@host`), up to the last `@` before the host.
 */
export function scrubSecrets(text: string): string {
  return text
    .replace(SECRET_HEADER, `$1$2${REDACTED}`)
    .replace(/\b(bearer)\s+[A-Za-z0-9._~+/=-]{16,}/gi, `$1 ${REDACTED}`)
    .replace(/(^|[\s"'(<[=]|\/\/)([^\s/:@"'<>]+):[^\s/"'<>]*@(?=[\w[])/gim, `$1$2:${REDACTED}@`);
}

/**
 * The configuration text with the value of every option whose name suggests
 * a secret (key, secret, password, token, credential) replaced. The console
 * receives the module's option list (`Name (id)  value`); the module already
 * masks the options it knows are secret, so this is a second line of defence.
 */
export function redactConfig(text: string): string {
  return text.replace(/^(\s*([A-Za-z][\w.-]*)\s+\([^)]*\)\s+)\S.*$/gm, (line, head: string, name: string) =>
    SECRET_NAME.test(name) ? `${head}${REDACTED}` : line,
  );
}

// ── The text ───────────────────────────────────────────────────────────

function iso(ms: number | null): string {
  return ms === null ? "unknown" : new Date(ms).toISOString();
}

function startLine(label: string, startMs: number | null, at: number): string {
  if (startMs === null) return `${label}: unknown`;
  const up = at >= startMs ? ` (running for ${formatDuration((at - startMs) / 1000)})` : "";
  return `${label}: ${iso(startMs)}${up}`;
}

/** What a section says when its read failed. */
const UNREAD = "could not be read";

function serverLine(server: Dialect | null): string {
  return `Web server: ${DIALECTS.find((d) => d.id === server)?.label ?? "not detected"}`;
}

function findingsLines(findings: DiagnosticsFinding[] | null): string[] {
  if (findings === null) return [`Findings: ${UNREAD}`];
  if (findings.length === 0) return ["Findings: none in one reading"];
  const out = [findings.length === 1 ? "Findings (1, from one reading):" : `Findings (${findings.length}, from one reading):`];
  for (const f of findings) {
    out.push(`- [${f.severity}] ${oneLine(f.label)}: ${oneLine(f.message)}`);
    out.push(`  Fix: ${oneLine(f.fix)}`);
  }
  return out;
}

function checksLines(checks: DiagnosticsCheck[] | null): string[] {
  if (checks === null) return [`Optimizer checks: ${UNREAD}`];
  if (checks.length === 0) return ["Optimizer checks: none reported"];
  return [
    "Optimizer checks:",
    ...checks.map((c) => `  ${oneLine(c.name)}: ${c.pass === true ? "pass" : c.pass === false ? "FAIL" : "reported, no verdict"}`),
  ];
}

function countersLines(counters: Record<string, number> | null): string[] {
  if (counters === null) return [`Key counters: ${UNREAD}`];
  return [
    "Key counters (since the module started):",
    ...KEY_COUNTERS.map((name) => `  ${name}: ${name in counters ? String(counters[name]) : "not reported"}`),
  ];
}

function cacheLines(cache: DiagnosticsCache | null, purge: DiagnosticsPurge | null): string[] {
  const out: string[] = [];
  if (cache === null) {
    out.push(`Caches: ${UNREAD}`);
  } else {
    out.push(cache.caches.length === 0 ? "Caches: none reported" : "Caches:");
    for (const c of cache.caches) out.push(`  ${oneLine(c.name)}: ${cap(oneLine(c.summary).trim(), CACHE_SUMMARY_CHARS)}`);
    for (const line of cache.backend) out.push(`  ${line}`);
  }
  const enabled = cache?.purgeEnabled;
  const purging = enabled === true ? "enabled" : enabled === false ? "not enabled" : "unknown";
  if (purge === null) {
    out.push(`Purging: ${purging}; purge set ${UNREAD}`);
  } else {
    const urls = purge.urls === 1 ? "1 URL" : `${purge.urls} URLs`;
    const global = purge.globalMs === null ? "none" : iso(purge.globalMs);
    out.push(`Purging: ${purging}; last whole-cache invalidation: ${global}; ${urls} in the purge set`);
  }
  return out;
}

function warningLines(messages: unknown): string[] {
  const warnings = lastWarnings(messages);
  const heading =
    warnings.length === 0
      ? "Warnings and errors: none in the log"
      : warnings.length === 1
        ? "The last warning or error:"
        : `The last ${warnings.length} warnings and errors, newest first:`;
  return [heading, ...warnings.map((w) => `[${w.severity}] ${maskClientAddresses(oneLine(w.message))}`)];
}

function configLines(d: DiagnosticsData, options: DiagnosticsOptions): string[] {
  if (!options.configText) return [];
  if (d.configText === null) return ["", `Configuration: ${UNREAD}`];
  const text = redactConfig(d.configText).replace(/\s+$/, "");
  return ["", "Configuration (values of options whose names suggest a secret are replaced):", text === "" ? "(empty)" : text];
}

function leftOutLines(options: DiagnosticsOptions): string[] {
  return [
    "Left out of this bundle:",
    options.configText
      ? "- the values of options whose names suggest a secret"
      : "- the configuration text (its SHA-256 is above)",
    ...(options.queryStrings ? [] : ["- query strings in addresses"]),
    "- client addresses in log lines",
    "- cookie and authorization header values, passwords in addresses",
    "- the addresses in the purge set",
    "- informational log lines, findings that need two readings",
  ];
}

/** The bundle as plain text, in a fixed order; `scope` is "whole server" or "this host (<host>)". */
export function diagnosticsText(
  d: DiagnosticsData,
  scope: string,
  options: DiagnosticsOptions = DEFAULT_DIAGNOSTICS_OPTIONS,
): string {
  const lines = [
    `${PRODUCT_NAME} diagnostics`,
    `Generated: ${new Date(d.generatedAt).toISOString()}`,
    `Console: ${scope}`,
    `Build: ${d.build}`,
    serverLine(d.server),
    `Browser: ${d.userAgent === "" ? "unknown" : oneLine(d.userAgent)}`,
    startLine("Module started", d.moduleStartMs, d.generatedAt),
    `Optimizer: ${d.optimizer}`,
    startLine("Optimizer started", d.optimizerStartMs, d.generatedAt),
    `Configuration SHA-256: ${d.configSha256 ?? UNREAD}`,
    "",
    ...findingsLines(d.findings),
    "",
    ...checksLines(d.checks),
    "",
    ...countersLines(d.counters),
    "",
    ...cacheLines(d.cache, d.purge),
    "",
    ...warningLines(d.messages),
    ...configLines(d, options),
    "",
    ...leftOutLines(options),
  ];
  const text = scrubSecrets(`${lines.join("\n")}\n`);
  return options.queryStrings ? text : stripQueryStrings(text);
}

// ── Reading ────────────────────────────────────────────────────────────

/** What the bundle says about the optimizer: its version, or why there is none. */
export function optimizerLine(health: PromiseSettledResult<DaemonHealthResponse>): string {
  if (health.status === "fulfilled") {
    const v = optimizerVersions(health.value);
    if (v.version === null) return "answering, version not reported";
    return v.commit === null ? v.version : `${v.version} (${v.commit})`;
  }
  const reason: unknown = health.reason;
  if (!(reason instanceof Error)) return "did not answer";
  switch (daemonAvailability(reason)) {
    case "not-configured":
      return "not configured";
    case "unreachable":
      return "configured, not answering";
    case "unsupported":
      return "too old to report its version";
    default:
      return "did not answer";
  }
}

/** The configuration text this console's scope runs with, else the server's; null without either. */
export function configTextOf(config: unknown): string | null {
  if (config === null || typeof config !== "object") return null;
  const c = config as { effective_config?: unknown; config?: unknown };
  if (typeof c.effective_config === "string" && c.effective_config !== "") return c.effective_config;
  return typeof c.config === "string" ? c.config : null;
}

/** SHA-256 of the configuration this console's scope runs with, else of the server's; null without either. */
export function configHash(config: unknown): string | null {
  const text = configTextOf(config);
  return text === null ? null : sha256Hex(text);
}

/** The optimizer's health checks, by name, with their verdict. */
export function healthChecks(health: DaemonHealthResponse | null): DiagnosticsCheck[] {
  const checks: unknown = health?.checks;
  if (checks === null || typeof checks !== "object" || Array.isArray(checks)) return [];
  const failed = new Set(failedChecks(health));
  return Object.entries(checks as Record<string, unknown>).map(([name, v]) => {
    const passed = v === true || (v !== null && typeof v === "object" && (v as { pass?: unknown }).pass === true);
    return { name, pass: failed.has(name) ? false : passed ? true : null };
  });
}

/** The cache page's summary: cache names and layers, the backend's own lines, the purge switch. */
export function cacheOf(c: CacheStructureResponse): DiagnosticsCache {
  const caches = Array.isArray(c.caches)
    ? c.caches.filter((x) => x !== null && typeof x === "object" && typeof x.name === "string").map((x) => ({
        name: x.name,
        summary: typeof x.summary === "string" ? x.summary : "",
      }))
    : [];
  const backend =
    typeof c.backend_stats === "string"
      ? c.backend_stats
          .split("\n")
          .map((l) => oneLine(l).trim())
          .filter((l) => l !== "")
          .slice(0, CACHE_BACKEND_LINES)
      : [];
  return { caches, backend, purgeEnabled: typeof c.purge_enabled === "boolean" ? c.purge_enabled : null };
}

function settledValue<T>(r: PromiseSettledResult<T>): T | null {
  return r.status === "fulfilled" ? r.value : null;
}

export interface CollectContext {
  build: string;
  userAgent: string;
  server: Dialect | null;
}

/** Read each source once and gather what the bundle is written from. Never rejects. */
export async function collectDiagnostics(
  api: DiagnosticsApi,
  ctx: CollectContext,
  now: () => number = Date.now,
): Promise<DiagnosticsData> {
  // The message log is read once, as the plain list: the findings group it
  // in the console (toMessageDigest), the bundle lists its last warnings.
  const [sample, [config, messages, cache, purge]] = await Promise.all([
    sampleOverview(api, now, { messages: false }),
    Promise.allSettled([api.getConfig(), api.getMessages(), api.getCacheStructure(), api.getPurgeSet()]),
  ]);
  sample.messages =
    messages.status === "fulfilled"
      ? { ok: true, data: messages.value }
      : { ok: false, error: messages.reason instanceof Error ? messages.reason : new Error(String(messages.reason)) };
  const at = sample.at;
  const snap = toSnapshot(sample, null, ctx.build);
  // A fresh tracker sees one reading: findings that judge a rise over time
  // need a second one and are left out (the bundle says so).
  const findings =
    snap.module === null
      ? null
      : new AlertTracker(ALERT_RULES).evaluate(snap, at).map((a) => ({
          severity: a.severity,
          label: a.label,
          message: a.message,
          fix: a.fix,
        }));
  const health: PromiseSettledResult<DaemonHealthResponse> = sample.health.ok
    ? { status: "fulfilled", value: sample.health.data }
    : { status: "rejected", reason: sample.health.error };
  const moduleStart = sample.module.ok ? variablesOf(sample.module.data).process_start_ms : undefined;
  const optimizerStart = snap.daemon === "ok" ? optimizerStartMs(snap) : undefined;
  const list: unknown = settledValue(messages);
  const purgeSet = settledValue(purge);
  const configValue = settledValue(config);
  return {
    generatedAt: at,
    build: ctx.build,
    userAgent: ctx.userAgent,
    server: ctx.server,
    moduleStartMs: moduleStart !== undefined && moduleStart > 0 ? moduleStart : null,
    optimizerStartMs: optimizerStart !== undefined && optimizerStart > 0 ? optimizerStart : null,
    optimizer: optimizerLine(health),
    checks: snap.daemon === "ok" ? healthChecks(snap.health) : null,
    findings,
    counters: snap.module,
    cache: cache.status === "fulfilled" && cache.value !== null && typeof cache.value === "object" ? cacheOf(cache.value) : null,
    purge:
      purgeSet !== null && typeof purgeSet === "object"
        ? {
            globalMs:
              typeof purgeSet.global_invalidation_timestamp_ms === "number" ? purgeSet.global_invalidation_timestamp_ms : null,
            urls: Array.isArray(purgeSet.purge_set) ? purgeSet.purge_set.length : 0,
          }
        : null,
    configSha256: configHash(configValue),
    configText: configTextOf(configValue),
    messages: list !== null && typeof list === "object" ? (list as { messages?: unknown }).messages : [],
  };
}

// ── Out of the page ────────────────────────────────────────────────────

/** The downloaded file's name: the product and the time of the reading, safe on every file system. */
export function diagnosticsFileName(generatedAt: number): string {
  const stamp = new Date(generatedAt).toISOString().replace(/\.\d+Z$/, "Z").replace(/:/g, "-");
  return `${PRODUCT_NAME}-diagnostics-${stamp}.txt`;
}

/** Offer `text` as a plain-text file download. False when the browser cannot. */
export function downloadText(text: string, fileName: string, doc: Document | undefined = globalThis.document): boolean {
  if (doc === undefined || typeof URL.createObjectURL !== "function") return false;
  try {
    const url = URL.createObjectURL(new Blob([text], { type: "text/plain;charset=utf-8" }));
    const a = doc.createElement("a");
    a.href = url;
    a.download = fileName;
    a.style.display = "none";
    doc.body.appendChild(a);
    a.click();
    a.remove();
    // Revoke once the click has been handled.
    setTimeout(() => URL.revokeObjectURL(url), 0);
    return true;
  } catch {
    return false;
  }
}

/**
 * Put `text` on the clipboard: the asynchronous clipboard API when the page
 * may use it, else a selected, invisible textarea and the copy command.
 * False when neither worked; the caller then points at the text box.
 */
export async function copyText(
  text: string,
  clipboard: Clipboardish | undefined = globalThis.navigator?.clipboard,
  doc: Document | undefined = globalThis.document,
): Promise<boolean> {
  if (clipboard !== undefined) {
    try {
      await clipboard.writeText(text);
      return true;
    } catch {
      // Refused (permissions, an insecure page): try the textarea.
    }
  }
  if (doc === undefined) return false;
  const area = doc.createElement("textarea");
  area.value = text;
  area.setAttribute("readonly", "");
  area.style.position = "fixed";
  area.style.top = "0";
  area.style.opacity = "0";
  doc.body.appendChild(area);
  area.select();
  let copied = false;
  try {
    copied = doc.execCommand("copy");
  } catch {
    copied = false;
  }
  area.remove();
  return copied;
}
