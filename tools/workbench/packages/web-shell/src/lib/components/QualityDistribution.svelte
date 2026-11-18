<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import type { QualityDistribution as QDist } from '#lib/quality.ts';

  let {
    distribution,
  }: {
    distribution: QDist;
  } = $props();

  let goodPct = $derived(
    distribution.total > 0
      ? (distribution.good / distribution.total) * 100
      : 0,
  );
  let acceptablePct = $derived(
    distribution.total > 0
      ? (distribution.acceptable / distribution.total) * 100
      : 0,
  );
  let poorPct = $derived(
    distribution.total > 0
      ? (distribution.poor / distribution.total) * 100
      : 0,
  );
</script>

{#if distribution.total > 0}
  <div class="quality-dist" role="img" aria-label="Quality distribution: {distribution.good} good, {distribution.acceptable} acceptable, {distribution.poor} poor">
    <div class="dist-bar">
      {#if goodPct > 0}
        <div
          class="dist-segment dist-good"
          style="width: {goodPct}%"
          title="Good: {distribution.good}"
        ></div>
      {/if}
      {#if acceptablePct > 0}
        <div
          class="dist-segment dist-acceptable"
          style="width: {acceptablePct}%"
          title="Acceptable: {distribution.acceptable}"
        ></div>
      {/if}
      {#if poorPct > 0}
        <div
          class="dist-segment dist-poor"
          style="width: {poorPct}%"
          title="Poor: {distribution.poor}"
        ></div>
      {/if}
    </div>
    <div class="dist-legend">
      <span class="dist-stat">
        <span class="dist-dot dist-dot-good" aria-hidden="true"></span>
        Good ({distribution.good})
      </span>
      <span class="dist-stat">
        <span class="dist-dot dist-dot-acceptable" aria-hidden="true"></span>
        Acceptable ({distribution.acceptable})
      </span>
      <span class="dist-stat">
        <span class="dist-dot dist-dot-poor" aria-hidden="true"></span>
        Poor ({distribution.poor})
      </span>
      <span class="dist-stat dist-stat-muted">
        Avg: {distribution.avg} | Min: {distribution.min} | Max: {distribution.max}
      </span>
    </div>
  </div>
{/if}

<style>
  .quality-dist {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .dist-bar {
    display: flex;
    height: 8px;
    border-radius: 4px;
    overflow: hidden;
    background: var(--ps-border);
  }

  .dist-segment {
    height: 100%;
    min-width: 2px;
    transition: width 0.2s ease;
  }

  .dist-good {
    background: var(--ps-success);
  }

  .dist-acceptable {
    background: var(--ps-warning);
  }

  .dist-poor {
    background: var(--ps-error);
  }

  .dist-legend {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    flex-wrap: wrap;
  }

  .dist-stat {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .dist-stat-muted {
    color: var(--ps-fg-muted);
    font-family: var(--ps-font-mono);
  }

  .dist-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
  }

  .dist-dot-good {
    background: var(--ps-success);
  }

  .dist-dot-acceptable {
    background: var(--ps-warning);
  }

  .dist-dot-poor {
    background: var(--ps-error);
  }

  @media (prefers-reduced-motion: reduce) {
    .dist-segment {
      transition: none;
    }
  }
</style>
