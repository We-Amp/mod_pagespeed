// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Hash routes the console has retired, and where each one lives now. An old
 * bookmark, a documentation link or a finding written before the move lands
 * on the new home; the router replaces the alias in the browser history, so
 * Back never returns to it (router.svelte.ts).
 */

import { parseHash } from "./hash-route";

export const REDIRECTS: Readonly<Record<string, string>> = {
  "#/console": "#/statistics?delta=1",
  "#/messages": "#/logs?source=module",
  "#/daemon/status": "#/optimizer",
  "#/daemon/back-pressure": "#/optimizer?section=load",
  "#/daemon/cache": "#/optimizer?section=cache",
};

/**
 * The new home of a retired route, keeping the old link's own parameters
 * (where both name one, the new home's wins); null for anything that is not
 * an alias.
 */
export function redirectTarget(hash: string): string | null {
  const { path, params } = parseHash(hash);
  if (!Object.prototype.hasOwnProperty.call(REDIRECTS, path)) return null;
  const target = parseHash(REDIRECTS[path]);
  const merged = new URLSearchParams(params);
  for (const [name, value] of target.params) merged.set(name, value);
  const query = merged.toString();
  return query === "" ? target.path : `${target.path}?${query}`;
}
