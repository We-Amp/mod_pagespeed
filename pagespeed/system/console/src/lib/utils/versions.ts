// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The version strings the console can honestly show. The module and the
 * console are built from one repository, so their stamps share a source;
 * the optimizer reports its own version and commit through health.
 */

import { availabilityText, daemonAvailability } from "./overview";

/** The friendly tag portion of a build stamp ("v1.16.0-3-gabc-dirty" →
 * "v1.16.0"); the whole stamp when it does not start with a tag. */
export function moduleTag(stamp: string): string {
  const match = /^(v\d+\.\d+\.\d+)/.exec(stamp.trim());
  return match ? match[1] : stamp.trim();
}

/** True when the build stamp carries uncommitted changes. */
export function isDirtyBuild(stamp: string): boolean {
  return stamp.trim().endsWith("-dirty");
}

export interface OptimizerVersions {
  version: string | null;
  commit: string | null;
}

/** The optimizer's version and commit from an untrusted health answer. */
export function optimizerVersions(health: unknown): OptimizerVersions {
  if (health === null || typeof health !== "object") {
    return { version: null, commit: null };
  }
  const h = health as Record<string, unknown>;
  const version = typeof h.version === "string" && h.version ? h.version : null;
  const commit =
    typeof h.git_commit === "string" && h.git_commit ? h.git_commit : null;
  return { version, commit };
}

/** The optimizer's version and commit, asked once with a single retry
 *  while it is busy; null fields when it cannot or will not say. */
export async function optimizerVersionsOnce(
  read: () => Promise<unknown>,
  wait: (ms: number) => Promise<void> = (ms) => new Promise((r) => setTimeout(r, ms)),
): Promise<OptimizerVersions> {
  try {
    return optimizerVersions(await read());
  } catch {
    try {
      await wait(1000);
      return optimizerVersions(await read());
    } catch {
      return { version: null, commit: null };
    }
  }
}

export interface OptimizerVersionsStatus extends OptimizerVersions {
  /** Why no version is shown: the optimizer's availability word ("Not
   *  configured", "Unreachable", …), or "Not available right now" when it
   *  stayed busy through the one retry; null when it answered. */
  unavailable: string | null;
}

/** The optimizer's versions for a page that asks once (About, the shortcut
 *  dialog): a busy or unanswered read (429, a network failure, a forwarded
 *  5xx) gets the one retry; a definite answer that there is no usable
 *  optimizer (not configured, unreachable, outdated) is reported at once. */
export async function optimizerVersionsStatus(
  read: () => Promise<unknown>,
  wait?: (ms: number) => Promise<void>,
): Promise<OptimizerVersionsStatus> {
  let answered = false;
  let word: string | null = null;
  const guarded = async (): Promise<unknown> => {
    try {
      const health = await read();
      answered = true;
      return health;
    } catch (err) {
      const availability = daemonAvailability(err instanceof Error ? err : new Error(String(err)));
      if (availability === "transient") throw err;
      word = availabilityText(availability).word;
      return null;
    }
  };
  const versions = await optimizerVersionsOnce(guarded, wait);
  return { ...versions, unavailable: answered ? null : (word ?? "Not available right now") };
}

/** One line for the Versions block: "Checking…" before the answer, the
 *  version with its commit, "Running (version not reported)", or why there
 *  is none. */
export function optimizerVersionText(status: OptimizerVersionsStatus | null): string {
  if (status === null) return "Checking…";
  if (status.unavailable !== null) return status.unavailable;
  if (status.version === null) return "Running (version not reported)";
  return status.commit === null ? status.version : `${status.version} (${status.commit})`;
}
