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
import { WEBSITE, WEBSITE_HOST } from "$lib/data/product-facts-console";
import { cooldownReasonLabel, errorCode } from "./daemon";
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

// ── The module's half ────────────────────────────────────────────────

export type ModuleLookupKind = "current" | "not-current" | "original-url" | "other-host" | "no-answer";

export interface ModuleLookup {
  kind: ModuleLookupKind;
  /** One sentence for the operator. */
  text: string;
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
  if (/cache_ok:\s*true/.test(value)) return lookup("current");
  if (/cache_ok:\s*false/.test(value)) return lookup("not-current");
  if (/unable to decode resource/i.test(value)) return lookup("original-url");
  return lookup("no-answer");
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

/** "In cooldown (Processing), 42 s remaining." A missing reason or time is left out, never guessed. */
export function cooldownText(entry: DaemonCooldownEntry): string {
  const reason = cooldownReasonLabel(entry.reason);
  const remaining =
    typeof entry.remaining_seconds === "number" && Number.isFinite(entry.remaining_seconds)
      ? `, ${Math.max(0, Math.round(entry.remaining_seconds))} s remaining`
      : "";
  return `In cooldown${reason !== null ? ` (${reason})` : ""}${remaining}.`;
}

/**
 * What a page that does not show the optimizer's record says about it, in
 * one line: that there is no optimizer, so the module's answer is the whole
 * answer (either console), or what the whole-server console adds.
 */
export function wholeServerLine(noOptimizer: boolean): { text: string; link: boolean } {
  return noOptimizer
    ? { text: "No optimizer is connected to this server: the module's answer is the whole answer.", link: false }
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

const DOCS = `${WEBSITE}/docs`;

const REASONS: ReadonlyArray<Omit<UrlReason, "evidence">> = [
  {
    id: "not-requested",
    title: "Not requested yet",
    text: "It is optimized after a request for it has passed through this server. Request it, then refresh.",
    doc: {
      label: "Pages are never served optimized",
      href: `${DOCS}/troubleshooting/#pages-are-never-served-optimized--cache-never-warms`,
    },
  },
  {
    id: "not-cacheable",
    title: "The response is not cacheable",
    text: "A private, no-store or already expired response is left as it is.",
    doc: { label: "Set Cache-Control headers", href: `${DOCS}/cache-control/` },
  },
  {
    id: "host-not-authorized",
    title: "The host is not authorized",
    text: "A resource on another host than its page is rewritten only when that host is authorized.",
    doc: { label: "Domain authorization", href: `${DOCS}/domain-configuration/#domain-authorization` },
  },
  {
    id: "original-kept",
    title: "It was tried and the original was kept",
    text: "The result was not smaller, or the file was too large or unreadable. It is tried again later.",
    doc: { label: "Images not converting", href: `${DOCS}/troubleshooting/#images-not-converting-to-webpavif` },
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
