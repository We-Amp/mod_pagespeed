// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The host lens: the one host the whole-server console narrows its
 * per-host views to. Hosts come from visitor-controlled data (any client
 * chooses its Host header, and the optimizer's index and the logs record
 * it), so every host passes normalizeLensHost first -- the optimizer's own
 * host-name rule -- and hosts are only ever compared exactly.
 */

import { DEFAULT_PATH, parseHash } from "./hash-route";
import { linkSegments } from "./linkify";

/** The hash parameter that carries the lens (shareable links). */
export const LENS_PARAM = "lens";
/** Where the lens is remembered between visits: the prefix of each console's own key. */
export const LENS_STORAGE_KEY = "pagespeed.console.lens";

/**
 * The storage key of one console's lens. The whole-server console and each
 * per-host console share one origin's localStorage, so the key carries the
 * console's own path: a per-host console's link with lens= can never
 * overwrite what the whole-server console remembers.
 */
export function lensStorageKey(consolePath: string): string {
  return `${LENS_STORAGE_KEY}:${consolePath}`;
}
/** At most this many hosts are offered. */
export const MAX_LENS_HOSTS = 100;

/** The longest name the optimizer keeps, and the longest value it looks at (a name, ":" and a 5-digit port). */
const MAX_HOST_NAME = 127;
const MAX_HOST_INPUT = MAX_HOST_NAME + 7;
const PORT = /^[0-9]{1,5}$/;
const NAME = /^[a-z0-9._-]+$/;
const IPV6_LITERAL = /^[0-9a-f:.]+$/;
const LETTER_OR_DIGIT = /[a-z0-9]/;

/**
 * A host as the optimizer names it in its serve savings per host, or null
 * for a value the optimizer refuses -- the same rule, so every row the
 * optimizer reports can be chosen and a chosen host can match a row: ASCII
 * letters lowercased (nothing else); a ":port" of 1-5 digits dropped (after
 * the "]" of a bracketed IPv6 literal); one trailing "." dropped; then 1-127
 * characters of letters, digits, ".", "-" and "_" with at least one letter
 * or digit, or a bracketed IPv6 literal of hexadecimal digits, ":" and ".".
 */
export function normalizeLensHost(value: unknown): string | null {
  if (typeof value !== "string" || value.length === 0 || value.length > MAX_HOST_INPUT) return null;
  let v = value.replace(/[A-Z]/g, (c) => c.toLowerCase());
  if (v.startsWith("[")) {
    const close = v.indexOf("]");
    if (close <= 1) return null;
    const rest = v.slice(close + 1);
    if (rest !== "" && !(rest.startsWith(":") && PORT.test(rest.slice(1)))) return null;
    const inner = v.slice(1, close);
    if (!IPV6_LITERAL.test(inner) || !LETTER_OR_DIGIT.test(inner)) return null;
    v = v.slice(0, close + 1);
  } else {
    const colon = v.lastIndexOf(":");
    if (colon >= 0) {
      if (!PORT.test(v.slice(colon + 1))) return null;
      v = v.slice(0, colon);
    }
    if (v.endsWith(".")) v = v.slice(0, -1);
    if (v === "" || v.endsWith(".")) return null;
    if (!NAME.test(v) || !LETTER_OR_DIGIT.test(v)) return null;
  }
  return v.length > MAX_HOST_NAME ? null : v;
}

/** The lens= parameter of a hash route, or null when absent or malformed. */
export function lensFromParams(params: URLSearchParams): string | null {
  return normalizeLensHost(params.get(LENS_PARAM));
}

/**
 * How the whole-server console's lens follows the address and the scope.
 * A lens= in the address selects that host. The remembered lens is
 * restored once, the first time the console is known to be whole-server
 * (at start from the admin path, or later from the configuration's
 * answer for a renamed admin path), and only when the address carries no
 * lens= and none is selected. The address is the live one: the control
 * rewrites it in place, so a lens the viewer cleared is never read back
 * from an older copy.
 */
export class LensFollower {
  #restored: boolean;
  readonly #stored: () => string | null;

  /** `restored`: the remembered lens was already loaded at start. */
  constructor(restored: boolean, stored: () => string | null) {
    this.#restored = restored;
    this.#stored = stored;
  }

  /** On a change of address or scope: the host to select (null: all hosts), or undefined to leave the lens. */
  next(isGlobal: boolean, addressHash: string, selected: string | null): string | null | undefined {
    if (!isGlobal) return undefined;
    const linked = lensFromParams(parseHash(addressHash).params);
    const restoring = !this.#restored;
    this.#restored = true;
    if (linked !== null) return linked;
    if (!restoring || selected !== null) return undefined;
    return this.#stored() ?? undefined;
  }
}

/** Whether one raw "name=value" pair of a query is a lens= pair. */
function isLensPair(pair: string): boolean {
  for (const name of new URLSearchParams(pair).keys()) return name === LENS_PARAM;
  return false;
}

/**
 * `hash` with exactly one lens= set to `host` (where the first one stood,
 * else at the end), or none for null. Only the lens= pairs are touched:
 * every other parameter keeps its bytes and its place, as it was written.
 */
export function withLens(hash: string, host: string | null): string {
  const raw = hash && hash !== "#" ? hash : DEFAULT_PATH;
  const q = raw.indexOf("?");
  const path = q < 0 ? raw : raw.slice(0, q);
  const pairs = q < 0 || q === raw.length - 1 ? [] : raw.slice(q + 1).split("&");
  const lensPair = host === null ? null : new URLSearchParams([[LENS_PARAM, host]]).toString();
  const out: string[] = [];
  let placed = false;
  for (const pair of pairs) {
    if (!isLensPair(pair)) out.push(pair);
    else if (lensPair !== null && !placed) {
      out.push(lensPair);
      placed = true;
    }
  }
  if (lensPair !== null && !placed) out.push(lensPair);
  return out.length === 0 ? path : `${path}?${out.join("&")}`;
}

/**
 * The address the whole-server console shows for its active lens, or null
 * to leave the address as it is: with a host selected, an address whose
 * lens= is missing, malformed or names another host gets the selected host,
 * every other parameter kept as written (any spelling of the selected host
 * already counts as carrying it); an address with several lens= keeps one.
 * Never with no lens selected -- a cleared lens is never written back --
 * and never on a per-host console, which neither reads nor writes lens=.
 */
export function lensAddressRewrite(isGlobal: boolean, addressHash: string, selected: string | null): string | null {
  if (!isGlobal || selected === null) return null;
  const params = parseHash(addressHash).params;
  if (lensFromParams(params) === selected && params.getAll(LENS_PARAM).length === 1) return null;
  return withLens(addressHash, selected);
}

function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** The remembered lens; null when there is none, it is malformed, or the store cannot be read. */
export function loadLens(storage: Storage | null = defaultStorage(), key: string = LENS_STORAGE_KEY): string | null {
  if (storage === null) return null;
  try {
    return normalizeLensHost(storage.getItem(key));
  } catch {
    return null;
  }
}

/** Remembers the lens (null forgets it). A store that refuses leaves the lens for this session only. */
export function saveLens(
  host: string | null,
  storage: Storage | null = defaultStorage(),
  key: string = LENS_STORAGE_KEY,
): void {
  if (storage === null) return;
  try {
    if (host === null) storage.removeItem(key);
    else storage.setItem(key, host);
  } catch {
    // Blocked or full: the lens lasts this session.
  }
}

/** The hosts of the http(s) URLs a text names -- exactly the runs that become links -- once each. */
export function hostsInText(text: string): string[] {
  const out: string[] = [];
  for (const segment of linkSegments(text)) {
    if (segment.kind !== "link") continue;
    let host: string | null;
    try {
      host = normalizeLensHost(new URL(segment.href).hostname);
    } catch {
      host = null;
    }
    if (host !== null && !out.includes(host)) out.push(host);
  }
  return out;
}

/** Whether a text names `host` in one of its URLs: an exact match, never a suffix. */
export function textNamesHost(text: string, host: string): boolean {
  return hostsInText(text).includes(host);
}

/** Where a host was seen: 0 the optimizer served it, 1 the optimizer indexed it, 2 a log line named it. */
export type HostRank = 0 | 1 | 2;

/**
 * The hosts the lens offers: at most `cap`. A host from a better source
 * displaces one from a worse source at the cap, so hosts typed into log
 * lines cannot crowd out the hosts the server actually served.
 */
export class HostSet {
  readonly #ranks = new Map<string, HostRank>();

  constructor(readonly cap: number = MAX_LENS_HOSTS) {}

  /** Adds hosts (anything outside the grammar is ignored); true when the list changed. */
  add(hosts: Iterable<unknown>, rank: HostRank): boolean {
    let changed = false;
    for (const raw of hosts) {
      const host = normalizeLensHost(raw);
      if (host === null) continue;
      const known = this.#ranks.get(host);
      if (known !== undefined) {
        if (rank < known) this.#ranks.set(host, rank);
        continue;
      }
      if (this.#ranks.size >= this.cap) {
        let worst: string | null = null;
        let worstRank = -1;
        for (const [h, r] of this.#ranks) {
          if (r > worstRank) {
            worst = h;
            worstRank = r;
          }
        }
        if (worst === null || worstRank <= rank) continue;
        this.#ranks.delete(worst);
      }
      this.#ranks.set(host, rank);
      changed = true;
    }
    return changed;
  }

  list(): string[] {
    return [...this.#ranks.keys()].sort();
  }
}
