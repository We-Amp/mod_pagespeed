// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/** The scope-carrying fields of a `/config` response. */
export interface ConfigScope {
  scope?: "global" | "vhost";
  host?: string;
}

/** A one-line description of which configuration scope is being shown. */
export function scopeLine(r: ConfigScope): string {
  if (r.scope === "vhost") {
    return `Configuration of this virtual host (${r.host ?? "unknown host"})`;
  }
  if (r.scope === "global") {
    return "Server-wide configuration (all virtual hosts)";
  }
  return "Configuration";
}

/** A one-line description of which statistics scope is being shown. */
export function statisticsScopeLine(isGlobal: boolean, host = ""): string {
  if (isGlobal) {
    return "Aggregate statistics across all virtual hosts (process-wide)";
  }
  const named = host ? ` (${host})` : "";
  return (
    `Statistics of this virtual host${named} — separate from other hosts only ` +
    "when per-virtual-host statistics are enabled"
  );
}

/**
 * The host to name for a per-vhost console. `/config` reports the server
 * context's "hostname:port"; the main-server context of a stock install has
 * no name (":0"), so the host the browser used stands in. A zero port is
 * not shown.
 */
export function displayHost(host: string | undefined, requestHost: string): string {
  // The daemon proxy's /config is untrusted: a non-string host (despite the
  // declared type) is treated the same as an absent one.
  const h = typeof host === "string" ? host.trim() : "";
  const colon = h.lastIndexOf(":");
  const bracket = h.lastIndexOf("]");
  const hasPort = colon > bracket;
  const name = hasPort ? h.slice(0, colon) : h;
  const port = hasPort ? h.slice(colon + 1) : "";
  if (name === "") return requestHost;
  return port === "" || port === "0" ? name : `${name}:${port}`;
}

/** The scope a console shows: the configuration's answer, else the admin path. */
export function resolveScope(
  pathSaysGlobal: boolean,
  cfg: ConfigScope | null,
  requestHost: string,
): { isGlobal: boolean; host: string } {
  const isGlobal = cfg?.scope !== undefined ? cfg.scope === "global" : pathSaysGlobal;
  return { isGlobal, host: isGlobal ? "" : displayHost(cfg?.host, requestHost) };
}

/**
 * Whether the console has actually settled, via `/config`'s own answer, on
 * serving a per-vhost scope -- not merely guessed from the admin path. The
 * admin-path heuristic (ConsoleScope's initial `isGlobal`) defaults to
 * "per-vhost" for any path that is not the conventional global one, which is
 * wrong for a renamed GlobalAdminPath until `/config` corrects it; a page
 * that wants to skip a whole-server-only request before even asking must
 * wait for that correction, not race it.
 */
export function knownPerVhostScope(cfg: ConfigScope | null, isGlobal: boolean): boolean {
  return cfg !== null && !isGlobal;
}
