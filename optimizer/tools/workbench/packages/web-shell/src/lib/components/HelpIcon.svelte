<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onMount, onDestroy, tick } from 'svelte';

  let {
    tooltip,
  }: {
    tooltip: string;
  } = $props();

  let expanded = $state(false);
  let hovered = $state(false);
  let wrapperEl: HTMLSpanElement | undefined = $state();
  let tooltipEl: HTMLDivElement | undefined = $state();
  let tooltipStyle = $state('');

  /** Show tooltip on click (persists) or hover (transient). */
  let visible = $derived(expanded || hovered);

  function positionTooltip() {
    if (!wrapperEl || !tooltipEl) return;
    const rect = wrapperEl.getBoundingClientRect();
    const tipRect = tooltipEl.getBoundingClientRect();
    const gap = 6;

    // Prefer above the icon.
    let top = rect.top - tipRect.height - gap;
    let left = rect.left + rect.width / 2 - tipRect.width / 2;

    // If it would go above the viewport, flip below.
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

  async function toggle(event: MouseEvent) {
    event.stopPropagation();
    expanded = !expanded;
    if (expanded) {
      await tick();
      positionTooltip();
    }
  }

  function handleKeydown(event: KeyboardEvent) {
    if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      expanded = !expanded;
    } else if (event.key === 'Escape') {
      expanded = false;
    }
  }

  async function handleMouseEnter() {
    hovered = true;
    await tick();
    positionTooltip();
  }

  function handleMouseLeave() {
    hovered = false;
  }

  /** Close expanded tooltip when clicking outside. */
  function handleDocumentClick(event: MouseEvent) {
    if (expanded && wrapperEl && !wrapperEl.contains(event.target as Node)) {
      expanded = false;
    }
  }

  onMount(() => {
    if (typeof document !== 'undefined') {
      document.addEventListener('click', handleDocumentClick, true);
    }
  });

  onDestroy(() => {
    if (typeof document !== 'undefined') {
      document.removeEventListener('click', handleDocumentClick, true);
    }
  });
</script>

<span class="help-icon-wrapper" bind:this={wrapperEl}>
  <button
    class="help-icon"
    onclick={toggle}
    onkeydown={handleKeydown}
    onmouseenter={handleMouseEnter}
    onmouseleave={handleMouseLeave}
    aria-label="Help: {tooltip}"
    aria-expanded={expanded}
  >
    <svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
      <circle cx="8" cy="8" r="7" stroke="currentColor" fill="none" stroke-width="1.5"/>
      <text x="8" y="12" text-anchor="middle" font-size="10" font-weight="600">?</text>
    </svg>
  </button>
</span>

{#if visible}
  <div
    class="help-tooltip-portal"
    role="tooltip"
    style={tooltipStyle}
    bind:this={tooltipEl}
  >{tooltip}</div>
{/if}

<style>
  .help-icon-wrapper {
    position: relative;
    display: inline-flex;
    align-items: center;
    vertical-align: middle;
  }

  .help-icon {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    width: 16px;
    height: 16px;
    padding: 0;
    margin: 0 0 0 2px;
    border: none;
    background: none;
    color: var(--ps-fg-muted);
    cursor: pointer;
    border-radius: 50%;
    flex-shrink: 0;
    transition: color 0.1s ease;
  }

  .help-icon:hover,
  .help-icon:focus-visible {
    color: var(--ps-accent);
  }

  .help-icon:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: 1px;
  }

  /* Portal tooltip — rendered outside the component's DOM tree
     but scoped styles still apply because Svelte adds the hash. */
  .help-tooltip-portal {
    position: fixed;
    z-index: 10000;
    max-width: 320px;
    min-width: 160px;
    padding: var(--ps-space-xs) var(--ps-space-sm);
    background: var(--ps-bg-tertiary, var(--ps-bg-secondary));
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
    line-height: 1.4;
    white-space: normal;
    pointer-events: none;
  }
</style>
