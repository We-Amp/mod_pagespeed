<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import HelpIcon from './HelpIcon.svelte';

  let {
    label,
    value = $bindable(),
    min = 0,
    max = 100,
    step = 1,
    warningBelow,
    warningAbove,
    changed = false,
    disabled = false,
    tooltip = '',
  }: {
    label: string;
    value: number;
    min?: number;
    max?: number;
    step?: number;
    warningBelow?: number;
    warningAbove?: number;
    changed?: boolean;
    disabled?: boolean;
    tooltip?: string;
  } = $props();

  let showWarning = $derived(
    (warningBelow !== undefined && value < warningBelow) ||
      (warningAbove !== undefined && value > warningAbove),
  );

  let warningMessage = $derived(() => {
    if (warningBelow !== undefined && value < warningBelow) {
      return `Value below ${warningBelow} may produce visible artifacts`;
    }
    if (warningAbove !== undefined && value > warningAbove) {
      return `Value above ${warningAbove} may yield diminishing returns`;
    }
    return '';
  });

  /** Percentage of the range for the filled track. */
  let fillPercent = $derived(((value - min) / (max - min)) * 100);
</script>

<div class="quality-slider" class:quality-slider-warning={showWarning}>
  <div class="slider-header">
    <span class="slider-label">
      {label}
      {#if changed}
        <span class="changed-dot" title="Modified from server value"></span>
      {/if}
      {#if tooltip}
        <HelpIcon {tooltip} />
      {/if}
    </span>
    <span
      class="slider-value"
      class:slider-value-warning={showWarning}
    >
      {value}
    </span>
  </div>

  <div class="slider-track-wrapper">
    <input
      type="range"
      class="ps-slider"
      class:slider-warning-track={showWarning}
      bind:value
      {min}
      {max}
      {step}
      {disabled}
      aria-label={label}
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuenow={value}
      style="--fill-percent: {fillPercent}%"
    />
  </div>

  {#if showWarning}
    <div class="slider-warning-text" role="status">
      {warningMessage()}
    </div>
  {/if}
</div>

<style>
  .quality-slider {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .slider-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-sm);
  }

  .slider-label {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
    cursor: default;
  }

  .changed-dot {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--ps-accent);
    flex-shrink: 0;
  }

  .slider-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-primary);
    min-width: 2.5ch;
    text-align: right;
  }

  .slider-value-warning {
    color: var(--ps-warning);
  }

  .slider-track-wrapper {
    position: relative;
  }

  /* Override the slider track color for warnings */
  .slider-warning-track {
    background: linear-gradient(
      to right,
      var(--ps-warning) var(--fill-percent, 0%),
      var(--ps-border) var(--fill-percent, 0%)
    );
  }

  .slider-warning-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-warning);
    line-height: 1.3;
  }
</style>
