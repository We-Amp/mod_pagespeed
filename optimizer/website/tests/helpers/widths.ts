// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Width list for the geometry oracle. GEOMETRY_WIDTHS is unset or "all" for the
// full sweep, or a comma-separated list such as "375" or "375,1280". The
// workflow passes the non-empty sentinel "all" on the nightly run because an
// empty string is falsy in a workflow expression (`cond && '' || 'x'` is 'x').
export const ALL_WIDTHS = [
  320, 360, 375, 390, 414, 640, 767, 768, 800, 928, 1023, 1024, 1279, 1280, 1440,
];

export function parseWidths(value: string | undefined): number[] {
  const v = (value ?? '').trim();
  if (v === '' || v === 'all') return ALL_WIDTHS;
  const widths = v.split(',').map((x) => Number(x.trim()));
  if (widths.some((w) => !Number.isInteger(w) || w <= 0)) {
    throw new Error(`GEOMETRY_WIDTHS: expected "all" or a comma-separated list, got "${v}"`);
  }
  return widths;
}
