// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  cacheHitRate,
  daemonSavedRaw,
  MODULE_SAVINGS_FAMILIES,
  moduleSavedRaw,
  moduleSavingsByType,
  moduleSavingsTotal,
  notYetOptimized,
  optimizedCopyHitRate,
  savingsSplit,
  splitSummaryText,
} from "./savings";
import { serveSavingsView } from "./serve-savings";

const GLOBAL = {
  css_filter_total_bytes_saved: 499345,
  css_filter_total_original_bytes: 1063346,
  javascript_total_bytes_saved: 12087254,
  javascript_total_original_bytes: 25498944,
  image_rewrite_total_bytes_saved: 303593,
  image_rewrite_total_original_bytes: 1116562,
  ipro_daemon_served: 3122,
  ipro_daemon_fallthrough: 8884,
};

describe("moduleSavingsByType", () => {
  it("breaks savings down per content type", () => {
    const rows = moduleSavingsByType(GLOBAL);
    expect(rows.map((r) => r.label)).toEqual(["CSS", "JavaScript", "Images"]);
    expect(rows[0]).toMatchObject({ saved: 499345, original: 1063346, percent: 47, regressed: false });
    expect(rows[1]).toMatchObject({ saved: 12087254, original: 25498944, percent: 47 });
    expect(rows[2]).toMatchObject({ saved: 303593, original: 1116562, percent: 27 });
  });

  it("clamps a negative saving to zero and flags the regression", () => {
    const rows = moduleSavingsByType({ ...GLOBAL, css_filter_total_bytes_saved: -2048 });
    const css = rows[0];
    expect(css.saved).toBe(0);
    expect(css.rawSaved).toBe(-2048);
    expect(css.regressed).toBe(true);
    expect(css.percent).toBe(0);
  });

  it("reports a null percent when there is no original to compare against", () => {
    const rows = moduleSavingsByType({});
    for (const row of rows) {
      expect(row.saved).toBe(0);
      expect(row.original).toBe(0);
      expect(row.percent).toBeNull();
      expect(row.regressed).toBe(false);
    }
  });

  it("survives garbage counters without producing NaN", () => {
    const junk = {
      css_filter_total_bytes_saved: "lots",
      css_filter_total_original_bytes: [],
      javascript_total_bytes_saved: Number.NaN,
      javascript_total_original_bytes: -50,
      image_rewrite_total_bytes_saved: { big: true },
      image_rewrite_total_original_bytes: undefined,
    };
    const rows = moduleSavingsByType(junk);
    for (const row of rows) {
      expect(Number.isFinite(row.saved)).toBe(true);
      expect(Number.isFinite(row.original)).toBe(true);
      expect(row.saved).toBeGreaterThanOrEqual(0);
      expect(row.original).toBeGreaterThanOrEqual(0);
      if (row.percent !== null) {
        expect(row.percent).toBeGreaterThanOrEqual(0);
        expect(row.percent).toBeLessThanOrEqual(100);
      }
    }
  });
});

describe("moduleSavingsTotal", () => {
  it("sums the families and computes the headline percent", () => {
    const total = moduleSavingsTotal(moduleSavingsByType(GLOBAL));
    expect(total.saved).toBe(12890192);
    expect(total.original).toBe(27678852);
    expect(total.percent).toBe(47);
    expect(total.regressed).toBe(false);
  });

  it("carries the regression flag from any family", () => {
    const rows = moduleSavingsByType({ ...GLOBAL, image_rewrite_total_bytes_saved: -1 });
    expect(moduleSavingsTotal(rows).regressed).toBe(true);
  });

  it("is null-percent when nothing has an original", () => {
    expect(moduleSavingsTotal(moduleSavingsByType({})).percent).toBeNull();
  });
});

describe("moduleSavedRaw", () => {
  it("equals the headline total", () => {
    expect(moduleSavedRaw(GLOBAL)).toBe(12890192);
  });

  it("never goes negative", () => {
    expect(
      moduleSavedRaw({
        css_filter_total_bytes_saved: -100,
        javascript_total_bytes_saved: -100,
        image_rewrite_total_bytes_saved: -100,
      }),
    ).toBe(0);
  });
});

describe("cacheHitRate", () => {
  it("is the share of in-place requests served from the optimizer cache", () => {
    const rate = cacheHitRate(GLOBAL);
    expect(rate.served).toBe(3122);
    expect(rate.total).toBe(12006);
    expect(rate.percent).toBe(26);
  });

  it("rounds to 0 rather than hiding a small rate", () => {
    expect(cacheHitRate({ ipro_daemon_served: 7, ipro_daemon_fallthrough: 1644 }).percent).toBe(0);
  });

  it("is null before any in-place request", () => {
    expect(cacheHitRate({}).percent).toBeNull();
  });

  it("clamps garbage to a non-negative count", () => {
    const rate = cacheHitRate({ ipro_daemon_served: -3, ipro_daemon_fallthrough: "x" });
    expect(rate.served).toBe(0);
    expect(rate.total).toBe(0);
    expect(rate.percent).toBeNull();
  });
});

describe("daemonSavedRaw", () => {
  it("sums original minus served over the served classes", () => {
    const block = {
      css: { hits: 197, optimized_bytes: 21447548, original_bytes: 21472396 },
      image: { hits: 45, optimized_bytes: 258379, original_bytes: 922228 },
    };
    expect(daemonSavedRaw(block)).toBe(688697);
  });

  it("keeps a negative total when serving grew the bytes", () => {
    const block = { css: { hits: 10, optimized_bytes: 12000, original_bytes: 10000 } };
    expect(daemonSavedRaw(block)).toBe(-2000);
  });

  it("is null when no class has served or the block is junk", () => {
    expect(daemonSavedRaw(null)).toBeNull();
    expect(daemonSavedRaw("junk")).toBeNull();
    expect(daemonSavedRaw({ css: { hits: 0, optimized_bytes: 0, original_bytes: 0 } })).toBeNull();
  });
});

it("MODULE_SAVINGS_FAMILIES names the counters the module publishes", () => {
  for (const family of MODULE_SAVINGS_FAMILIES) {
    expect(family.savedCounter).toMatch(/_total_bytes_saved$/);
    expect(family.originalCounter).toMatch(/_total_original_bytes$/);
  }
});

describe("optimizedCopyHitRate", () => {
  const NEW_MODULE = {
    ipro_daemon_served: 1151,
    ipro_daemon_fallthrough: 8847,
    ipro_daemon_fallthrough_css: 2,
    ipro_daemon_fallthrough_js: 1,
    ipro_daemon_fallthrough_image: 3,
  };

  it("divides by the optimizable classes and reports the excluded rest", () => {
    const rate = optimizedCopyHitRate(NEW_MODULE);
    expect(rate.mode).toBe("optimizable");
    expect(rate.served).toBe(1151);
    expect(rate.total).toBe(1157);
    expect(rate.percent).toBe(99);
    expect(rate.excluded).toBe(8841);
  });

  it("falls back to all in-place requests when the classes are absent", () => {
    const rate = optimizedCopyHitRate({
      ipro_daemon_served: 1151,
      ipro_daemon_fallthrough: 8847,
    });
    expect(rate.mode).toBe("all-in-place");
    expect(rate.total).toBe(9998);
    expect(rate.percent).toBe(12); // 1151 of 9998 = 11.5%, rounds to 12
    expect(rate.excluded).toBeNull();
  });

  it("treats a partial counter set as the fallback (old modules send all or none)", () => {
    expect(
      optimizedCopyHitRate({
        ipro_daemon_served: 10,
        ipro_daemon_fallthrough: 10,
        ipro_daemon_fallthrough_css: 2,
      }).mode,
    ).toBe("all-in-place");
  });

  it("survives garbage counters without producing NaN", () => {
    const rate = optimizedCopyHitRate({
      ipro_daemon_served: "1151",
      ipro_daemon_fallthrough: Number.NaN,
      ipro_daemon_fallthrough_css: -2,
      ipro_daemon_fallthrough_js: null,
      ipro_daemon_fallthrough_image: 3,
    });
    expect(Number.isFinite(rate.served)).toBe(true);
    expect(rate.percent === null || (rate.percent >= 0 && rate.percent <= 100)).toBe(true);
  });

  it("shows no rate when nothing was in place yet", () => {
    expect(optimizedCopyHitRate({}).percent).toBeNull();
  });
});

describe("savingsSplit", () => {
  const PROD_LIKE = {
    css: {
      original_bytes: 197_186_782,
      optimized_bytes: 197_186_782,
      hits: 1783,
      by_encoding: {
        identity: { hits: 1783, bytes: 197_186_782 },
        gzip: { hits: 0, bytes: 0 },
        br: { hits: 0, bytes: 0 },
      },
    },
    image: {
      original_bytes: 500_000,
      optimized_bytes: 100_000,
      hits: 2,
      by_encoding: {
        identity: { hits: 2, bytes: 100_000 },
        gzip: { hits: 0, bytes: 0 },
        br: { hits: 0, bytes: 0 },
      },
    },
  };
  const VERDICTS = {
    css: { already_optimal: { count: 1, bytes: 110_554 } },
    image: { already_optimal: { count: 2, bytes: 120_000 } },
  };

  it("files a no-savings type under already optimal", () => {
    const [css] = savingsSplit(PROD_LIKE, VERDICTS);
    expect(css.label).toBe("CSS");
    expect(css.segments.map((s) => s.count)).toEqual([1783, 0, 0]);
    expect(css.verdict).toEqual({ count: 1, bytes: 110_554 });
    expect(css.encodingsKnown).toBe(true);
    expect(css.savedBytes).toBe(0);
  });

  it("files a saving type under optimized and served", () => {
    const image = savingsSplit(PROD_LIKE, VERDICTS).find((s) => s.key === "image");
    expect(image?.segments.map((s) => s.count)).toEqual([0, 2, 0]);
    expect(image?.savedBytes).toBe(400_000);
  });

  it("marks stored encodings unknown without the by-encoding block", () => {
    const legacy = {
      css: { original_bytes: 1000, optimized_bytes: 900, hits: 5 },
    };
    const [css] = savingsSplit(legacy, null);
    expect(css.encodingsKnown).toBe(false);
    expect(css.totalServes).toBe(5);
    expect(css.segments.map((s) => s.count)).toEqual([0, 5, 0]);
  });

  it("reports no data for a type with neither serves nor verdicts", () => {
    const [js] = savingsSplit(PROD_LIKE, VERDICTS).filter((s) => s.key === "js");
    expect(js.hasData).toBe(false);
    expect(js.totalServes).toBe(0);
  });

  it("survives garbage blocks", () => {
    expect(() => savingsSplit("junk", 42)).not.toThrow();
    expect(savingsSplit({ css: "junk" }, { css: "junk" })[0].hasData).toBe(false);
  });

  it("clamps untrusted verdict and encoding numbers", () => {
    const [css] = savingsSplit(
      {
        css: {
          original_bytes: 1000,
          optimized_bytes: 1000,
          hits: 4,
          by_encoding: {
            identity: { hits: 4, bytes: 1000 },
            gzip: { hits: -5, bytes: 0 },
            br: { hits: "3", bytes: null },
          },
        },
      },
      { css: { already_optimal: { count: 1, bytes: "110554" } } },
    );
    expect(css.segments.map((s) => s.count)).toEqual([4, 0, 0]);
    expect(css.verdict).toBeNull();
    expect(css.totalServes).toBe(4);
  });

  it("counts encoded serves in the third segment", () => {
    const served = {
      css: {
        original_bytes: 1000,
        optimized_bytes: 400,
        hits: 4,
        by_encoding: {
          identity: { hits: 2, bytes: 400 },
          gzip: { hits: 1, bytes: 200 },
          br: { hits: 1, bytes: 160 },
        },
      },
    };
    const [css] = savingsSplit(served, null);
    expect(css.segments.map((s) => s.count)).toEqual([0, 2, 2]);
    expect(css.totalServes).toBe(4);
  });
});

describe("savingsSplit bytes basis", () => {
  const entry = (gzip: number, br: number) => ({
    css: {
      original_bytes: 1000,
      optimized_bytes: 400,
      hits: 2 + gzip + br,
      by_encoding: {
        identity: { hits: 2, bytes: 400 },
        gzip: { hits: gzip, bytes: gzip * 100 },
        br: { hits: br, bytes: br * 80 },
      },
    },
  });

  it("names the bytes optimized bytes while no compressed copy was served", () => {
    expect(savingsSplit(entry(0, 0), null)[0].bytesBasis).toBe("measured in optimized bytes");
  });

  it("names them transfer bytes once compressed copies are served", () => {
    expect(savingsSplit(entry(1, 0), null)[0].bytesBasis).toBe(
      "measured in transfer bytes, because compressed copies were served",
    );
    expect(savingsSplit(entry(0, 3), null)[0].bytesBasis).toBe(
      "measured in transfer bytes, because compressed copies were served",
    );
  });

  it("stays optimized bytes for an older optimizer and for garbage counts", () => {
    const legacy = { css: { original_bytes: 1000, optimized_bytes: 900, hits: 5 } };
    expect(savingsSplit(legacy, null)[0].bytesBasis).toBe("measured in optimized bytes");
    expect(savingsSplit(entry(-4, 0), null)[0].bytesBasis).toBe("measured in optimized bytes");
  });
});

describe("savingsSplit summary", () => {
  /** The per-type table's row for a type: the figures the summary must repeat. */
  const tableRow = (block: unknown, key: string) => {
    const row = serveSavingsView(block)?.served.find((r) => r.key === key);
    return row === undefined ? undefined : { saved: row.saved, original: row.original, percent: row.percent };
  };
  const byEncoding = (identity: [number, number], gzip: [number, number], br: [number, number]) => ({
    identity: { hits: identity[0], bytes: identity[1] },
    gzip: { hits: gzip[0], bytes: gzip[1] },
    br: { hits: br[0], bytes: br[1] },
  });

  it("an identity-only type: the saving is stated once, the served row has only its requests", () => {
    const block = {
      image: { original_bytes: 500_000, optimized_bytes: 100_000, hits: 2, by_encoding: byEncoding([2, 100_000], [0, 0], [0, 0]) },
    };
    const image = savingsSplit(block, null).find((s) => s.key === "image")!;
    expect(image.summary).toEqual({ saved: 400_000, original: 500_000, percent: 80 });
    expect(image.summary).toEqual(tableRow(block, "image"));
    expect(image.segments.map((s) => s.count)).toEqual([0, 2, 0]);
    // The optimized-and-served segment carries the bytes it sent, never the type's saving.
    expect(image.segments[1].bytes).toBe(100_000);
    expect(splitSummaryText(image.summary!, image.bytesBasis)).toBe(
      "Saved 391 KB of 488 KB (80%), measured in optimized bytes",
    );
  });

  it("a compressed-only type: no saving lands on the row with no requests", () => {
    // Only compressed copies served: 4 requests, 64.2 KB sent of a 432 KB original.
    const block = {
      css: {
        original_bytes: 442_368,
        optimized_bytes: 65_741,
        hits: 4,
        by_encoding: byEncoding([0, 0], [1, 15_000], [3, 50_741]),
      },
    };
    const [css] = savingsSplit(block, null);
    expect(css.summary).toEqual({ saved: 376_627, original: 442_368, percent: 85 });
    expect(css.summary).toEqual(tableRow(block, "css"));
    expect(css.segments.map((s) => s.count)).toEqual([0, 0, 4]);
    expect(css.segments[1].bytes).toBe(0);
    expect(css.segments[2].bytes).toBe(65_741);
    const text = splitSummaryText(css.summary!, css.bytesBasis);
    expect(text).toBe("Saved 368 KB of 432 KB (85%), measured in transfer bytes, because compressed copies were served");
    expect(text).not.toMatch(/\([^)]*\(/);
  });

  it("a mixed type: one summary, the rows split the requests", () => {
    const block = {
      css: {
        original_bytes: 1000,
        optimized_bytes: 760,
        hits: 4,
        by_encoding: byEncoding([2, 400], [1, 200], [1, 160]),
      },
    };
    const [css] = savingsSplit(block, null);
    expect(css.summary).toEqual({ saved: 240, original: 1000, percent: 24 });
    expect(css.summary).toEqual(tableRow(block, "css"));
    expect(css.segments.map((s) => s.count)).toEqual([0, 2, 2]);
    expect(css.segments[1].bytes).toBe(400);
    expect(css.segments[2].bytes).toBe(360);
    expect(splitSummaryText(css.summary!, css.bytesBasis)).toBe(
      "Saved 240 B of 1,000 B (24%), measured in transfer bytes, because compressed copies were served",
    );
  });

  it("a type with no serves has no summary", () => {
    const split = savingsSplit(
      { css: { original_bytes: 0, optimized_bytes: 0, hits: 0, by_encoding: byEncoding([0, 0], [0, 0], [0, 0]) } },
      null,
    );
    for (const s of split) {
      expect(s.summary).toBeNull();
      expect(s.hasData).toBe(false);
    }
    expect(savingsSplit(null, null).every((s) => s.summary === null)).toBe(true);
  });

  it("states no percentage without an original, and never prints NaN", () => {
    const [css] = savingsSplit({ css: { original_bytes: 0, optimized_bytes: 0, hits: 3 } }, null);
    expect(css.summary).toEqual({ saved: 0, original: 0, percent: null });
    const text = splitSummaryText(css.summary!, css.bytesBasis);
    expect(text).toBe("Saved 0 B of 0 B, measured in optimized bytes");
    for (const bad of [Number.NaN, Number.POSITIVE_INFINITY, -1]) {
      const t = splitSummaryText({ saved: bad, original: bad, percent: bad }, "measured in optimized bytes");
      expect(t).not.toMatch(/NaN|undefined|Infinity/);
      expect(t).not.toContain("%");
    }
  });

  it("garbage byte counts give no summary rather than a made-up one", () => {
    const [css] = savingsSplit({ css: { original_bytes: "lots", optimized_bytes: 10, hits: 3 } }, null);
    expect(css.summary).toBeNull();
  });
});

describe("notYetOptimized", () => {
  it("adds the pending and cold classes", () => {
    expect(notYetOptimized({ serve_classes: { original_pending: 3, original_cold: 4 } })).toBe(7);
  });
  it("is null without the classes (an older optimizer)", () => {
    expect(notYetOptimized({})).toBeNull();
    expect(notYetOptimized(null)).toBeNull();
  });
});
