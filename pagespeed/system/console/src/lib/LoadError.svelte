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
  import { tick } from "svelte";
  import { DOCS_URL } from "$lib/data/product-facts-console";
  import { markProgrammaticFocus } from "$lib/utils/programmatic-focus";
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
  let button = $state<HTMLButtonElement | null>(null);

  async function retry() {
    if (trying || onretry === null) return;
    const pressed = button;
    trying = true;
    try {
      await onretry();
    } catch {
      // The page keeps the failure it shows; nothing to add here.
    } finally {
      trying = false;
    }
    await tick();
    // The read worked and the page replaced this notice: the button that had
    // the focus is gone. Hand the focus to the page's heading, as after a
    // navigation, unless the user has moved it elsewhere meanwhile.
    if (pressed === null || pressed.isConnected) return;
    const active = document.activeElement;
    if (active !== null && active !== document.body) return;
    const heading =
      document.querySelector<HTMLElement>("main [data-page-focus]") ?? document.querySelector<HTMLElement>("main h1");
    if (heading === null) return;
    if (!heading.hasAttribute("tabindex")) heading.setAttribute("tabindex", "-1");
    markProgrammaticFocus(heading);
    heading.focus();
  }
</script>

<div class="notice notice-error load-error" role="alert" data-testid={testid}>
  <p class="notice-title">{text.title}</p>
  <p class="load-error-cause">{text.cause}</p>
  <p class="load-error-actions">
    {#if onretry !== null}
      <button bind:this={button} type="button" class="btn btn-secondary" aria-disabled={trying} onclick={retry}>
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
  /* The frame, the colours and the spacing between children are the shared
     .notice classes; only what is this component's own is here. */
  .load-error {
    margin: var(--ps-space-md) 0;
  }

  /* The title reads at body size; the rest keeps the notice's small text. */
  .load-error .notice-title {
    font-size: var(--ps-font-size-base);
  }

  /* A button and a link side by side. */
  .load-error-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--ps-space-md);
  }

  .load-error-actions a {
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
