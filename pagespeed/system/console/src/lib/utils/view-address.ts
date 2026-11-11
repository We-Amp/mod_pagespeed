// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * A control that changes the view of the page it is on (the source of the
 * Logs timeline, the pack of the Rule packs decision log) keeps the page
 * mounted and writes its choice into the address in place, so the address
 * can still be shared and a reload shows the same view. Following a link
 * or a shortcut stays a navigation, and a navigation always mounts the
 * page afresh from the address.
 */

import { DEFAULT_PATH } from "./hash-route";

/** The name of one raw "name=value" pair of a query. */
function pairName(pair: string): string | null {
  for (const name of new URLSearchParams(pair).keys()) return name;
  return null;
}

/**
 * `hash` with each named parameter set to its value, or removed for null.
 * A parameter that is already there is replaced where it stands (a repeated
 * one is kept once); a new one goes to the end. Every other parameter keeps
 * its bytes and its place, as it was written.
 */
export function withParams(hash: string, changes: Readonly<Record<string, string | null>>): string {
  const raw = hash && hash !== "#" ? hash : DEFAULT_PATH;
  const q = raw.indexOf("?");
  const path = q < 0 ? raw : raw.slice(0, q);
  const pairs = q < 0 ? [] : raw.slice(q + 1).split("&").filter((pair) => pair !== "");
  const placed = new Set<string>();
  const out: string[] = [];
  const encoded = (name: string, value: string) => new URLSearchParams([[name, value]]).toString();
  for (const pair of pairs) {
    const name = pairName(pair);
    if (name === null || !Object.prototype.hasOwnProperty.call(changes, name)) {
      out.push(pair);
      continue;
    }
    const value = changes[name];
    if (value === null || placed.has(name)) continue;
    out.push(encoded(name, value));
    placed.add(name);
  }
  for (const [name, value] of Object.entries(changes)) {
    if (value !== null && !placed.has(name)) out.push(encoded(name, value));
  }
  return out.length === 0 ? path : `${path}?${out.join("&")}`;
}

/**
 * Writes the parameters into the current address in place: no history
 * entry, no hashchange, so the page stays mounted with its state and the
 * keyboard focus where it is.
 */
export function replaceViewParams(changes: Readonly<Record<string, string | null>>): void {
  history.replaceState(history.state, "", withParams(window.location.hash || DEFAULT_PATH, changes));
}
