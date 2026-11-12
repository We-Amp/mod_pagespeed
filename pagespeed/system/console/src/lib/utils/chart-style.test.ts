// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

import { describe, expect, it } from "vitest";
import {
  AREA_OPACITY,
  dashArray,
  LINE_WIDTH,
  opaque,
  SERIES_DASHES,
  SERIES_TOKENS,
  seriesDash,
  seriesStyle,
  seriesToken,
  withOpacity,
  type ChartColours,
} from "./chart-style";

const THEME: ChartColours = {
  text: "#57534e",
  grid: "#e7e5e4",
  axis: "#d6d3d1",
  series: ["#1d4ed8", "#92400e", "#0891b2", "#44403c"],
  forced: false,
};
const FORCED: ChartColours = {
  text: "rgb(0, 0, 0)",
  grid: "rgb(96, 96, 96)",
  axis: "rgb(0, 0, 0)",
  series: ["rgb(0, 102, 204)"],
  forced: true,
};

describe("series tokens and dashes", () => {
  it("one token per slot, repeating after the last", () => {
    expect(seriesToken(0)).toBe("--ps-chart-1");
    expect(seriesToken(3)).toBe("--ps-chart-4");
    expect(seriesToken(4)).toBe("--ps-chart-1");
    expect(SERIES_TOKENS).toHaveLength(SERIES_DASHES.length);
  });
  it("every dash pattern differs from every other", () => {
    const keys = SERIES_DASHES.map((d) => d.join(","));
    expect(new Set(keys).size).toBe(keys.length);
    expect(SERIES_DASHES[0]).toEqual([]);
  });
  it("a single series is solid; several get a pattern each", () => {
    expect(seriesDash(0, 1)).toEqual([]);
    expect(seriesDash(0, 2)).toEqual([]);
    expect(seriesDash(1, 2)).toEqual([6, 4]);
    expect(dashArray(0, 1)).toBe("none");
    expect(dashArray(1, 2)).toBe("6 4");
  });
});

describe("withOpacity", () => {
  it("turns a hex colour into rgba", () => {
    expect(withOpacity("#1d4ed8", 0.08)).toBe("rgba(29, 78, 216, 0.08)");
  });
  it("gives up on any other form", () => {
    expect(withOpacity("rgb(0, 102, 204)", 0.5)).toBeNull();
    expect(withOpacity("Highlight", 0.5)).toBeNull();
    expect(withOpacity("#abc", 0.5)).toBeNull();
  });
});

describe("opaque", () => {
  it("drops the transparency of an rgba colour and keeps any other form", () => {
    expect(opaque("rgba(0, 230, 255, 0.8)")).toBe("rgb(0, 230, 255)");
    expect(opaque("rgba(0 230 255 / 0.8)")).toBe("rgb(0, 230, 255)");
    expect(opaque("rgb(1, 2, 3)")).toBe("rgb(1, 2, 3)");
    expect(opaque("Highlight")).toBe("Highlight");
  });
});

describe("seriesStyle", () => {
  it("a single series: the first colour, a thin solid line and a light fill", () => {
    expect(seriesStyle(0, 1, THEME)).toEqual({
      stroke: "#1d4ed8",
      width: LINE_WIDTH,
      dash: [],
      fill: `rgba(29, 78, 216, ${AREA_OPACITY})`,
    });
  });
  it("several series: a colour and a dash pattern each, no fill", () => {
    const styles = [0, 1, 2].map((i) => seriesStyle(i, 3, THEME));
    expect(styles.map((s) => s.stroke)).toEqual(["#1d4ed8", "#92400e", "#0891b2"]);
    expect(styles.map((s) => s.dash)).toEqual([[], [6, 4], [2, 3]]);
    expect(styles.every((s) => s.fill === undefined)).toBe(true);
  });
  it("forced colours: one system colour for every series, told apart by dashes, never filled", () => {
    const styles = [0, 1].map((i) => seriesStyle(i, 2, FORCED));
    expect(styles.map((s) => s.stroke)).toEqual(["rgb(0, 102, 204)", "rgb(0, 102, 204)"]);
    expect(styles[0].dash).not.toEqual(styles[1].dash);
    expect(seriesStyle(0, 1, FORCED).fill).toBeUndefined();
  });
});
