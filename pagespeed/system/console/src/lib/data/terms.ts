// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's glossary: one name per thing, used by every page. A page
 * that names one of these imports it from here instead of spelling it out,
 * so a figure reads the same on Overview, Savings, Graphs and Optimizer status.
 */
export const TERMS = {
  /** The module's figure: bytes its rewriting took off the responses it handled. */
  savedByRewriting: "Saved by rewriting",
  /** The optimizer's figure: transfer bytes saved on responses served from its cache. */
  savedByOptimizerCache: "Saved by the optimizer's cache",
  /** The share of requests answered with an optimized copy from the optimizer's cache. */
  servedOptimized: "Served optimized",
  /** The whole-server console's control that narrows pages to one host (`lens=` in the address). */
  hostFilter: "Host filter",
} as const;
