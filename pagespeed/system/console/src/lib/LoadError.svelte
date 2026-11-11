<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // A page's first load failed and there is nothing to show yet: what could
  // not be loaded and the likely cause in words, announced at once
  // (role="alert"), with a way to ask again and the transport message folded
  // away. A failed refresh above data already on screen is RefreshNotice's
  // job (polite).
  import { DOCS_URL } from "$lib/UnavailableCard.svelte";
  import { describeLoadError } from "$lib/utils/load-error";

  let {
    error,
    noun,
    onretry = null,
    testid = "load-error",
  }: {
    /** The failure: an error, or its message. */
    error: unknown;
    /** What could not be loaded, completing "Could not load ...": "the statistics". */
    noun: string;
    /** Asks again; without it there is no "Try again". */
    onretry?: (() => unknown) | null;
    testid?: string;
  } = $props();

  let text = $derived(describeLoadError(error, noun));
  let trying = $state(false);

  async function retry() {
    if (trying || onretry === null) return;
    trying = true;
    try {
      await onretry();
    } catch {
      // The page keeps the failure it shows; nothing to add here.
    } finally {
      trying = false;
    }
  }
</script>

<div class="notice notice--error load-error" role="alert" data-testid={testid}>
  <p class="notice-title">{text.title}</p>
  <p class="notice-body">{text.cause}</p>
  <p class="notice-actions">
    {#if onretry !== null}
      <button type="button" class="btn btn-secondary" aria-disabled={trying} onclick={retry}>
        {trying ? "Trying again..." : "Try again"}
      </button>
    {/if}
    <a href={DOCS_URL}>About the admin console</a>
  </p>
  {#if text.raw !== null}
    <details class="load-error-details">
      <summary>Technical details</summary>
      <p class="load-error-raw"><code>{text.raw}</code></p>
    </details>
  {/if}
</div>

<style>
  /* Layout only, until the shared notice styles cover it. Text stays in
     --ps-text; the colour is on the left border. */
  .load-error {
    max-width: var(--ps-card-max);
    margin: var(--ps-space-md) 0;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-left: 3px solid var(--ps-error);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
    overflow-wrap: anywhere;
  }

  .load-error p {
    margin: 0;
  }

  .notice-title {
    font-weight: 600;
  }

  .notice-body,
  .load-error-details {
    margin-top: var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
  }

  .load-error .notice-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--ps-space-md);
    margin-top: var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
  }

  .notice-actions a {
    color: var(--ps-primary);
  }

  .load-error-details summary {
    cursor: pointer;
    color: var(--ps-text-secondary);
  }

  .load-error-raw code {
    font-family: var(--ps-font-mono);
  }
</style>
