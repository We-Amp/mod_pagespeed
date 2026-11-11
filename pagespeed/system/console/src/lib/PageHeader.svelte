<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onDestroy, type Snippet } from "svelte";
  import { setAutoRefresh } from "./api/auto-refresh";
  import { pollers } from "./api/poller";
  import { clock } from "./now.svelte";
  import { formatIsoTitle } from "./utils/format";
  import { REFRESH_RESULT_MS, refreshResultText, updatedText } from "./utils/page-header";

  /** Without `onToggle` the page refreshes only when asked (Configuration). */
  interface RefreshControls {
    autoRefresh?: boolean;
    intervalMs?: number;
    onToggle?: () => void;
    /** May resolve to the poller's outcome, so a failed refresh is said. */
    onRefresh: () => unknown;
  }

  interface Props {
    title: string;
    subtitle?: string;
    updatedAt?: number | null;
    refresh?: RefreshControls;
    toolbar?: Snippet;
  }

  let { title, subtitle, updatedAt = null, refresh, toolbar }: Props = $props();

  let busy = $state(false);
  // What the last refresh the viewer asked for came to; never a timer's.
  let result = $state("");
  let resultTimer: ReturnType<typeof setTimeout> | undefined;

  async function refreshNow(): Promise<void> {
    if (!refresh) return;
    clearTimeout(resultTimer);
    result = "";
    busy = true;
    let outcome: unknown;
    try {
      outcome = await refresh.onRefresh();
    } catch {
      outcome = "failed";
    }
    busy = false;
    result = refreshResultText(outcome);
    resultTimer = setTimeout(() => (result = ""), REFRESH_RESULT_MS);
  }

  function toggle(): void {
    if (!refresh?.onToggle) return;
    const on = !refresh.autoRefresh;
    refresh.onToggle();
    setAutoRefresh(on);
  }

  // The refresh shortcut and "Retry now" refresh every poller on screen:
  // through this member they get the same feedback as the button.
  const unregister = pollers.add({ refresh: refreshNow });
  onDestroy(() => {
    unregister();
    clearTimeout(resultTimer);
  });
</script>

<header class="page-header">
  <div class="page-title">
    <h1>{title}</h1>
    {#if subtitle}<p class="page-subtitle">{subtitle}</p>{/if}
  </div>
  {#if refresh || toolbar}
    <div class="page-toolbar control-row">
      {#if toolbar}{@render toolbar()}{/if}
      {#if refresh}
        <div class="control-group">
        {#if refresh.onToggle}
          <!-- The state is in the words and in aria-pressed; the name stays the same. -->
          <button
            type="button"
            class="btn refresh-toggle"
            aria-pressed={refresh.autoRefresh}
            title={refresh.autoRefresh ? "Pause on every page, also after a reload" : "Refresh automatically again"}
            onclick={toggle}
          >
            <span class="refresh-dot" aria-hidden="true"></span>Auto-refresh<span aria-hidden="true"
              >{refresh.autoRefresh ? " on" : " off"}</span
            > · {Math.max(1, Math.round((refresh.intervalMs ?? 0) / 1000))} s
          </button>
        {/if}
        <button
          type="button"
          class="btn btn-icon"
          aria-label="Refresh"
          aria-busy={busy}
          title="Refresh now"
          onclick={refreshNow}
        >
          <svg
            viewBox="0 0 16 16"
            width="16"
            height="16"
            aria-hidden="true"
            fill="none"
            stroke="currentColor"
            stroke-width="1.5"
            stroke-linecap="round"
            stroke-linejoin="round"
          >
            <path d="M13.5 8a5.5 5.5 0 1 1-1.61-3.89" />
            <path d="M13.5 2.5v2.6h-2.6" />
          </svg>
        </button>
        <!-- The stamp counts once a second: keep it outside any live region. -->
        <span
          class="page-updated field-text"
          data-testid="page-updated"
          title={updatedAt === null ? undefined : formatIsoTitle(updatedAt)}
        >{updatedText(updatedAt, clock.now, refresh.autoRefresh ?? true, refresh.intervalMs)}</span>
        <span class="field-text page-updated" role="status" data-testid="refresh-result">{result}</span>
        </div>
      {/if}
    </div>
  {/if}
</header>

<style>
  .page-header {
    display: flex;
    flex-wrap: wrap;
    align-items: flex-start;
    justify-content: space-between;
    gap: var(--ps-space-sm) var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
  }
  .page-title h1 {
    margin: 0;
    font-size: var(--ps-type-title-size);
    font-weight: var(--ps-type-title-weight);
    letter-spacing: var(--ps-type-title-tracking);
  }
  .page-subtitle {
    margin: var(--ps-space-xs) 0 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
  .page-updated {
    font-size: var(--ps-font-size-xs);
  }
  /* Auto-refresh is a setting, not the page's action: no accent when on. */
  .refresh-dot {
    width: 0.5rem;
    height: 0.5rem;
    border: 1px solid currentColor;
    border-radius: 50%;
  }
  .refresh-toggle[aria-pressed="true"] .refresh-dot {
    border-color: var(--ps-success);
    background: var(--ps-success);
  }
  [aria-busy="true"] {
    cursor: progress;
    opacity: 0.6;
  }
  /* On a desktop a toolbar that wraps below the title, or onto a second
     line, stays flush right, under the host selector. (768px: the
     console's phone breakpoint.) */
  @media (min-width: 769px) {
    .page-toolbar {
      margin-left: auto;
      justify-content: flex-end;
    }
  }
</style>
