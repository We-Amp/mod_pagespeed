<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import type { ConfigResponse } from '@pagespeed/api-client';
  import {
    loadSnapshots,
    addSnapshot,
    deleteSnapshot,
    diffSnapshots,
  } from '#lib/config-persistence.ts';
  import type { ConfigSnapshot, ConfigDiffEntry } from '#lib/config-persistence.ts';

  // -- Props ----------------------------------------------------------------

  interface Props {
    /** Current live config (for "save snapshot" action). */
    currentConfig: ConfigResponse | null;
    /** Callback when user wants to restore a snapshot. */
    onrestore?: (config: ConfigResponse) => void;
  }

  let { currentConfig, onrestore }: Props = $props();

  // -- State ----------------------------------------------------------------

  let snapshots = $state<ConfigSnapshot[]>([]);
  let newSnapshotName = $state('');
  let saveError = $state<string | null>(null);
  let saveSuccess = $state<string | null>(null);

  // Diff mode state
  let diffMode = $state(false);
  let diffSelectA = $state<string | null>(null);
  let diffSelectB = $state<string | null>(null);
  let diffEntries = $state<ConfigDiffEntry[]>([]);

  // -- Lifecycle ------------------------------------------------------------

  if (typeof window !== 'undefined') {
    snapshots = loadSnapshots();
  }

  // -- Actions --------------------------------------------------------------

  function handleSaveSnapshot() {
    if (!currentConfig) return;
    const name = newSnapshotName.trim();
    if (!name) {
      saveError = 'Please enter a snapshot name.';
      return;
    }
    saveError = null;
    snapshots = addSnapshot(name, currentConfig);
    newSnapshotName = '';
    saveSuccess = `Snapshot "${name}" saved.`;
    setTimeout(() => {
      saveSuccess = null;
    }, 3000);
  }

  function handleDelete(id: string) {
    snapshots = deleteSnapshot(id);
    // Clear diff if either was deleted.
    if (diffSelectA === id || diffSelectB === id) {
      diffSelectA = null;
      diffSelectB = null;
      diffEntries = [];
    }
  }

  function handleRestore(snap: ConfigSnapshot) {
    if (onrestore) {
      onrestore(snap.config);
    }
  }

  function toggleDiffMode() {
    diffMode = !diffMode;
    if (!diffMode) {
      diffSelectA = null;
      diffSelectB = null;
      diffEntries = [];
    }
  }

  function handleDiffSelect(id: string) {
    if (!diffMode) return;
    if (diffSelectA === null) {
      diffSelectA = id;
    } else if (diffSelectA === id) {
      diffSelectA = null;
      diffEntries = [];
    } else if (diffSelectB === null || diffSelectB !== id) {
      diffSelectB = id;
      computeDiff();
    } else {
      diffSelectB = null;
      diffEntries = [];
    }
  }

  function computeDiff() {
    if (!diffSelectA || !diffSelectB) {
      diffEntries = [];
      return;
    }
    const snapA = snapshots.find((s) => s.id === diffSelectA);
    const snapB = snapshots.find((s) => s.id === diffSelectB);
    if (!snapA || !snapB) {
      diffEntries = [];
      return;
    }
    diffEntries = diffSnapshots(snapA.config, snapB.config);
  }

  function formatTimestamp(iso: string): string {
    try {
      return new Date(iso).toLocaleString();
    } catch {
      return iso;
    }
  }

  function formatValue(val: unknown): string {
    if (typeof val === 'boolean') return val ? 'true' : 'false';
    return String(val);
  }
</script>

<div class="snapshots-panel">
  <div class="snapshots-header">
    <h2 class="section-heading">Config Snapshots</h2>
    <button
      class="ps-btn ps-btn-ghost"
      class:ps-btn-active={diffMode}
      onclick={toggleDiffMode}
      title={diffMode ? 'Exit comparison mode' : 'Compare two snapshots'}
      aria-label={diffMode ? 'Exit comparison mode' : 'Compare two snapshots'}
    >
      {diffMode ? 'Exit Compare' : 'Compare'}
    </button>
  </div>

  {#if diffMode}
    <p class="diff-hint">
      Select two snapshots to compare. Click a snapshot to select/deselect it.
    </p>
  {/if}

  <!-- Save new snapshot -->
  <div class="snapshot-save-row">
    <input
      type="text"
      class="ps-input snapshot-name-input"
      placeholder="Snapshot name..."
      bind:value={newSnapshotName}
      disabled={!currentConfig}
      aria-label="Snapshot name"
      onkeydown={(e) => {
        if (e.key === 'Enter') handleSaveSnapshot();
      }}
    />
    <button
      class="ps-btn ps-btn-secondary"
      onclick={handleSaveSnapshot}
      disabled={!currentConfig || !newSnapshotName.trim()}
    >
      Save Snapshot
    </button>
  </div>

  {#if saveError}
    <div class="snapshot-message snapshot-message-error" role="alert">
      {saveError}
    </div>
  {/if}
  {#if saveSuccess}
    <div class="snapshot-message snapshot-message-success" role="status">
      {saveSuccess}
    </div>
  {/if}

  <!-- Snapshot list -->
  {#if snapshots.length === 0}
    <p class="empty-text">
      No snapshots saved yet. Save the current configuration to create one.
    </p>
  {:else}
    <div class="snapshot-list">
      {#each snapshots as snap}
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div
          class="snapshot-card"
          class:snapshot-selected-a={diffMode && diffSelectA === snap.id}
          class:snapshot-selected-b={diffMode && diffSelectB === snap.id}
          role={diffMode ? 'button' : undefined}
          tabindex={diffMode ? 0 : undefined}
          onclick={diffMode ? () => handleDiffSelect(snap.id) : undefined}
          onkeydown={diffMode
            ? (e) => {
                if (e.key === 'Enter' || e.key === ' ') {
                  e.preventDefault();
                  handleDiffSelect(snap.id);
                }
              }
            : undefined}
        >
          <div class="snapshot-info">
            <span class="snapshot-name">{snap.name}</span>
            <span class="snapshot-date">{formatTimestamp(snap.created_at)}</span>
            {#if diffMode && diffSelectA === snap.id}
              <span class="ps-badge ps-badge-info">A</span>
            {/if}
            {#if diffMode && diffSelectB === snap.id}
              <span class="ps-badge ps-badge-warning">B</span>
            {/if}
          </div>
          {#if !diffMode}
            <div class="snapshot-actions">
              <button
                class="ps-btn ps-btn-ghost"
                onclick={() => handleRestore(snap)}
                title="Restore this snapshot"
                aria-label="Restore snapshot {snap.name}"
              >
                Restore
              </button>
              <button
                class="ps-btn ps-btn-ghost snapshot-delete-btn"
                onclick={() => handleDelete(snap.id)}
                title="Delete this snapshot"
                aria-label="Delete snapshot {snap.name}"
              >
                Delete
              </button>
            </div>
          {/if}
        </div>
      {/each}
    </div>
  {/if}

  <!-- Diff view -->
  {#if diffMode && diffEntries.length > 0}
    <div class="diff-view">
      <h3 class="diff-title">
        Differences ({diffEntries.length} field{diffEntries.length === 1 ? '' : 's'})
      </h3>
      <div class="diff-table-scroll">
        <table class="diff-table">
          <thead>
            <tr>
              <th scope="col">Field</th>
              <th scope="col" class="diff-col-a">
                Snapshot A
                <span class="diff-snap-name">
                  {snapshots.find((s) => s.id === diffSelectA)?.name ?? ''}
                </span>
              </th>
              <th scope="col" class="diff-col-b">
                Snapshot B
                <span class="diff-snap-name">
                  {snapshots.find((s) => s.id === diffSelectB)?.name ?? ''}
                </span>
              </th>
            </tr>
          </thead>
          <tbody>
            {#each diffEntries as entry}
              <tr>
                <td class="diff-field">{entry.label}</td>
                <td class="diff-val diff-col-a mono">{formatValue(entry.valueA)}</td>
                <td class="diff-val diff-col-b mono">{formatValue(entry.valueB)}</td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
    </div>
  {:else if diffMode && diffSelectA && diffSelectB}
    <p class="diff-no-changes">The selected snapshots are identical.</p>
  {/if}
</div>

<style>
  .snapshots-panel {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-md);
  }

  .snapshots-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
  }

  .section-heading {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  .diff-hint {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    margin: 0;
    padding: var(--ps-space-xs) var(--ps-space-sm);
    background: var(--ps-bg-secondary);
    border-radius: var(--ps-radius-sm);
  }

  .snapshot-save-row {
    display: flex;
    gap: var(--ps-space-sm);
    align-items: center;
  }

  .snapshot-name-input {
    flex: 1;
    min-width: 0;
  }

  .snapshot-message {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-radius: var(--ps-radius-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
  }

  .snapshot-message-error {
    background: var(--ps-error-bg);
    color: var(--ps-error);
    border: 1px solid var(--ps-error);
  }

  .snapshot-message-success {
    background: var(--ps-success-bg);
    color: var(--ps-success);
    border: 1px solid var(--ps-success);
  }

  .empty-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  .snapshot-list {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .snapshot-card {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    background: var(--ps-bg-primary);
    transition: border-color 0.15s ease;
  }

  .snapshot-card:hover {
    border-color: var(--ps-fg-muted);
  }

  .snapshot-selected-a {
    border-color: var(--ps-accent);
    background: color-mix(in srgb, var(--ps-accent) 8%, var(--ps-bg-primary));
  }

  .snapshot-selected-b {
    border-color: var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 8%, var(--ps-bg-primary));
  }

  .snapshot-info {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    min-width: 0;
  }

  .snapshot-name {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 500;
    color: var(--ps-fg-primary);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .snapshot-date {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    white-space: nowrap;
  }

  .snapshot-actions {
    display: flex;
    gap: var(--ps-space-xs);
    flex-shrink: 0;
  }

  .snapshot-delete-btn {
    color: var(--ps-error);
  }

  .snapshot-delete-btn:hover {
    color: var(--ps-error);
    opacity: 0.8;
  }

  /* Diff view */
  .diff-view {
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    background: var(--ps-bg-primary);
  }

  .diff-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  .diff-table-scroll {
    overflow-x: auto;
  }

  .diff-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
  }

  .diff-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
    white-space: nowrap;
  }

  .diff-table td {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
  }

  .diff-table tbody tr:last-child td {
    border-bottom: none;
  }

  .diff-field {
    font-weight: 500;
    color: var(--ps-fg-primary);
  }

  .diff-val {
    color: var(--ps-fg-primary);
  }

  .diff-col-a {
    color: var(--ps-accent);
  }

  .diff-col-b {
    color: var(--ps-warning);
  }

  .diff-snap-name {
    display: block;
    font-weight: 400;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  .diff-no-changes {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    margin: 0;
    text-align: center;
    padding: var(--ps-space-md);
  }

  .mono {
    font-family: var(--ps-font-mono);
  }

  .ps-btn-active {
    background: var(--ps-accent);
    color: var(--ps-bg-primary);
    border-color: var(--ps-accent);
  }

  @media (max-width: 767px) {
    .snapshot-save-row {
      flex-direction: column;
    }

    .snapshot-card {
      flex-direction: column;
      align-items: flex-start;
      gap: var(--ps-space-xs);
    }

    .snapshot-actions {
      width: 100%;
      justify-content: flex-end;
    }
  }
</style>
