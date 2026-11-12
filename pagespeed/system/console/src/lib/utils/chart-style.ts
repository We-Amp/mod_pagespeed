// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// How a chart's series are drawn. Colour is never the only channel: in a
// chart with several series each one also has its own dash pattern, so the
// series stay apart in grey print, for colour-blind readers and under forced
// colours, where every series takes the one highlight colour.

/** The series colours as theme-token names, in drawing order. */
export const SERIES_TOKENS = ["--ps-chart-1", "--ps-chart-2", "--ps-chart-3", "--ps-chart-4"] as const;

/** One dash pattern per series, in CSS pixels (dash, gap, ...); the first is solid. */
export const SERIES_DASHES: ReadonlyArray<readonly number[]> = [[], [6, 4], [2, 3], [10, 3, 2, 3]];

/** The line width, in CSS pixels. */
export const LINE_WIDTH = 1.5;

/** The opacity of the area under a single series. */
export const AREA_OPACITY = 0.08;

/** The colours a chart is drawn with, resolved from the page. */
export interface ChartColours {
  /** Axis labels. */
  text: string;
  /** Grid lines and ticks: fainter than the data. */
  grid: string;
  /** The axis line along the plot's edges. */
  axis: string;
  /** One colour per series slot (`SERIES_TOKENS`), or one colour for every series. */
  series: readonly string[];
  /** True under forced colours: no translucent fill, system colours only. */
  forced: boolean;
}

export interface SeriesStyle {
  stroke: string;
  /** CSS pixels. */
  width: number;
  /** CSS pixels; empty for a solid line. */
  dash: number[];
  /** The area under the line; absent when the series has no fill. */
  fill?: string;
}

/** The token name of the colour for series `index`. */
export function seriesToken(index: number): string {
  return SERIES_TOKENS[index % SERIES_TOKENS.length];
}

/**
 * The dash pattern of series `index` in a chart of `count` series: solid when
 * the chart has one series, otherwise one pattern per series.
 */
export function seriesDash(index: number, count: number): number[] {
  if (count <= 1) return [];
  return [...SERIES_DASHES[index % SERIES_DASHES.length]];
}

/** The same pattern as an SVG `stroke-dasharray` ("none" for solid). */
export function dashArray(index: number, count: number): string {
  const dash = seriesDash(index, count);
  return dash.length === 0 ? "none" : dash.join(" ");
}

/**
 * A colour at an opacity, for a canvas fill: "#rrggbb" in (the chart tokens
 * are pinned to that form), "rgba(r, g, b, a)" out; null for any other form.
 */
export function withOpacity(colour: string, opacity: number): string | null {
  const hex = /^#([0-9a-f]{6})$/i.exec(colour.trim());
  if (hex === null) return null;
  const n = parseInt(hex[1], 16);
  return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, ${opacity})`;
}

/**
 * A colour without its transparency: "rgba(r, g, b, a)" in, "rgb(r, g, b)"
 * out; any other form unchanged. A system colour can come back translucent,
 * and a thin line drawn translucent fades into the background.
 */
export function opaque(colour: string): string {
  const fn = /^rgba\(\s*(\d+)[,\s]+(\d+)[,\s]+(\d+)\s*[,/]\s*[\d.]+%?\s*\)$/i.exec(colour.trim());
  return fn === null ? colour : `rgb(${fn[1]}, ${fn[2]}, ${fn[3]})`;
}

/**
 * How series `index` of `count` is drawn. A single series gets a light area
 * fill under its line; several series get a dash pattern each and no fill,
 * so the areas do not cover each other. Under forced colours nothing is
 * filled: a translucent system colour is not the user's palette.
 */
export function seriesStyle(index: number, count: number, colours: ChartColours): SeriesStyle {
  const stroke = colours.series[index % colours.series.length];
  const style: SeriesStyle = { stroke, width: LINE_WIDTH, dash: seriesDash(index, count) };
  if (count === 1 && !colours.forced) {
    const fill = withOpacity(stroke, AREA_OPACITY);
    if (fill !== null) style.fill = fill;
  }
  return style;
}
