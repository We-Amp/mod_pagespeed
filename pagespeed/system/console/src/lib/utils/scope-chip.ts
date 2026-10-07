// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Whether a card shows its scope chip ("whole server" / "this host"). A
 * chip is shown only where the card's scope differs from what the console
 * as a whole covers, or could be misread:
 * - on the whole-server console, only while a host lens is active, and
 *   then only on cards whose figures do not follow it (they cover the
 *   whole server);
 * - on a per-host console, only on cards whose figures cover the whole
 *   server (the optimizer's).
 * A hidden chip's text stays in the card's accessible name.
 */
export function scopeChipShown(coversWholeServer: boolean, isGlobal: boolean, lensActive: boolean): boolean {
  return coversWholeServer && (!isGlobal || lensActive);
}
