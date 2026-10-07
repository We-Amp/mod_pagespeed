<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script module lang="ts">
  // The chart palette as theme-token names, shared with in-card legends.
  export const SERIES_PALETTE = ["--ps-primary", "--ps-success", "--ps-warning", "--ps-error"] as const;
</script>

<script lang="ts">
  // A time-series line chart on uPlot, themed from the console's CSS tokens
  // (read once at mount; a reload re-reads them). A visually hidden
  // paragraph summarises the series and names the chart (aria-labelledby),
  // so the text alternative is announced once and is also there to copy.
  import { onDestroy, onMount } from "svelte";
  import uPlot from "uplot";
  import "uplot/dist/uPlot.min.css";
  import {
    alignedData,
    axisFont,
    seriesSummary,
    timeTickLabel,
    valueAxisWidth,
    type ChartSeries,
  } from "./utils/chart-summary";

  interface Props {
    title: string;
    /** Epoch seconds, oldest first. */
    timestamps: number[];
    series: ChartSeries[];
    /** The plot's height, or its least height when `aspect` is set. */
    height?: number;
    /** Width over height: the plot grows with its width, from `height` up to `maxHeight`. */
    aspect?: number;
    maxHeight?: number;
    showLegend?: boolean;
    /** Formats the latest values in the text summary (e.g. formatRate). */
    formatValue?: (v: number) => string;
    /** Appended to the y-axis tick labels (e.g. "B/s"). */
    unit?: string;
    /** The y-axis labels for the axis's splits (e.g. bytesRateTicks); overrides `unit`. */
    tickValues?: (splits: ReadonlyArray<number | null>) => string[];
  }

  let { title, timestamps, series, height = 200, aspect, maxHeight, showLegend, formatValue, unit = "", tickValues }: Props = $props();
  const uid = $props.id();
  const summaryId = `ts-chart-${uid}`;

  let container: HTMLDivElement;
  let chart: uPlot | null = null;

  /** The plot's height for a width: fixed, or following the width within bounds. */
  function heightFor(width: number): number {
    if (aspect === undefined || width <= 0) return height;
    return Math.round(Math.min(maxHeight ?? height, Math.max(height, width / aspect)));
  }
  // The measured height once the chart knows its width; until then, `height`.
  let plotHeight = $state(0);
  let shownHeight = $derived(plotHeight > 0 ? plotHeight : height);
  let observer: ResizeObserver | null = null;

  function cssVar(name: string, fallback: string): string {
    const value = getComputedStyle(container).getPropertyValue(name).trim();
    return value === "" ? fallback : value;
  }

  function axisValue(v: number): string {
    const abs = Math.abs(v);
    if (abs >= 1e9) return `${(v / 1e9).toFixed(1)}G`;
    if (abs >= 1e6) return `${(v / 1e6).toFixed(1)}M`;
    if (abs >= 1e3) return `${(v / 1e3).toFixed(1)}k`;
    return String(Math.round(v * 100) / 100);
  }

  // uPlot's default value-axis width; an axis never gets narrower than this.
  const MIN_VALUE_AXIS = 50;

  // The value axis reserves room for its widest label, so a label such as
  // "1.50 KB/s" is shown in full instead of losing its number on the left.
  // uPlot keeps the axis font in device pixels; measured widths are divided
  // back to CSS pixels. After the first passes the size is kept so the
  // layout settles.
  function valueAxisSize(self: uPlot, values: string[] | null, axisIdx: number, cycleNum: number): number {
    const axis = self.axes[axisIdx] as uPlot.Axis & { _size?: number };
    if (cycleNum > 1 && typeof axis._size === "number") return axis._size;
    const font = axis.font as unknown as [string, number] | string | undefined;
    const ratio = uPlot.pxRatio || 1;
    const ctx = self.ctx;
    if (font !== undefined) ctx.font = Array.isArray(font) ? font[0] : font;
    const extra = (axis.ticks?.show === false ? 0 : (axis.ticks?.size ?? 10)) + (axis.gap ?? 5);
    return valueAxisWidth(values, (label) => ctx.measureText(label).width / ratio, extra, MIN_VALUE_AXIS);
  }

  function buildOptions(width: number, plot: number): uPlot.Options {
    const text = cssVar("--ps-text-secondary", "#555555");
    const border = cssVar("--ps-border", "#dddddd");
    const font = axisFont(cssVar("--ps-font-mono", "monospace"));
    return {
      width,
      height: plot,
      legend: { show: showLegend ?? series.length > 1 },
      cursor: { show: true },
      scales: { x: { time: true } },
      axes: [
        {
          stroke: text,
          font,
          grid: { stroke: border },
          // Room for an hh:mm:ss label between ticks; the step between
          // ticks decides whether seconds are shown.
          space: 80,
          values: (_u, splits) => {
            const step = splits.length > 1 ? Math.abs(splits[1] - splits[0]) : 60;
            return splits.map((v) => timeTickLabel(v, step));
          },
        },
        {
          stroke: text,
          font,
          grid: { stroke: border },
          size: valueAxisSize,
          values: (_u, splits) => {
            const labels = tickValues ? tickValues(splits) : splits.map((v) => (v === null ? "" : axisValue(v) + unit));
            // The labels as drawn (the formatter's own output, nothing else),
            // so the axis width can be checked against them.
            container.dataset.valueLabels = JSON.stringify(labels);
            return labels;
          },
        },
      ],
      series: [
        {},
        ...series.map((s, i) => ({
          label: s.label,
          stroke: s.color ?? cssVar(SERIES_PALETTE[i % SERIES_PALETTE.length], "#1a73e8"),
          width: 2,
          spanGaps: false,
        })),
      ],
    };
  }

  onMount(() => {
    plotHeight = heightFor(container.clientWidth);
    chart = new uPlot(buildOptions(container.clientWidth, plotHeight), alignedData(timestamps, series), container);
    observer = new ResizeObserver((entries) => {
      const width = entries[0]?.contentRect.width ?? 0;
      if (chart === null || width <= 0) return;
      plotHeight = heightFor(width);
      chart.setSize({ width, height: plotHeight });
    });
    observer.observe(container);
  });

  // Data updates come from the page's poll; setData redraws in place.
  $effect(() => {
    chart?.setData(alignedData(timestamps, series));
  });

  onDestroy(() => {
    observer?.disconnect();
    chart?.destroy();
    chart = null;
  });
</script>

<div class="ts-chart" bind:this={container} role="img" aria-labelledby={summaryId} style:height="{shownHeight}px"></div>
<p class="visually-hidden chart-summary-text" id={summaryId}>{seriesSummary(title, series, formatValue)}</p>

<style>
  .ts-chart {
    width: 100%;
  }

  .ts-chart :global(.u-over),
  .ts-chart :global(.u-under) {
    /* keep uPlot's canvases inside the card's rounded corners */
    border-radius: var(--ps-border-radius);
  }
</style>
