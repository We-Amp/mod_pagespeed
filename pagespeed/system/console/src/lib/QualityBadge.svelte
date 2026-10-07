<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { qualityTier, qualityTierLabel } from "$lib/utils/quality";

  let { score, showLabel = true }: { score: number; showLabel?: boolean } = $props();

  // A non-finite score (the daemon is untrusted) renders as 0, never "NaN".
  let safeScore = $derived(Number.isFinite(score) ? score : 0);
  let tier = $derived(qualityTier(safeScore));
  let tooltip = $derived(qualityTierLabel(tier));
  let barWidth = $derived(Math.max(0, Math.min(100, safeScore)));
</script>

<span
  class="quality-badge quality-{tier}"
  title={tooltip}
  role="img"
  aria-label="SSIMULACRA2 score {safeScore.toFixed(1)}: {tooltip}"
>
  <span class="quality-bar-track" aria-hidden="true">
    <span class="quality-bar-fill" style="width: {barWidth}%"></span>
  </span>
  <span class="quality-score">{safeScore.toFixed(1)}</span>
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
    font-size: var(--ps-font-size-xs);
    font-weight: 500;
    line-height: 1.5;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    white-space: nowrap;
    cursor: default;
  }
  /* Plain text on the tier tint; the tier colour is carried by the border
     and the bar fill (a status token as text fell below AA on the 12% tint
     in the light scheme). */
  .quality-good {
    background: color-mix(in srgb, var(--ps-success) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-success);
  }
  .quality-acceptable {
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-warning);
  }
  .quality-poor {
    background: color-mix(in srgb, var(--ps-error) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-error);
  }
  .quality-bar-track {
    display: inline-block;
    width: 32px;
    height: 4px;
    border-radius: 2px;
    overflow: hidden;
    vertical-align: middle;
    background: var(--ps-border);
  }
  .quality-good .quality-bar-fill { background: var(--ps-success); }
  .quality-acceptable .quality-bar-fill { background: var(--ps-warning); }
  .quality-poor .quality-bar-fill { background: var(--ps-error); }
  .quality-bar-fill {
    display: block;
    height: 100%;
    border-radius: 2px;
  }
  .quality-score {
    font-family: var(--ps-font-mono);
    font-weight: 600;
  }
  .quality-label {
    font-weight: 400;
  }
</style>
