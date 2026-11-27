// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Classifies what the gallery generator saw when it polled an example's
// "after" page, so the website can say honestly why an example has (or has
// not) a source diff. Pure functions: no network, unit-tested with fixtures.
//
// Statuses (stored per example as `status`):
//   settled      the body changed and held; `diff` shows the filter's work.
//   header       the body did not change, but the response gained a resource
//                hint header (Link: rel=preload / dns-prefetch / preconnect)
//                that the original did not carry; the hint is in the evidence.
//   beacon       the only change is the beacon instrumentation (the script
//                that reports from a real browser), whether it held or showed
//                on some polls only (the module instruments a share of
//                responses). The filter acts once a real browser has reported
//                back, which a fetch cannot do.
//   not-applied  the body never changed and no hint header appeared, for every
//                user agent tried, over the whole polling window: the filter
//                does not apply on this host. The evidence says what was tried.
//   unsettled    the body changed at some point but never held, and not only by
//                beacon instrumentation: the rewrite is still in progress; run
//                the generator again later.

export const STATUSES = [
  "settled",
  "header",
  "beacon",
  "not-applied",
  "unsettled",
];

const BEACON_MARKER = /\/mod_pagespeed_beacon\b|pagespeed\.CriticalImages\b/;

/** True when the HTML carries mod_pagespeed's beacon instrumentation. */
export function hasBeaconInstrumentation(html) {
  return BEACON_MARKER.test(html);
}

// Drops each script element whose text reports to the beacon. Scans for the
// tags case-insensitively instead of matching them with a regular expression,
// so end tags spelled with trailing spaces (`</script >`) are found too.
function withoutBeaconScripts(html) {
  const lower = html.toLowerCase();
  let out = "";
  let i = 0;
  for (;;) {
    const open = lower.indexOf("<script", i);
    if (open === -1) break;
    const close = lower.indexOf("</script", open);
    const gt = close === -1 ? -1 : lower.indexOf(">", close);
    const end = gt === -1 ? html.length : gt + 1;
    const block = html.slice(open, end);
    out += html.slice(i, open) + (BEACON_MARKER.test(block) ? "" : block);
    i = end;
  }
  return out + html.slice(i);
}

/**
 * Removes the beacon instrumentation the module injects: script blocks that
 * report to the beacon endpoint, and the per-image `onload` check and
 * `data-pagespeed-url-hash` attributes that go with them.
 */
export function stripBeaconInstrumentation(html) {
  return withoutBeaconScripts(html)
    .replace(/\s+onload="if\(window\.pagespeed\)[^"]*"/g, "")
    .replace(/\s+data-pagespeed-url-hash="[^"]*"/g, "");
}

// Whitespace and self-close spelling are not a filter's work.
function normalize(html) {
  return html
    .replace(/\s+/g, " ")
    .replace(/\s*\/?>/g, ">")
    .replace(/>\s+</g, "><")
    .trim();
}

/** True when `after` differs from `before` only by beacon instrumentation. */
export function isBeaconOnly(beforeHtml, afterHtml) {
  if (
    !hasBeaconInstrumentation(afterHtml) ||
    hasBeaconInstrumentation(beforeHtml)
  ) {
    return false;
  }
  return (
    normalize(stripBeaconInstrumentation(afterHtml)) ===
    normalize(stripBeaconInstrumentation(beforeHtml))
  );
}

const HINT_REL = /\brel\s*=\s*"?(preload|dns-prefetch|preconnect|prefetch)\b/i;

/** The resource-hint entries of a `Link` response header (lowercase keys). */
export function hintLinks(headers) {
  const link = headers?.link;
  if (!link) return [];
  // Split on commas that start a new `<url>` entry.
  return link
    .split(/,\s*(?=<)/)
    .map((s) => s.trim())
    .filter((s) => HINT_REL.test(s));
}

/**
 * What a status rests on; stored per example as `statusEvidence`.
 * @typedef {{
 *   userAgent: string,          // user agent of the stored after capture
 *   userAgentsTried: string[],
 *   requests: number,           // after-page polls made
 *   windowSeconds: number,      // time spent polling
 *   moduleHeader: string,       // X-Mod-Pagespeed on the after response
 *   bodyChanged: boolean,
 *   beaconInstrumented: boolean,
 *   hintHeader?: string,        // header status only
 * }} SettleEvidence
 */

/**
 * Decides an example's status from its before capture and the polls made of
 * its after page, in the order they were made.
 *
 * @param {{ body: string, headers?: Record<string, string> }} before
 * @param {Array<{
 *   userAgent: string,            // 'default' or 'browser'
 *   settled: boolean,             // the body differed and held
 *   everDiffered: boolean,        // the body differed on at least one poll
 *   body: string,                 // the last body seen
 *   changedBody?: string | null,  // the last body that differed from before
 *   headers?: Record<string, string>, // the last response's headers
 *   attempts: number,
 *   windowMs: number,             // time from first to last poll
 * }>} polls
 * @returns {{ status: string, settled: boolean, poll: number, body: string, evidence: SettleEvidence }}
 *   `body` is the after capture to store; `poll` is the poll it came from.
 */
export function classifySettle(before, polls) {
  if (!polls.length) throw new Error("classifySettle needs at least one poll");
  const beforeHints = new Set(hintLinks(before.headers));
  const evidence = (poll, extra) => ({
    userAgent: polls[poll].userAgent,
    userAgentsTried: [...new Set(polls.map((p) => p.userAgent))],
    requests: polls.reduce((n, p) => n + p.attempts, 0),
    windowSeconds: Math.round(polls.reduce((n, p) => n + p.windowMs, 0) / 1000),
    moduleHeader:
      polls[poll].headers?.["x-mod-pagespeed"] ||
      polls[poll].headers?.["x-page-speed"] ||
      "",
    bodyChanged: polls.some((p) => p.everDiffered),
    beaconInstrumented: polls.some(
      (p) =>
        hasBeaconInstrumentation(p.body) ||
        hasBeaconInstrumentation(p.changedBody ?? ""),
    ),
    ...extra,
  });

  // A body that changed and held. If the change is only the beacon script,
  // the filter is waiting for a real browser.
  const verdict = (status, i, body, extra) => ({
    status,
    settled: status === "settled",
    poll: i,
    body,
    evidence: evidence(i, extra),
  });
  for (let i = 0; i < polls.length; i++) {
    if (!polls[i].settled) continue;
    const status = isBeaconOnly(before.body, polls[i].body)
      ? "beacon"
      : "settled";
    return verdict(status, i, polls[i].body);
  }

  // A body that changed only by the beacon instrumentation, on some polls.
  for (let i = 0; i < polls.length; i++) {
    const changed = polls[i].changedBody;
    if (changed && isBeaconOnly(before.body, changed)) {
      return verdict("beacon", i, changed);
    }
  }

  // No body change, but a hint header the original did not have.
  for (let i = 0; i < polls.length; i++) {
    const added = hintLinks(polls[i].headers).filter(
      (h) => !beforeHints.has(h),
    );
    if (added.length) {
      return verdict("header", i, polls[i].body, {
        hintHeader: added.join(", "),
      });
    }
  }

  const last = polls.length - 1;
  if (polls.some((p) => p.everDiffered)) {
    return verdict("unsettled", last, polls[last].body);
  }
  return verdict("not-applied", last, polls[last].body);
}

/**
 * The status of an entry written before `status` existed, derived from its
 * stored fields only. The website can use the same rule as a fallback.
 */
export function legacyStatus(entry) {
  if (!entry || entry.error) return undefined;
  if (entry.status) return entry.status;
  if (!entry.settled) return "unsettled";
  const b = entry.before?.html ?? "";
  const a = entry.after?.html ?? "";
  return isBeaconOnly(b, a) ? "beacon" : "settled";
}

/**
 * The parts of the generated entries that count as a material change, as one
 * string. Measuring a live, async, cache-stateful backend is jittery: poll
 * counts and summed subresource bytes vary run to run even when nothing
 * meaningful changed. The stable, displayed signals are the status, the source
 * `diff` and the `requests` delta. If none changed, the generator leaves the
 * data file byte-identical, so the scheduled refresh only reports a real
 * change.
 *
 * A `beacon` entry is compared by status only: its capture carries a
 * per-response nonce in the beacon script, so its diff differs on every run.
 */
export function materialSignature(examples) {
  return JSON.stringify(
    Object.fromEntries(
      Object.entries(examples)
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([k, v]) => [
          k,
          v.error
            ? { error: v.error }
            : {
                status: v.status,
                settled: v.settled,
                requests: v.savings?.requests,
                diff: v.status === "beacon" ? undefined : v.diff,
              },
        ]),
    ),
  );
}
