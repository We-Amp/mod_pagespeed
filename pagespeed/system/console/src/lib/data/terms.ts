// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * The console's glossary: one name per thing, used by every page. A page
 * that names one of these imports it from here instead of spelling it out,
 * so a figure reads the same on Overview, Savings, Graphs and Optimizer status.
 */
export const TERMS = {
  /** The module's rewrite-time figure: bytes saved when a resource is optimized, counted once per resource. */
  savedByRewriting: "Saved at rewrite time",
  /** The delivered figure: original bytes minus bytes sent, over the optimized responses sent. */
  savedInDelivery: "Saved in delivery",
  /** The same figure where no family heading stands above it: the server's own rewriting only, never the optimizer cache's responses. */
  savedInDeliveryOwn: "Saved in delivery by this server",
  /** The optimizer's figure: transfer bytes saved on responses served from its cache. */
  savedByOptimizerCache: "Saved by the optimizer's cache",
  /** The share of requests answered with an optimized copy from the optimizer's cache. */
  servedOptimized: "Served optimized",
  /** The whole-server console's control that narrows pages to one host (`lens=` in the address). */
  hostFilter: "Host filter",
} as const;
