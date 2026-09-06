<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

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

  // ---- Cache summary parser ----

  interface CacheNode {
    type: string;
    props: Record<string, string>;
    children: CacheNode[];
  }

  /** Parse a cache summary expression like "Compressed(Fallback(small=Stats(...)))" into a tree. */
  function parseCacheSummary(s: string): CacheNode | null {
    s = s.trim();
    if (!s || s === "none") return null;

    let pos = 0;

    function parseNode(): CacheNode | null {
      // Read type name (letters, digits, angle brackets for SharedMemCache<64>)
      let name = "";
      while (pos < s.length && s[pos] !== "(" && s[pos] !== ")" && s[pos] !== "," && s[pos] !== "=") {
        name += s[pos];
        pos++;
      }
      name = name.trim();
      if (!name) return null;

      const node: CacheNode = { type: name, props: {}, children: [] };

      // If followed by '(', parse contents
      if (pos < s.length && s[pos] === "(") {
        pos++; // skip '('
        // Parse comma-separated entries inside parens
        while (pos < s.length && s[pos] !== ")") {
          skipWhitespace();
          // Check if this is key=value or just a positional child
          const startPos = pos;
          let key = "";
          while (pos < s.length && s[pos] !== "=" && s[pos] !== "(" && s[pos] !== ")" && s[pos] !== ",") {
            key += s[pos];
            pos++;
          }
          key = key.trim();

          if (pos < s.length && s[pos] === "=") {
            pos++; // skip '='
            // value is either a nested node or a plain string
            const child = parseNode();
            if (child) {
              child.props["_key"] = key;
              node.children.push(child);
            } else {
              node.props[key] = "";
            }
          } else {
            // Rewind — this was a positional arg that is itself a node name
            pos = startPos;
            const child = parseNode();
            if (child) {
              node.children.push(child);
            }
          }

          skipWhitespace();
          if (pos < s.length && s[pos] === ",") {
            pos++; // skip comma
          }
        }
        if (pos < s.length && s[pos] === ")") {
          pos++; // skip ')'
        }
      }

      return node;
    }

    function skipWhitespace() {
      while (pos < s.length && (s[pos] === " " || s[pos] === "\t")) pos++;
    }

    return parseNode();
  }

  /** Describe a cache node type with a user-friendly label. */
  function nodeLabel(type: string): string {
    if (type.startsWith("SharedMemCache")) {
      const m = type.match(/<(\d+)>/);
      return m ? `Shared Memory (${m[1]} segments)` : "Shared Memory";
    }
    const labels: Record<string, string> = {
      HTTPCache: "HTTP Cache",
      CycloneCache: "Cyclone (Disk)",
      Compressed: "Compression Layer",
      Fallback: "Fallback Strategy",
      Stats: "Statistics Wrapper",
    };
    return labels[type] || type;
  }

  /** Get a short icon/symbol for a cache type. */
  function nodeIcon(type: string): string {
    if (type.startsWith("SharedMemCache")) return "\u{1F4BB}"; // memory
    const icons: Record<string, string> = {
      HTTPCache: "\u{1F310}",
      CycloneCache: "\u{1F4BF}",
      Compressed: "\u{1F5DC}",
      Fallback: "\u{1F500}",
      Stats: "\u{1F4CA}",
    };
    return icons[type] || "\u{1F4E6}";
  }

  /** Flatten cache tree into a list of layers with depth, for display. */
  interface CacheLayer {
    depth: number;
    icon: string;
    label: string;
    role: string; // e.g. "small", "large", or ""
    prefix: string; // stats prefix if present
  }

  function flattenTree(node: CacheNode | null, depth: number = 0, role: string = ""): CacheLayer[] {
    if (!node) return [];
    const layers: CacheLayer[] = [];
    const keyRole = node.props["_key"] || role;

    // For Stats nodes, extract the prefix and continue into the cache child
    if (node.type === "Stats") {
      // The prefix is stored as a child with _key="prefix", its type is the prefix value
      const prefixChild = node.children.find((c) => c.props["_key"] === "prefix");
      const prefix = prefixChild ? prefixChild.type : "";
      const cacheChildren = node.children.filter((c) => c.props["_key"] !== "prefix");
      // Stats wraps exactly one cache child usually
      for (const child of cacheChildren) {
        // Don't pass "cache" as a role — it's structural, not meaningful
        const childRole = child.props["_key"] === "cache" ? keyRole : (child.props["_key"] || keyRole);
        const childLayers = flattenTree(child, depth, childRole);
        if (childLayers.length > 0) {
          childLayers[0].prefix = prefix;
        }
        layers.push(...childLayers);
      }
      // If Stats has no cache children, show the Stats node itself
      if (cacheChildren.length === 0) {
        layers.push({
          depth,
          icon: nodeIcon(node.type),
          label: nodeLabel(node.type),
          role: keyRole,
          prefix,
        });
      }
      return layers;
    }

    layers.push({
      depth,
      icon: nodeIcon(node.type),
      label: nodeLabel(node.type),
      role: keyRole,
      prefix: "",
    });

    for (const child of node.children) {
      layers.push(...flattenTree(child, depth + 1, ""));
    }

    return layers;
  }

  /** Parse a Property Cache summary which has multiple cohorts. */
  interface CacheCohort {
    name: string;
    layers: CacheLayer[];
  }

  function parseCacheCohorts(summary: string): CacheCohort[] | null {
    const lines = summary.split("\n").filter((l) => l.trim());
    // Check if this looks like cohorts (name:expression per line)
    if (lines.length < 2 || !lines.every((l) => l.includes(":"))) return null;

    return lines.map((line) => {
      const colonIdx = line.indexOf(":");
      const name = line.substring(0, colonIdx).trim();
      const expr = line.substring(colonIdx + 1).trim();
      const tree = parseCacheSummary(expr);
      return { name, layers: flattenTree(tree) };
    });
  }

  /** Parse backend_stats into key-value pairs. */
  function parseBackendStats(raw: string): Array<{ key: string; value: string }> {
    if (!raw || !raw.trim()) return [];
    return raw
      .split("\n")
      .filter((l) => l.trim())
      .map((line) => {
        const eqIdx = line.indexOf(":");
        if (eqIdx >= 0) {
          return { key: line.substring(0, eqIdx).trim(), value: line.substring(eqIdx + 1).trim() };
        }
        const spIdx = line.indexOf(" ");
        if (spIdx >= 0) {
          return { key: line.substring(0, spIdx).trim(), value: line.substring(spIdx + 1).trim() };
        }
        return { key: line.trim(), value: "" };
      });
  }

  /** Format a role label for display. */
  function formatRole(role: string): string {
    return role
      .replace(/_/g, " ")
      .replace(/\b\w/g, (c) => c.toUpperCase());
  }
</script>

<div class="page">
  <div class="page-header">
    <h1>Caches</h1>
    {#if purgeEnabled !== undefined}
      <span class="purge-badge" class:purge-on={purgeEnabled} class:purge-off={!purgeEnabled}>
        {purgeEnabled ? "Purge enabled" : "Purge disabled"}
      </span>
    {/if}
  </div>

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
              {@const cohorts = parseCacheCohorts(cache.summary)}
              {@const tree = cohorts ? null : parseCacheSummary(cache.summary)}
              {@const layers = tree ? flattenTree(tree) : []}
              <div class="cache-card">
                <div class="cache-header">
                  <h3 class="cache-name">{cache.name}</h3>
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
                                <span class="layer-icon">{layer.icon}</span>
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
                          <span class="layer-icon">{layer.icon}</span>
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
                <h3 class="cache-name">Backend Stats</h3>
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
            <h3>Result for: <code class="lookup-url-display">{lookupResult.url || lookupUrl}</code></h3>
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

  .page-header {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
  }

  .page-header h1 {
    margin: 0;
  }

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
    color: var(--ps-success);
    border: 1px solid color-mix(in srgb, var(--ps-success) 30%, transparent);
  }

  .purge-off {
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-warning);
    border: 1px solid color-mix(in srgb, var(--ps-warning) 30%, transparent);
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

  /* ---- Cache cards ---- */

  .cache-list {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .cache-card {
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
    color: var(--ps-text-tertiary);
    border: 1px solid var(--ps-border);
  }

  .cache-body {
    padding: var(--ps-space-md);
    background: var(--ps-bg);
  }

  .cache-empty {
    color: var(--ps-text-tertiary);
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

  .layer-icon {
    font-size: var(--ps-font-size-base);
    flex-shrink: 0;
    width: 1.4em;
    text-align: center;
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
    color: var(--ps-text-tertiary);
    font-family: var(--ps-font-mono);
    margin-left: auto;
  }

  /* ---- Cohort grid (Property Cache) ---- */

  .cohort-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
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

    .cohort-grid {
      grid-template-columns: 1fr;
    }
  }
</style>
