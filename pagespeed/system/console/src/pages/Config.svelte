<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import type { ConfigResponse } from "$lib/api/types";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);

  let data = $state<ConfigResponse | null>(null);
  let error = $state<string | null>(null);
  let loading = $state(true);

  async function fetchConfig() {
    loading = true;
    error = null;
    try {
      data = await api.getConfig();
    } catch (err) {
      error = err instanceof Error ? err.message : String(err);
    } finally {
      loading = false;
    }
  }

  // Fetch on mount.
  fetchConfig();

  let configText = $derived(
    data?.config ?? (data as Record<string, unknown>)?.raw ?? "",
  );
</script>

<div class="page">
  <div class="header">
    <h1>Configuration</h1>
    <button class="btn btn-primary" onclick={fetchConfig} disabled={loading}>
      {loading ? "Loading..." : "Refresh"}
    </button>
  </div>

  {#if loading && !data}
    <p class="loading">Loading configuration...</p>
  {:else if error}
    <div class="error-box">
      <p class="error">{error}</p>
      <button class="btn btn-secondary" onclick={fetchConfig}>Retry</button>
    </div>
  {:else if configText}
    <div class="config-wrapper">
      <pre class="config-block">{configText}</pre>
    </div>
  {:else}
    <p class="empty">No configuration data available.</p>
  {/if}
</div>

<style>
  .page {
    max-width: 960px;
  }

  .header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  h1 {
    margin: 0;
  }

  .config-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
    max-height: 80vh;
  }

  .config-block {
    padding: var(--ps-space-md);
    margin: 0;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    line-height: 1.6;
    white-space: pre-wrap;
    word-break: break-word;
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
  }

  .error-box {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    padding: var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-error);
    border-radius: var(--ps-border-radius);
  }

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
  }

  .btn:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  .btn-primary {
    background: var(--ps-primary);
    color: var(--ps-text-inverse);
    border-color: var(--ps-primary);
  }

  .btn-primary:hover:not(:disabled) {
    background: var(--ps-primary-hover);
  }

  .btn-secondary {
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .btn-secondary:hover {
    background: var(--ps-surface-hover);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
    margin: 0;
  }

  .empty {
    color: var(--ps-text-tertiary);
  }
</style>
