<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Variant format badge, ported from the 2.0 dashboard onto this
  // console's theme tokens (color-mix tints; AA text-token foregrounds).
  let { format }: { format: string } = $props();
  let normalized = $derived((format ?? "").toUpperCase() || "—");
  let cls = $derived(`format-${(format ?? "").toLowerCase().replace(/[^a-z0-9]/g, "")}`);
</script>

<span class="format-badge {cls}" aria-label="Image format: {normalized}">{normalized}</span>

<style>
  .format-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    line-height: 1.5;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    white-space: nowrap;
    letter-spacing: 0.02em;
    /* Unknown/original: neutral. */
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }
  /* Category badges: plain text on the category tint, the category colour
     carried by the border (a status token as text fell below AA on the
     12% tint in the light scheme). */
  .format-webp {
    background: color-mix(in srgb, var(--ps-primary) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-primary);
  }
  .format-avif {
    background: color-mix(in srgb, var(--ps-secondary) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-secondary);
  }
  .format-jpeg,
  .format-jpg {
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-warning);
  }
  .format-png {
    background: color-mix(in srgb, var(--ps-success) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-success);
  }
</style>
