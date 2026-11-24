// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  DELIVERED_VARIABLES,
  deliveredFamilies,
  deliveredFamily,
  deliveredWindow,
  growthOver,
  requestsAvoided,
  sizesText,
  smallerText,
  windowLine,
} from "./delivered";
import { graphsView } from "./graphs-view";

const MODULE = {
  image_served_original_bytes: 4_000_000,
  image_served_optimized_bytes: 1_000_000,
  css_served_original_bytes: 200_000,
  css_served_optimized_bytes: 150_000,
  javascript_served_original_bytes: 1_000_000,
  javascript_served_optimized_bytes: 600_000,
};
const OPTIMIZER_CACHE = {
  ipro_daemon_served_image_original_bytes: 900_000,
  ipro_daemon_served_image_optimized_bytes: 300_000,
  ipro_daemon_served_css_original_bytes: 50_000,
  ipro_daemon_served_css_optimized_bytes: 40_000,
  ipro_daemon_served_js_original_bytes: 0,
  ipro_daemon_served_js_optimized_bytes: 0,
};
const ZEROS = Object.fromEntries(DELIVERED_VARIABLES.map((name) => [name, 0]));

describe("deliveredFamilies", () => {
  it("reads the twelve variables, six per family", () => {
    expect(DELIVERED_VARIABLES).toHaveLength(12);
    expect(new Set(DELIVERED_VARIABLES).size).toBe(12);
    expect(DELIVERED_VARIABLES).toContain("javascript_served_optimized_bytes");
    expect(DELIVERED_VARIABLES).toContain("ipro_daemon_served_js_original_bytes");
  });

  it("gives original, sent, saved and the share per type, for this server's own rewriting", () => {
    const [module] = deliveredFamilies({ ...MODULE, ...OPTIMIZER_CACHE });
    expect(module).toMatchObject({ key: "module", label: "Optimized by this server", reported: true, anyLarger: false });
    expect(module.rows.map((r) => r.label)).toEqual(["CSS", "JavaScript", "Images"]);
    expect(module.rows[0]).toMatchObject({ original: 200_000, sent: 150_000, saved: 50_000, percent: 25, direction: "smaller" });
    expect(module.rows[1]).toMatchObject({ original: 1_000_000, sent: 600_000, saved: 400_000, percent: 40 });
    expect(module.rows[2]).toMatchObject({ original: 4_000_000, sent: 1_000_000, saved: 3_000_000, percent: 75 });
    expect(module.total).toMatchObject({ original: 5_200_000, sent: 1_750_000, saved: 3_450_000, counted: true });
    expect(smallerText(module.total)).toBe("66% smaller");
    expect(sizesText(module.total)).toBe("4.96 MB → 1.67 MB");
  });

  it("keeps the optimizer cache's responses in a family of their own, never added to the first", () => {
    const [module, cache] = deliveredFamilies({ ...MODULE, ...OPTIMIZER_CACHE });
    expect(cache).toMatchObject({ key: "optimizer-cache", label: "Answered from the optimizer cache", reported: true });
    expect(cache.total).toMatchObject({ original: 950_000, sent: 340_000, saved: 610_000 });
    // Neither total holds the other's bytes.
    expect(module.total.original + cache.total.original).toBe(6_150_000);
    expect(module.total.original).toBe(5_200_000);
    // A type of a family that sent something, with nothing sent itself, has no percentage.
    expect(cache.rows[1]).toMatchObject({ key: "js", counted: false, percent: null, direction: null });
    expect(smallerText(cache.rows[1])).toBe("—");
  });

  it("right after a restart: counters at zero are nothing sent yet, not 0% saved", () => {
    for (const family of deliveredFamilies(ZEROS)) {
      expect(family.reported).toBe(true);
      expect(family.total).toMatchObject({ original: 0, sent: 0, saved: 0, percent: null, direction: null, counted: false });
      expect(smallerText(family.total)).toBe("—");
      for (const row of family.rows) {
        expect(row.counted).toBe(false);
        expect(row.percent).toBeNull();
        expect(smallerText(row)).toBe("—");
        expect(smallerText(row)).not.toContain("0%");
      }
    }
  });

  it("an older module that reports none of the variables is not reported, and is not a row of zeros", () => {
    const [module, cache] = deliveredFamilies({ css_filter_total_bytes_saved: 12 });
    expect(module.reported).toBe(false);
    expect(cache.reported).toBe(false);
    expect(module.total.counted).toBe(false);
    // One family present, the other absent (a module between the two additions).
    const [m2, c2] = deliveredFamilies(MODULE);
    expect(m2.reported).toBe(true);
    expect(c2.reported).toBe(false);
  });

  it("a negative saving stays negative: optimized responses larger than their originals", () => {
    const family = deliveredFamily(
      { ...MODULE, javascript_served_original_bytes: 1000, javascript_served_optimized_bytes: 1040 },
      "module",
    );
    const js = family.rows[1];
    expect(js).toMatchObject({ saved: -40, percent: 4, direction: "larger", counted: true });
    expect(smallerText(js)).toBe("4% larger");
    expect(family.anyLarger).toBe(true);
    // The other types still count for what they saved; the total is the signed sum.
    expect(family.total.saved).toBe(50_000 + 3_000_000 - 40);
    expect(family.total.direction).toBe("smaller");
  });

  it("a family that is larger overall has a negative total, and the share is bounded", () => {
    const family = deliveredFamily({ css_served_original_bytes: 100, css_served_optimized_bytes: 350 }, "module");
    expect(family.total).toMatchObject({ saved: -250, percent: 100, percentOver: true, direction: "larger" });
    expect(smallerText(family.total)).toBe("more than 100% larger");
    expect(deliveredFamily({ css_served_original_bytes: 100, css_served_optimized_bytes: 200 }, "module").total).toMatchObject({
      percent: 100,
      percentOver: false,
    });
  });

  it("a real saving under half a percent reads <1%, and no change reads same size", () => {
    const tiny = deliveredFamily({ image_served_original_bytes: 100_000, image_served_optimized_bytes: 99_900 }, "module");
    expect(smallerText(tiny.rows[2])).toBe("<1% smaller");
    const same = deliveredFamily({ image_served_original_bytes: 500, image_served_optimized_bytes: 500 }, "module");
    expect(same.rows[2]).toMatchObject({ saved: 0, direction: "same", counted: true });
    expect(smallerText(same.rows[2])).toBe("same size");
  });

  it("never divides by a zero original: sent bytes without an original have no percentage", () => {
    const odd = deliveredFamily({ css_served_original_bytes: 0, css_served_optimized_bytes: 70 }, "module");
    expect(odd.rows[0]).toMatchObject({ saved: -70, percent: null, direction: "larger", counted: true });
    expect(smallerText(odd.rows[0])).toBe("—");
  });

  it("reads untrusted values defensively: negative, non-finite and non-numeric count as zero", () => {
    const family = deliveredFamily(
      {
        css_served_original_bytes: -5,
        css_served_optimized_bytes: Number.NaN,
        image_served_original_bytes: "many",
        image_served_optimized_bytes: Number.POSITIVE_INFINITY,
      },
      "module",
    );
    expect(family.total).toMatchObject({ original: 0, sent: 0, counted: false });
    expect(deliveredFamilies(null).every((f) => !f.reported)).toBe(true);
  });
});

describe("growthOver", () => {
  it("is the last sample minus the first for a counter that only grew", () => {
    expect(growthOver([10, 25, 25, 60])).toBe(50);
  });
  it("counts on from zero after a restart, so the history spans it", () => {
    // 100 -> 400 (+300), restart, 0 -> 50 (+50), -> 90 (+40).
    expect(growthOver([100, 400, 50, 90])).toBe(300 + 50 + 40);
  });
  it("skips gaps and unusable samples", () => {
    expect(growthOver([null, 5, null, 9, Number.NaN, 12])).toBe(7);
    expect(growthOver([])).toBe(0);
    expect(growthOver([42])).toBe(0);
  });
});

describe("deliveredWindow", () => {
  const HOUR = 3_600_000;
  const T0 = 1_790_000_000_000;

  it("says nothing without a graphs endpoint, and points at logging without a log", () => {
    expect(deliveredWindow(null)).toEqual({ kind: "unavailable" });
    expect(deliveredWindow(graphsView({ error: "no graphs here" }))).toEqual({ kind: "unavailable" });
    expect(deliveredWindow(graphsView({}))).toEqual({ kind: "no-log" });
    expect(deliveredWindow(graphsView({ timestamps: [], variables: {} }))).toEqual({ kind: "no-log" });
  });

  it("states the span the log really covers, and survives a restart inside it", () => {
    const w = deliveredWindow(
      graphsView({
        timestamps: [T0, T0 + 3 * HOUR, T0 + 6 * HOUR, T0 + 9 * HOUR],
        variables: {
          image_served_original_bytes: [1_000_000, 3_000_000, 200_000, 1_200_000],
          image_served_optimized_bytes: [250_000, 750_000, 50_000, 300_000],
          css_served_original_bytes: [0, 0, 0, 0],
          css_served_optimized_bytes: [0, 0, 0, 0],
          num_flushes: [1, 2, 3, 4],
        },
      }),
    );
    expect(w.kind).toBe("data");
    if (w.kind !== "data") return;
    expect(w.spanText).toBe("9 h");
    expect(w.fromMs).toBe(T0);
    expect(w.toMs).toBe(T0 + 9 * HOUR);
    const [module, cache] = w.families;
    // +2,000,000 before the restart, +200,000 and +1,000,000 after it.
    expect(module.rows[2]).toMatchObject({ original: 3_200_000, sent: 800_000, percent: 75 });
    expect(module.rows[0].counted).toBe(false);
    expect(cache.total.counted).toBe(false);
    expect(windowLine(module)).toBe("Images 75% smaller (3.05 MB → 781 KB)");
  });

  it("is quiet for a log with one sample or without growth in these variables", () => {
    expect(deliveredWindow(graphsView({ timestamps: [T0], variables: { image_served_original_bytes: [5] } }))).toEqual({
      kind: "quiet",
    });
    expect(
      deliveredWindow(
        graphsView({
          timestamps: [T0, T0 + HOUR],
          variables: { image_served_original_bytes: [700, 700], image_served_optimized_bytes: [300, 300] },
        }),
      ),
    ).toEqual({ kind: "quiet" });
    // A log written by a version without these variables.
    expect(deliveredWindow(graphsView({ timestamps: [T0, T0 + HOUR], variables: { num_flushes: [1, 9] } }))).toEqual({
      kind: "quiet",
    });
  });
});

describe("requestsAvoided", () => {
  it("lists the non-zero inlining and combining counters, singular and plural", () => {
    expect(
      requestsAvoided({ image_inline: 1204, num_css_inlined: 1, num_js_inlined: 0, css_file_count_reduction: 37 }),
    ).toEqual([
      { counter: "image_inline", count: 1204, text: "images inlined" },
      { counter: "num_css_inlined", count: 1, text: "stylesheet inlined" },
      { counter: "css_file_count_reduction", count: 37, text: "stylesheet requests combined away" },
    ]);
    expect(requestsAvoided({})).toEqual([]);
  });
});
