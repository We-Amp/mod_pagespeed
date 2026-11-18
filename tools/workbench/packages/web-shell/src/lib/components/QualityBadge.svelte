<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { qualityTier, qualityTierLabel } from '#lib/quality.ts';

  let {
    score,
    showLabel = true,
  }: {
    score: number;
    showLabel?: boolean;
  } = $props();

  let tier = $derived(qualityTier(score));
  let tooltip = $derived(qualityTierLabel(tier));
  let barWidth = $derived(Math.max(0, Math.min(100, score)));
</script>

<span
  class="quality-badge"
  class:quality-good={tier === 'good'}
  class:quality-acceptable={tier === 'acceptable'}
  class:quality-poor={tier === 'poor'}
  title={tooltip}
  role="img"
  aria-label="SSIMULACRA2 score {score.toFixed(1)}: {tooltip}"
>
  <span class="quality-bar-track" aria-hidden="true">
    <span class="quality-bar-fill" style="width: {barWidth}%"></span>
  </span>
  <span class="quality-score">{score.toFixed(1)}</span>
  {#if showLabel}
    <span class="quality-label">SSIM2</span>
  {/if}
</span>

<style>
  .quality-badge {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    cursor: default;
  }

  .quality-good {
    background: var(--ps-success-bg);
    color: var(--ps-success);
  }

  .quality-acceptable {
    background: var(--ps-warning-bg);
    color: var(--ps-warning);
  }

  .quality-poor {
    background: var(--ps-error-bg);
    color: var(--ps-error);
  }

  .quality-bar-track {
    display: inline-block;
    width: 32px;
    height: 4px;
    border-radius: 2px;
    overflow: hidden;
    vertical-align: middle;
  }

  .quality-good .quality-bar-track {
    background: color-mix(in srgb, var(--ps-success) 20%, transparent);
  }

  .quality-acceptable .quality-bar-track {
    background: color-mix(in srgb, var(--ps-warning) 20%, transparent);
  }

  .quality-poor .quality-bar-track {
    background: color-mix(in srgb, var(--ps-error) 20%, transparent);
  }

  .quality-bar-fill {
    display: block;
    height: 100%;
    border-radius: 2px;
    transition: width 0.2s ease;
  }

  .quality-good .quality-bar-fill {
    background: var(--ps-success);
  }

  .quality-acceptable .quality-bar-fill {
    background: var(--ps-warning);
  }

  .quality-poor .quality-bar-fill {
    background: var(--ps-error);
  }

  .quality-score {
    font-family: var(--ps-font-mono);
    font-weight: 600;
  }

  .quality-label {
    font-weight: 400;
    opacity: 0.8;
  }

  @media (prefers-reduced-motion: reduce) {
    .quality-bar-fill {
      transition: none;
    }
  }
</style>
