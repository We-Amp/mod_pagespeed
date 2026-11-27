// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

const ticks = [..."lazy"].map((ch, i) => ch.charCodeAt(0) + i);

export function lazySummary() {
  return `lazy:${ticks.reduce((a, b) => a ^ b, 0)}`;
}
