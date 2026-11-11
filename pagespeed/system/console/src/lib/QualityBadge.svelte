<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { qualityTier, qualityTierLabel, type QualityTier } from "$lib/utils/quality";

  // The tier's tint; the bar fill repeats the tier colour.
  const TINT: Record<QualityTier, string> = {
    good: "badge-tint-ok",
    acceptable: "badge-tint-warning",
    poor: "badge-tint-error",
  };

  let { score, showLabel = true }: { score: number; showLabel?: boolean } = $props();

  // A non-finite score (the daemon is untrusted) renders as 0, never "NaN".
  let safeScore = $derived(Number.isFinite(score) ? score : 0);
  let tier = $derived(qualityTier(safeScore));
  let tooltip = $derived(qualityTierLabel(tier));
  let barWidth = $derived(Math.max(0, Math.min(100, safeScore)));
</script>

<span
  class="badge quality-badge quality-{tier} {TINT[tier]}"
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
    font-weight: 500;
    cursor: default;
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
