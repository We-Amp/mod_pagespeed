// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * "Inspect a URL": the operator pastes a URL and the console answers why it
 * is, or is not, optimized. The answer has two halves. The module's half
 * (its cache lookup, a link to its log entries naming the URL, the usual reasons)
 * stands on every console; the optimizer's half (its record of the URL, a
 * cooldown, its log) is the whole-server console's. Everything here is
 * plain data: the page renders it as text, never as markup.
 */

import { ApiError } from "$lib/api/client";
import type { CacheEntryKey, CacheEntryResponse, DaemonCooldownEntry } from "$lib/api/types";
import { DOCS, WEBSITE_HOST } from "$lib/data/product-facts-console";
import { routes } from "$lib/routes";
import { parseHash } from "./hash-route";
import { cooldownReasonLabel, errorCode, NO_OPTIMIZER_CONFIGURED } from "./daemon";
import { formatBytes, formatSpan } from "./format";
import { formatSavingsPercent, qualityTier } from "./quality";
import type { VariantRow } from "./url-detail";
import { isOptimizedVariant, type UrlStatus } from "./url-status";
import { detailHref, displayUrl, entryKeyFromAbsoluteUrl } from "./urls-api";

// ── The pasted URL ───────────────────────────────────────────────────

export type InspectTarget =
  | { ok: true; key: CacheEntryKey; href: string }
  | { ok: false; message: string };

/** What the form says when the text is not a URL the console can look up. */
export const INSPECT_INPUT_HELP = "Enter a full URL, starting with http:// or https://.";

/**
 * The pasted text as the URL's inspect page. Only a full http(s) URL is
 * accepted: the optimizer files a URL under its scheme and host, so neither
 * is guessed. Surrounding whitespace is dropped; a fragment never reaches
 * the server and is ignored.
 */
export function inspectTarget(text: string): InspectTarget {
  const key = entryKeyFromAbsoluteUrl(text.trim());
  return key === null ? { ok: false, message: INSPECT_INPUT_HELP } : { ok: true, key, href: detailHref(key) };
}

/** The Logs page narrowed to the entries that name the URL, at every level. */
export function logsHrefFor(key: CacheEntryKey, source: "module" | "optimizer"): string {
  const params = new URLSearchParams({ source, level: "debug", q: displayUrl(key) });
  return `#/logs?${params.toString()}`;
}

/**
 * The inspect page of a URL found in a log line; null when it is not one
 * the console can look up, or is a page of the product's own website (the
 * documentation link in a message is never the operator's URL).
 */
export function inspectHrefForLink(href: string): string | null {
  const key = entryKeyFromAbsoluteUrl(href);
  return key === null || key.host === WEBSITE_HOST ? null : detailHref(key);
}

/**
 * The way back from a URL's page: the address the reader came from, with
 * the view it had (a list's page and filter, a Logs search), when that was
 * another page of the console; otherwise the page it lives under on this
 * console (URLs, or Overview on a per-host console).
 */
export function wayBack(previous: string, isGlobal: boolean): { href: string; label: string } {
  const path = parseHash(previous || "#/").path;
  const from = path === "#/urls/detail" ? undefined : routes.find((r) => r.path === path);
  const route = from ?? routes.find((r) => r.path === (isGlobal ? "#/urls" : "#/overview"));
  return { href: from ? previous : (route?.path ?? "#/overview"), label: route?.title ?? route?.label ?? "Overview" };
}

// ── The module's half ────────────────────────────────────────────────

export type ModuleLookupKind = "current" | "not-current" | "original-url" | "other-host" | "no-answer";

export interface ModuleLookup {
  kind: ModuleLookupKind;
  /** One sentence for the operator. */
  text: string;
  /** What a current result says it did: an optimized rewrite, the original kept, or nothing (null). */
  outcome?: "optimized" | "kept" | null;
}

const LOOKUP_TEXT: Record<ModuleLookupKind, string> = {
  current: "The module holds a current rewrite result for this URL.",
  "not-current": "The module holds no current rewrite result for this URL.",
  "original-url": "This is an original URL; the module's cache lookup answers only for rewritten (.pagespeed.) URLs.",
  "other-host": "This URL belongs to another host; that host's console answers for it.",
  "no-answer": "The module gave no lookup result.",
};

const lookup = (kind: ModuleLookupKind): ModuleLookup => ({ kind, text: LOOKUP_TEXT[kind] });

/**
 * The module's cache lookup (`/cache?url=`) in words. The module answers
 * 200 with a text `value` in every case it understood the question: a dump
 * of the cached rewrite result (the Caches page shows it in full), or the
 * reason there is none to look for. Anything else -- a failed read, a body
 * without that text -- is no result, and is never read as one.
 * `settled` is the lookup's slot of the page's composite read.
 */
export function moduleLookup(settled: PromiseSettledResult<CacheEntryResponse> | undefined): ModuleLookup {
  if (settled === undefined) return lookup("no-answer");
  if (settled.status === "rejected") {
    const error: unknown = settled.reason;
    if (error instanceof ApiError && error.status === 403 && /another host/i.test(errorCode(error))) return lookup("other-host");
    return lookup("no-answer");
  }
  const body: unknown = settled.value;
  const value = body !== null && typeof body === "object" ? (body as CacheEntryResponse).value : undefined;
  if (typeof value !== "string") return lookup("no-answer");
  // Anchored to its own line: the key line above it carries the URL itself.
  if (/^cache_ok:\s*true$/m.test(value)) return { ...lookup("current"), outcome: lookupOutcome(value) };
  if (/^cache_ok:\s*false$/m.test(value)) return lookup("not-current");
  if (/unable to decode resource/i.test(value)) return lookup("original-url");
  return lookup("no-answer");
}

/**
 * What a current result's partitions say (their text dump, two-space
 * indented): a partition with its own rewritten `url` is an optimized
 * result; one marked `optimizable: false` is the original kept. A result
 * without partitions, or one that says neither, says nothing either way.
 * An `input`'s url is indented deeper and never counts.
 */
function lookupOutcome(value: string): "optimized" | "kept" | null {
  let outcome: "optimized" | "kept" | null = null;
  for (const [, body] of value.matchAll(/partition \{\n([\s\S]*?)\n\}/g)) {
    if (/^ {2}optimizable: false/m.test(body)) outcome ??= "kept";
    else if (/^ {2}url: "/m.test(body)) return "optimized";
  }
  return outcome;
}

/** An answer that cannot change while the page is open: asking again would only repeat it. */
export const lookupIsFinal = (kind: ModuleLookupKind): boolean => kind === "original-url" || kind === "other-host";

// ── The optimizer's half ─────────────────────────────────────────────

/** The cooldown entry for exactly this URL (scheme + host + path), or null. */
export function cooldownFor(cooldowns: readonly DaemonCooldownEntry[], key: CacheEntryKey): DaemonCooldownEntry | null {
  for (const entry of cooldowns) {
    if (entry.url === key.url && entry.hostname === key.host && entry.scheme === key.scheme) return entry;
  }
  return null;
}

/** "In cooldown (Processing), 2 min remaining." A missing reason or time is left out, never guessed. */
export function cooldownText(entry: DaemonCooldownEntry): string {
  const reason = cooldownReasonLabel(entry.reason);
  const remaining =
    typeof entry.remaining_seconds === "number" && Number.isFinite(entry.remaining_seconds)
      ? `, ${formatSpan(Math.round(entry.remaining_seconds))} remaining`
      : "";
  return `In cooldown${reason !== null ? ` (${reason})` : ""}${remaining}.`;
}

// ── The verdict ──────────────────────────────────────────────────────

export type UrlVerdictKind = "optimized" | "not-yet" | "kept" | "unknown";

export interface UrlVerdict {
  kind: UrlVerdictKind;
  /** The verdict in a word or two, then the sentence that backs it. */
  word: string;
  text: string;
}

/**
 * The optimizer's record as this page could read it: read (held or "not
 * in its index"), no optimizer configured, refused on a per-host console,
 * not answering, or otherwise unreadable (too old, not supported, failed).
 */
export type RecordAccess = "read" | "none" | "not-here" | "not-answering" | "unreadable";

export interface UrlVerdictInput {
  /** The optimizer's record: its variants and their status; null when it sent none. */
  record: { rows: readonly VariantRow[]; status: UrlStatus | null } | null;
  /** The optimizer answered that it holds nothing for the URL. */
  notInIndex: boolean;
  access: RecordAccess;
  cooldown: DaemonCooldownEntry | null;
  lookup: ModuleLookup;
}

/** " Browsers that accept WebP get 30.3 KB instead of 44.2 KB (-31.5%). Visual score 92.5, good quality." */
function servedText(rows: readonly VariantRow[]): string {
  // The most-served optimized variant stands for what browsers get.
  let best: VariantRow | null = null;
  for (const row of rows) {
    if (row.isSentinel || !isOptimizedVariant(row.raw) || row.size === null || !row.originalSize) continue;
    if (best === null || row.hits > best.hits) best = row;
  }
  // No saving to name when the variant is no smaller.
  if (best === null || best.size === null || best.originalSize === null || best.size >= best.originalSize) return "";
  const f = best.format === "original" ? best.encoding : best.format;
  // A re-encode in the origin's own format goes to every browser.
  const name = f === "webp" ? "WebP" : f === "avif" || f === "svg" ? f.toUpperCase() : f === "gzip" || f === "brotli" ? f : "";
  const q = best.quality === null ? "" : ` Visual score ${best.quality.toFixed(1)}, ${qualityTier(best.quality)} quality.`;
  return ` Browsers${name === "" ? "" : ` that accept ${name}`} get ${formatBytes(best.size)} instead of ${formatBytes(best.originalSize)} (${formatSavingsPercent(best.originalSize, best.size)}).${q}`;
}

const UNREADABLE = "The optimizer's record could not be read.";
const WORD: Record<UrlVerdictKind, string> = {
  optimized: "Optimized.",
  "not-yet": "Not optimized yet.",
  kept: "Not optimized.",
  unknown: "Unknown.",
};

/**
 * Whether this URL is optimized, in one plain line, from what the page
 * already read: the optimizer's record, its cooldown list and the
 * module's lookup. It never claims more than they show: optimized only on
 * an optimized variant or an optimized module result; "not yet" only when
 * the optimizer's record was read (or there is none to read); otherwise
 * unknown, with the reason.
 */
export function urlVerdict(input: UrlVerdictInput): UrlVerdict {
  const { record, cooldown, lookup, access } = input;
  const status = record?.status ?? null;
  const say = (kind: UrlVerdictKind, text: string): UrlVerdict => ({ kind, word: WORD[kind], text: ` ${text}` });
  if (status === "complete" || status === "partial" || status === "revalidating")
    return { ...say("optimized", ""), text: servedText(record?.rows ?? []) };
  if (lookup.outcome === "optimized") return say("optimized", "The module holds an optimized result for it.");
  if (cooldown !== null) return say("not-yet", cooldownText(cooldown));
  if (lookup.outcome === "kept") return say("kept", "The module tried it and kept the original.");
  // A variant without its content class is one whose metadata could not be read: no evidence either way.
  if (record !== null)
    return record.rows.some((r) => !r.isSentinel && r.typeClass === "")
      ? say("unknown", UNREADABLE)
      : say("not-yet", "The optimizer holds no optimized variant of it.");
  if (input.notInIndex) return say("not-yet", "The optimizer has no record of it.");
  if (access === "none" && lookup.kind === "not-current") return say("not-yet", LOOKUP_TEXT["not-current"]);
  return say(
    "unknown",
    access === "not-here"
      ? "The optimizer's record is on the whole-server console."
      : access === "not-answering"
        ? "The optimizer is not answering."
        : access === "unreadable"
          ? UNREADABLE
          : lookup.kind === "original-url"
            ? "The module's lookup answers only for rewritten URLs, and no optimizer is configured."
            : lookup.kind === "current"
              ? "The module's result does not say."
              : LOOKUP_TEXT[lookup.kind === "other-host" ? "other-host" : "no-answer"],
  );
}

/**
 * What a page that does not show the optimizer's record says about it, in
 * one line: that there is no optimizer, so the module's answer is the whole
 * answer (either console), or what the whole-server console adds.
 */
export function wholeServerLine(noOptimizer: boolean): { text: string; link: boolean } {
  return noOptimizer
    ? { text: `${NO_OPTIMIZER_CONFIGURED} The module's answer is the whole answer.`, link: false }
    : { text: "The whole-server console adds the optimizer's record of this URL.", link: true };
}

// ── The reasons ──────────────────────────────────────────────────────

export interface UrlReason {
  id: "not-requested" | "not-cacheable" | "host-not-authorized" | "original-kept";
  title: string;
  text: string;
  /** A fixed documentation link, never a value from an endpoint. */
  doc: { label: string; href: string };
  /** Evidence from this console that the reason holds for this URL; null when it has none. */
  evidence: string | null;
}

const REASONS: ReadonlyArray<Omit<UrlReason, "evidence">> = [
  {
    id: "not-requested",
    title: "Not requested yet",
    text: "It is optimized after a request for it has passed through this server. Request it, then refresh.",
    doc: { label: "Pages are never served optimized", href: DOCS.neverOptimized },
  },
  {
    id: "not-cacheable",
    title: "The response is not cacheable",
    text: "A private, no-store or already expired response is left as it is.",
    doc: { label: "Set Cache-Control headers", href: DOCS.cacheControl },
  },
  {
    id: "host-not-authorized",
    title: "The host is not authorized",
    text: "A resource on another host than its page is rewritten only when that host is authorized.",
    doc: { label: "Domain authorization", href: DOCS.domainAuthorization },
  },
  {
    id: "original-kept",
    title: "It was tried and the original was kept",
    text: "The result was not smaller, or the file was too large or unreadable. It is tried again later.",
    doc: { label: "Images not converting", href: DOCS.imagesNotConverting },
  },
];

/**
 * The usual reasons a URL is not optimized, each with its documentation.
 * A reason this console has evidence for comes first and carries it: a
 * cooldown means the URL was tried.
 */
export function urlReasons(cooldown: DaemonCooldownEntry | null): UrlReason[] {
  const reasons = REASONS.map((reason): UrlReason => ({
    ...reason,
    evidence: reason.id === "original-kept" && cooldown !== null ? cooldownText(cooldown) : null,
  }));
  return [...reasons.filter((r) => r.evidence !== null), ...reasons.filter((r) => r.evidence === null)];
}
