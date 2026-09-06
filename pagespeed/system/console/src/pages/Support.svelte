<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Support: the console's single, gentle pointer to support subscriptions.
  // No license state, no activation flow, no nag — a dismissible panel; the
  // dismissal is remembered across page loads (support-panel.ts).
  import {
    PRODUCT_NAME,
    SUPPORT_URL,
    VENDOR,
  } from "$lib/data/product-facts-console";
  import {
    loadSupportDismissed,
    saveSupportDismissed,
  } from "$lib/utils/support-panel";

  let dismissed = $state(loadSupportDismissed());

  function setDismissed(value: boolean) {
    dismissed = value;
    saveSupportDismissed(value);
  }
</script>

<div class="page">
  <div class="header">
    <h1>Support</h1>
  </div>

  {#if !dismissed}
    <div class="support-panel" data-testid="support-panel">
      <button
        class="dismiss"
        onclick={() => setDismissed(true)}
        aria-label="Dismiss support panel"
      >&times;</button>
      <p class="support-text">
        {PRODUCT_NAME} is developed and maintained by {VENDOR}. A support
        subscription funds that work; the software is fully functional without
        one.
      </p>
      <a
        href={SUPPORT_URL}
        target="_blank"
        rel="noopener noreferrer"
        class="support-link"
      >View support subscriptions &rarr;</a>
    </div>
  {:else}
    <p class="dismissed-note">
      The support panel is hidden.
      <button class="show-again" onclick={() => setDismissed(false)}>
        Show again
      </button>
    </p>
  {/if}
</div>

<style>
  .page {
    max-width: 680px;
  }

  .header {
    margin-bottom: var(--ps-space-lg);
  }

  h1 {
    margin: 0;
  }

  .support-panel {
    position: relative;
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .dismiss {
    position: absolute;
    top: var(--ps-space-sm);
    right: var(--ps-space-sm);
    background: none;
    border: none;
    color: var(--ps-text-tertiary);
    font-size: var(--ps-font-size-lg);
    line-height: 1;
    cursor: pointer;
    padding: var(--ps-space-xs);
    border-radius: var(--ps-border-radius);
  }

  .dismiss:hover {
    color: var(--ps-text);
    background: var(--ps-surface-hover);
  }

  .support-text {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    line-height: 1.5;
    margin-bottom: var(--ps-space-md);
    padding-right: var(--ps-space-lg);
  }

  .support-link {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
    text-decoration: none;
    font-weight: 500;
  }

  .support-link:hover {
    text-decoration: underline;
  }

  .dismissed-note {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-tertiary);
  }

  .show-again {
    background: none;
    border: none;
    padding: 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
    cursor: pointer;
    text-decoration: underline;
  }
</style>
