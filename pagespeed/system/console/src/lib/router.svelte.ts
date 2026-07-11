import type { Component } from "svelte";

export interface Route {
  path: string;
  label: string;
  icon: string;
  component: () => Promise<{ default: Component }>;
}

export const routes: Route[] = [
  {
    path: "#/statistics",
    label: "Statistics",
    icon: "chart-bar",
    component: () => import("../pages/Statistics.svelte"),
  },
  {
    path: "#/configuration",
    label: "Configuration",
    icon: "cog",
    component: () => import("../pages/Config.svelte"),
  },
  {
    path: "#/histograms",
    label: "Histograms",
    icon: "chart-area",
    component: () => import("../pages/Histograms.svelte"),
  },
  {
    path: "#/caches",
    label: "Caches",
    icon: "database",
    component: () => import("../pages/Cache.svelte"),
  },
  {
    path: "#/console",
    label: "Console",
    icon: "terminal",
    component: () => import("../pages/Console.svelte"),
  },
  {
    path: "#/messages",
    label: "Messages",
    icon: "envelope",
    component: () => import("../pages/Messages.svelte"),
  },
  {
    path: "#/graphs",
    label: "Graphs",
    icon: "chart-line",
    component: () => import("../pages/Graphs.svelte"),
  },
  {
    path: "#/license",
    label: "License",
    icon: "key",
    component: () => import("../pages/License.svelte"),
  },
  {
    path: "#/about",
    label: "About",
    icon: "info-circle",
    component: () => import("../pages/About.svelte"),
  },
];

function getHash(): string {
  return window.location.hash || "#/statistics";
}

class Router {
  hash: string = $state(getHash());

  constructor() {
    window.addEventListener("hashchange", () => {
      this.hash = getHash();
    });
  }

  get currentRoute(): Route {
    // An unknown hash (stale bookmark, doc-link drift) must not leave the app
    // stuck on a permanent "Loading..." spinner. Fall back to the first route.
    return routes.find((r) => r.path === this.hash) ?? routes[0];
  }

  navigate(path: string): void {
    window.location.hash = path;
  }
}

export const router = new Router();
