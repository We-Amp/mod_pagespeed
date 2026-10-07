// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { formatRelative } from "./format";

/** The "updated …" stamp next to a page's refresh controls. */
export function updatedText(updatedAt: number | null, now: number, autoRefresh: boolean): string {
  if (updatedAt === null) return "updated —";
  const rel = formatRelative(updatedAt, now);
  return autoRefresh ? `updated ${rel}` : `paused · updated ${rel}`;
}
