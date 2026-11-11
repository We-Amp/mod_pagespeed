// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Whether the pages refresh by themselves. Pausing is a choice the viewer
 * makes for this browser: it holds on every page and after a reload, until
 * it is turned back on. A fresh browser refreshes automatically. Storage
 * that is missing or throws (private modes) only means the choice is not
 * remembered.
 */

export const AUTO_REFRESH_STORAGE_KEY = "pagespeed.console.autoRefresh";

const listeners = new Set<(on: boolean) => void>();

function storage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}

/** True unless the viewer paused auto-refresh in this browser. */
export function autoRefreshOn(store: Storage | null = storage()): boolean {
  try {
    return store?.getItem(AUTO_REFRESH_STORAGE_KEY) !== "off";
  } catch {
    return true;
  }
}

/** Remembers the choice and tells every poller on screen. */
export function setAutoRefresh(on: boolean, store: Storage | null = storage()): void {
  try {
    if (on) store?.removeItem(AUTO_REFRESH_STORAGE_KEY);
    else store?.setItem(AUTO_REFRESH_STORAGE_KEY, "off");
  } catch {
    // Not remembered; this page view still follows the choice.
  }
  for (const fn of listeners) fn(on);
}

/** Calls `fn` on every change of the choice; returns an unsubscribe. */
export function onAutoRefreshChange(fn: (on: boolean) => void): () => void {
  listeners.add(fn);
  return () => {
    listeners.delete(fn);
  };
}
