// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The route table: plain data, no runes and no `window`, so it is
// unit-testable without a Svelte-aware transform (router.svelte.ts, which
// does need one for its `$state`, re-exports these for its callers).

import type { Component } from "svelte";

export type NavGroup = "overview" | "module" | "optimizer" | "help";

export interface Route {
  path: string;
  label: string;
  /** The page's own title when it differs from its sidebar label (the tab title and the h1). */
  title?: string;
  icon: string;
  group: NavGroup;
  /** Not in the sidebar (a detail view reached from another page). */
  hidden?: boolean;
  /** The sidebar entry this page lives under (a hidden page's parent is marked current). */
  parent?: string;
  /** Sidebar entry only on the whole-server console (the page itself still
   * loads on a per-vhost console and explains the scope there). */
  globalOnly?: boolean;
  /** The page narrows itself to the host lens; on any other page the shell says the lens does not apply. */
  hostAware?: boolean;
  component: () => Promise<{ default: Component }>;
}

/** Navigation groups, in order; the overview stands alone above them. A muted group's heading is quieter. */
export const NAV_GROUPS: ReadonlyArray<{ id: NavGroup; label: string; muted?: boolean }> = [
  { id: "overview", label: "" },
  { id: "module", label: "Module" },
  { id: "optimizer", label: "Optimizer" },
  { id: "help", label: "Help", muted: true },
];

export const routes: Route[] = [
  {
    path: "#/overview",
    label: "Overview",
    icon: "home",
    group: "overview",
    component: () => import("../pages/Overview.svelte"),
  },
  {
    path: "#/savings",
    label: "Savings",
    icon: "piggy-bank",
    group: "overview",
    hostAware: true,
    component: () => import("../pages/Savings.svelte"),
  },
  {
    path: "#/urls",
    label: "URLs",
    icon: "link",
    group: "overview",
    globalOnly: true,
    hostAware: true,
    component: () => import("../pages/Urls.svelte"),
  },
  {
    path: "#/urls/detail",
    label: "URL Detail",
    icon: "link",
    group: "overview",
    hidden: true,
    globalOnly: true,
    parent: "#/urls",
    hostAware: true,
    component: () => import("../pages/UrlDetail.svelte"),
  },
  {
    path: "#/statistics",
    label: "Statistics",
    icon: "chart-bar",
    group: "module",
    component: () => import("../pages/Statistics.svelte"),
  },
  {
    path: "#/graphs",
    label: "Graphs",
    icon: "chart-line",
    group: "module",
    hidden: true,
    parent: "#/statistics",
    hostAware: true,
    component: () => import("../pages/Graphs.svelte"),
  },
  {
    path: "#/histograms",
    label: "Histograms",
    icon: "chart-area",
    group: "module",
    component: () => import("../pages/Histograms.svelte"),
  },
  {
    path: "#/caches",
    label: "Caches",
    icon: "database",
    group: "module",
    component: () => import("../pages/Cache.svelte"),
  },
  {
    path: "#/configuration",
    label: "Configuration",
    icon: "cog",
    group: "module",
    component: () => import("../pages/Config.svelte"),
  },
  {
    path: "#/optimizer",
    label: "Status",
    title: "Optimizer status",
    icon: "server",
    group: "optimizer",
    component: () => import("../pages/OptimizerStatus.svelte"),
  },
  {
    path: "#/logs",
    label: "Logs",
    icon: "scroll",
    group: "optimizer",
    hostAware: true,
    component: () => import("../pages/Logs.svelte"),
  },
  {
    path: "#/support",
    label: "Support",
    icon: "heart",
    group: "help",
    component: () => import("../pages/Support.svelte"),
  },
  {
    path: "#/about",
    label: "About",
    icon: "info-circle",
    group: "help",
    component: () => import("../pages/About.svelte"),
  },
];
