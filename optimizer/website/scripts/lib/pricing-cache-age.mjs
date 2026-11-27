// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// pricing-cache-age.mjs — the age of the committed pricing file, as used by
// scripts/fetch-pricing.js for its staleness check and its messages.
//
// The age is measured from the data's own `fetchedAt` timestamp, which
// records when the prices were fetched. The file's mtime is not used: it
// records when a checkout or a copy wrote the file, so a fresh clone of
// months-old prices would look minutes old.

/**
 * Age of a pricing data object at `nowMs`.
 * Returns { hours, fetchedAt }, or null when `fetchedAt` is missing or not a
 * parseable date (the age is then unknown, and the data counts as stale).
 * A timestamp in the future (clock skew) counts as 0 hours old.
 */
export function pricingCacheAge(data, nowMs = Date.now()) {
  const fetchedAt = data && typeof data.fetchedAt === 'string' ? data.fetchedAt : null;
  const fetchedMs = fetchedAt === null ? NaN : Date.parse(fetchedAt);
  if (Number.isNaN(fetchedMs)) return null;
  return { hours: Math.max(0, nowMs - fetchedMs) / 3600000, fetchedAt };
}

/** True when the age is known and below the limit. */
export function isPricingCacheFresh(age, limitHours) {
  return age !== null && age.hours < limitHours;
}

/**
 * The age as the build messages print it, naming the timestamp it was
 * measured from, e.g. "fetched 2026-06-13T20:26:56.809Z, 2857h ago by its
 * fetchedAt timestamp; limit is 48h".
 */
export function describePricingCacheAge(age, limitHours) {
  if (age === null) {
    return `no valid fetchedAt timestamp, so its age is unknown; limit is ${limitHours}h`;
  }
  return (
    `fetched ${age.fetchedAt}, ${Math.round(age.hours)}h ago by its fetchedAt timestamp; ` +
    `limit is ${limitHours}h`
  );
}
