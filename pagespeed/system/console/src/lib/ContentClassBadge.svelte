<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Content-class badge (photo / screenshot / illustration / noisy).
  let { contentClass }: { contentClass: string } = $props();
  let label = $derived(
    contentClass.charAt(0).toUpperCase() + contentClass.slice(1),
  );
  let icon = $derived(
    contentClass === "photo"
      ? "\u{1D4AB}"
      : contentClass === "screenshot"
        ? "▣"
        : contentClass === "illustration"
          ? "⭐"
          : contentClass === "noisy"
            ? "≈"
            : "●",
  );
  let cls = $derived(`class-${contentClass.replace(/[^a-z]/g, "")}`);
</script>

<span class="content-class-badge {cls}" aria-label="Content class: {label}">
  <span class="content-class-icon" aria-hidden="true">{icon}</span>
  {label}
</span>

<style>
  .content-class-badge {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    padding: 1px var(--ps-space-sm);
    font-size: var(--ps-font-size-xs);
    font-weight: 500;
    line-height: 1.5;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    white-space: nowrap;
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }
  .content-class-icon {
    font-size: 10px;
    line-height: 1;
  }
  /* Category badges: plain text on the category tint, the category colour
     carried by the border (a status token as text fell below AA on the
     12% tint in the light scheme). */
  .class-photo {
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-warning);
  }
  .class-screenshot {
    background: color-mix(in srgb, var(--ps-primary) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-primary);
  }
  .class-illustration {
    background: color-mix(in srgb, var(--ps-success) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-success);
  }
  .class-noisy {
    background: color-mix(in srgb, var(--ps-error) 12%, var(--ps-bg));
    color: var(--ps-text);
    border-color: var(--ps-error);
  }
</style>
