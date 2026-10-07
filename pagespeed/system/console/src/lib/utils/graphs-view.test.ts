// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { ApiError } from "$lib/api/client";
import { graphsView, intervalLabel, isGraphsAbsent, perVhostConsoleHref } from "./graphs-view";

describe("graphsView", () => {
  it("an error body means the server has no graphs endpoint here", () => {
    expect(graphsView({ error: "console_logger must be enabled to use '?json'" })).toEqual({
      kind: "absent",
      detail: "console_logger must be enabled to use '?json'",
    });
    expect(graphsView("<html>")).toEqual({ kind: "absent", detail: "unexpected reply" });
  });
  it("{} means the statistics log has no samples at all yet", () => {
    expect(graphsView({})).toEqual({ kind: "no-log" });
  });
  it("a log with nothing in the range is an empty range", () => {
    expect(graphsView({ timestamps: [], variables: {} })).toEqual({ kind: "empty-range" });
    expect(graphsView({ timestamps: [1, 2], variables: { a: [] } })).toEqual({ kind: "empty-range" });
  });
  it("samples become series; a non-number becomes a gap, not a dropped slot", () => {
    expect(graphsView({ timestamps: [1000, 2000, 3000], variables: { a: [1, 2, 3], b: [3, "x", null] } })).toEqual({
      kind: "data",
      timestamps: [1000, 2000, 3000],
      graphs: [{ name: "a", data: [1, 2, 3] }, { name: "b", data: [3, null, null] }],
    });
    expect(graphsView({ timestamps: [1000], graphs: [{ name: "c", data: [5] }, { data: [1] }] })).toEqual({
      kind: "data",
      timestamps: [1000],
      graphs: [{ name: "c", data: [5] }],
    });
  });

  it("a non-numeric sample in the middle of a series does not shift the samples after it", () => {
    // Without a gap in its place, the "5" at index 2 would slide to index 1
    // and read against timestamps[1] (2000) instead of its own timestamps[2]
    // (3000) -- exactly the misalignment a dropped (rather than gapped)
    // sample would cause.
    const view = graphsView({
      timestamps: [1000, 2000, 3000, 4000],
      variables: { mixed: [1, "bad", 5, 8] },
    });
    expect(view).toEqual({
      kind: "data",
      timestamps: [1000, 2000, 3000, 4000],
      graphs: [{ name: "mixed", data: [1, null, 5, 8] }],
    });
    if (view.kind === "data") {
      expect(view.graphs[0].data).toHaveLength(view.timestamps.length);
    }
  });
});

describe("isGraphsAbsent", () => {
  it("404 and 501 mean no endpoint; anything else is a failure", () => {
    expect(isGraphsAbsent(new ApiError(404, "Unknown admin page: graphs"))).toBe(true);
    expect(isGraphsAbsent(new ApiError(501, "x"))).toBe(true);
    expect(isGraphsAbsent(new ApiError(500, "x"))).toBe(false);
    expect(isGraphsAbsent(new Error("x"))).toBe(false);
  });
});

describe("intervalLabel", () => {
  it("names the typical gap between samples", () => {
    expect(intervalLabel([0, 5000, 10000, 15000])).toBe("5 s");
    expect(intervalLabel([0, 60_000, 120_000])).toBe("1 min");
    expect(intervalLabel([0, 300_000, 600_000, 660_000])).toBe("5 min");
    expect(intervalLabel([0, 7_200_000])).toBe("2 h");
  });
  it("is unknown with fewer than two distinct samples", () => {
    expect(intervalLabel([])).toBeNull();
    expect(intervalLabel([1000])).toBeNull();
    expect(intervalLabel([1000, 1000])).toBeNull();
  });
});

describe("perVhostConsoleHref", () => {
  it("points the global console at this host's own console", () => {
    expect(perVhostConsoleHref("/pagespeed_global_admin")).toBe("/pagespeed_admin/#/graphs");
  });
  it("has no answer for a renamed admin path", () => {
    expect(perVhostConsoleHref("/secret-admin")).toBeNull();
    expect(perVhostConsoleHref("")).toBeNull();
  });
});
