// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { DEFAULT_PATH, parseHash } from "$lib/utils/hash-route";
import { redirectTarget } from "$lib/utils/redirects";

// The route table itself is plain data with no runes, kept in ./routes so it
// is unit-testable without a Svelte-aware transform; re-exported here so
// existing callers of this module see no difference.
export { NAV_GROUPS, routes, type NavGroup, type Route } from "./routes";
import { routes, type Route } from "./routes";

function getHash(): string {
  const raw = window.location.hash || DEFAULT_PATH;
  // A retired route: swap it for its new home in place, before any page
  // loads, so the page loads once and Back never returns to the alias.
  const target = redirectTarget(raw);
  if (target === null) return raw;
  history.replaceState(history.state, "", target);
  return target;
}

class Router {
  hash: string = $state(getHash());
  /**
   * Bumped on every hashchange. The shell rewrites the address in place to
   * carry the host lens, which leaves `hash` as it was read; a later change
   * to that same string sets an equal value, which nothing would see. A
   * reader that must see every navigation reads this instead.
   */
  navigations: number = $state(0);

  constructor() {
    window.addEventListener("hashchange", () => {
      this.hash = getHash();
      this.navigations += 1;
    });
  }

  /** The page part of the hash, without its "?query". */
  get path(): string {
    return parseHash(this.hash).path;
  }

  /** The hash's "?query" parameters (a page reads them when it mounts). */
  get params(): URLSearchParams {
    return parseHash(this.hash).params;
  }

  get currentRoute(): Route {
    // An unknown hash (stale bookmark, doc-link drift) must not leave the app
    // stuck on a permanent "Loading..." spinner. Fall back to the first route.
    return routes.find((r) => r.path === this.path) ?? routes[0];
  }

  navigate(path: string): void {
    window.location.hash = path;
  }
}

export const router = new Router();
