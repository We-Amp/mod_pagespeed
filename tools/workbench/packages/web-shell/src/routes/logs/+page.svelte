<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy, tick } from 'svelte';
  import type { Readable } from 'svelte/store';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    LogLevel,
    LogSource,
    WsLogMessage,
  } from '@pagespeed/api-client';
  import HelpIcon from '#lib/components/HelpIcon.svelte';
  import {
    exportJson,
    triggerDownload,
    timestampedFilename,
  } from '#lib/export.ts';
  import {
    ALL_SOURCES,
    ALL_LEVELS,
    MAX_LOG_ENTRIES,
    MODULE_COLORS,
    filterLogs,
    formatLogTimestamp,
    formatLogLine,
    tagEntry,
    appendToBuffer,
  } from '#lib/log-utils.ts';
  import type { ClientLogEntry } from '#lib/log-utils.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Constants -------------------------------------------------------------

  const SEARCH_DEBOUNCE_MS = 300;

  const LEVEL_CLASSES: Record<LogLevel, string> = {
    debug: 'level-debug',
    info: 'level-info',
    warning: 'level-warning',
    error: 'level-error',
  };

  // -- Reactive state --------------------------------------------------------

  let connected = $state(false);
  let logs: ClientLogEntry[] = $state([]);
  let droppedCount = $state(0);

  // Filters
  let enabledSources: Record<LogSource, boolean> = $state({
    worker: true,
    cache: true,
    chrome: true,
  });
  let enabledLevels: Record<LogLevel, boolean> = $state({
    debug: true,
    info: true,
    warning: true,
    error: true,
  });
  let searchText = $state('');
  let searchInput = $state('');
  let searchTimer: ReturnType<typeof setTimeout> | undefined;

  // Auto-scroll
  let paused = $state(false);
  let newWhilePaused = $state(0);
  let logContainer: HTMLElement | undefined = $state();

  // Filtered view
  let filteredLogs = $derived(
    filterLogs(logs, enabledSources, enabledLevels, searchText),
  );

  // -- Connection subscription -----------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  // -- WebSocket subscription ------------------------------------------------

  const unsubWs = transport.subscribe('/v1/ws/logs', (raw: unknown) => {
    const msg = raw as WsLogMessage;
    if (msg.type === 'snapshot') {
      logs = msg.entries.slice(-MAX_LOG_ENTRIES).map(tagEntry);
      droppedCount = 0;
    } else if (msg.type === 'log') {
      const tagged = tagEntry(msg);
      logs = appendToBuffer(logs, tagged);
      if (paused) {
        newWhilePaused++;
      }
    } else if (msg.type === 'overflow') {
      droppedCount += msg.dropped_count;
    }
  });

  // -- Cleanup ---------------------------------------------------------------

  onDestroy(() => {
    unsubWs();
    unsubConn();
    if (searchTimer) clearTimeout(searchTimer);
  });

  // -- Functions -------------------------------------------------------------

  function handleSearchInput(): void {
    if (searchTimer) clearTimeout(searchTimer);
    searchTimer = setTimeout(() => {
      searchText = searchInput;
    }, SEARCH_DEBOUNCE_MS);
  }

  function clearLogs(): void {
    logs = [];
    droppedCount = 0;
    newWhilePaused = 0;
  }

  function togglePause(): void {
    paused = !paused;
    if (!paused) {
      newWhilePaused = 0;
    }
  }

  function moduleColor(module: string): string {
    return MODULE_COLORS[module] ?? 'var(--ps-fg-muted)';
  }

  function exportTxt(): void {
    const lines = filteredLogs.map(formatLogLine);
    const content = lines.join('\n') + '\n';
    const filename = timestampedFilename('pagespeed-logs', 'txt');
    triggerDownload(content, filename, 'text/plain');
  }

  function exportJsonLogs(): void {
    const filename = timestampedFilename('pagespeed-logs', 'json');
    exportJson(filteredLogs, filename);
  }

  // -- Auto-scroll effect ----------------------------------------------------

  $effect(() => {
    // Access filteredLogs.length to register as dependency.
    const _ = filteredLogs.length;
    if (!paused && logContainer) {
      tick().then(() => {
        if (logContainer) {
          logContainer.scrollTop = logContainer.scrollHeight;
        }
      });
    }
  });
</script>

<svelte:head>
  <title>Debug Console - ModPageSpeed</title>
</svelte:head>

{#if !connected}
  <div class="page-center">
    <div class="ps-card center-card">
      <div class="ps-spinner" aria-label="Connecting to debug console"></div>
      <p class="center-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else}
  <div class="log-page">
    <!-- Toolbar -->
    <div class="toolbar">
      <div class="toolbar-row">
        <div class="filter-group">
          <span class="filter-label">Source</span>
          {#each ALL_SOURCES as source}
            <label class="toggle-chip" class:toggle-chip-active={enabledSources[source]}>
              <input
                type="checkbox"
                bind:checked={enabledSources[source]}
                class="sr-only"
              />
              {source.charAt(0).toUpperCase() + source.slice(1)}
            </label>
          {/each}
        </div>

        <div class="filter-group">
          <span class="filter-label">Level</span>
          {#each ALL_LEVELS as level}
            <label
              class="toggle-chip {LEVEL_CLASSES[level]}"
              class:toggle-chip-active={enabledLevels[level]}
            >
              <input
                type="checkbox"
                bind:checked={enabledLevels[level]}
                class="sr-only"
              />
              {level.charAt(0).toUpperCase() + level.slice(1)}
            </label>
          {/each}
        </div>

        <div class="search-group">
          <input
            type="search"
            class="search-input"
            placeholder="Search logs..."
            bind:value={searchInput}
            oninput={handleSearchInput}
            aria-label="Search logs"
          />
        </div>
      </div>

      <div class="toolbar-row toolbar-actions">
        <div class="toolbar-info">
          <span class="log-count">{filteredLogs.length.toLocaleString()} / {logs.length.toLocaleString()} entries</span>
          {#if droppedCount > 0}
            <span class="ps-badge ps-badge-warning" title="Log lines that were not shown here: they left the optimizer's log buffer (its most recent 2000 lines) before they could be sent, and they are not kept anywhere else. GET /v1/logs returns what the buffer still holds.">{droppedCount} dropped</span>
          {/if}
        </div>

        <div class="toolbar-buttons">
          <button class="ps-btn ps-btn-sm" onclick={togglePause} title={paused ? 'Resume auto-scroll' : 'Pause auto-scroll'}>
            {#if paused}
              Resume
              {#if newWhilePaused > 0}
                <span class="ps-badge ps-badge-info badge-inline">{newWhilePaused} new</span>
              {/if}
            {:else}
              Pause
            {/if}
          </button>
          <button class="ps-btn ps-btn-sm" onclick={clearLogs} title="Clear all logs">
            Clear
          </button>
          <button class="ps-btn ps-btn-sm" onclick={exportTxt} title="Export visible logs as TXT">
            Export TXT
          </button>
          <button class="ps-btn ps-btn-sm" onclick={exportJsonLogs} title="Export visible logs as JSON">
            Export JSON
          </button>
          <HelpIcon tooltip="Debug console streams worker logs via WebSocket. Use source and level toggles to filter. The worker maintains a ring buffer of the last 5000 entries. Auto-scroll pauses when you click Pause; new messages are counted in the badge." />
        </div>
      </div>
    </div>

    <!-- Log stream -->
    <div class="log-container" bind:this={logContainer} role="log" aria-live="off" aria-label="Log stream">
      {#if filteredLogs.length === 0}
        <div class="log-empty">
          {#if logs.length === 0}
            <p>No log entries yet. Logs will appear here when the worker generates them.</p>
          {:else}
            <p>No entries match the current filters.</p>
          {/if}
        </div>
      {:else}
        {#each filteredLogs as entry (entry._clientId)}
          <div class="log-line {LEVEL_CLASSES[entry.level]}">
            <span class="log-ts">{formatLogTimestamp(entry.timestamp)}</span>
            <span class="log-level-badge {LEVEL_CLASSES[entry.level]}">{entry.level.toUpperCase()}</span>
            <span class="log-source-badge">{entry.source}</span>
            {#if entry.module}
              <span class="log-module-badge" style="color: {moduleColor(entry.module)}">[{entry.module}]</span>
            {/if}
            <span class="log-message">{entry.message}</span>
            {#if entry.details}
              <span class="log-details">{JSON.stringify(entry.details)}</span>
            {/if}
          </div>
        {/each}
      {/if}
    </div>
  </div>
{/if}

<style>
  /* ── Layout ──────────────────────────────────────────────────── */

  .page-center {
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100%;
    padding: var(--ps-space-xl);
  }

  .center-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    padding: var(--ps-space-xl);
    text-align: center;
  }

  .center-text {
    margin: 0;
    color: var(--ps-fg-muted);
  }

  .log-page {
    display: flex;
    flex-direction: column;
    height: 100%;
    overflow: hidden;
  }

  /* ── Toolbar ─────────────────────────────────────────────────── */

  .toolbar {
    flex-shrink: 0;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border);
    background: var(--ps-bg-secondary);
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .toolbar-row {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--ps-space-md);
  }

  .toolbar-actions {
    justify-content: space-between;
  }

  .filter-group {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .filter-label {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    color: var(--ps-fg-muted);
    margin-right: 2px;
  }

  .toggle-chip {
    display: inline-flex;
    align-items: center;
    padding: 2px 8px;
    font-size: var(--ps-font-size-sm);
    border: 1px solid var(--ps-border);
    border-radius: 10px;
    cursor: pointer;
    color: var(--ps-fg-muted);
    background: transparent;
    transition:
      background-color 0.1s,
      border-color 0.1s,
      color 0.1s;
    user-select: none;
  }

  .toggle-chip:hover {
    background: var(--ps-bg-hover);
  }

  .toggle-chip-active {
    background: var(--ps-bg-hover);
    border-color: var(--ps-accent);
    color: var(--ps-fg-primary);
  }

  .toggle-chip-active.level-debug {
    border-color: #6b7280;
  }

  .toggle-chip-active.level-info {
    border-color: var(--ps-accent);
  }

  .toggle-chip-active.level-warning {
    border-color: var(--ps-warning);
  }

  .toggle-chip-active.level-error {
    border-color: var(--ps-error);
  }

  .sr-only {
    position: absolute;
    width: 1px;
    height: 1px;
    padding: 0;
    margin: -1px;
    overflow: hidden;
    clip: rect(0, 0, 0, 0);
    white-space: nowrap;
    border: 0;
  }

  .search-group {
    flex: 1;
    min-width: 160px;
    max-width: 320px;
  }

  .search-input {
    width: 100%;
    padding: 4px 8px;
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-family);
    color: var(--ps-fg-primary);
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: 4px;
  }

  .search-input::placeholder {
    color: var(--ps-fg-muted);
  }

  .search-input:focus {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: -1px;
  }

  .toolbar-info {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .log-count {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  .toolbar-buttons {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .badge-inline {
    margin-left: 4px;
    font-size: 10px;
    padding: 1px 5px;
  }

  /* ── Log container ───────────────────────────────────────────── */

  .log-container {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    font-family: var(--ps-font-family-mono, 'Menlo', 'Consolas', monospace);
    font-size: 12px;
    line-height: 1.5;
    padding: var(--ps-space-xs) 0;
    background: var(--ps-bg-primary);
  }

  .log-empty {
    display: flex;
    justify-content: center;
    align-items: center;
    height: 100%;
    color: var(--ps-fg-muted);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .log-empty p {
    margin: 0;
  }

  /* ── Log line ────────────────────────────────────────────────── */

  .log-line {
    display: flex;
    align-items: baseline;
    gap: 6px;
    padding: 1px var(--ps-space-md);
    white-space: nowrap;
  }

  .log-line:hover {
    background: var(--ps-bg-hover);
  }

  .log-line.level-error {
    background: color-mix(in srgb, var(--ps-error) 8%, transparent);
  }

  .log-line.level-error:hover {
    background: color-mix(in srgb, var(--ps-error) 14%, transparent);
  }

  .log-line.level-warning {
    background: color-mix(in srgb, var(--ps-warning) 5%, transparent);
  }

  .log-ts {
    color: var(--ps-fg-muted);
    flex-shrink: 0;
  }

  .log-level-badge {
    flex-shrink: 0;
    width: 52px;
    text-align: center;
    font-size: 10px;
    font-weight: 600;
    padding: 0 4px;
    border-radius: 3px;
    letter-spacing: 0.03em;
  }

  .log-level-badge.level-debug {
    color: #9ca3af;
    background: color-mix(in srgb, #6b7280 15%, transparent);
  }

  .log-level-badge.level-info {
    color: var(--ps-accent);
    background: color-mix(in srgb, var(--ps-accent) 15%, transparent);
  }

  .log-level-badge.level-warning {
    color: var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
  }

  .log-level-badge.level-error {
    color: var(--ps-error);
    background: color-mix(in srgb, var(--ps-error) 15%, transparent);
  }

  .log-source-badge {
    flex-shrink: 0;
    width: 52px;
    text-align: center;
    font-size: 10px;
    font-weight: 500;
    color: var(--ps-fg-muted);
    padding: 0 4px;
    border: 1px solid var(--ps-border);
    border-radius: 3px;
  }

  .log-module-badge {
    flex-shrink: 0;
    font-size: 11px;
    font-weight: 500;
  }

  .log-message {
    color: var(--ps-fg-primary);
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .log-details {
    color: var(--ps-fg-muted);
    font-size: 11px;
    overflow: hidden;
    text-overflow: ellipsis;
  }
</style>
