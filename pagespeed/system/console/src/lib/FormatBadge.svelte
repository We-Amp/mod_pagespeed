<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Variant format badge, ported from the 2.0 dashboard onto this
  // console's theme tokens (color-mix tints; AA text-token foregrounds).
  let { format }: { format: string } = $props();
  let normalized = $derived((format ?? "").toUpperCase() || "—");
  // Each known format has its category tint; an unknown one (or the
  // original) stays neutral.
  const TINT: Record<string, string> = {
    webp: "badge-tint-primary",
    avif: "badge-tint-secondary",
    jpeg: "badge-tint-warning",
    jpg: "badge-tint-warning",
    png: "badge-tint-ok",
  };
  let cls = $derived(TINT[(format ?? "").toLowerCase().replace(/[^a-z0-9]/g, "")] ?? "");
</script>

<span class="badge format-badge {cls}" aria-label="Image format: {normalized}">{normalized}</span>

<style>
  .format-badge {
    font-family: var(--ps-font-mono);
    letter-spacing: 0.02em;
  }
</style>
