// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError } from "$lib/api/client";
import { foldStatus, optimizerSection, sampleStatus, statusView, type StatusSample } from "./optimizer-status";

const good = <T>(data: T) => ({ ok: true as const, data });
const bad = (status: number, text = "x") => ({ ok: false as const, error: new ApiError(status, text) });

function sample(over: Partial<StatusSample> = {}): StatusSample {
  return {
    health: good({ status: "ok" }),
    stats: good({ cache: { entries: 3 } }),
    cooldowns: good({ cooldowns: [] }),
    ...over,
  };
}

describe("sampleStatus", () => {
  it("reads the three leaves at once and never rejects", async () => {
    const s = await sampleStatus({
      daemonHealth: async () => ({ status: "ok" }),
      daemonStats: async () => {
        throw new ApiError(502, "daemon_unreachable");
      },
      daemonCooldowns: async () => ({ cooldowns: [] }),
    });
    expect(s.health).toEqual({ ok: true, data: { status: "ok" } });
    expect(s.stats.ok).toBe(false);
    expect(s.cooldowns.ok).toBe(true);
  });

  it("a rejection that is not an Error still settles as one", async () => {
    const s = await sampleStatus({
      daemonHealth: () => Promise.reject("nope"),
      daemonStats: async () => ({}),
      daemonCooldowns: async () => [],
    });
    expect(s.health.ok).toBe(false);
    expect(!s.health.ok && s.health.error instanceof Error).toBe(true);
  });
});

describe("foldStatus", () => {
  it("a busy read keeps that read's previous answer", () => {
    const prev = sample();
    const next = sample({ stats: bad(429) });
    expect(foldStatus(prev, next).stats).toEqual(prev.stats);
  });

  it("a busy health or statistics read with nothing to keep is thrown, so the poller retries soon", () => {
    expect(() => foldStatus(null, sample({ stats: bad(429) }))).toThrow(ApiError);
    expect(() => foldStatus(sample({ health: bad(502) }), sample({ health: bad(429) }))).toThrow(ApiError);
  });

  it("a busy cooldown read with nothing to keep is not thrown: the list is optional", () => {
    const folded = foldStatus(null, sample({ cooldowns: bad(429) }));
    expect(folded.cooldowns.ok).toBe(false);
  });

  it("any other failure replaces the previous answer", () => {
    const folded = foldStatus(sample(), sample({ stats: bad(502) }));
    expect(folded.stats.ok).toBe(false);
  });
});

describe("statusView", () => {
  it("nothing read yet: no data and no explanation", () => {
    const v = statusView(null);
    expect(v.health).toBeNull();
    expect(v.stats).toBeNull();
    expect(v.allUnavailable).toBe(false);
  });

  it("health and statistics both unavailable: one page-level explanation, from the statistics read", () => {
    const v = statusView(sample({ health: bad(404), stats: bad(502, "daemon_unreachable") }));
    expect(v.allUnavailable).toBe(true);
    expect(v.unavailableError?.message).toContain("502");
  });

  it("only one of them unavailable: each section explains its own", () => {
    const v = statusView(sample({ stats: bad(501) }));
    expect(v.allUnavailable).toBe(false);
    expect(v.health).toEqual({ status: "ok" });
    expect(v.statsError).toBeInstanceOf(ApiError);
  });

  it("a real error next to an unavailable read: no page-level block, each section shows its own", () => {
    const healthFails = statusView(sample({ health: bad(500, "boom"), stats: bad(503) }));
    expect(healthFails.allUnavailable).toBe(false);
    expect(healthFails.unavailableError).toBeNull();
    expect(healthFails.healthError?.message).toContain("500");
    expect(healthFails.statsError?.message).toContain("503");
    const statsFails = statusView(sample({ health: bad(502, "daemon_unreachable"), stats: bad(500, "boom") }));
    expect(statsFails.allUnavailable).toBe(false);
    expect(statsFails.statsError?.message).toContain("500");
  });

  it("two plain server errors are not 'unreachable'", () => {
    const v = statusView(sample({ health: bad(500), stats: bad(500) }));
    expect(v.allUnavailable).toBe(false);
  });

  it("an unreadable cooldown list is null, not an empty list", () => {
    expect(statusView(sample({ cooldowns: bad(501) })).cooldowns).toBeNull();
  });
});

describe("optimizerSection", () => {
  it("names only the three sections", () => {
    expect(optimizerSection("load")).toBe("load");
    expect(optimizerSection("cache")).toBe("cache");
    expect(optimizerSection("health")).toBe("health");
    expect(optimizerSection("Load")).toBeNull();
    expect(optimizerSection("__proto__")).toBeNull();
    expect(optimizerSection(null)).toBeNull();
  });
});
