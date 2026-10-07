// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** The tab a key moves to inside a tablist: arrows move and wrap, Home/End jump; other keys: null. */
export function nextTabId(ids: readonly string[], current: string, key: string): string | null {
  const i = ids.indexOf(current);
  if (i < 0) return null;
  switch (key) {
    case "ArrowRight":
    case "ArrowDown":
      return ids[(i + 1) % ids.length];
    case "ArrowLeft":
    case "ArrowUp":
      return ids[(i - 1 + ids.length) % ids.length];
    case "Home":
      return ids[0];
    case "End":
      return ids[ids.length - 1];
    default:
      return null;
  }
}
