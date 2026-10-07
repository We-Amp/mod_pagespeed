// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The Optimizer status page's one read: health, statistics and the cooldown
 * list, settled independently. Each section renders from its own read, so an
 * optimizer that answers one and not another still shows what it answered;
 * only when neither health nor statistics answers, and one of them says
 * "no optimizer here" (404/501/502/503), does the page show one explanation
 * for everything.
 */

import { isBusyAnswer } from "$lib/api/poller";
import type { DaemonCooldownsResponse, DaemonHealthResponse, DaemonStatsResponse } from "$lib/api/types";
import { isDaemonUnavailable } from "./daemon";

export type Settled<T> = { ok: true; data: T } | { ok: false; error: Error };

export interface StatusSample {
  health: Settled<DaemonHealthResponse>;
  stats: Settled<DaemonStatsResponse>;
  cooldowns: Settled<DaemonCooldownsResponse>;
}

export interface StatusApi {
  daemonHealth(): Promise<DaemonHealthResponse>;
  daemonStats(): Promise<DaemonStatsResponse>;
  daemonCooldowns(): Promise<DaemonCooldownsResponse>;
}

/** The page's sections, in page order; a section= link names one. */
export const OPTIMIZER_SECTIONS = ["health", "load", "cache"] as const;
export type OptimizerSection = (typeof OPTIMIZER_SECTIONS)[number];

export function optimizerSection(value: string | null): OptimizerSection | null {
  return value !== null && (OPTIMIZER_SECTIONS as readonly string[]).includes(value) ? (value as OptimizerSection) : null;
}

const asError = (reason: unknown): Error => (reason instanceof Error ? reason : new Error(String(reason)));

function settle<T>(r: PromiseSettledResult<T>): Settled<T> {
  return r.status === "fulfilled" ? { ok: true, data: r.value } : { ok: false, error: asError(r.reason) };
}

/** Reads the three leaves at once; never rejects. */
export async function sampleStatus(api: StatusApi): Promise<StatusSample> {
  const [health, stats, cooldowns] = await Promise.allSettled([
    api.daemonHealth(),
    api.daemonStats(),
    api.daemonCooldowns(),
  ]);
  return { health: settle(health), stats: settle(stats), cooldowns: settle(cooldowns) };
}

function keepOnBusy<T>(prev: Settled<T> | undefined, next: Settled<T>): Settled<T> {
  return !next.ok && isBusyAnswer(next.error) && prev?.ok === true ? prev : next;
}

/**
 * Folds a new sample onto the previous one: a busy (429) read -- another tab
 * holds the optimizer's one read for that endpoint -- keeps that read's
 * previous good answer. A busy health or statistics read with no previous
 * answer is thrown, so the poller's busy rule asks again within a second
 * instead of the section waiting a whole interval; the cooldown list is
 * optional and never thrown for.
 */
export function foldStatus(prev: StatusSample | null, next: StatusSample): StatusSample {
  const folded: StatusSample = {
    health: keepOnBusy(prev?.health, next.health),
    stats: keepOnBusy(prev?.stats, next.stats),
    cooldowns: keepOnBusy(prev?.cooldowns, next.cooldowns),
  };
  for (const read of [folded.health, folded.stats] as Settled<unknown>[]) {
    if (!read.ok && isBusyAnswer(read.error)) throw read.error;
  }
  return folded;
}

export interface StatusView {
  health: DaemonHealthResponse | null;
  healthError: Error | null;
  stats: DaemonStatsResponse | null;
  statsError: Error | null;
  /** null: the cooldown list could not be read (an older optimizer, say). */
  cooldowns: DaemonCooldownsResponse | null;
  /** Neither health nor statistics answered, and both say "no optimizer here". */
  allUnavailable: boolean;
  /** The failure the page-level explanation names (statistics first). */
  unavailableError: Error | null;
}

export function statusView(s: StatusSample | null): StatusView {
  if (s === null) {
    return { health: null, healthError: null, stats: null, statsError: null, cooldowns: null, allUnavailable: false, unavailableError: null };
  }
  const healthError = s.health.ok ? null : s.health.error;
  const statsError = s.stats.ok ? null : s.stats.error;
  const allUnavailable =
    healthError !== null && statsError !== null && isDaemonUnavailable(healthError) && isDaemonUnavailable(statsError);
  return {
    health: s.health.ok ? s.health.data : null,
    healthError,
    stats: s.stats.ok ? s.stats.data : null,
    statsError,
    cooldowns: s.cooldowns.ok ? s.cooldowns.data : null,
    allUnavailable,
    unavailableError: allUnavailable ? statsError : null,
  };
}
