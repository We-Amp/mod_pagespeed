<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { tick } from 'svelte';
  import type { Snippet } from 'svelte';

  let {
    text,
    children,
  }: {
    /** Tooltip text (supports line breaks). */
    text: string;
    /** Wrapped content (slot). */
    children: Snippet;
  } = $props();

  let visible = $state(false);
  let wrapperEl: HTMLSpanElement | undefined = $state();
  let tooltipEl: HTMLDivElement | undefined = $state();
  let tooltipStyle = $state('');

  const tooltipId = `ps-tooltip-${Math.random().toString(36).slice(2, 9)}`;

  function positionTooltip() {
    if (!wrapperEl || !tooltipEl) return;
    const rect = wrapperEl.getBoundingClientRect();
    const tipRect = tooltipEl.getBoundingClientRect();
    const gap = 6;

    // Prefer above.
    let top = rect.top - tipRect.height - gap;
    let left = rect.left + rect.width / 2 - tipRect.width / 2;

    // Flip below if clipped at top.
    if (top < 4) {
      top = rect.bottom + gap;
    }
    // Clamp horizontally.
    if (left < 4) left = 4;
    if (left + tipRect.width > window.innerWidth - 4) {
      left = window.innerWidth - tipRect.width - 4;
    }

    tooltipStyle = `top:${top}px;left:${left}px`;
  }

  async function show() {
    visible = true;
    await tick();
    positionTooltip();
  }

  function hide() {
    visible = false;
  }

  function handleKeydown(e: KeyboardEvent) {
    if (e.key === 'Escape' && visible) {
      hide();
    }
  }
</script>

<span
  class="tooltip-trigger"
  bind:this={wrapperEl}
  onmouseenter={show}
  onmouseleave={hide}
  onfocusin={show}
  onfocusout={hide}
  onkeydown={handleKeydown}
  aria-describedby={visible ? tooltipId : undefined}
>
  {@render children()}
</span>

{#if visible}
  <div
    class="tooltip-portal"
    id={tooltipId}
    role="tooltip"
    style={tooltipStyle}
    bind:this={tooltipEl}
  >{text}</div>
{/if}

<style>
  .tooltip-trigger {
    display: inline;
  }

  .tooltip-portal {
    position: fixed;
    z-index: 10000;
    max-width: 320px;
    min-width: 120px;
    padding: var(--ps-space-xs) var(--ps-space-sm);
    background: var(--ps-bg-tertiary, var(--ps-bg-secondary));
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
    line-height: 1.4;
    white-space: pre-line;
    pointer-events: none;
  }
</style>
