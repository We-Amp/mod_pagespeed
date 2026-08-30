/**
 * The Support panel's dismissed state, remembered across page loads.
 * Same storage-failure tolerance as stat-table.ts: Safari's private mode and
 * "block all cookies" both make `localStorage` throw, and a storage failure
 * must never break the page.
 */

export const SUPPORT_DISMISSED_STORAGE_KEY = "pagespeed.support.dismissed";

/** Whether the operator dismissed the Support panel. Defaults to showing it. */
export function loadSupportDismissed(
  storage: Storage | null = defaultStorage(),
): boolean {
  try {
    return storage?.getItem(SUPPORT_DISMISSED_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/** Persist the dismissal. A storage failure must never break the page. */
export function saveSupportDismissed(
  dismissed: boolean,
  storage: Storage | null = defaultStorage(),
): void {
  try {
    storage?.setItem(SUPPORT_DISMISSED_STORAGE_KEY, dismissed ? "1" : "0");
  } catch {
    // Ignore: the toggle still works for this page view, it just won't persist.
  }
}

/** `localStorage` when the environment has one; merely touching it can throw. */
function defaultStorage(): Storage | null {
  try {
    return typeof localStorage === "undefined" ? null : localStorage;
  } catch {
    return null;
  }
}
