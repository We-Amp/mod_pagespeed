// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Every string the savings split shows, in one place. The labels for the
 * compression, not-yet-attributed and negative-note strings are the ruled
 * wording; the others are still placeholder wording.
 */
export const SPLIT_TEXT = {
  /** Shown instead of the split while too few of the served bytes can be split. */
  unavailable: "split available after cached entries refresh",
  /** Shown when the optimizer does not report the split at all (an older version). */
  notReported: "this optimizer does not report the split",
  unsplitLabel: "Not yet attributed",
  encodingLabel: "Saved by mod_pagespeed compression",
  rewriteLabel: "Saved by optimization",
  coverageNote: (percent: number): string => `split covers ${percent}% of served bytes`,
  /** Appended to a negative optimization figure. */
  negativeNote: "served bytes exceed the compressed original",
  /** The same, when the HTML class is a negative contributor to it. */
  negativeNoteMarkup: "served bytes exceed the compressed original; the page gained markup",
} as const;
