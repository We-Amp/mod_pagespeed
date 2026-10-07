// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { routes } from "$lib/routes";
import { parseHash } from "./hash-route";
import { REDIRECTS, redirectTarget } from "./redirects";

describe("redirectTarget", () => {
  it("sends each retired optimizer page to its section of Optimizer status", () => {
    expect(redirectTarget("#/daemon/status")).toBe("#/optimizer");
    expect(redirectTarget("#/daemon/back-pressure")).toBe("#/optimizer?section=load");
    expect(redirectTarget("#/daemon/cache")).toBe("#/optimizer?section=cache");
    expect(redirectTarget("#/console")).toBe("#/statistics?delta=1");
    expect(redirectTarget("#/messages")).toBe("#/logs?source=module");
    expect(redirectTarget("#/messages?level=error")).toBe("#/logs?level=error&source=module");
  });

  it("keeps the old link's own parameters; the new home's win", () => {
    expect(redirectTarget("#/daemon/cache?x=1")).toBe("#/optimizer?x=1&section=cache");
    expect(redirectTarget("#/daemon/cache?section=health")).toBe("#/optimizer?section=cache");
  });

  it("leaves every real route and anything unknown alone", () => {
    for (const hash of ["#/optimizer", "#/optimizer?section=load", "#/overview", "#/logs?level=error", "#/graphs", "", "#", "#/nope", "#/daemon"]) {
      expect(redirectTarget(hash), hash).toBeNull();
    }
  });

  it("an inherited object key is not an alias", () => {
    expect(redirectTarget("#/__proto__")).toBeNull();
    expect(redirectTarget("constructor")).toBeNull();
  });

  it("every target is a route the console has, and no alias is one", () => {
    const paths = new Set(routes.map((r) => r.path));
    for (const [alias, target] of Object.entries(REDIRECTS)) {
      expect(paths.has(parseHash(target).path), target).toBe(true);
      expect(paths.has(alias), alias).toBe(false);
    }
  });
});
