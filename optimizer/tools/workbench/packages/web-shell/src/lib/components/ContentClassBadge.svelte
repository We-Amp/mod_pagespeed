<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    contentClass,
  }: {
    contentClass: string;
  } = $props();

  let label = $derived(
    contentClass.charAt(0).toUpperCase() + contentClass.slice(1),
  );

  /** Unicode icon per content class, avoiding emoji per code style rules. */
  let icon = $derived(
    contentClass === 'photo'
      ? '\u{1D4AB}' /* script P */
      : contentClass === 'screenshot'
        ? '\u25A3' /* square with fill */
        : contentClass === 'illustration'
          ? '\u2B50' /* star (outline) */
          : contentClass === 'noisy'
            ? '\u2248' /* approximately equal */
            : '\u25CF' /* filled circle */,
  );
</script>

<span
  class="content-class-badge content-class-{contentClass}"
  aria-label="Content class: {label}"
>
  <span class="content-class-icon" aria-hidden="true">{icon}</span>
  {label}
</span>

<style>
  .content-class-badge {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    /* Default styling */
    background: var(--ps-bg-tertiary);
    color: var(--ps-fg-secondary);
  }

  .content-class-icon {
    font-size: 10px;
    line-height: 1;
  }

  /* Photo — warm amber/brown */
  .content-class-photo {
    background: var(--ps-quality-photo-bg);
    color: var(--ps-quality-photo-fg);
  }

  /* Screenshot — cool blue-gray */
  .content-class-screenshot {
    background: var(--ps-quality-screenshot-bg);
    color: var(--ps-quality-screenshot-fg);
  }

  /* Illustration — teal/green */
  .content-class-illustration {
    background: var(--ps-quality-illustration-bg);
    color: var(--ps-quality-illustration-fg);
  }

  /* Noisy — muted red/pink */
  .content-class-noisy {
    background: var(--ps-quality-noisy-bg);
    color: var(--ps-quality-noisy-fg);
  }
</style>
