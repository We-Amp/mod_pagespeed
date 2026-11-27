<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    format,
  }: {
    format: string;
  } = $props();

  let normalized = $derived((format ?? '').toUpperCase());
</script>

<span
  class="format-badge format-{(format ?? '').toLowerCase()}"
  aria-label="Image format: {normalized}"
>
  {normalized}
</span>

<style>
  .format-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    letter-spacing: 0.02em;
    /* Default: gray for unknown/original */
    background: var(--ps-bg-tertiary);
    color: var(--ps-fg-secondary);
  }

  /* WebP — blue */
  .format-webp {
    background: var(--ps-quality-webp-bg);
    color: var(--ps-quality-webp-fg);
  }

  /* AVIF — purple */
  .format-avif {
    background: var(--ps-quality-avif-bg);
    color: var(--ps-quality-avif-fg);
  }

  /* JPEG — orange */
  .format-jpeg,
  .format-jpg {
    background: var(--ps-quality-jpeg-bg);
    color: var(--ps-quality-jpeg-fg);
  }

  /* PNG — green */
  .format-png {
    background: var(--ps-quality-png-bg);
    color: var(--ps-quality-png-fg);
  }

  /* SVG — teal */
  .format-svg {
    background: color-mix(in srgb, #0d9488 12%, transparent);
    color: #0d9488;
  }

  /* Original — neutral */
  .format-original {
    background: var(--ps-bg-tertiary);
    color: var(--ps-fg-secondary);
  }
</style>
