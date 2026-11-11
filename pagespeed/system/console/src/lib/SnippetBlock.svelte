<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // A setting to paste, one tab per web server syntax, with Copy. The tab is
  // preselected (utils/dialect.ts) and a choice moves every snippet on the
  // page. Copy falls back to selecting the text when the browser refuses the
  // clipboard, which it does for a console opened over plain HTTP.
  import { onDestroy, tick } from "svelte";
  import Tabs from "./Tabs.svelte";
  import {
    DIALECTS,
    chooseDialect,
    detectedDialect,
    onDialectChoice,
    pickDialect,
    type Dialect,
  } from "./utils/dialect";

  let { id, snippets }: { id: string; snippets: Partial<Record<Dialect, string>> } = $props();

  let tabs = $derived(DIALECTS.filter((d) => snippets[d.id] !== undefined));
  // Bumped on every choice: the choice lives in a plain module, not in runes.
  let choices = $state(0);
  let active = $derived.by(() => {
    void choices;
    const id = pickDialect(tabs.map((d) => d.id));
    return tabs.find((d) => d.id === id);
  });
  // Read once, at mount: findings render after the first answers, which carry the header.
  let detected = $derived(tabs.find((d) => d.id === detectedDialect()));
  let status = $state("");
  let code: HTMLElement | undefined = $state();

  onDestroy(
    onDialectChoice(() => {
      choices++;
      status = "";
    }),
  );

  async function copy() {
    // Emptied first, so a second Copy is announced again.
    status = "";
    await tick();
    try {
      await navigator.clipboard.writeText((active && snippets[active.id]) ?? "");
      status = "Copied";
    } catch {
      if (code === undefined) return;
      const selection = getSelection();
      selection?.selectAllChildren(code);
      let copied = false;
      try {
        copied = document.execCommand("copy");
      } catch {
        // Selected is enough.
      }
      if (copied) selection?.removeAllRanges();
      status = copied ? "Copied" : "Selected: copy it with the keyboard";
    }
  }
</script>

{#if active}
  <!-- Off inside the findings' polite region: a tab change is not read out; "Copied" has its own status. -->
  <div class="snippet" aria-live="off" data-testid="snippet">
    <Tabs
      {tabs}
      active={active.id}
      label="Web server"
      idPrefix="snippet-{id}"
      panelId="snippet-{id}-panel"
      onselect={(d) => chooseDialect(d as Dialect)}
    />
    <div id="snippet-{id}-panel" role="tabpanel" aria-labelledby="snippet-{id}-tab-{active.id}">
      <pre bind:this={code} data-testid="snippet-code">{snippets[active.id]}</pre>
      <p class="snippet-actions">
        <button
          type="button"
          class="btn btn-secondary btn-compact"
          data-testid="snippet-copy"
          onclick={copy}
        >Copy</button>
        <span role="status" data-testid="snippet-status">{status}</span>
        {#if detected}
          <span class="muted" data-testid="snippet-detected">From the Server header: {detected.label}</span>
        {/if}
      </p>
    </div>
  </div>
{/if}

<style>
  .snippet {
    margin: 0 0 var(--ps-space-sm);
  }

  .snippet :global(.tabs) {
    margin-bottom: var(--ps-space-xs);
  }

  .snippet :global(.tab) {
    padding: var(--ps-space-xs) var(--ps-space-sm);
  }

  /* Long lines wrap, so the block never scrolls sideways; the copied text is the unwrapped one. */
  pre {
    margin: 0 0 var(--ps-space-xs);
    padding: var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .snippet-actions {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--ps-space-sm);
    margin: 0;
  }
</style>
