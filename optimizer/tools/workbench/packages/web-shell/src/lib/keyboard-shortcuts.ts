// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Keyboard shortcut definitions and handler for the PageSpeed web console.
 *
 * Shortcuts:
 *   Ctrl+K / Cmd+K  — Focus the URL input on the current page
 *   Escape          — Close any open modal, popover, or dropdown
 *   ?               — Toggle the keyboard shortcuts help tooltip
 */

export interface ShortcutEntry {
  /** Key combination displayed to the user (e.g. "Ctrl+K"). */
  label: string;
  /** macOS-specific label (e.g. "Cmd+K"). */
  macLabel: string;
  /** Description of the shortcut. */
  description: string;
}

/** All registered keyboard shortcuts. */
export const SHORTCUTS: ShortcutEntry[] = [
  {
    label: 'Ctrl+K',
    macLabel: '\u2318K',
    description: 'Focus URL input',
  },
  {
    label: 'Escape',
    macLabel: 'Escape',
    description: 'Close modal / popover',
  },
  {
    label: '?',
    macLabel: '?',
    description: 'Toggle shortcut help',
  },
];

/** Detect macOS for displaying the correct modifier key. */
export function isMac(): boolean {
  if (typeof navigator === 'undefined') return false;
  return navigator.platform?.toUpperCase().includes('MAC') ?? false;
}

/**
 * Returns the display label for a shortcut, choosing the macOS variant
 * when appropriate.
 */
export function shortcutLabel(entry: ShortcutEntry): string {
  return isMac() ? entry.macLabel : entry.label;
}
