// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// The scan endpoint answers failures with short strings. This maps the known
// ones to site copy; the raw string is never shown.

export const SCAN_ERROR_FALLBACK = 'The scan did not complete.';

const RATE_LIMIT_COPY = 'Rate limit reached. Try again in a minute.';
const BUSY_COPY = 'The scanner is busy. Try again in a moment.';
export const SCAN_UNREACHABLE_COPY =
  'Could not reach the scanner service. Check the URL and try again.';

const RULES: [RegExp, string][] = [
  [/^rate limit/, RATE_LIMIT_COPY],
  [/^busy/, BUSY_COPY],
  [
    /can.?t be scanned|public http/,
    'That address cannot be scanned. It must be a public website address starting with http or https.',
  ],
  [/^(scan failed|internal)\b/, 'The scan did not complete. Try again in a moment.'],
  [/^forbidden\b/, 'The scan was refused.'],
  [/^missing\b/, 'No address was given.'],
];

/** Sentence-case copy for a scanner error string; unknown strings get a generic sentence. */
export function scanErrorCopy(raw: unknown): string {
  const text = typeof raw === 'string' ? raw.trim().toLowerCase() : '';
  for (const [re, copy] of RULES) if (re.test(text)) return copy;
  return SCAN_ERROR_FALLBACK;
}

/** Copy for a non-JSON answer from the site's API proxy, chosen by its HTTP status. */
export function proxyErrorText(status: number | undefined): string {
  if (status === 429) return RATE_LIMIT_COPY;
  if (status !== undefined && status >= 500) return BUSY_COPY;
  return SCAN_UNREACHABLE_COPY;
}
