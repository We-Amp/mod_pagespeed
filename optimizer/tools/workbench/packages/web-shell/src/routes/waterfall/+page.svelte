<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type { Readable } from 'svelte/store';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    CaptureWaterfallResponse,
  } from '@pagespeed/api-client';
  import { createCaptureClient, formatBytes } from '@pagespeed/api-client';
  import { resolve } from '$app/paths';
  import WaterfallChart from '#lib/components/WaterfallChart.svelte';
  import HelpIcon from '#lib/components/HelpIcon.svelte';
  import type { WaterfallData } from '#lib/waterfall-types.ts';
  import { formatDuration, truncateUrl, waterfallEndTime } from '#lib/waterfall-types.ts';
  import { exportJson, timestampedFilename } from '#lib/export.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const capture = createCaptureClient(transport);

  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Reactive state --------------------------------------------------------

  let connected = $state(false);

  // Form inputs.
  let urlInput = $state('');
  let viewportWidth = $state(1280);
  let throughProxy = $state(false);

  // Loading / error state.
  let loadingDirect = $state(false);
  let loadingProxy = $state(false);
  let errorDirect = $state<string | null>(null);
  let errorProxy = $state<string | null>(null);

  // Capture results.
  let directResult = $state<CaptureWaterfallResponse | null>(null);
  let proxyResult = $state<CaptureWaterfallResponse | null>(null);

  // Viewport presets.
  const VIEWPORT_PRESETS = [
    { label: 'Mobile', width: 480 },
    { label: 'Tablet', width: 768 },
    { label: 'Desktop', width: 1024 },
    { label: 'Wide', width: 1280 },
  ];

  // -- Subscriptions ---------------------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  onDestroy(() => {
    unsubConn();
  });

  // -- Derived values --------------------------------------------------------

  let loading = $derived(loadingDirect || loadingProxy);

  let directData = $derived<WaterfallData | null>(
    directResult
      ? {
          entries: directResult.entries,
          navigationTiming: directResult.navigation_timing,
          totalTransferSize: directResult.total_transfer_size,
          totalDecodedSize: directResult.total_decoded_size,
        }
      : null,
  );

  let proxyData = $derived<WaterfallData | null>(
    proxyResult
      ? {
          entries: proxyResult.entries,
          navigationTiming: proxyResult.navigation_timing,
          totalTransferSize: proxyResult.total_transfer_size,
          totalDecodedSize: proxyResult.total_decoded_size,
        }
      : null,
  );

  let hasResults = $derived(directResult !== null || proxyResult !== null);
  let hasBothResults = $derived(directResult !== null && proxyResult !== null);

  // Comparison metrics.
  let transferSizeSavings = $derived.by(() => {
    if (!directResult || !proxyResult) return null;
    const directSize = directResult.total_transfer_size;
    const proxySize = proxyResult.total_transfer_size;
    const diff = directSize - proxySize;
    const pct = directSize > 0 ? (diff / directSize) * 100 : 0;
    return { direct: directSize, proxy: proxySize, diff, pct };
  });

  let resourceCountDiff = $derived.by(() => {
    if (!directResult || !proxyResult) return null;
    return {
      direct: directResult.entries.length,
      proxy: proxyResult.entries.length,
      diff: directResult.entries.length - proxyResult.entries.length,
    };
  });

  let finishTimeDiff = $derived.by(() => {
    if (!directData || !proxyData) return null;
    const directEnd = waterfallEndTime(directData);
    const proxyEnd = waterfallEndTime(proxyData);
    const diff = directEnd - proxyEnd;
    const pct = directEnd > 0 ? (diff / directEnd) * 100 : 0;
    return { direct: directEnd, proxy: proxyEnd, diff, pct };
  });

  // Per-resource delta annotations: match by URL and compute size diff.
  let resourceDeltas = $derived.by(() => {
    if (!directResult || !proxyResult) return new Map<string, number>();
    const directMap = new Map<string, number>();
    for (const e of directResult.entries) {
      directMap.set(e.url, (directMap.get(e.url) ?? 0) + e.size);
    }
    const deltas = new Map<string, number>();
    for (const e of proxyResult.entries) {
      const directSize = directMap.get(e.url);
      if (directSize !== undefined) {
        deltas.set(e.url, directSize - e.size);
      }
    }
    return deltas;
  });

  // -- URL validation --------------------------------------------------------

  function isValidUrl(input: string): boolean {
    try {
      const url = new URL(input);
      return url.protocol === 'http:' || url.protocol === 'https:';
    } catch {
      return false;
    }
  }

  let urlValid = $derived(isValidUrl(urlInput));

  // -- Actions ---------------------------------------------------------------

  function parseError(e: unknown): string {
    if (e instanceof Error) {
      // Check for 503 status in the message (ApiError).
      if (e.message.includes('503') || e.message.includes('Service Unavailable')) {
        return 'Chrome is not available. Start the worker with --enable-browser-analysis to enable capture.';
      }
      return e.message;
    }
    return String(e);
  }

  async function captureDirect() {
    if (!urlValid) return;
    loadingDirect = true;
    errorDirect = null;
    directResult = null;
    try {
      directResult = await capture.captureWaterfall({
        url: urlInput,
        viewport_width: viewportWidth,
        through_proxy: false,
      });
    } catch (e) {
      errorDirect = parseError(e);
    } finally {
      loadingDirect = false;
    }
  }

  async function captureProxy() {
    if (!urlValid) return;
    loadingProxy = true;
    errorProxy = null;
    proxyResult = null;
    try {
      proxyResult = await capture.captureWaterfall({
        url: urlInput,
        viewport_width: viewportWidth,
        through_proxy: true,
      });
    } catch (e) {
      errorProxy = parseError(e);
    } finally {
      loadingProxy = false;
    }
  }

  async function captureBoth() {
    if (!urlValid) return;
    // Run sequentially: direct first, then proxy, to get a clean comparison.
    await captureDirect();
    await captureProxy();
  }

  async function captureSingle() {
    if (!urlValid) return;
    if (throughProxy) {
      await captureProxy();
    } else {
      await captureDirect();
    }
  }

  function handleExportJson() {
    const payload: Record<string, unknown> = {
      exported_at: new Date().toISOString(),
      url: urlInput,
      viewport_width: viewportWidth,
    };
    if (directResult) {
      payload.direct = directResult;
    }
    if (proxyResult) {
      payload.proxy = proxyResult;
    }
    if (transferSizeSavings) {
      payload.comparison = {
        transfer_size_savings_bytes: transferSizeSavings.diff,
        transfer_size_savings_pct: Number(transferSizeSavings.pct.toFixed(1)),
        resource_count_diff: resourceCountDiff?.diff ?? 0,
        finish_time_diff_ms: finishTimeDiff
          ? Number(finishTimeDiff.diff.toFixed(1))
          : 0,
      };
    }
    exportJson(payload, timestampedFilename('pagespeed-waterfall', 'json'));
  }

  function handleKeydown(event: KeyboardEvent) {
    if (event.key === 'Enter' && urlValid && !loading) {
      captureSingle();
    }
  }
</script>

{#if !connected}
  <div class="page-center">
    <div class="ps-card center-card">
      <div class="ps-spinner" aria-label="Connecting to worker"></div>
      <p class="center-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else}
  <div class="waterfall-page">
    <!-- Header -->
    <div class="page-header">
      <h1 class="page-title">Waterfall Viewer</h1>
    </div>

    <!-- Controls -->
    <div class="ps-card controls-card">
      <div class="controls-row">
        <div class="url-field">
          <label class="field-label" for="url-input">URL</label>
          <input
            id="url-input"
            type="url"
            class="ps-input url-input"
            placeholder="https://example.com"
            bind:value={urlInput}
            onkeydown={handleKeydown}
          />
        </div>

        <div class="viewport-field">
          <label class="field-label" for="viewport-select">Viewport</label>
          <select
            id="viewport-select"
            class="ps-select"
            bind:value={viewportWidth}
          >
            {#each VIEWPORT_PRESETS as preset}
              <option value={preset.width}>{preset.label} ({preset.width}px)</option>
            {/each}
          </select>
        </div>

        <div class="proxy-field">
          <label class="field-label">
            <input
              type="checkbox"
              bind:checked={throughProxy}
            />
            Through PageSpeed
            <HelpIcon tooltip="When enabled, the capture request is routed through the PageSpeed proxy so you see the optimized version. When off, the page is loaded directly from the origin server." />
          </label>
        </div>
      </div>

      <div class="actions-row">
        <button
          class="ps-btn ps-btn-primary"
          onclick={captureSingle}
          disabled={!urlValid || loading}
        >
          {#if loadingDirect || loadingProxy}
            <div class="ps-spinner ps-spinner-sm" aria-label="Capturing waterfall"></div>
            Capturing...
          {:else}
            Capture
          {/if}
        </button>
        <button
          class="ps-btn ps-btn-secondary"
          onclick={captureBoth}
          disabled={!urlValid || loading}
        >
          {#if loadingDirect && loadingProxy}
            <div class="ps-spinner ps-spinner-sm" aria-label="Capturing both waterfalls"></div>
          {/if}
          Capture Both (Original + Optimized)
          <HelpIcon tooltip="Runs two captures sequentially: first directly to the origin (baseline), then through PageSpeed (optimized). Compare the waterfall timings and transfer sizes side by side." />
        </button>

        {#if hasResults}
          <button
            class="ps-btn ps-btn-secondary"
            onclick={handleExportJson}
            title="Export waterfall data as JSON"
          >
            Export JSON
          </button>
          <a
            class="ps-btn ps-btn-secondary"
            href="{resolve('/diff')}"
            title="Compare screenshots in Visual Diff"
          >
            View Visual Diff
          </a>
        {/if}
      </div>

      {#if loading}
        <div class="capture-hint">
          Capturing waterfall can take up to 30 seconds while the page loads...
        </div>
      {/if}
    </div>

    <!-- Errors -->
    {#if errorDirect}
      <div class="ps-card error-card">
        <strong>Direct capture error:</strong> {errorDirect}
      </div>
    {/if}
    {#if errorProxy}
      <div class="ps-card error-card">
        <strong>Proxy capture error:</strong> {errorProxy}
      </div>
    {/if}

    <!-- Comparison summary bar -->
    {#if hasBothResults}
      <div class="comparison-summary">
        {#if transferSizeSavings}
          <div class="summary-metric">
            <span class="summary-label">Transfer Size</span>
            <span
              class="summary-value"
              class:summary-positive={transferSizeSavings.diff > 0}
              class:summary-negative={transferSizeSavings.diff < 0}
            >
              {transferSizeSavings.diff > 0 ? '-' : '+'}{formatBytes(Math.abs(transferSizeSavings.diff))}
              ({Math.abs(transferSizeSavings.pct).toFixed(1)}%)
            </span>
          </div>
        {/if}
        {#if resourceCountDiff}
          <div class="summary-metric">
            <span class="summary-label">Resources</span>
            <span class="summary-value">
              {resourceCountDiff.direct} vs {resourceCountDiff.proxy}
              {#if resourceCountDiff.diff !== 0}
                <span
                  class:summary-positive={resourceCountDiff.diff > 0}
                  class:summary-negative={resourceCountDiff.diff < 0}
                >
                  ({resourceCountDiff.diff > 0 ? '-' : '+'}{Math.abs(resourceCountDiff.diff)})
                </span>
              {/if}
            </span>
          </div>
        {/if}
        {#if finishTimeDiff}
          <div class="summary-metric">
            <span class="summary-label">Finish Time</span>
            <span
              class="summary-value"
              class:summary-positive={finishTimeDiff.diff > 0}
              class:summary-negative={finishTimeDiff.diff < 0}
            >
              {formatDuration(finishTimeDiff.direct)} vs {formatDuration(finishTimeDiff.proxy)}
              ({finishTimeDiff.diff > 0 ? '-' : '+'}{Math.abs(finishTimeDiff.pct).toFixed(1)}%)
            </span>
          </div>
        {/if}
      </div>
    {/if}

    <!-- Aggregate metrics bar (single result) -->
    {#if hasResults && !hasBothResults}
      <div class="aggregate-bar">
        {#if directResult && directData}
          <div class="agg-metric">
            <span class="agg-label">Total Transfer</span>
            <span class="agg-value">{formatBytes(directResult.total_transfer_size)}</span>
          </div>
          <div class="agg-metric">
            <span class="agg-label">Resources</span>
            <span class="agg-value">{directResult.entries.length}</span>
          </div>
          <div class="agg-metric">
            <span class="agg-label">Finish Time</span>
            <span class="agg-value">{formatDuration(waterfallEndTime(directData))}</span>
          </div>
        {:else if proxyResult && proxyData}
          <div class="agg-metric">
            <span class="agg-label">Total Transfer</span>
            <span class="agg-value">{formatBytes(proxyResult.total_transfer_size)}</span>
          </div>
          <div class="agg-metric">
            <span class="agg-label">Resources</span>
            <span class="agg-value">{proxyResult.entries.length}</span>
          </div>
          <div class="agg-metric">
            <span class="agg-label">Finish Time</span>
            <span class="agg-value">{formatDuration(waterfallEndTime(proxyData))}</span>
          </div>
        {/if}
      </div>
    {/if}

    <!-- Waterfall charts -->
    {#if hasBothResults && directData && proxyData}
      <!-- Side-by-side mode -->
      <div class="side-by-side">
        <div class="side-panel">
          <WaterfallChart data={directData} title="Original (Baseline)" />
        </div>
        <div class="side-panel">
          <WaterfallChart data={proxyData} title="Optimized (Through PageSpeed)" />
        </div>
      </div>

      <!-- Per-resource size deltas -->
      {#if resourceDeltas.size > 0}
        <div class="ps-card delta-card">
          <h2 class="section-title">Per-Resource Size Savings</h2>
          <div class="delta-table-scroll">
            <table class="delta-table">
              <thead>
                <tr>
                  <th scope="col">URL</th>
                  <th scope="col" class="num-col">Original</th>
                  <th scope="col" class="num-col">Optimized</th>
                  <th scope="col" class="num-col">Savings</th>
                </tr>
              </thead>
              <tbody>
                {#each [...resourceDeltas.entries()]
                  .sort((a, b) => b[1] - a[1]) as [url, delta]}
                  {@const directEntry = directResult?.entries.find((e) => e.url === url)}
                  {@const proxyEntry = proxyResult?.entries.find((e) => e.url === url)}
                  {#if directEntry && proxyEntry}
                    <tr>
                      <td class="url-cell" title={url}>{truncateUrl(url, 60)}</td>
                      <td class="num-col mono">{formatBytes(directEntry.size)}</td>
                      <td class="num-col mono">{formatBytes(proxyEntry.size)}</td>
                      <td
                        class="num-col mono"
                        class:delta-positive={delta > 0}
                        class:delta-negative={delta < 0}
                      >
                        {delta > 0 ? '-' : delta < 0 ? '+' : ''}{formatBytes(Math.abs(delta))}
                      </td>
                    </tr>
                  {/if}
                {/each}
              </tbody>
            </table>
          </div>
        </div>
      {/if}
    {:else if directData}
      <!-- Single direct result -->
      <div class="single-chart">
        <WaterfallChart data={directData} title="Original (Baseline)" />
      </div>
    {:else if proxyData}
      <!-- Single proxy result -->
      <div class="single-chart">
        <WaterfallChart data={proxyData} title="Optimized (Through PageSpeed)" />
      </div>
    {:else if !loading && !errorDirect && !errorProxy}
      <!-- Empty state -->
      <div class="empty-state">
        <div class="ps-card empty-card">
          <p class="empty-text">
            Enter a URL above and click Capture to visualize the network waterfall.
          </p>
          <p class="empty-hint">
            Use "Capture Both" to compare original vs. optimized side by side.
          </p>
        </div>
      </div>
    {/if}
  </div>
{/if}


<style>
  /* -- Page layout -------------------------------------------------------- */

  .waterfall-page {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1600px;
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-lg);
  }

  .page-center {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 60vh;
    padding: var(--ps-space-xl);
  }

  .center-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 320px;
    text-align: center;
  }

  .center-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  /* -- Header ------------------------------------------------------------- */

  .page-header {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
  }

  .page-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  /* -- Controls ----------------------------------------------------------- */

  .controls-card {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .controls-row {
    display: flex;
    gap: var(--ps-space-md);
    align-items: flex-end;
    flex-wrap: wrap;
  }

  .url-field {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    flex: 1;
    min-width: 280px;
  }

  .url-input {
    width: 100%;
  }

  .viewport-field {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    min-width: 180px;
  }

  .proxy-field {
    display: flex;
    align-items: center;
    padding-bottom: var(--ps-space-xs);
  }

  .proxy-field label {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
    cursor: pointer;
    white-space: nowrap;
  }

  .field-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
  }

  .actions-row {
    display: flex;
    gap: var(--ps-space-sm);
    align-items: center;
    flex-wrap: wrap;
  }

  .capture-hint {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    font-style: italic;
  }

  /* -- Error card --------------------------------------------------------- */

  .error-card {
    background: color-mix(in srgb, var(--ps-error) 8%, var(--ps-bg-primary));
    border-color: var(--ps-error);
    color: var(--ps-error);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  /* -- Comparison summary ------------------------------------------------- */

  .comparison-summary {
    display: flex;
    gap: var(--ps-space-xl);
    padding: var(--ps-space-md) var(--ps-space-lg);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    flex-wrap: wrap;
  }

  .summary-metric {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .summary-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .summary-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
  }

  .summary-positive {
    color: var(--ps-success);
  }

  .summary-negative {
    color: var(--ps-error);
  }

  /* -- Aggregate bar (single result) -------------------------------------- */

  .aggregate-bar {
    display: flex;
    gap: var(--ps-space-xl);
    padding: var(--ps-space-md) var(--ps-space-lg);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-md);
    flex-wrap: wrap;
  }

  .agg-metric {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .agg-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .agg-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
  }

  /* -- Side-by-side layout ------------------------------------------------ */

  .side-by-side {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--ps-space-md);
  }

  .side-panel {
    min-width: 0;
    overflow: hidden;
  }

  .single-chart {
    width: 100%;
  }

  /* -- Delta table -------------------------------------------------------- */

  .section-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  .delta-card {
    overflow: hidden;
  }

  .delta-table-scroll {
    overflow-x: auto;
  }

  .delta-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .delta-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
    white-space: nowrap;
  }

  .delta-table td {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    color: var(--ps-fg-primary);
    border-bottom: 1px solid var(--ps-border);
  }

  .delta-table tbody tr:last-child td {
    border-bottom: none;
  }

  .url-cell {
    max-width: 400px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .num-col {
    text-align: right;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .mono {
    font-family: var(--ps-font-mono);
  }

  .delta-positive {
    color: var(--ps-success);
    font-weight: 500;
  }

  .delta-negative {
    color: var(--ps-error);
    font-weight: 500;
  }

  /* -- Empty state -------------------------------------------------------- */

  .empty-state {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 30vh;
  }

  .empty-card {
    max-width: 480px;
    text-align: center;
    padding: var(--ps-space-xl);
  }

  .empty-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0 0 var(--ps-space-sm);
  }

  .empty-hint {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    margin: 0;
  }

  /* -- Spinner inline ----------------------------------------------------- */

  :global(.ps-spinner-sm) {
    width: 14px;
    height: 14px;
  }

  /* -- Responsive --------------------------------------------------------- */

  @media (max-width: 1023px) {
    .side-by-side {
      grid-template-columns: 1fr;
    }
  }

  @media (max-width: 767px) {
    .waterfall-page {
      padding: var(--ps-space-md);
    }

    .controls-row {
      flex-direction: column;
      align-items: stretch;
    }

    .url-field {
      min-width: auto;
    }

    .viewport-field {
      min-width: auto;
    }

    .actions-row {
      flex-direction: column;
      align-items: stretch;
    }
  }
</style>
