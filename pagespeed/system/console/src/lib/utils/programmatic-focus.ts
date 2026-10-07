// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The shell moves focus to the new page's heading after a navigation, for
 * screen reader and keyboard continuity. That focus is the console's, not
 * the user's, so it draws no focus box -- whatever the browser's
 * focus-visible heuristic says about it. The mark lasts until the user next
 * presses a key or a pointer; from then on the ordinary :focus-visible ring
 * applies.
 */

export const PROGRAMMATIC_FOCUS_CLASS = "programmatic-focus";

/** The one pending clear: each mark replaces it, so listeners never pile up. */
let pending: (() => void) | null = null;

export function markProgrammaticFocus(
  el: HTMLElement,
  win: EventTarget = window,
): void {
  // A heading marked earlier and never cleared by input loses its mark now.
  pending?.();
  el.classList.add(PROGRAMMATIC_FOCUS_CLASS);
  // Capture phase, so a handler that stops the event cannot keep the mark.
  // The options object (not the boolean form) is what every EventTarget
  // implementation matches on removal.
  const capture = { capture: true };
  const clear = (): void => {
    el.classList.remove(PROGRAMMATIC_FOCUS_CLASS);
    win.removeEventListener("keydown", clear, capture);
    win.removeEventListener("pointerdown", clear, capture);
    if (pending === clear) pending = null;
  };
  win.addEventListener("keydown", clear, capture);
  win.addEventListener("pointerdown", clear, capture);
  pending = clear;
}
