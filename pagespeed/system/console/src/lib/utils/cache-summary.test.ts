// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { flattenTree, nodeKindLabel, nodeLabel, parseBackendStats, parseCacheCohorts, parseCacheSummary } from "./cache-summary";

// Real summaries from a stock install.
const HTTP = "HTTPCache(Stats(prefix=file_cache,cache=CycloneCache))";
const METADATA =
  "Compressed(WriteThroughCache(l1=Stats(prefix=shm_cache,cache=SharedMemCache<64>),l2=Stats(prefix=file_cache_small,cache=CycloneCache(small_tier))))";
const PROPERTY =
  `beacon_cohort:Stats(prefix=pcache-cohorts-beacon_cohort,cache=${METADATA})\n` +
  `dom:Stats(prefix=pcache-cohorts-dom,cache=${METADATA})`;

const layers = (s: string) => flattenTree(parseCacheSummary(s));

describe("cache summaries", () => {
  it("the HTTP cache: a disk cache behind a statistics prefix", () => {
    const l = layers(HTTP);
    expect(l.map((x) => x.label)).toEqual(["HTTP Cache", "Cyclone Disk Cache"]);
    expect(l[1].prefix).toBe("file_cache");
  });

  it("the metadata cache: compression over a write-through pair, with L1/L2 roles and the tier as an argument", () => {
    const l = layers(METADATA);
    expect(l.map((x) => x.label)).toEqual([
      "Compression",
      "Write-Through (L1, then L2)",
      "Shared Memory (64-byte blocks)",
      "Cyclone Disk Cache (Small Tier)",
    ]);
    expect(l.map((x) => x.role)).toEqual(["", "", "l1", "l2"]);
  });

  it("the property cache: one pipeline per cohort", () => {
    const cohorts = parseCacheCohorts(PROPERTY)!;
    expect(cohorts.map((c) => c.name)).toEqual(["beacon_cohort", "dom"]);
    expect(cohorts[0].layers.map((x) => x.label)).toContain("Write-Through (L1, then L2)");
  });

  it("labels: known types in words, unknown types as themselves, arguments in parentheses", () => {
    expect(nodeLabel("SharedMemCache<64>")).toBe("Shared Memory (64-byte blocks)");
    expect(nodeLabel("SomethingNew")).toBe("SomethingNew");
    expect(nodeLabel("CycloneCache", ["small_tier"])).toBe("Cyclone Disk Cache (Small Tier)");
  });

  it("kinds: one word per cache layer, no pictures", () => {
    expect(nodeKindLabel("SharedMemCache<64>")).toBe("Memory");
    expect(nodeKindLabel("CycloneCache")).toBe("Disk");
    expect(nodeKindLabel("HTTPCache")).toBe("HTTP");
    expect(nodeKindLabel("SomethingNew")).toBe("Cache");
    // METADATA is Compressed(WriteThroughCache(SharedMemCache<64>, CycloneCache)):
    // WriteThroughCache is not in the kinds map, so it takes the generic
    // "Cache" — its full label ("Write-Through (L1, then L2)") says what it is.
    expect(layers(METADATA).map((x) => x.kind)).toEqual(["Compressed", "Cache", "Memory", "Disk"]);
  });

  it("backend statistics: key and value per line", () => {
    expect(parseBackendStats("Current entries: 51329\nCurrent size bytes: 83039256")).toEqual([
      { key: "Current entries", value: "51329" },
      { key: "Current size bytes", value: "83039256" },
    ]);
  });

  it("'none' and empty summaries have no layers", () => {
    expect(parseCacheSummary("none")).toBeNull();
    expect(parseCacheSummary("   ")).toBeNull();
  });
});
