// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Every string the savings split shows, in one place. The split divides the
 * bytes saved by the optimizer's cache into what compression saved and what
 * optimization saved; the notes are whole sentences that name both parts.
 */
export const SPLIT_TEXT = {
  /** Shown instead of the split while too few of the served bytes can be split. */
  unavailable: "The split between compression and optimization appears once the cached entries have refreshed.",
  /** Shown when the optimizer does not report the split at all (an older version). */
  notReported: "This optimizer version does not split this figure into compression and optimization.",
  unsplitLabel: "Not yet attributed",
  encodingLabel: "Saved by mod_pagespeed compression",
  rewriteLabel: "Saved by optimization",
  coverageNote: (percent: number): string => `The split covers ${percent}% of the served bytes.`,
  /** Appended to a negative optimization figure. */
  negativeNote: "served bytes exceed the compressed original",
  /** The same, when the HTML class is a negative contributor to it. */
  negativeNoteMarkup: "served bytes exceed the compressed original; the page gained markup",
} as const;
