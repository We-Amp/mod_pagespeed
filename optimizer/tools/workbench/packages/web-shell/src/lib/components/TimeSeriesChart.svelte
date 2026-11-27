<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import type uPlot from 'uplot';

  // -- Props ----------------------------------------------------------------

  interface SeriesConfig {
    label: string;
    color: string;
    data: number[];
  }

  let {
    title,
    series,
    timestamps,
    height = 200,
  }: {
    title: string;
    series: SeriesConfig[];
    timestamps: number[];
    height?: number;
  } = $props();

  // -- Internal state -------------------------------------------------------

  let containerEl: HTMLDivElement | undefined = $state();
  let chart: uPlot | null = null;
  let resizeObserver: ResizeObserver | null = null;
  let destroyed = false;

  // -- Theme helpers --------------------------------------------------------

  /** Read a CSS custom property from the document, with fallback. */
  function getCssVar(name: string, fallback: string): string {
    if (typeof getComputedStyle === 'undefined') return fallback;
    const value = getComputedStyle(document.documentElement)
      .getPropertyValue(name)
      .trim();
    return value || fallback;
  }

  function getThemeColors() {
    return {
      bg: getCssVar('--ps-bg-primary', '#1e1e1e'),
      fg: getCssVar('--ps-fg-primary', '#d4d4d4'),
      border: getCssVar('--ps-border', '#3c3c3c'),
      muted: getCssVar('--ps-fg-muted', '#6e6e6e'),
    };
  }

  // -- Chart lifecycle ------------------------------------------------------

  function buildOpts(width: number): uPlot.Options {
    const theme = getThemeColors();

    const uSeries: uPlot.Series[] = [
      {}, // x-axis series (timestamps)
      ...series.map((s) => ({
        label: s.label,
        stroke: s.color,
        width: 1.5,
        points: { show: false },
      })),
    ];

    return {
      width,
      height,
      cursor: {
        drag: { x: false, y: false },
      },
      series: uSeries,
      axes: [
        {
          stroke: theme.muted,
          grid: { stroke: theme.border, width: 1 },
          ticks: { stroke: theme.border, width: 1 },
          font: `11px ${getCssVar('--ps-font-mono', 'monospace')}`,
        },
        {
          stroke: theme.muted,
          grid: { stroke: theme.border, width: 1 },
          ticks: { stroke: theme.border, width: 1 },
          font: `11px ${getCssVar('--ps-font-mono', 'monospace')}`,
          size: 50,
        },
      ],
      legend: {
        show: true,
      },
    };
  }

  function buildData(): uPlot.AlignedData {
    const aligned: number[][] = [timestamps];
    for (const s of series) {
      aligned.push(s.data);
    }
    return aligned as uPlot.AlignedData;
  }

  function createChart(): void {
    if (!containerEl) return;
    destroyChart();

    const width = containerEl.clientWidth;
    if (width <= 0) return;

    // Dynamic import is not needed; we guard with onMount for SSR safety.
    // uPlot is imported at the top but only used after mount.
    import('uplot').then((mod) => {
      // Also import the CSS side-effect.
      import('uplot/dist/uPlot.min.css');

      const UPlot = mod.default;
      if (!containerEl || destroyed) return;

      chart = new UPlot(
        buildOpts(containerEl.clientWidth),
        buildData(),
        containerEl,
      );
    });
  }

  function destroyChart(): void {
    if (chart) {
      chart.destroy();
      chart = null;
    }
  }

  // -- Mount / destroy / resize ---------------------------------------------

  onMount(() => {
    createChart();

    resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        if (chart && entry.contentRect.width > 0) {
          chart.setSize({
            width: entry.contentRect.width,
            height,
          });
        }
      }
    });

    if (containerEl) {
      resizeObserver.observe(containerEl);
    }
  });

  onDestroy(() => {
    destroyed = true;
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }
    destroyChart();
  });

  // -- Reactive data updates ------------------------------------------------

  $effect(() => {
    // Re-read series and timestamps to create dependency.
    const _s = series;
    const _t = timestamps;

    if (chart && _t.length > 0) {
      chart.setData(buildData());
    }
  });
</script>

<div class="chart-wrapper">
  <h3 class="chart-title">{title}</h3>
  <div class="chart-container" bind:this={containerEl}></div>
</div>

<style>
  .chart-wrapper {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .chart-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .chart-container {
    width: 100%;
    overflow: hidden;
  }

  /* Style the uPlot legend to blend with the theme */
  .chart-container :global(.u-legend) {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    padding: var(--ps-space-xs) 0 0;
  }

  .chart-container :global(.u-legend .u-series) {
    padding: 0 var(--ps-space-sm) 0 0;
  }

  .chart-container :global(.u-legend .u-value) {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }
</style>
