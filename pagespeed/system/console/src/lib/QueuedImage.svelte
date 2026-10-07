<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // One cached-variant image, loaded through the page's shared limiter and
  // the busy-retry policy: at most two content images load at once, a
  // busy (429) answer is retried at 1 s / 2 s / 4 s, any other failure
  // shows "Preview unavailable". The <img> src is the proxied content URL;
  // bytes never pass through JS. Explicit width/height come from the
  // measured natural size once loaded (the daemon exposes no dimensions).
  import { onDestroy, onMount } from "svelte";
  import {
    loadWithRetry,
    realSleep,
    type Limiter,
    type PreviewOutcome,
  } from "$lib/utils/image-queue";

  let {
    src,
    alt,
    limiter,
    probe,
    imgClass = "",
    imgStyle = "",
    onready,
  }: {
    src: string;
    alt: string;
    limiter: Limiter;
    probe: () => Promise<number>;
    imgClass?: string;
    imgStyle?: string;
    onready?: (img: HTMLImageElement) => void;
  } = $props();

  let attemptNo = $state(0); // a new number mounts a fresh <img>: a real reload
  let showing = $state(false);
  let outcome = $state<PreviewOutcome | null>(null);
  let natural = $state<{ w: number; h: number } | null>(null);
  let destroyed = false;
  let settle: ((loaded: boolean) => void) | null = null;

  function attempt(): Promise<boolean> {
    return new Promise((resolve) => {
      settle = resolve;
      attemptNo += 1;
      showing = true;
    });
  }

  function onload(event: Event) {
    const img = event.currentTarget as HTMLImageElement;
    natural = { w: img.naturalWidth, h: img.naturalHeight };
    onready?.(img);
    settle?.(true);
    settle = null;
  }

  function onerror() {
    showing = false;
    settle?.(false);
    settle = null;
  }

  async function run() {
    outcome = null;
    const release = await limiter.acquire();
    try {
      if (destroyed) return;
      outcome = await loadWithRetry(attempt, probe, realSleep, () => destroyed);
    } finally {
      release();
    }
  }

  onMount(() => {
    void run();
  });
  onDestroy(() => {
    destroyed = true;
    settle?.(false);
    settle = null;
  });
</script>

{#if showing}
  {#key attemptNo}
    <img
      {src}
      {alt}
      class={imgClass}
      style={imgStyle}
      width={natural?.w}
      height={natural?.h}
      draggable="false"
      {onload}
      {onerror}
    />
  {/key}
{/if}
{#if outcome === "busy"}
  <p class="preview-note" role="status">
    The server is busy.
    <button type="button" class="link-button" onclick={() => void run()}>Try again</button>
  </p>
{:else if outcome === "failed"}
  <p class="preview-note" role="status">Preview unavailable.</p>
{:else if !showing}
  <p class="preview-note">Loading preview…</p>
{/if}

<style>
  .preview-note {
    margin: 0;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }
  .link-button {
    padding: 0;
    border: none;
    background: none;
    color: var(--ps-primary);
    font: inherit;
    text-decoration: underline;
    cursor: pointer;
  }
  .link-button:focus-visible {
    outline: 2px solid var(--ps-primary);
    outline-offset: 2px;
  }
</style>
