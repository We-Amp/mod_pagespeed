// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** `plural(1, 'host')` is "1 host"; `plural(2, 'host')` is "2 hosts". Pass `many` for irregular words. */
export function plural(n: number, one: string, many = one + 's'): string {
  return `${n} ${n === 1 ? one : many}`;
}
