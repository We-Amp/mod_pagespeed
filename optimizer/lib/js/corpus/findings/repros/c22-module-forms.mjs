// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Module-only forms: top-level await, dynamic import, `import.meta`, and a
// regex statement after an export.
export const label = 'm';
export function check(s) { return /^_/.test(s); }
const mod = await Promise.resolve({ ok: true });
export const ok = mod?.ok ?? false;
export const here = import.meta.url;
const re = /a|b/;
export function pick(s) { return re.test(s); }
import('./c22-other.mjs').then(() => {}, () => {});
