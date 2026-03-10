<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import type {
    CacheEntryResponse,
    PurgeResponse,
    PurgeSetResponse,
  } from "$lib/api/types";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const cacheStructure = usePolling(() => api.getCacheStructure(), 30000);

  let activeTab = $state<"structure" | "lookup" | "purge" | "purgeset">("structure");

  // -- Cache Lookup state --
  let lookupUrl = $state("");
  let lookupResult = $state<CacheEntryResponse | null>(null);
  let lookupError = $state<string | null>(null);
  let lookupLoading = $state(false);

  async function doLookup() {
    if (!lookupUrl.trim()) return;
    lookupLoading = true;
    lookupError = null;
    lookupResult = null;
    try {
      lookupResult = await api.getCacheEntry(lookupUrl.trim());
    } catch (err) {
      lookupError = err instanceof Error ? err.message : String(err);
    } finally {
      lookupLoading = false;
    }
  }

  // -- Purge state --
  let purgeUrl = $state("");
  let purgeResult = $state<PurgeResponse | null>(null);
  let purgeError = $state<string | null>(null);
  let purgeLoading = $state(false);

  async function doPurge() {
    if (!purgeUrl.trim()) return;
    purgeLoading = true;
    purgeError = null;
    purgeResult = null;
    try {
      purgeResult = await api.purgeUrl(purgeUrl.trim());
    } catch (err) {
      purgeError = err instanceof Error ? err.message : String(err);
    } finally {
      purgeLoading = false;
    }
  }

  async function doPurgeAll() {
    purgeLoading = true;
    purgeError = null;
    purgeResult = null;
    try {
      purgeResult = await api.purgeUrl("*");
    } catch (err) {
      purgeError = err instanceof Error ? err.message : String(err);
    } finally {
      purgeLoading = false;
    }
  }

  // -- Purge Set state --
  let purgeSetData = $state<PurgeSetResponse | null>(null);
  let purgeSetError = $state<string | null>(null);
  let purgeSetLoading = $state(false);

  async function fetchPurgeSet() {
    purgeSetLoading = true;
    purgeSetError = null;
    try {
      purgeSetData = await api.getPurgeSet();
    } catch (err) {
      purgeSetError = err instanceof Error ? err.message : String(err);
    } finally {
      purgeSetLoading = false;
    }
  }

  let caches = $derived.by(() => {
    const d = cacheStructure.data;
    if (!d) return null;
    if (d.caches && d.caches.length > 0) return d.caches;
    return null;
  });

  let backendStats = $derived.by(() => {
    const d = cacheStructure.data;
    if (!d) return "";
    return d.backend_stats ? String(d.backend_stats) : "";
  });

  let structureFallbackText = $derived.by(() => {
    const d = cacheStructure.data;
    if (!d) return "";
    if (d.caches) return ""; // Using structured display instead.
    return d.structure ??
      (d as Record<string, unknown>)?.raw ?? "";
  });

  // Load purge set when switching to that tab.
  $effect(() => {
    if (activeTab === "purgeset") {
      fetchPurgeSet();
    }
  });
</script>

<div class="page">
  <h1>Caches</h1>

  <div class="tabs">
    <button
      class="tab"
      class:active={activeTab === "structure"}
      onclick={() => (activeTab = "structure")}
    >
      Cache Structure
    </button>
    <button
      class="tab"
      class:active={activeTab === "lookup"}
      onclick={() => (activeTab = "lookup")}
    >
      Cache Lookup
    </button>
    <button
      class="tab"
      class:active={activeTab === "purge"}
      onclick={() => (activeTab = "purge")}
    >
      Purge
    </button>
    <button
      class="tab"
      class:active={activeTab === "purgeset"}
      onclick={() => (activeTab = "purgeset")}
    >
      Purge Set
    </button>
  </div>

  <div class="tab-content">
    {#if activeTab === "structure"}
      <div class="section">
        {#if cacheStructure.loading}
          <p class="loading">Loading cache structure...</p>
        {:else if cacheStructure.error}
          <p class="error">{cacheStructure.error.message}</p>
        {:else if caches}
          <div class="cache-list">
            {#each caches as cache}
              <div class="cache-card">
                <h3 class="cache-name">{cache.name}</h3>
                <pre class="cache-summary">{cache.summary}</pre>
              </div>
            {/each}
          </div>
          {#if backendStats}
            <div class="pre-wrapper">
              <h3 class="cache-name">Backend Stats</h3>
              <pre>{backendStats}</pre>
            </div>
          {/if}
        {:else if structureFallbackText}
          <div class="pre-wrapper">
            <pre>{structureFallbackText}</pre>
          </div>
        {:else}
          <p class="empty">No cache structure data available.</p>
        {/if}
      </div>

    {:else if activeTab === "lookup"}
      <div class="section">
        <form class="form-row" onsubmit={(e) => { e.preventDefault(); doLookup(); }}>
          <label class="form-label" for="lookup-url">URL to look up:</label>
          <input
            id="lookup-url"
            type="text"
            class="form-input"
            placeholder="https://example.com/image.jpg"
            bind:value={lookupUrl}
          />
          <button class="btn btn-primary" type="submit" disabled={lookupLoading || !lookupUrl.trim()}>
            {lookupLoading ? "Looking up..." : "Lookup"}
          </button>
        </form>

        {#if lookupError}
          <div class="result-box result-error">
            <strong>Error:</strong> {lookupError}
          </div>
        {/if}

        {#if lookupResult}
          <div class="result-box" class:result-error={!!lookupResult.error}>
            <h3>Result for: {lookupResult.url || lookupUrl}</h3>
            {#if lookupResult.error}
              <p class="error">{lookupResult.error}</p>
            {:else if lookupResult.value}
              <pre class="result-pre">{lookupResult.value}</pre>
            {:else}
              <p class="empty">No cache entry found for this URL.</p>
            {/if}
          </div>
        {/if}
      </div>

    {:else if activeTab === "purge"}
      <div class="section">
        <form class="form-row" onsubmit={(e) => { e.preventDefault(); doPurge(); }}>
          <label class="form-label" for="purge-url">URL to purge:</label>
          <input
            id="purge-url"
            type="text"
            class="form-input"
            placeholder="https://example.com/image.jpg"
            bind:value={purgeUrl}
          />
          <button class="btn btn-primary" type="submit" disabled={purgeLoading || !purgeUrl.trim()}>
            {purgeLoading ? "Purging..." : "Purge URL"}
          </button>
        </form>

        <div class="purge-all-row">
          <button class="btn btn-danger" onclick={doPurgeAll} disabled={purgeLoading}>
            Purge All (*)
          </button>
          <span class="hint">This invalidates the entire cache.</span>
        </div>

        {#if purgeError}
          <div class="result-box result-error">
            <strong>Error:</strong> {purgeError}
          </div>
        {/if}

        {#if purgeResult}
          <div class="result-box" class:result-success={purgeResult.success} class:result-error={!purgeResult.success}>
            <p>
              {#if purgeResult.success}
                Purge successful.
              {:else}
                Purge failed.
              {/if}
              {#if purgeResult.message}
                {purgeResult.message}
              {/if}
            </p>
          </div>
        {/if}
      </div>

    {:else if activeTab === "purgeset"}
      <div class="section">
        <div class="section-header">
          <button class="btn btn-secondary" onclick={fetchPurgeSet} disabled={purgeSetLoading}>
            {purgeSetLoading ? "Loading..." : "Refresh"}
          </button>
        </div>

        {#if purgeSetError}
          <p class="error">{purgeSetError}</p>
        {:else if purgeSetData}
          {#if purgeSetData.global_invalidation_timestamp_ms}
            <p class="meta">
              Global invalidation timestamp: {new Date(purgeSetData.global_invalidation_timestamp_ms).toLocaleString()}
            </p>
          {/if}

          {#if purgeSetData.purge_set && purgeSetData.purge_set.length > 0}
            <div class="purge-list">
              <table>
                <thead>
                  <tr>
                    <th>#</th>
                    <th>Purged URL</th>
                  </tr>
                </thead>
                <tbody>
                  {#each purgeSetData.purge_set as url, i}
                    <tr>
                      <td class="row-num">{i + 1}</td>
                      <td class="url-cell">{url}</td>
                    </tr>
                  {/each}
                </tbody>
              </table>
            </div>
          {:else if purgeSetData.purge_enabled === false}
            <p class="empty">Cache purging is disabled. Enable it with <code>EnableCachePurge on</code> in your configuration.</p>
          {:else}
            <p class="empty">Purge set is empty.</p>
          {/if}
        {:else if purgeSetLoading}
          <p class="loading">Loading purge set...</p>
        {/if}
      </div>
    {/if}
  </div>
</div>

<style>
  .page {
    max-width: 960px;
  }

  h1 {
    margin-bottom: var(--ps-space-md);
  }

  .tabs {
    display: flex;
    border-bottom: 2px solid var(--ps-border);
    margin-bottom: var(--ps-space-lg);
    gap: 0;
    overflow-x: auto;
  }

  .tab {
    padding: var(--ps-space-sm) var(--ps-space-lg);
    border: none;
    background: none;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    cursor: pointer;
    border-bottom: 2px solid transparent;
    margin-bottom: -2px;
    white-space: nowrap;
  }

  .tab:hover {
    color: var(--ps-text);
    background: var(--ps-surface-hover);
  }

  .tab.active {
    color: var(--ps-primary);
    border-bottom-color: var(--ps-primary);
    font-weight: 600;
  }

  .tab-content {
    min-height: 200px;
  }

  .section {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .section-header {
    display: flex;
    justify-content: flex-end;
  }

  .form-row {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
  }

  .form-label {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-text);
  }

  .form-input {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    background: var(--ps-bg);
    color: var(--ps-text);
    width: 100%;
  }

  .form-input:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }

  .purge-all-row {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    padding-top: var(--ps-space-sm);
    border-top: 1px solid var(--ps-border-light);
  }

  .hint {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
  }

  .result-box {
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
  }

  .result-box h3 {
    font-size: var(--ps-font-size-sm);
    margin-bottom: var(--ps-space-sm);
    word-break: break-all;
  }

  .result-error {
    border-color: var(--ps-error);
    background: color-mix(in srgb, var(--ps-error) 5%, var(--ps-bg));
  }

  .result-success {
    border-color: var(--ps-success);
    background: color-mix(in srgb, var(--ps-success) 5%, var(--ps-bg));
  }

  .result-pre {
    margin: 0;
    white-space: pre-wrap;
    word-break: break-all;
    font-size: var(--ps-font-size-sm);
  }

  .cache-list {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .cache-card {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: hidden;
  }

  .cache-name {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    padding: var(--ps-space-sm) var(--ps-space-md);
    margin: 0;
    background: var(--ps-bg-tertiary);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .cache-summary {
    padding: var(--ps-space-md);
    margin: 0;
    font-size: var(--ps-font-size-sm);
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-word;
    background: var(--ps-bg-secondary);
  }

  .pre-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
    max-height: 70vh;
  }

  .pre-wrapper pre {
    padding: var(--ps-space-md);
    margin: 0;
    font-size: var(--ps-font-size-sm);
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-word;
    background: var(--ps-bg-secondary);
  }

  .purge-list {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
  }

  table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }

  thead {
    background: var(--ps-bg-tertiary);
  }

  th {
    text-align: left;
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
  }

  td {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border-light);
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  .row-num {
    width: 50px;
    color: var(--ps-text-tertiary);
  }

  .url-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }

  .meta {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
    align-self: flex-start;
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

  .btn-secondary:hover:not(:disabled) {
    background: var(--ps-surface-hover);
  }

  .btn-danger {
    background: var(--ps-error);
    color: var(--ps-text-inverse);
    border-color: var(--ps-error);
  }

  .btn-danger:hover:not(:disabled) {
    opacity: 0.9;
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
  }

  .empty {
    color: var(--ps-text-tertiary);
  }

  @media (max-width: 600px) {
    .tabs {
      gap: 0;
    }

    .tab {
      padding: var(--ps-space-sm) var(--ps-space-md);
      font-size: var(--ps-font-size-xs);
    }
  }
</style>
