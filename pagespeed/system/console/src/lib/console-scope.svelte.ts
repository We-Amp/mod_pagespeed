// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { resolveScope, type ConfigScope } from "$lib/utils/config-scope";
import { configDigest } from "$lib/utils/last-visit";

/**
 * Which scope this console shows. Starts from the admin path (the
 * conventional global path name) and is settled by the configuration's own
 * answer (`/config`: scope and host), which the shell reads once.
 */
export class ConsoleScope {
  isGlobal = $state(false);
  /** The host a per-vhost console names; "" on the global console. */
  host = $state("");
  /** The configuration's answer with a displayable host; null until it arrives, or if it fails. */
  config = $state<ConfigScope | null>(null);
  /** A digest of the configuration text from that same read (configDigest); null until it arrives. */
  configDigest = $state<string | null>(null);
  /**
   * Resolves once a configuration read has answered: the scope and the
   * digest are then the server's, not the admin path's guess. A failed read
   * leaves it pending until the shell's retry answers.
   */
  readonly configRead: Promise<void>;

  #resolveConfigRead: () => void = () => {};

  readonly #pathSaysGlobal: boolean;
  readonly #requestHost: string;

  constructor(pathSaysGlobal: boolean, requestHost: string) {
    this.#pathSaysGlobal = pathSaysGlobal;
    this.#requestHost = requestHost;
    const r = resolveScope(pathSaysGlobal, null, requestHost);
    this.isGlobal = r.isGlobal;
    this.host = r.host;
    this.configRead = new Promise((resolve) => {
      this.#resolveConfigRead = resolve;
    });
  }

  apply(cfg: ConfigScope, configText?: unknown): void {
    const r = resolveScope(this.#pathSaysGlobal, cfg, this.#requestHost);
    this.isGlobal = r.isGlobal;
    this.host = r.host;
    this.config = { scope: cfg.scope, host: r.host };
    if (typeof configText === "string") this.configDigest = configDigest(configText);
    this.#resolveConfigRead();
  }
}
