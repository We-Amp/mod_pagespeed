<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    CaptureScreenshotResponse,
  } from '@pagespeed/api-client';
  import { createCaptureClient } from '@pagespeed/api-client';
  import { ApiError } from '@pagespeed/api-client';
  import { resolve } from '$app/paths';
  import type { Readable } from 'svelte/store';
  import ImageDiff from '#lib/components/ImageDiff.svelte';
  import HelpIcon from '#lib/components/HelpIcon.svelte';
  import type { DiffMode } from '#lib/components/ImageDiff.svelte';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const capture = createCaptureClient(transport);

  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Reactive state --------------------------------------------------------

  let connected = $state(false);

  // Capture input state.
  let url = $state('');
  let viewportWidth = $state(1024);
  let fullPage = $state(true);

  // Capture result state.
  let beforeData = $state<CaptureScreenshotResponse | null>(null);
  let afterData = $state<CaptureScreenshotResponse | null>(null);
  let capturingBefore = $state(false);
  let capturingAfter = $state(false);
  let capturingBoth = $state(false);
  let captureError = $state<string | null>(null);

  // View controls.
  let mode: DiffMode = $state('slider');
  let zoomLevel = $state(1); // 1 = fit, specific values for 100%, 200%
  let fitMode = $state(true);

  // -- Subscriptions ---------------------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  onDestroy(() => {
    unsubConn();
  });

  // -- Derived values --------------------------------------------------------

  let beforeSrc = $derived(
    beforeData ? `data:image/png;base64,${beforeData.png_base64}` : '',
  );
  let afterSrc = $derived(
    afterData ? `data:image/png;base64,${afterData.png_base64}` : '',
  );
  let hasBoth = $derived(beforeSrc !== '' && afterSrc !== '');
  let hasAny = $derived(beforeSrc !== '' || afterSrc !== '');
  let isCapturing = $derived(
    capturingBefore || capturingAfter || capturingBoth,
  );

  // -- Actions ---------------------------------------------------------------

  async function captureBefore() {
    if (!url.trim()) return;
    capturingBefore = true;
    captureError = null;
    try {
      beforeData = await capture.captureScreenshot({
        url: url.trim(),
        viewport_width: viewportWidth,
        through_proxy: false,
        full_page: fullPage,
      });
    } catch (e) {
      captureError = formatCaptureError(e);
      beforeData = null;
    } finally {
      capturingBefore = false;
    }
  }

  async function captureAfter() {
    if (!url.trim()) return;
    capturingAfter = true;
    captureError = null;
    try {
      afterData = await capture.captureScreenshot({
        url: url.trim(),
        viewport_width: viewportWidth,
        through_proxy: true,
        full_page: fullPage,
      });
    } catch (e) {
      captureError = formatCaptureError(e);
      afterData = null;
    } finally {
      capturingAfter = false;
    }
  }

  async function captureBothScreenshots() {
    if (!url.trim()) return;
    capturingBoth = true;
    captureError = null;

    try {
      const [beforeResult, afterResult] = await Promise.allSettled([
        capture.captureScreenshot({
          url: url.trim(),
          viewport_width: viewportWidth,
          through_proxy: false,
          full_page: fullPage,
        }),
        capture.captureScreenshot({
          url: url.trim(),
          viewport_width: viewportWidth,
          through_proxy: true,
          full_page: fullPage,
        }),
      ]);

      if (beforeResult.status === 'fulfilled') {
        beforeData = beforeResult.value;
      } else {
        captureError = formatCaptureError(beforeResult.reason);
        beforeData = null;
      }

      if (afterResult.status === 'fulfilled') {
        afterData = afterResult.value;
      } else {
        if (!captureError) {
          captureError = formatCaptureError(afterResult.reason);
        }
        afterData = null;
      }
    } finally {
      capturingBoth = false;
    }
  }

  function formatCaptureError(e: unknown): string {
    if (e instanceof ApiError && e.status === 503) {
      return 'Chrome is not available. Start the worker with --enable-browser-analysis to enable capture.';
    }
    if (e instanceof Error) return e.message;
    return String(e);
  }

  function setZoom(level: number) {
    fitMode = false;
    zoomLevel = level;
  }

  function setFit() {
    fitMode = true;
    zoomLevel = 1;
  }

  function downloadImage(data: CaptureScreenshotResponse, label: 'original' | 'optimized') {
    const link = document.createElement('a');
    link.href = `data:image/png;base64,${data.png_base64}`;
    const filename = `${label}-${new URL(data.url).hostname}-${data.viewport_width}w.png`;
    link.download = filename;
    link.click();
  }

  // -- Viewport presets -------------------------------------------------------

  const VIEWPORT_PRESETS = [
    { label: 'Mobile', width: 480 },
    { label: 'Tablet', width: 768 },
    { label: 'Desktop', width: 1024 },
  ];
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
  <div class="diff-page">
    <div class="page-header">
      <h1 class="page-title">Visual Diff</h1>
      <p class="page-description">
        Compare original and optimized page screenshots side by side.
      </p>
    </div>

    <!-- Capture controls -->
    <div class="ps-card capture-controls">
      <div class="capture-url-row">
        <label class="field-label" for="diff-url">URL</label>
        <input
          id="diff-url"
          type="url"
          class="ps-input url-input"
          placeholder="https://example.com"
          bind:value={url}
          disabled={isCapturing}
          onkeydown={(e) => {
            if (e.key === 'Enter') captureBothScreenshots();
          }}
        />
      </div>

      <div class="capture-options-row">
        <div class="field-group">
          <label class="field-label" for="viewport-select">Viewport</label>
          <select
            id="viewport-select"
            class="ps-select viewport-select"
            bind:value={viewportWidth}
            disabled={isCapturing}
          >
            {#each VIEWPORT_PRESETS as preset}
              <option value={preset.width}>
                {preset.label} ({preset.width}px)
              </option>
            {/each}
          </select>
        </div>

        <div class="field-group">
          <label class="field-label checkbox-label">
            <input
              type="checkbox"
              bind:checked={fullPage}
              disabled={isCapturing}
            />
            Full page
            <HelpIcon tooltip="When enabled, captures the entire scrollable page. When off, captures only the visible viewport area." />
          </label>
        </div>
      </div>

      <div class="capture-actions">
        <button
          class="ps-btn ps-btn-secondary"
          onclick={captureBefore}
          disabled={isCapturing || !url.trim()}
        >
          {#if capturingBefore}
            <div class="ps-spinner" aria-label="Capturing original screenshot"></div>
          {/if}
          Capture Original
        </button>

        <button
          class="ps-btn ps-btn-secondary"
          onclick={captureAfter}
          disabled={isCapturing || !url.trim()}
        >
          {#if capturingAfter}
            <div class="ps-spinner" aria-label="Capturing optimized screenshot"></div>
          {/if}
          Capture Optimized
        </button>

        <button
          class="ps-btn ps-btn-primary"
          onclick={captureBothScreenshots}
          disabled={isCapturing || !url.trim()}
          title="Captures two screenshots: one from the origin (original) and one through PageSpeed (optimized) for side-by-side comparison"
        >
          {#if capturingBoth}
            <div class="ps-spinner" aria-label="Capturing both screenshots"></div>
          {/if}
          Capture Both
        </button>
      </div>

      {#if captureError}
        <div class="capture-error">
          <span class="ps-badge ps-badge-error">{captureError}</span>
        </div>
      {/if}
    </div>

    {#if hasAny}
      <!-- View controls -->
      <div class="view-controls">
        <!-- Mode selector -->
        <div class="mode-selector">
          <button
            class="ps-btn mode-btn"
            class:mode-active={mode === 'slider'}
            onclick={() => (mode = 'slider')}
            disabled={!hasBoth}
            title="Drag a divider to reveal original vs. optimized"
          >
            Slider
          </button>
          <button
            class="ps-btn mode-btn"
            class:mode-active={mode === 'blink'}
            onclick={() => (mode = 'blink')}
            disabled={!hasBoth}
            title="Alternates between original and optimized on a timer"
          >
            Blink
          </button>
          <button
            class="ps-btn mode-btn"
            class:mode-active={mode === 'pixeldiff'}
            onclick={() => (mode = 'pixeldiff')}
            disabled={!hasBoth}
            title="Highlights pixel-level differences between the two screenshots"
          >
            Pixel Diff
          </button>
        </div>

        <!-- Zoom controls -->
        <div class="zoom-controls">
          <button
            class="ps-btn ps-btn-ghost zoom-btn"
            class:zoom-active={fitMode}
            onclick={setFit}
          >
            Fit
          </button>
          <button
            class="ps-btn ps-btn-ghost zoom-btn"
            class:zoom-active={!fitMode && zoomLevel === 1}
            onclick={() => setZoom(1)}
          >
            100%
          </button>
          <button
            class="ps-btn ps-btn-ghost zoom-btn"
            class:zoom-active={!fitMode && zoomLevel === 2}
            onclick={() => setZoom(2)}
          >
            200%
          </button>
        </div>

        <!-- Export -->
        {#if beforeData || afterData}
          <div class="export-controls">
            {#if beforeData}
              <button
                class="ps-btn ps-btn-ghost"
                onclick={() => downloadImage(beforeData!, 'original')}
              >
                Export Original
              </button>
            {/if}
            {#if afterData}
              <button
                class="ps-btn ps-btn-ghost"
                onclick={() => downloadImage(afterData!, 'optimized')}
              >
                Export Optimized
              </button>
            {/if}
            <a
              class="ps-btn ps-btn-ghost"
              href="{resolve('/waterfall')}"
              title="View network waterfall"
            >
              View Waterfall
            </a>
          </div>
        {/if}
      </div>

      <!-- Image comparison area -->
      <div
        class="comparison-area"
        class:comparison-fit={fitMode}
      >
        {#if hasBoth}
          <ImageDiff
            {beforeSrc}
            {afterSrc}
            {mode}
            zoom={fitMode ? 1 : zoomLevel}
          />
        {:else if beforeData && !afterData}
          <div class="single-image-view">
            <span class="single-label">Original</span>
            <img
              src={beforeSrc}
              alt="Before screenshot"
              class="single-img"
              style={fitMode
                ? 'max-width:100%;height:auto'
                : `width:${(beforeData.viewport_width ?? 1024) * zoomLevel}px`}
            />
          </div>
        {:else if afterData && !beforeData}
          <div class="single-image-view">
            <span class="single-label">Optimized</span>
            <img
              src={afterSrc}
              alt="After screenshot"
              class="single-img"
              style={fitMode
                ? 'max-width:100%;height:auto'
                : `width:${(afterData.viewport_width ?? 1024) * zoomLevel}px`}
            />
          </div>
        {/if}
      </div>

      <!-- Metadata panel -->
      {#if beforeData || afterData}
        <div class="ps-card metadata-panel">
          <h2 class="section-title">Capture Details</h2>
          <div class="metadata-grid">
            {#if beforeData}
              <div class="metadata-col">
                <h3 class="metadata-heading">Original</h3>
                <dl class="metadata-list">
                  <dt>URL</dt>
                  <dd class="mono">{beforeData.url}</dd>
                  <dt>Viewport</dt>
                  <dd>{beforeData.viewport_width} x {beforeData.viewport_height}</dd>
                  <dt>Image size</dt>
                  <dd>{beforeData.viewport_width} x {beforeData.viewport_height}px</dd>
                </dl>
              </div>
            {/if}
            {#if afterData}
              <div class="metadata-col">
                <h3 class="metadata-heading">Optimized</h3>
                <dl class="metadata-list">
                  <dt>URL</dt>
                  <dd class="mono">{afterData.url}</dd>
                  <dt>Viewport</dt>
                  <dd>{afterData.viewport_width} x {afterData.viewport_height}</dd>
                  <dt>Image size</dt>
                  <dd>{afterData.viewport_width} x {afterData.viewport_height}px</dd>
                </dl>
              </div>
            {/if}
          </div>
        </div>
      {/if}
    {:else if !isCapturing}
      <!-- Empty state -->
      <div class="empty-state">
        <div class="ps-card empty-card">
          <p class="empty-text">
            Enter a URL above and capture screenshots to compare original vs.
            optimized versions.
          </p>
          <p class="empty-hint">
            "Original" captures directly from the origin server. "Optimized"
            captures through PageSpeed.
          </p>
        </div>
      </div>
    {/if}
  </div>
{/if}

<style>
  /* -- Page layout -------------------------------------------------------- */

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

  .diff-page {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1400px;
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-lg);
  }

  .page-header {
    margin-bottom: 0;
  }

  .page-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-xs);
  }

  .page-description {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  /* -- Capture controls --------------------------------------------------- */

  .capture-controls {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .capture-url-row {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .url-input {
    max-width: 600px;
  }

  .field-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
  }

  .capture-options-row {
    display: flex;
    align-items: flex-end;
    gap: var(--ps-space-lg);
    flex-wrap: wrap;
  }

  .field-group {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .viewport-select {
    width: 200px;
  }

  .checkbox-label {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
    cursor: pointer;
    min-height: 28px;
  }

  .capture-actions {
    display: flex;
    gap: var(--ps-space-sm);
    flex-wrap: wrap;
  }

  .capture-error {
    margin-top: var(--ps-space-xs);
  }

  /* -- View controls ------------------------------------------------------ */

  .view-controls {
    display: flex;
    align-items: center;
    gap: var(--ps-space-lg);
    flex-wrap: wrap;
  }

  .mode-selector {
    display: flex;
    gap: 0;
    border: 1px solid var(--ps-border-input);
    border-radius: var(--ps-radius-sm);
    overflow: hidden;
  }

  .mode-btn {
    border: none;
    border-radius: 0;
    border-right: 1px solid var(--ps-border-input);
    background: transparent;
    color: var(--ps-fg-secondary);
    padding: var(--ps-space-xs) var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
    min-height: 28px;
  }

  .mode-btn:last-child {
    border-right: none;
  }

  .mode-btn:hover:not(:disabled) {
    background: var(--ps-bg-hover);
  }

  .mode-btn:focus-visible {
    outline: 2px solid var(--ps-accent);
    outline-offset: -2px;
    z-index: 1;
  }

  .mode-active {
    background: var(--ps-accent) !important;
    color: var(--ps-accent-fg) !important;
  }

  .zoom-controls {
    display: flex;
    gap: var(--ps-space-xs);
  }

  .zoom-btn {
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    min-height: 28px;
  }

  .zoom-active {
    background: var(--ps-bg-hover);
    color: var(--ps-fg-primary);
    font-weight: 600;
  }

  .export-controls {
    display: flex;
    gap: var(--ps-space-xs);
    margin-left: auto;
  }

  /* -- Comparison area ---------------------------------------------------- */

  .comparison-area {
    overflow: auto;
    border-radius: var(--ps-radius-md);
    background: var(--ps-bg-secondary);
    min-height: 200px;
  }

  .comparison-fit {
    display: flex;
    justify-content: center;
  }

  .comparison-fit :global(.image-diff) {
    max-width: 100%;
    width: auto !important;
    height: auto !important;
  }

  .comparison-fit :global(.image-diff img),
  .comparison-fit :global(.image-diff canvas) {
    max-width: 100%;
    height: auto !important;
    width: auto !important;
  }

  /* -- Single image view -------------------------------------------------- */

  .single-image-view {
    position: relative;
  }

  .single-label {
    position: absolute;
    top: var(--ps-space-sm);
    left: var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: #fff;
    background: rgba(0, 0, 0, 0.5);
    padding: 2px var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    z-index: 1;
  }

  .single-img {
    display: block;
  }

  /* -- Metadata panel ----------------------------------------------------- */

  .section-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  .metadata-panel {
    font-family: var(--ps-font-family);
  }

  .metadata-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--ps-space-xl);
  }

  .metadata-heading {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-secondary);
    margin: 0 0 var(--ps-space-sm);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .metadata-list {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: var(--ps-space-xs) var(--ps-space-md);
    margin: 0;
  }

  .metadata-list dt {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    white-space: nowrap;
  }

  .metadata-list dd {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
    margin: 0;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .mono {
    font-family: var(--ps-font-mono);
  }

  /* -- Empty state -------------------------------------------------------- */

  .empty-state {
    display: flex;
    justify-content: center;
    padding: var(--ps-space-xl) 0;
  }

  .empty-card {
    max-width: 480px;
    text-align: center;
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

  /* -- Responsive --------------------------------------------------------- */

  @media (max-width: 767px) {
    .diff-page {
      padding: var(--ps-space-md);
    }

    .metadata-grid {
      grid-template-columns: 1fr;
    }

    .capture-options-row {
      flex-direction: column;
      align-items: flex-start;
    }

    .view-controls {
      flex-direction: column;
      align-items: flex-start;
    }

    .export-controls {
      margin-left: 0;
    }
  }
</style>
