<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { formatBytes } from '@pagespeed/api-client';
  import { formatSavingsPercent, savingsPercent } from '#lib/quality.ts';

  let {
    originalSize,
    optimizedSize,
    format = '',
  }: {
    originalSize: number;
    optimizedSize: number;
    format?: string;
  } = $props();

  let pct = $derived(savingsPercent(originalSize, optimizedSize));
  let savingsLabel = $derived(formatSavingsPercent(originalSize, optimizedSize));
  let optimizedRatio = $derived(
    originalSize > 0
      ? Math.max(2, Math.min(100, (optimizedSize / originalSize) * 100))
      : 100,
  );
  let hasSavings = $derived(pct > 0);
</script>

<div
  class="size-comparison"
  role="img"
  aria-label="Size: {formatBytes(originalSize)} original, {formatBytes(optimizedSize)} optimized ({savingsLabel})"
>
  <div class="size-bar-track">
    <div
      class="size-bar-original"
      style="width: 100%"
      aria-hidden="true"
    ></div>
    <div
      class="size-bar-optimized"
      class:size-bar-savings={hasSavings}
      style="width: {optimizedRatio}%"
      aria-hidden="true"
    ></div>
  </div>
  <div class="size-labels">
    <span class="size-detail">
      <span class="size-original">{formatBytes(originalSize)}</span>
      <span class="size-arrow" aria-hidden="true">&#x2192;</span>
      <span class="size-optimized">{formatBytes(optimizedSize)}</span>
      {#if format}
        <span class="size-format">{format}</span>
      {/if}
    </span>
    <span
      class="size-savings"
      class:savings-positive={hasSavings}
      class:savings-negative={pct < 0}
    >
      {savingsLabel}
    </span>
  </div>
</div>

<style>
  .size-comparison {
    display: flex;
    flex-direction: column;
    gap: 3px;
    min-width: 140px;
  }

  .size-bar-track {
    position: relative;
    height: 6px;
    border-radius: 3px;
    overflow: hidden;
  }

  .size-bar-original {
    position: absolute;
    inset: 0;
    background: var(--ps-border);
    border-radius: 3px;
  }

  .size-bar-optimized {
    position: absolute;
    top: 0;
    left: 0;
    height: 100%;
    background: var(--ps-fg-muted);
    border-radius: 3px;
    transition: width 0.2s ease;
    z-index: 1;
  }

  .size-bar-savings {
    background: var(--ps-accent);
  }

  .size-labels {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-sm);
  }

  .size-detail {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .size-original {
    color: var(--ps-fg-muted);
  }

  .size-arrow {
    color: var(--ps-fg-muted);
    font-size: 10px;
  }

  .size-optimized {
    color: var(--ps-fg-primary);
    font-weight: 500;
  }

  .size-format {
    color: var(--ps-fg-muted);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
  }

  .size-savings {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    white-space: nowrap;
    color: var(--ps-fg-muted);
  }

  .savings-positive {
    color: var(--ps-success);
  }

  .savings-negative {
    color: var(--ps-error);
  }

  @media (prefers-reduced-motion: reduce) {
    .size-bar-optimized {
      transition: none;
    }
  }
</style>
