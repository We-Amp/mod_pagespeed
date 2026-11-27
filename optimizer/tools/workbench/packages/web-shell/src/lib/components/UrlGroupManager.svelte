<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onMount } from 'svelte';
  import type {
    ApiTransport,
    CacheAlternatesResponse,
    AlternateInfo,
  } from '@pagespeed/api-client';
  import { createCacheClient, formatBytes } from '@pagespeed/api-client';
  import {
    loadUrlGroups,
    addUrlGroup,
    deleteUrlGroup,
    renameUrlGroup,
    removeUrlFromGroup,
  } from '#lib/url-groups.ts';
  import type { UrlGroup, UrlGroupSummaryEntry } from '#lib/url-groups.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const cache = createCacheClient(transport);

  // -- Props -----------------------------------------------------------------

  interface Props {
    /** Optional: pre-select a group by ID. */
    selectedGroupId?: string;
  }

  let { selectedGroupId }: Props = $props();

  // -- State -----------------------------------------------------------------

  let groups = $state<UrlGroup[]>([]);
  let newGroupName = $state('');
  let createError = $state<string | null>(null);

  // Detail view state (reacts to prop changes via $derived for initial, $state for nav)
  let activeGroupId = $state<string | null>(null);

  $effect(() => {
    if (selectedGroupId) {
      activeGroupId = selectedGroupId;
    }
  });
  let batchResults = $state<UrlGroupSummaryEntry[]>([]);
  let batchLoading = $state(false);
  let batchError = $state<string | null>(null);

  // Rename state
  let renamingId = $state<string | null>(null);
  let renameValue = $state('');

  // -- Lifecycle -------------------------------------------------------------

  onMount(() => {
    groups = loadUrlGroups();
  });

  // -- Derived ---------------------------------------------------------------

  let activeGroup = $derived(
    activeGroupId ? groups.find((g) => g.id === activeGroupId) : null,
  );

  let totalSavings = $derived(() => {
    if (batchResults.length === 0) return 0;
    return batchResults.reduce((sum, r) => sum + r.savings, 0);
  });

  let totalOriginal = $derived(() => {
    if (batchResults.length === 0) return 0;
    return batchResults.reduce((sum, r) => sum + r.original_size, 0);
  });

  // -- Actions ---------------------------------------------------------------

  function handleCreateGroup() {
    const name = newGroupName.trim();
    if (!name) {
      createError = 'Please enter a group name.';
      return;
    }
    createError = null;
    groups = addUrlGroup(name);
    newGroupName = '';
  }

  function handleDeleteGroup(id: string) {
    groups = deleteUrlGroup(id);
    if (activeGroupId === id) {
      activeGroupId = null;
      batchResults = [];
    }
  }

  function startRename(group: UrlGroup) {
    renamingId = group.id;
    renameValue = group.name;
  }

  function handleRename() {
    if (!renamingId) return;
    const name = renameValue.trim();
    if (name) {
      groups = renameUrlGroup(renamingId, name);
    }
    renamingId = null;
    renameValue = '';
  }

  function selectGroup(id: string) {
    activeGroupId = id;
    batchResults = [];
    batchError = null;
  }

  function backToGroups() {
    activeGroupId = null;
    batchResults = [];
    batchError = null;
  }

  function handleRemoveUrl(url: string, hostname: string) {
    if (!activeGroupId) return;
    groups = removeUrlFromGroup(activeGroupId, url, hostname);
  }

  async function runBatchInspect() {
    if (!activeGroup) return;
    batchLoading = true;
    batchError = null;
    batchResults = [];

    const results: UrlGroupSummaryEntry[] = [];

    for (const entry of activeGroup.urls) {
      try {
        const resp: CacheAlternatesResponse = await cache.getAlternates({
          url: entry.url,
          hostname: entry.hostname,
          ...(entry.site ? { site: entry.site } : {}),
        });
        const alternates = resp.alternates;
        const altCount = alternates.length;

        // Find the original alternate for baseline.
        const original = alternates.find(
          (a: AlternateInfo) =>
            a.format === 'original' && !a.is_sentinel,
        );
        const origSize = original?.size ?? 0;

        // Sum up sizes of all non-sentinel alternates.
        const totalSize = alternates
          .filter((a: AlternateInfo) => !a.is_sentinel)
          .reduce((s: number, a: AlternateInfo) => s + a.size, 0);

        // Savings: original size minus the smallest non-original,
        // non-sentinel alternate's size. If no optimized alternates,
        // savings is 0.
        const optimized = alternates.filter(
          (a: AlternateInfo) =>
            a.format !== 'original' && !a.is_sentinel,
        );
        let savings = 0;
        if (original && optimized.length > 0) {
          const smallest = Math.min(
            ...optimized.map((a: AlternateInfo) => a.size),
          );
          savings = Math.max(0, origSize - smallest);
        }

        results.push({
          url: entry.url,
          hostname: entry.hostname,
          alternate_count: altCount,
          total_size: totalSize,
          original_size: origSize,
          savings,
        });
      } catch (e) {
        results.push({
          url: entry.url,
          hostname: entry.hostname,
          alternate_count: 0,
          total_size: 0,
          original_size: 0,
          savings: 0,
        });
      }
    }

    batchResults = results;
    batchLoading = false;
  }

  function truncateUrl(url: string, maxLen: number = 60): string {
    if (url.length <= maxLen) return url;
    return url.slice(0, maxLen - 3) + '...';
  }
</script>

<div class="url-group-manager">
  {#if activeGroup}
    <!-- Detail view for a selected group -->
    <div class="group-detail">
      <button class="ps-btn ps-btn-ghost back-btn" onclick={backToGroups}>
        &larr; Back to groups
      </button>
      <div class="group-detail-header">
        <h2 class="section-heading">{activeGroup.name}</h2>
        <span class="url-count">
          {activeGroup.urls.length} URL{activeGroup.urls.length === 1 ? '' : 's'}
        </span>
      </div>

      {#if activeGroup.urls.length === 0}
        <p class="empty-text">
          This group has no URLs. Add URLs from the URL Inspector above by
          clicking "Add to Group" on any URL.
        </p>
      {:else}
        <div class="batch-actions">
          <button
            class="ps-btn ps-btn-primary"
            onclick={runBatchInspect}
            disabled={batchLoading || activeGroup.urls.length === 0}
          >
            {#if batchLoading}
              <span class="ps-spinner" style="width:14px;height:14px" aria-hidden="true"></span>
              Inspecting...
            {:else}
              Batch Inspect
            {/if}
          </button>
        </div>

        {#if batchError}
          <div class="batch-message batch-message-error" role="alert">
            {batchError}
          </div>
        {/if}

        <!-- URL list for this group -->
        <div class="group-url-list">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col">URL</th>
                <th scope="col">Hostname</th>
                {#if batchResults.length > 0}
                  <th scope="col" class="num-col">Alternates</th>
                  <th scope="col" class="num-col">Original</th>
                  <th scope="col" class="num-col">Savings</th>
                {/if}
                <th scope="col" class="action-col"></th>
              </tr>
            </thead>
            <tbody>
              {#each activeGroup.urls as entry, i}
                <tr>
                  <td class="url-cell mono" title={entry.url}>
                    {truncateUrl(entry.url)}
                  </td>
                  <td>{entry.hostname}</td>
                  {#if batchResults.length > 0 && batchResults[i]}
                    <td class="num-col mono">
                      {batchResults[i].alternate_count}
                    </td>
                    <td class="num-col mono">
                      {formatBytes(batchResults[i].original_size)}
                    </td>
                    <td class="num-col mono savings-cell">
                      {#if batchResults[i].savings > 0}
                        -{formatBytes(batchResults[i].savings)}
                      {:else}
                        --
                      {/if}
                    </td>
                  {/if}
                  <td class="action-col">
                    <button
                      class="ps-btn ps-btn-ghost remove-btn"
                      onclick={() =>
                        handleRemoveUrl(entry.url, entry.hostname)}
                      title="Remove from group"
                      aria-label="Remove {entry.url} from group"
                    >
                      Remove
                    </button>
                  </td>
                </tr>
              {/each}
            </tbody>
            {#if batchResults.length > 0}
              <tfoot>
                <tr class="totals-row">
                  <td colspan="2"><strong>Totals</strong></td>
                  <td class="num-col mono">
                    {batchResults.reduce((s, r) => s + r.alternate_count, 0)}
                  </td>
                  <td class="num-col mono">
                    {formatBytes(totalOriginal())}
                  </td>
                  <td class="num-col mono savings-cell">
                    {#if totalSavings() > 0}
                      <strong>-{formatBytes(totalSavings())}</strong>
                    {:else}
                      --
                    {/if}
                  </td>
                  <td></td>
                </tr>
              </tfoot>
            {/if}
          </table>
        </div>
      {/if}
    </div>
  {:else}
    <!-- Group list view -->
    <div class="group-list-view">
      <h2 class="section-heading">URL Groups</h2>

      <div class="group-create-row">
        <input
          type="text"
          class="ps-input group-name-input"
          placeholder="New group name..."
          bind:value={newGroupName}
          aria-label="New group name"
          onkeydown={(e) => {
            if (e.key === 'Enter') handleCreateGroup();
          }}
        />
        <button
          class="ps-btn ps-btn-secondary"
          onclick={handleCreateGroup}
          disabled={!newGroupName.trim()}
        >
          Create Group
        </button>
      </div>

      {#if createError}
        <div class="batch-message batch-message-error" role="alert">
          {createError}
        </div>
      {/if}

      {#if groups.length === 0}
        <p class="empty-text">
          No URL groups defined. Create a group to start organizing URLs.
        </p>
      {:else}
        <div class="group-cards">
          {#each groups as group}
            <div class="group-card">
              <div
                class="group-card-main"
                role="button"
                tabindex="0"
                onclick={() => selectGroup(group.id)}
                onkeydown={(e) => {
                  if (e.key === 'Enter' || e.key === ' ') {
                    e.preventDefault();
                    selectGroup(group.id);
                  }
                }}
              >
                {#if renamingId === group.id}
                  <input
                    type="text"
                    class="ps-input rename-input"
                    bind:value={renameValue}
                    aria-label="Rename group"
                    onkeydown={(e) => {
                      if (e.key === 'Enter') handleRename();
                      if (e.key === 'Escape') {
                        renamingId = null;
                      }
                    }}
                    onblur={handleRename}
                    onclick={(e) => e.stopPropagation()}
                  />
                {:else}
                  <span class="group-name">{group.name}</span>
                {/if}
                <span class="group-meta">
                  {group.urls.length} URL{group.urls.length === 1 ? '' : 's'}
                </span>
              </div>
              <div class="group-card-actions">
                <button
                  class="ps-btn ps-btn-ghost"
                  onclick={(e) => {
                    e.stopPropagation();
                    startRename(group);
                  }}
                  title="Rename group"
                  aria-label="Rename group {group.name}"
                >
                  Rename
                </button>
                <button
                  class="ps-btn ps-btn-ghost group-delete-btn"
                  onclick={(e) => {
                    e.stopPropagation();
                    handleDeleteGroup(group.id);
                  }}
                  title="Delete group"
                  aria-label="Delete group {group.name}"
                >
                  Delete
                </button>
              </div>
            </div>
          {/each}
        </div>
      {/if}
    </div>
  {/if}
</div>

<style>
  .url-group-manager {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .section-heading {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  .empty-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  /* Group creation */
  .group-create-row {
    display: flex;
    gap: var(--ps-space-sm);
    align-items: center;
    margin-top: var(--ps-space-sm);
  }

  .group-name-input {
    flex: 1;
    min-width: 0;
  }

  /* Group list */
  .group-cards {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    margin-top: var(--ps-space-sm);
  }

  .group-card {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    background: var(--ps-bg-primary);
    transition: border-color 0.15s ease;
  }

  .group-card:hover {
    border-color: var(--ps-fg-muted);
  }

  .group-card-main {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    cursor: pointer;
    flex: 1;
    min-width: 0;
  }

  .group-card-main:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: 2px;
    border-radius: var(--ps-radius-sm);
  }

  .group-name {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 500;
    color: var(--ps-fg-primary);
  }

  .group-meta {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  .group-card-actions {
    display: flex;
    gap: var(--ps-space-xs);
    flex-shrink: 0;
  }

  .group-delete-btn {
    color: var(--ps-error);
  }

  .rename-input {
    max-width: 200px;
  }

  /* Detail view */
  .group-detail {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .back-btn {
    align-self: flex-start;
  }

  .group-detail-header {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .url-count {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  .batch-actions {
    display: flex;
    gap: var(--ps-space-sm);
  }

  .batch-message {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
  }

  .batch-message-error {
    background: var(--ps-error-bg);
    color: var(--ps-error);
    border: 1px solid var(--ps-error);
  }

  /* Data table */
  .group-url-list {
    overflow-x: auto;
  }

  .data-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .data-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
    white-space: nowrap;
  }

  .data-table td {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    color: var(--ps-fg-primary);
    border-bottom: 1px solid var(--ps-border);
  }

  .data-table tbody tr:last-child td {
    border-bottom: none;
  }

  .data-table tfoot td {
    border-top: 2px solid var(--ps-border);
    padding-top: var(--ps-space-sm);
    font-weight: 500;
  }

  .num-col {
    text-align: right;
  }

  .action-col {
    text-align: right;
    width: 80px;
  }

  .url-cell {
    max-width: 400px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .mono {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .savings-cell {
    color: var(--ps-success);
  }

  .totals-row td {
    background: var(--ps-bg-secondary);
  }

  .remove-btn {
    color: var(--ps-error);
    font-size: var(--ps-font-size-sm);
  }

  @media (max-width: 767px) {
    .group-create-row {
      flex-direction: column;
    }

    .group-card {
      flex-direction: column;
      align-items: flex-start;
      gap: var(--ps-space-xs);
    }

    .group-card-actions {
      width: 100%;
      justify-content: flex-end;
    }
  }
</style>
