<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onMount, onDestroy } from 'svelte';
  import type { ScaleLinear } from 'd3-scale';
  import type {
    WaterfallData,
    WaterfallEntry,
    NavigationTiming,
    TimingPhase,
    MilestoneConfig,
  } from '../waterfall-types';
  import {
    TIMING_PHASES,
    MILESTONES,
    RESOURCE_TYPE_LABELS,
    RESOURCE_TYPE_COLORS,
    totalDuration,
    formatBytes,
    formatDuration,
    truncateUrl,
    waterfallEndTime,
  } from '../waterfall-types';

  // -- Props ----------------------------------------------------------------

  let {
    data,
    title = 'Network Waterfall',
    rowHeight = 22,
    maxHeight = 600,
  }: {
    data: WaterfallData;
    title?: string;
    rowHeight?: number;
    maxHeight?: number;
  } = $props();

  // -- Internal state -------------------------------------------------------

  let containerEl: HTMLDivElement | undefined = $state();
  let containerWidth: number = $state(800);
  let resizeObserver: ResizeObserver | null = null;
  let destroyed = false;

  // D3 modules (dynamically imported for SSR safety).
  let d3Scale: typeof import('d3-scale') | null = $state(null);
  let d3Format: typeof import('d3-format') | null = $state(null);

  // Tooltip state.
  let tooltipEntry: WaterfallEntry | null = $state(null);
  let tooltipX: number = $state(0);
  let tooltipY: number = $state(0);

  // -- Layout constants -----------------------------------------------------

  const LABEL_WIDTH = 200;
  const SIZE_LABEL_WIDTH = 70;
  const LEFT_PADDING = 8;
  const RIGHT_PADDING = 16;
  const TOP_AXIS_HEIGHT = 24;
  const BOTTOM_PADDING = 4;

  // -- Derived values -------------------------------------------------------

  let chartWidth = $derived(
    Math.max(
      100,
      containerWidth - LABEL_WIDTH - SIZE_LABEL_WIDTH - LEFT_PADDING - RIGHT_PADDING,
    ),
  );

  let endTime = $derived(waterfallEndTime(data));

  let xScale: ScaleLinear<number, number> | null = $derived.by(() => {
    if (!d3Scale || endTime <= 0) return null;
    return d3Scale
      .scaleLinear()
      .domain([0, endTime])
      .range([0, chartWidth]);
  });

  let chartHeight = $derived(
    TOP_AXIS_HEIGHT + data.entries.length * rowHeight + BOTTOM_PADDING,
  );

  let svgHeight = $derived(Math.min(chartHeight, maxHeight));

  let ticks = $derived.by(() => {
    if (!xScale) return [];
    return xScale.ticks(Math.max(2, Math.floor(chartWidth / 80)));
  });

  let tickFormat = $derived.by(() => {
    if (!d3Format || endTime <= 0) return (v: number) => `${v}`;
    if (endTime >= 10000) {
      return (v: number) => `${(v / 1000).toFixed(1)}s`;
    }
    return (v: number) => `${Math.round(v)}ms`;
  });

  // -- Tooltip helpers ------------------------------------------------------

  function showTooltip(event: MouseEvent, entry: WaterfallEntry): void {
    tooltipEntry = entry;
    tooltipX = event.clientX;
    tooltipY = event.clientY;
  }

  function hideTooltip(): void {
    tooltipEntry = null;
  }

  // -- Bar segment computation -----------------------------------------------

  interface BarSegment {
    x: number;
    width: number;
    phase: TimingPhase;
    durationMs: number;
  }

  function computeSegments(entry: WaterfallEntry): BarSegment[] {
    if (!xScale) return [];
    const segments: BarSegment[] = [];
    let offset = entry.startTime;

    for (const phase of TIMING_PHASES) {
      const dur = entry.timing[phase.key];
      if (dur > 0) {
        const x = xScale(offset);
        const w = xScale(offset + dur) - x;
        segments.push({
          x,
          width: Math.max(w, 1), // minimum 1px so tiny phases are visible
          phase,
          durationMs: dur,
        });
      }
      offset += dur;
    }
    return segments;
  }

  // -- Milestone helpers -----------------------------------------------------

  interface MilestoneLine {
    x: number;
    config: MilestoneConfig;
    time: number;
  }

  let milestoneLines: MilestoneLine[] = $derived.by(() => {
    if (!xScale) return [];
    const lines: MilestoneLine[] = [];
    const nt = data.navigationTiming;
    for (const m of MILESTONES) {
      const t = nt[m.key];
      if (t > 0) {
        lines.push({ x: xScale(t), config: m, time: t });
      }
    }
    return lines;
  });

  // -- Lifecycle ------------------------------------------------------------

  onMount(async () => {
    // Dynamic import for SSR safety.
    const [scaleModule, formatModule] = await Promise.all([
      import('d3-scale'),
      import('d3-format'),
    ]);

    if (destroyed) return;
    d3Scale = scaleModule;
    d3Format = formatModule;

    // Observe container width for responsive resizing.
    resizeObserver = new ResizeObserver((entries) => {
      for (const entry of entries) {
        if (entry.contentRect.width > 0) {
          containerWidth = entry.contentRect.width;
        }
      }
    });

    if (containerEl) {
      containerWidth = containerEl.clientWidth;
      resizeObserver.observe(containerEl);
    }
  });

  onDestroy(() => {
    destroyed = true;
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }
  });
</script>

<div class="waterfall-wrapper">
  <h3 class="waterfall-title">{title}</h3>

  {#if data.entries.length === 0}
    <div class="waterfall-empty">No network requests recorded. Try reloading the page or checking the URL.</div>
  {:else}
    <!-- Legend -->
    <div class="waterfall-legend">
      {#each TIMING_PHASES as phase}
        <span class="legend-item">
          <span class="legend-swatch" style="background:{phase.color}"></span>
          {phase.label}
        </span>
      {/each}
      <span class="legend-separator">|</span>
      {#each MILESTONES as m}
        <span class="legend-item milestone-label" style="color:{m.color}">
          {m.label}
        </span>
      {/each}
    </div>

    <div
      class="waterfall-container"
      bind:this={containerEl}
      style="max-height:{maxHeight}px"
    >
      {#if xScale}
        <svg
          width={containerWidth}
          height={chartHeight}
          class="waterfall-svg"
          role="img"
          aria-label="Network waterfall chart showing {data.entries.length} requests"
        >
          <!-- Time axis at top -->
          <g class="axis" transform="translate({LABEL_WIDTH + LEFT_PADDING}, 0)">
            {#each ticks as tick}
              <line
                x1={xScale(tick)}
                y1={TOP_AXIS_HEIGHT}
                x2={xScale(tick)}
                y2={chartHeight}
                class="grid-line"
              />
              <text
                x={xScale(tick)}
                y={TOP_AXIS_HEIGHT - 6}
                class="axis-label"
                text-anchor="middle"
              >
                {tickFormat(tick)}
              </text>
            {/each}
          </g>

          <!-- Milestone vertical lines -->
          <g
            class="milestones"
            transform="translate({LABEL_WIDTH + LEFT_PADDING}, 0)"
          >
            {#each milestoneLines as ml}
              <line
                x1={ml.x}
                y1={TOP_AXIS_HEIGHT}
                x2={ml.x}
                y2={chartHeight}
                stroke={ml.config.color}
                stroke-width="1.5"
                stroke-dasharray={ml.config.dashArray}
                opacity="0.7"
              />
              <text
                x={ml.x}
                y={TOP_AXIS_HEIGHT - 6}
                fill={ml.config.color}
                class="milestone-text"
                text-anchor="middle"
              >
                {ml.config.label}
              </text>
            {/each}
          </g>

          <!-- Rows -->
          {#each data.entries as entry, i}
            {@const y = TOP_AXIS_HEIGHT + i * rowHeight}
            {@const segments = computeSegments(entry)}
            {@const dur = totalDuration(entry.timing)}
            {@const resourceColor = RESOURCE_TYPE_COLORS[entry.resourceType]}
            {@const resourceLabel = RESOURCE_TYPE_LABELS[entry.resourceType]}

            <g
              class="waterfall-row"
              class:row-even={i % 2 === 0}
              role="graphics-symbol"
              aria-label="{RESOURCE_TYPE_LABELS[entry.resourceType]}: {truncateUrl(entry.url, 40)}"
              onmouseenter={(e: MouseEvent) => showTooltip(e, entry)}
              onmouseleave={hideTooltip}
            >
              <!-- Row background for hover -->
              <rect
                x="0"
                {y}
                width={containerWidth}
                height={rowHeight}
                class="row-bg"
              />

              <!-- Resource type badge -->
              <rect
                x="4"
                y={y + 3}
                width="36"
                height={rowHeight - 6}
                rx="3"
                fill={resourceColor}
                opacity="0.15"
              />
              <text
                x="22"
                y={y + rowHeight / 2}
                class="resource-type-label"
                fill={resourceColor}
                text-anchor="middle"
                dominant-baseline="central"
              >
                {resourceLabel}
              </text>

              <!-- URL label -->
              <text
                x="46"
                y={y + rowHeight / 2}
                class="url-label"
                dominant-baseline="central"
              >
                {truncateUrl(entry.url, 28)}
              </text>

              <!-- Timing bars -->
              <g transform="translate({LABEL_WIDTH + LEFT_PADDING}, 0)">
                {#each segments as seg}
                  <rect
                    x={seg.x}
                    y={y + 4}
                    width={seg.width}
                    height={rowHeight - 8}
                    rx="2"
                    fill={seg.phase.color}
                    opacity="0.85"
                  />
                {/each}
              </g>

              <!-- Duration / size label on the right -->
              <text
                x={LABEL_WIDTH + LEFT_PADDING + chartWidth + 8}
                y={y + rowHeight / 2}
                class="size-label"
                dominant-baseline="central"
              >
                {formatDuration(dur)} / {formatBytes(entry.size)}
              </text>
            </g>
          {/each}
        </svg>
      {:else}
        <div class="waterfall-loading">Loading chart...</div>
      {/if}
    </div>

    <!-- Summary bar -->
    <div class="waterfall-summary">
      <span>{data.entries.length} requests</span>
      <span>{formatBytes(data.totalTransferSize)} transferred</span>
      <span>{formatBytes(data.totalDecodedSize)} decoded</span>
      <span>Finish: {formatDuration(endTime)}</span>
    </div>
  {/if}

  <!-- Tooltip -->
  {#if tooltipEntry}
    {@const te = tooltipEntry}
    {@const dur = totalDuration(te.timing)}
    <div
      class="waterfall-tooltip"
      style="left:{tooltipX + 12}px;top:{tooltipY + 12}px"
    >
      <div class="tooltip-url">{truncateUrl(te.url, 80)}</div>
      <div class="tooltip-meta">
        {te.method} {te.status}
        {#if te.fromCache}&nbsp;(cached){/if}
        {#if te.fromServiceWorker}&nbsp;(SW){/if}
        &mdash; {te.protocol}
      </div>
      <table class="tooltip-table">
        <tbody>
          {#each TIMING_PHASES as phase}
            {@const v = te.timing[phase.key]}
            {#if v > 0}
              <tr>
                <td>
                  <span
                    class="tooltip-swatch"
                    style="background:{phase.color}"
                  ></span>
                  {phase.label}
                </td>
                <td class="tooltip-value">{formatDuration(v)}</td>
              </tr>
            {/if}
          {/each}
          <tr class="tooltip-total">
            <td>Total</td>
            <td class="tooltip-value">{formatDuration(dur)}</td>
          </tr>
        </tbody>
      </table>
      <div class="tooltip-sizes">
        {formatBytes(te.size)} transferred / {formatBytes(te.decodedSize)} decoded
      </div>
    </div>
  {/if}
</div>

<style>
  .waterfall-wrapper {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
  }

  .waterfall-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .waterfall-empty {
    color: var(--ps-fg-muted);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-lg);
    text-align: center;
  }

  .waterfall-loading {
    color: var(--ps-fg-muted);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-lg);
    text-align: center;
  }

  /* -- Legend ------------------------------------------------------------ */

  .waterfall-legend {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
    align-items: center;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    padding: var(--ps-space-xs) 0;
  }

  .legend-item {
    display: inline-flex;
    align-items: center;
    gap: 4px;
  }

  .legend-swatch {
    display: inline-block;
    width: 10px;
    height: 10px;
    border-radius: 2px;
  }

  .legend-separator {
    color: var(--ps-fg-muted);
    opacity: 0.5;
  }

  .milestone-label {
    font-weight: 600;
  }

  /* -- Chart container -------------------------------------------------- */

  .waterfall-container {
    width: 100%;
    overflow-x: hidden;
    overflow-y: auto;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    background: var(--ps-bg-primary);
  }

  .waterfall-svg {
    display: block;
  }

  /* -- Axis ------------------------------------------------------------- */

  .grid-line {
    stroke: var(--ps-border);
    stroke-width: 0.5;
    opacity: 0.5;
  }

  .axis-label {
    fill: var(--ps-fg-muted);
    font-family: var(--ps-font-mono);
    font-size: 10px;
  }

  .milestone-text {
    font-family: var(--ps-font-mono);
    font-size: 9px;
    font-weight: 600;
  }

  /* -- Rows ------------------------------------------------------------- */

  .row-bg {
    fill: transparent;
    transition: fill 0.1s ease;
  }

  .waterfall-row:hover .row-bg {
    fill: var(--ps-bg-hover);
  }

  .waterfall-row.row-even .row-bg {
    fill: var(--ps-bg-secondary);
    opacity: 0.3;
  }

  .waterfall-row:hover.row-even .row-bg {
    fill: var(--ps-bg-hover);
    opacity: 1;
  }

  .resource-type-label {
    font-family: var(--ps-font-mono);
    font-size: 9px;
    font-weight: 700;
    text-transform: uppercase;
  }

  .url-label {
    fill: var(--ps-fg-primary);
    font-family: var(--ps-font-mono);
    font-size: 11px;
  }

  .size-label {
    fill: var(--ps-fg-muted);
    font-family: var(--ps-font-mono);
    font-size: 10px;
  }

  /* -- Summary ---------------------------------------------------------- */

  .waterfall-summary {
    display: flex;
    gap: var(--ps-space-lg);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    padding: var(--ps-space-xs) 0;
    border-top: 1px solid var(--ps-border);
  }

  /* -- Tooltip ---------------------------------------------------------- */

  .waterfall-tooltip {
    position: fixed;
    z-index: 1000;
    background: var(--ps-bg-tertiary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
    pointer-events: none;
    max-width: 400px;
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
  }

  .tooltip-url {
    font-family: var(--ps-font-mono);
    font-size: 11px;
    word-break: break-all;
    margin-bottom: var(--ps-space-xs);
    color: var(--ps-fg-link);
  }

  .tooltip-meta {
    font-size: 10px;
    color: var(--ps-fg-secondary);
    margin-bottom: var(--ps-space-sm);
  }

  .tooltip-table {
    border-collapse: collapse;
    width: 100%;
    margin-bottom: var(--ps-space-xs);
  }

  .tooltip-table td {
    padding: 1px var(--ps-space-xs);
    font-family: var(--ps-font-mono);
    font-size: 11px;
  }

  .tooltip-value {
    text-align: right;
    color: var(--ps-fg-secondary);
  }

  .tooltip-swatch {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 2px;
    margin-right: 4px;
    vertical-align: middle;
  }

  .tooltip-total {
    border-top: 1px solid var(--ps-border);
    font-weight: 600;
  }

  .tooltip-sizes {
    font-size: 10px;
    color: var(--ps-fg-muted);
  }
</style>
