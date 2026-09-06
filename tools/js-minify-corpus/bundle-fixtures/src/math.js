// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

export function clamp(value, lo, hi) {
  if (Number.isNaN(value)) return lo;
  return Math.min(hi, Math.max(lo, value));
}

export function movingAverage(series, width) {
  const out = [];
  for (let i = 0; i < series.length; i += 1) {
    const lo = Math.max(0, i - width + 1);
    const window = series.slice(lo, i + 1);
    out.push(window.reduce((acc, v) => acc + v, 0) / window.length);
  }
  return out;
}
