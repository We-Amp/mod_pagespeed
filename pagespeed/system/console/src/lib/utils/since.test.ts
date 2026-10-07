// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { sinceLabel, WARMUP_MS } from "./since";

const NOW = Date.parse("2026-10-02T12:00:00Z");

describe("sinceLabel", () => {
  it("labels minutes, hours and days", () => {
    expect(sinceLabel(NOW - 30_000, NOW).text).toBe("since 1 min");
    expect(sinceLabel(NOW - 5 * 60_000, NOW).text).toBe("since 5 min");
    expect(sinceLabel(NOW - 90 * 60_000, NOW).text).toBe("since 1 h");
    expect(sinceLabel(NOW - 23 * 60 * 60_000, NOW).text).toBe("since 23 h");
    expect(sinceLabel(NOW - 3 * 86_400_000, NOW).text).toBe("since 3 d");
  });

  it("carries the ISO instant for a tooltip", () => {
    const label = sinceLabel(NOW - 5 * 60_000, NOW);
    expect(label.iso).toBe(new Date(NOW - 5 * 60_000).toISOString());
  });

  it("flags windows younger than the warm-up period", () => {
    expect(sinceLabel(NOW - WARMUP_MS + 1, NOW).warming).toBe(true);
    expect(sinceLabel(NOW - WARMUP_MS, NOW).warming).toBe(false);
    expect(sinceLabel(NOW - 3 * 86_400_000, NOW).warming).toBe(false);
  });

  it("says 'since restart' for absent, non-finite and future starts", () => {
    expect(sinceLabel(undefined, NOW)).toEqual({
      text: "since restart",
      iso: null,
      warming: false,
    });
    expect(sinceLabel(null, NOW).text).toBe("since restart");
    expect(sinceLabel(Number.NaN, NOW).text).toBe("since restart");
    // A start up to five minutes ahead (clock skew between hosts) still labels.
    expect(sinceLabel(NOW + 2_000, NOW).text).toBe("since 1 min");
    expect(sinceLabel(NOW + 60_000, NOW).text).toBe("since 1 min");
    expect(sinceLabel(NOW + 5 * 60_000, NOW).text).toBe("since 1 min");
    // Further ahead than that is not a believable start.
    expect(sinceLabel(NOW + 5 * 60_000 + 1, NOW).text).toBe("since restart");
    expect(sinceLabel(NOW + 6 * 60_000, NOW).text).toBe("since restart");
  });

  it("treats a start of zero or below as unknown, not as the epoch", () => {
    expect(sinceLabel(0, NOW)).toEqual({
      text: "since restart",
      iso: null,
      warming: false,
    });
    expect(sinceLabel(-5, NOW).text).toBe("since restart");
  });
});
