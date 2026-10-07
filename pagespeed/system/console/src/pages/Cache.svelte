<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { usePolling } from "$lib/api/polling.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import Tabs from "$lib/Tabs.svelte";
  import type {
    CacheEntryResponse,
    PurgeResponse,
    PurgeSetResponse,
  } from "$lib/api/types";
  import { useConsole } from "$lib/api/context";
  import {
    flattenTree,
    formatRole,
    parseBackendStats,
    parseCacheCohorts,
    parseCacheSummary,
  } from "$lib/utils/cache-summary";
  import { formatIsoTitle, formatRelative } from "$lib/utils/format";
  import { detailHref, entryKeyFromAbsoluteUrl } from "$lib/utils/urls-api";

  const { api } = useConsole();

  let updatedAt = $state<number | null>(null);
  const cacheStructure = usePolling(
    () =>
      api.getCacheStructure().then((c) => {
        updatedAt = Date.now();
        return c;
      }),
    30000,
  );

  type CacheTab = "structure" | "lookup" | "purge" | "purgeset";

  let activeTab = $state<CacheTab>("structure");

  // -- Cache Lookup state --
  let lookupUrl = $state("");
  let lookupResult = $state<CacheEntryResponse | null>(null);
  let lookupError = $state<string | null>(null);
  let lookupLoading = $state(false);

  // The optimizer names an entry by path + host + scheme: split the
  // looked-up URL with the URL API; no link when it is not an absolute
  // http(s) URL.
  let lookupEntry = $derived(
    lookupResult ? entryKeyFromAbsoluteUrl(lookupResult.url || lookupUrl.trim()) : null,
  );

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
    if (
      !window.confirm(
        "Purge the ENTIRE cache? This invalidates every cached resource and can " +
          "cause a load spike on your origin as the cache refills.",
      )
    ) {
      return;
    }
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

  let purgeEnabled = $derived.by(() => {
    const d = cacheStructure.data;
    if (!d) return undefined;
    return d.purge_enabled;
  });

  const TABS: ReadonlyArray<{ id: CacheTab; label: string; needsPurge: boolean }> = [
    { id: "structure", label: "Cache Structure", needsPurge: false },
    { id: "lookup", label: "Cache Lookup", needsPurge: false },
    { id: "purge", label: "Purge", needsPurge: true },
    { id: "purgeset", label: "Purge Set", needsPurge: true },
  ];

  // The purge views are offered only where purging can work.
  let visibleTabs = $derived(TABS.filter((t) => !t.needsPurge || purgeEnabled !== false));
  let shownTab = $derived<CacheTab>(visibleTabs.some((t) => t.id === activeTab) ? activeTab : "structure");

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

  // The purge set is read when its tab is opened (and on its Refresh
  // button), from the click handler -- not from an effect.
  function selectTab(tab: CacheTab) {
    activeTab = tab;
    if (tab === "purgeset") void fetchPurgeSet();
  }

  function toggleAutoRefresh() {
    if (cacheStructure.autoRefresh) {
      cacheStructure.stop();
    } else {
      cacheStructure.start();
    }
  }

</script>

<div class="page">
  <PageHeader
    title="Caches"
    updatedAt={updatedAt}
    refresh={{
      autoRefresh: cacheStructure.autoRefresh,
      intervalMs: 30000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => cacheStructure.refresh(),
    }}
  >
    {#snippet toolbar()}
      {#if purgeEnabled !== undefined}
        <span class="purge-badge" class:purge-on={purgeEnabled} class:purge-off={!purgeEnabled}>
          {purgeEnabled ? "Purge enabled" : "Purge disabled"}
        </span>
      {/if}
    {/snippet}
  </PageHeader>

  {#if purgeEnabled === false}
    <p class="purge-note" data-testid="purge-disabled-note">
      Purging is off on this server, so the purge views are hidden. Turn it on with
      the EnableCachePurge option in the server's PageSpeed configuration.
    </p>
  {/if}

  <Tabs
    tabs={visibleTabs}
    active={shownTab}
    label="Cache views"
    idPrefix="cache"
    panelId="cache-panel"
    onselect={(id) => selectTab(id as CacheTab)}
  />

  <div class="tab-content" role="tabpanel" id="cache-panel" aria-labelledby="cache-tab-{shownTab}" tabindex="0">
    {#if shownTab === "structure"}
      <div class="section">
        {#if cacheStructure.loading}
          <p class="loading">Loading cache structure...</p>
        {:else if cacheStructure.error && !cacheStructure.data}
          <LoadError message={cacheStructure.error.message} />
        {:else if caches}
          <RefreshNotice error={cacheStructure.error} />
          <div class="cache-list">
            {#each caches as cache}
              {@const cohorts = parseCacheCohorts(cache.summary)}
              {@const tree = cohorts ? null : parseCacheSummary(cache.summary)}
              {@const layers = tree ? flattenTree(tree) : []}
              <div class="cache-card">
                <div class="cache-header">
                  <h2 class="cache-name">{cache.name}</h2>
                  {#if cache.summary === "none"}
                    <span class="cache-badge cache-badge-off">Not configured</span>
                  {/if}
                </div>

                {#if cache.summary === "none"}
                  <div class="cache-body cache-empty">
                    <p>No backing store configured for this cache.</p>
                  </div>
                {:else if cohorts}
                  <!-- Property Cache: multiple cohorts -->
                  <div class="cache-body">
                    <div class="cohort-grid">
                      {#each cohorts as cohort}
                        <div class="cohort-card">
                          <div class="cohort-name">{formatRole(cohort.name)}</div>
                          <div class="layer-stack">
                            {#each cohort.layers as layer}
                              <div class="layer" style="--depth: {layer.depth}">
                                <span class="layer-kind" aria-hidden="true">{layer.kind}</span>
                                <span class="layer-label">{layer.label}</span>
                                {#if layer.role}
                                  <span class="layer-role">{formatRole(layer.role)}</span>
                                {/if}
                                {#if layer.prefix}
                                  <span class="layer-prefix">{layer.prefix}</span>
                                {/if}
                              </div>
                            {/each}
                          </div>
                        </div>
                      {/each}
                    </div>
                  </div>
                {:else if layers.length > 0}
                  <!-- Single cache pipeline -->
                  <div class="cache-body">
                    <div class="layer-stack">
                      {#each layers as layer}
                        <div class="layer" style="--depth: {layer.depth}">
                          <span class="layer-kind" aria-hidden="true">{layer.kind}</span>
                          <span class="layer-label">{layer.label}</span>
                          {#if layer.role}
                            <span class="layer-role">{formatRole(layer.role)}</span>
                          {/if}
                          {#if layer.prefix}
                            <span class="layer-prefix">{layer.prefix}</span>
                          {/if}
                        </div>
                      {/each}
                    </div>
                  </div>
                {:else}
                  <!-- Fallback for unparseable summaries -->
                  <div class="cache-body">
                    <pre class="cache-summary-raw">{cache.summary}</pre>
                  </div>
                {/if}
              </div>
            {/each}
          </div>

          {#if backendStats}
            {@const stats = parseBackendStats(backendStats)}
            <div class="cache-card">
              <div class="cache-header">
                <h2 class="cache-name">Backend Stats</h2>
              </div>
              <div class="cache-body">
                {#if stats.length > 0}
                  <div class="stats-grid">
                    {#each stats as stat}
                      <div class="stat-row">
                        <span class="stat-key">{stat.key}</span>
                        <span class="stat-value">{stat.value}</span>
                      </div>
                    {/each}
                  </div>
                {:else}
                  <pre class="cache-summary-raw">{backendStats}</pre>
                {/if}
              </div>
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

    {:else if shownTab === "lookup"}
      <div class="section">
        <form class="form-row" onsubmit={(e) => { e.preventDefault(); doLookup(); }}>
          <label class="form-label" for="lookup-url">URL to look up:</label>
          <input
            id="lookup-url"
            type="text"
            class="form-input field"
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
            <h2 class="result-title">Result for: <code class="lookup-url-display">{lookupResult.url || lookupUrl}</code></h2>
            {#if lookupResult.error}
              <p class="error" role="alert">{lookupResult.error}</p>
            {:else if lookupResult.value}
              <pre class="result-pre">{lookupResult.value}</pre>
            {:else}
              <p class="empty">No cache entry found for this URL.</p>
            {/if}
            {#if lookupEntry !== null}
              <p class="result-actions">
                <a href={detailHref(lookupEntry)}>Inspect in the optimizer's URL index</a>
              </p>
            {/if}
          </div>
        {/if}
      </div>

    {:else if shownTab === "purge"}
      <div class="section">
        <form class="form-row" onsubmit={(e) => { e.preventDefault(); doPurge(); }}>
          <label class="form-label" for="purge-url">URL to purge:</label>
          <input
            id="purge-url"
            type="text"
            class="form-input field"
            placeholder="https://example.com/image.jpg"
            bind:value={purgeUrl}
          />
          <button class="btn btn-primary" type="submit" disabled={purgeLoading || !purgeUrl.trim()}>
            {purgeLoading ? "Purging..." : "Purge URL"}
          </button>
        </form>

        <div class="purge-all-row control-group">
          <button class="btn btn-danger" onclick={doPurgeAll} disabled={purgeLoading}>
            Purge All (*)
          </button>
          <span class="hint field-text">This invalidates the entire cache.</span>
        </div>

        {#if purgeError}
          <div class="result-box result-error">
            <strong>Error:</strong> {purgeError}
          </div>
        {/if}

        {#if purgeResult}
          <div class="result-box" class:result-success={purgeResult.success} class:result-error={!purgeResult.success}>
            <p>
              {#if purgeResult.error}
                Purge failed. {purgeResult.error}
              {:else if purgeResult.message}
                {purgeResult.message}
              {:else if purgeResult.success}
                Purge successful.
              {:else}
                Purge failed.
              {/if}
            </p>
          </div>
        {/if}
      </div>

    {:else if shownTab === "purgeset"}
      <div class="section">
        <div class="section-header">
          <button class="btn btn-secondary" onclick={fetchPurgeSet} disabled={purgeSetLoading}>
            {purgeSetLoading ? "Loading..." : "Refresh"}
          </button>
        </div>

        {#if purgeSetError}
          <p class="error" role="alert">{purgeSetError}</p>
        {:else if purgeSetData}
          {#if purgeSetData.global_invalidation_timestamp_ms}
            <p class="meta">
              Global invalidation timestamp:
              <time title={formatIsoTitle(purgeSetData.global_invalidation_timestamp_ms)}>{formatRelative(purgeSetData.global_invalidation_timestamp_ms, Date.now())}</time>
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
  .purge-badge {
    display: inline-flex;
    align-items: center;
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-radius: 999px;
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    letter-spacing: 0.02em;
    line-height: 1;
  }

  .purge-on {
    background: color-mix(in srgb, var(--ps-success) 12%, var(--ps-bg));
    color: var(--ps-success-text);
    border: 1px solid color-mix(in srgb, var(--ps-success) 30%, transparent);
  }

  .purge-off {
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-warning-text);
    border: 1px solid color-mix(in srgb, var(--ps-warning) 30%, transparent);
  }

  .purge-note {
    margin: 0 0 var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
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
    font-family: var(--ps-font-mono);
    width: 100%;
  }

  .form-input:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }

  .purge-all-row {
    padding-top: var(--ps-space-sm);
    border-top: 1px solid var(--ps-border-light);
  }

  .hint {
    font-size: var(--ps-font-size-xs);
  }

  .result-box {
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
  }

  .result-box .result-title {
    font-size: var(--ps-font-size-sm);
    margin-bottom: var(--ps-space-sm);
    word-break: break-all;
  }

  .lookup-url-display {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    background: var(--ps-bg-tertiary);
    padding: 1px 4px;
    border-radius: 3px;
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

  .result-actions {
    margin-top: var(--ps-space-sm);
  }

  /* ---- Cache cards ---- */

  /* One column on a laptop; more on a wide window. The cards flow down
     the columns and never break, so a short card is followed directly by
     the next one instead of leaving a hole beside a tall one. */
  .cache-list {
    columns: var(--ps-card-max);
    column-gap: var(--ps-space-md);
  }

  .cache-card {
    break-inside: avoid;
    margin-bottom: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    overflow: hidden;
  }

  .cache-header {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-tertiary);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .cache-name {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    margin: 0;
    flex: 1;
  }

  .cache-badge {
    font-size: var(--ps-font-size-xs);
    padding: 2px var(--ps-space-sm);
    border-radius: 999px;
    font-weight: 500;
  }

  .cache-badge-off {
    background: var(--ps-bg-secondary);
    color: var(--ps-text-secondary);
    border: 1px solid var(--ps-border);
  }

  .cache-body {
    padding: var(--ps-space-md);
    background: var(--ps-bg);
  }

  .cache-empty {
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .cache-summary-raw {
    margin: 0;
    font-size: var(--ps-font-size-sm);
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-word;
  }

  /* ---- Layer stack (pipeline visualization) ---- */

  .layer-stack {
    display: flex;
    flex-direction: column;
    gap: 0;
  }

  .layer {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    padding-left: calc(var(--ps-space-md) + var(--depth, 0) * var(--ps-space-lg));
    font-size: var(--ps-font-size-sm);
    border-left: 2px solid var(--ps-border-light);
    margin-left: var(--ps-space-sm);
    position: relative;
  }

  .layer::before {
    content: "";
    position: absolute;
    left: -2px;
    top: 50%;
    width: 8px;
    height: 2px;
    background: var(--ps-border);
  }

  .layer:last-child {
    border-left-color: transparent;
  }

  .layer:last-child::before {
    height: calc(50% + 1px);
    top: 0;
    width: 2px;
    background: var(--ps-border-light);
  }

  .layer:last-child::after {
    content: "";
    position: absolute;
    left: -2px;
    top: 50%;
    width: 8px;
    height: 2px;
    background: var(--ps-border);
  }

  .layer-kind {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
    flex-shrink: 0;
  }

  .layer-label {
    font-weight: 500;
    color: var(--ps-text);
  }

  .layer-role {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-primary);
    font-weight: 600;
    background: var(--ps-primary-light);
    padding: 1px 6px;
    border-radius: 999px;
  }

  .layer-prefix {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
    font-family: var(--ps-font-mono);
    margin-left: auto;
  }

  /* ---- Cohort grid (Property Cache) ---- */

  .cohort-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, 22rem), 1fr));
    gap: var(--ps-space-md);
  }

  .cohort-card {
    border: 1px solid var(--ps-border-light);
    border-radius: var(--ps-border-radius);
    overflow: hidden;
  }

  .cohort-name {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--ps-text-secondary);
    padding: var(--ps-space-xs) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border-bottom: 1px solid var(--ps-border-light);
  }

  /* ---- Stats grid (Backend Stats) ---- */

  .stats-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0;
  }

  .stat-row {
    display: contents;
  }

  .stat-key {
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-text-secondary);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .stat-value {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    color: var(--ps-text);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border-light);
    text-align: right;
  }

  /* ---- Existing styles ---- */

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
    color: var(--ps-text-secondary);
  }

  .url-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }

  .meta {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  /* In the column forms a button keeps its own width. */
  .form-row .btn {
    align-self: flex-start;
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
  }

  .empty {
    color: var(--ps-text-secondary);
  }

  @media (max-width: 600px) {
    .cohort-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
