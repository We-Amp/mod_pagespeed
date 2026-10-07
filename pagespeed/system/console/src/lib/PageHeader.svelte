<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import type { Snippet } from "svelte";
  import { formatIsoTitle } from "./utils/format";
  import { updatedText } from "./utils/page-header";

  interface RefreshControls {
    autoRefresh: boolean;
    intervalMs: number;
    onToggle: () => void;
    onRefresh: () => void;
  }

  interface Props {
    title: string;
    subtitle?: string;
    updatedAt?: number | null;
    refresh?: RefreshControls;
    toolbar?: Snippet;
  }

  let { title, subtitle, updatedAt = null, refresh, toolbar }: Props = $props();
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
        <button
          type="button"
          class="btn btn-secondary"
          aria-pressed={refresh.autoRefresh}
          onclick={refresh.onToggle}
        >
          Auto-refresh · {Math.max(1, Math.round(refresh.intervalMs / 1000))} s
        </button>
        <button
          type="button"
          class="btn btn-icon"
          aria-label="Refresh"
          title="Refresh"
          onclick={refresh.onRefresh}
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
        <span
          class="page-updated field-text"
          data-testid="page-updated"
          title={updatedAt === null ? undefined : formatIsoTitle(updatedAt)}
        >{updatedText(updatedAt, Date.now(), refresh.autoRefresh)}</span>
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
    font-size: var(--ps-font-size-3xl);
    font-weight: 600;
  }
  .page-subtitle {
    margin: var(--ps-space-xs) 0 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
  .page-updated {
    font-size: var(--ps-font-size-xs);
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
