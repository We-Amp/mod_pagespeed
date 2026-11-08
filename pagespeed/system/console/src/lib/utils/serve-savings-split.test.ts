// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import { formatSignedBytes } from "./format";
import { SPLIT_MIN_COVERAGE, serveSavingsView, splitCells } from "./serve-savings";

// Fixture-only numbers: a text-heavy mix with one class that grew.
const css = {
  hits: 5,
  original_bytes: 154250,
  optimized_bytes: 73419,
  split: { original_bytes: 154250, compressed_original_bytes: 107988, optimized_bytes: 73419 },
};
const js = {
  hits: 5,
  original_bytes: 53635,
  optimized_bytes: 24952,
  split: { original_bytes: 53635, compressed_original_bytes: 40185, optimized_bytes: 24952 },
};
const html = {
  hits: 5,
  original_bytes: 140847,
  optimized_bytes: 150905,
  split: { original_bytes: 140847, compressed_original_bytes: 99177, optimized_bytes: 150905 },
};
const image = {
  hits: 9,
  original_bytes: 5765920,
  optimized_bytes: 2034574,
  split: { original_bytes: 5765920, compressed_original_bytes: 5765920, optimized_bytes: 2034574 },
};

describe("the savings split", () => {
  it("adds up to the signed saving exactly, over every class", () => {
    const view = serveSavingsView({ css, js, html, image });
    const split = view?.total?.split;
    expect(split).not.toBeNull();
    expect(split!.coverage).toBe(1);
    expect(split!.available).toBe(true);
    expect(split!.encodingSaved + split!.rewriteSaved).toBe(view!.total!.saved);
    for (const row of view!.served) {
      expect(row.split!.encodingSaved + row.split!.rewriteSaved).toBe(row.saved);
    }
  });

  it("shows a class that grew as negative, not as zero", () => {
    const view = serveSavingsView({ html });
    const row = view!.served[0];
    expect(row.saved).toBe(-10058);
    expect(row.split!.rewriteSaved).toBeLessThan(0);
    expect(row.percent).toBe(0);
  });

  it("the headline counts a resource type that grew, so it equals the two parts", () => {
    const view = serveSavingsView({ css, js, html, image });
    const total = view!.total!;
    const oldHeadline = [css, js, html, image].reduce(
      (sum, c) => sum + Math.max(0, c.original_bytes - c.optimized_bytes),
      0,
    );
    // Fixture figures: the old clamped headline was 3,840,860; the signed one is 10,058 lower.
    expect(oldHeadline).toBe(3840860);
    expect(total.saved).toBe(3830802);
    expect(oldHeadline - total.saved).toBe(10058);
    expect(total.split!.encodingSaved + total.split!.rewriteSaved).toBe(total.saved);
  });

  it("shows a negative compression figure signed, not clamped", () => {
    const grown = {
      hits: 1,
      original_bytes: 40,
      optimized_bytes: 30,
      split: { original_bytes: 40, compressed_original_bytes: 52, optimized_bytes: 30 },
    };
    const split = serveSavingsView({ css: grown })!.total!.split!;
    expect(split.encodingSaved).toBe(-12);
    expect(split.encodingSaved + split.rewriteSaved + split.unsplit).toBe(10);
    expect(splitCells(split).encoding).toBe("−12.0 B");
  });

  it("adds an unsplit remainder so the three parts equal the headline at 95% coverage", () => {
    const partial = {
      hits: 10,
      original_bytes: 100000,
      optimized_bytes: 40000,
      // 95% of the original bytes carry the split; the rest saved something too.
      split: { original_bytes: 95000, compressed_original_bytes: 60000, optimized_bytes: 38000 },
    };
    const view = serveSavingsView({ css: partial })!;
    const split = view.total!.split!;
    expect(split.coverage).toBeCloseTo(0.95);
    expect(split.available).toBe(true);
    expect(split.encodingSaved).toBe(35000);
    expect(split.rewriteSaved).toBe(22000);
    expect(split.unsplit).toBe(3000); // (100000 - 40000) - 35000 - 22000
    expect(split.encodingSaved + split.rewriteSaved + split.unsplit).toBe(view.total!.saved);
    // The footer cells carry the same three parts, so they add up to "Saved".
    const cells = splitCells(split);
    expect(cells.unsplit).toBe(formatSignedBytes(3000));
    expect(cells.encoding).toBe(formatSignedBytes(35000));
    expect(cells.rewrite).toBe(formatSignedBytes(22000));
    // A class with no split at all counts wholly as unsplit in the total.
    const mixed = serveSavingsView({ css: partial, js: { hits: 1, original_bytes: 1000, optimized_bytes: 400 } })!;
    const mt = mixed.total!;
    expect(mt.split!.encodingSaved + mt.split!.rewriteSaved + mt.split!.unsplit).toBe(mt.saved);
  });

  it("table cells are a dash unless the row's split is available; an all-zero row is not measured", () => {
    const zero = { ...css, split: { original_bytes: 0, compressed_original_bytes: 0, optimized_bytes: 0 } };
    const view = serveSavingsView({ css: zero, js })!;
    const rows = Object.fromEntries(view.served.map((r) => [r.key, r]));
    expect(splitCells(rows.css.split)).toEqual({ encoding: "—", rewrite: "—", unsplit: "—" });
    expect(splitCells(rows.js.split).encoding).not.toBe("—");
    expect(splitCells(null)).toEqual({ encoding: "—", rewrite: "—", unsplit: "—" });
    const low = { ...css, split: { original_bytes: 50000, compressed_original_bytes: 30000, optimized_bytes: 20000 } };
    expect(splitCells(serveSavingsView({ css: low })!.served[0].split).encoding).toBe("—");
  });

  it("flags the markup note only when the HTML class is a negative contributor", () => {
    const grew = (original: number, compressed: number, servedBytes: number) => ({
      hits: 1,
      original_bytes: original,
      optimized_bytes: servedBytes,
      split: { original_bytes: original, compressed_original_bytes: compressed, optimized_bytes: servedBytes },
    });
    // HTML negative only: the total optimization part is negative and HTML caused it.
    const htmlOnly = serveSavingsView({ html: grew(1000, 300, 500), css: grew(100, 50, 40) })!.total!.split!;
    expect(htmlOnly.rewriteSaved).toBeLessThan(0);
    expect(htmlOnly.htmlGrew).toBe(true);
    // CSS negative only: negative total, but not from HTML.
    const cssOnly = serveSavingsView({ css: grew(1000, 300, 500), html: grew(100, 50, 40) })!.total!.split!;
    expect(cssOnly.rewriteSaved).toBeLessThan(0);
    expect(cssOnly.htmlGrew).toBe(false);
    // Both negative.
    const both = serveSavingsView({ html: grew(1000, 300, 500), css: grew(1000, 300, 500) })!.total!.split!;
    expect(both.rewriteSaved).toBeLessThan(0);
    expect(both.htmlGrew).toBe(true);
    // No HTML class at all.
    expect(serveSavingsView({ css: grew(1000, 300, 500) })!.total!.split!.htmlGrew).toBe(false);
  });

  it("is unavailable below the coverage threshold", () => {
    const partial = { ...css, split: { original_bytes: 50000, compressed_original_bytes: 30000, optimized_bytes: 20000 } };
    const split = serveSavingsView({ css: partial })!.total!.split!;
    expect(split.coverage).toBeLessThan(SPLIT_MIN_COVERAGE);
    expect(split.available).toBe(false);
  });

  it("is absent for an optimizer that reports no split", () => {
    const { split: _split, ...old } = css;
    const view = serveSavingsView({ css: old });
    expect(view!.total!.split).toBeNull();
    expect(view!.served[0].split).toBeNull();
    // An all-zero block (a writer that does not record it) is coverage 0.
    const zero = { ...css, split: { original_bytes: 0, compressed_original_bytes: 0, optimized_bytes: 0 } };
    expect(serveSavingsView({ css: zero })!.total!.split!.available).toBe(false);
  });

  it("ignores an inconsistent or malformed block", () => {
    const over = { ...css, split: { original_bytes: 999999999, compressed_original_bytes: 1, optimized_bytes: 1 } };
    expect(serveSavingsView({ css: over })!.served[0].split).toBeNull();
    const bad = { ...css, split: { original_bytes: "x", compressed_original_bytes: 1, optimized_bytes: 1 } };
    expect(serveSavingsView({ css: bad })!.served[0].split).toBeNull();
    const negative = { ...css, split: { original_bytes: -1, compressed_original_bytes: 1, optimized_bytes: 1 } };
    expect(serveSavingsView({ css: negative })!.served[0].split).toBeNull();
  });
});
