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
  import {
    ALL_SOURCES,
    ALL_LEVELS,
    MAX_LOG_ENTRIES,
    MODULE_COLORS,
    filterLogs,
    formatLogTimestamp,
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

  let paused = $state(false);
  let newWhilePaused = $state(0);
  let logContainer: HTMLElement | undefined = $state();

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

  // -- Auto-scroll effect ----------------------------------------------------

  $effect(() => {
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

<div class="lp-toolbar">
  <div class="lp-filters">
    {#each ALL_LEVELS as level}
      <label
        class="lp-chip {LEVEL_CLASSES[level]}"
        class:lp-chip-active={enabledLevels[level]}
      >
        <input
          type="checkbox"
          bind:checked={enabledLevels[level]}
          class="sr-only"
        />
        {level.charAt(0).toUpperCase() + level.slice(1)}
      </label>
    {/each}

    {#each ALL_SOURCES as source}
      <label class="lp-chip" class:lp-chip-active={enabledSources[source]}>
        <input
          type="checkbox"
          bind:checked={enabledSources[source]}
          class="sr-only"
        />
        {source.charAt(0).toUpperCase() + source.slice(1)}
      </label>
    {/each}
  </div>

  <input
    type="search"
    class="lp-search"
    placeholder="Filter..."
    bind:value={searchInput}
    oninput={handleSearchInput}
    aria-label="Filter logs"
  />

  <span class="lp-count">{filteredLogs.length}/{logs.length}</span>

  <button class="lp-btn" onclick={togglePause} title={paused ? 'Resume' : 'Pause'}>
    {#if paused}
      Resume
      {#if newWhilePaused > 0}
        <span class="lp-badge">{newWhilePaused}</span>
      {/if}
    {:else}
      Pause
    {/if}
  </button>
  <button class="lp-btn" onclick={clearLogs} title="Clear">Clear</button>
</div>

<div class="lp-output" bind:this={logContainer} role="log" aria-live="off">
  {#if !connected}
    <div class="lp-empty">Waiting for connection...</div>
  {:else if filteredLogs.length === 0}
    <div class="lp-empty">
      {#if logs.length === 0}
        No log entries yet.
      {:else}
        No entries match filters.
      {/if}
    </div>
  {:else}
    {#each filteredLogs as entry (entry._clientId)}
      <div class="lp-line {LEVEL_CLASSES[entry.level]}">
        <span class="lp-ts">{formatLogTimestamp(entry.timestamp)}</span>
        <span class="lp-level {LEVEL_CLASSES[entry.level]}">{entry.level.toUpperCase()}</span>
        <span class="lp-source">{entry.source}</span>
        {#if entry.module}
          <span class="lp-module" style="color: {moduleColor(entry.module)}">[{entry.module}]</span>
        {/if}
        <span class="lp-msg">{entry.message}</span>
        {#if entry.details}
          <span class="lp-details">{JSON.stringify(entry.details)}</span>
        {/if}
      </div>
    {/each}
  {/if}
</div>

<style>
  /* ── Toolbar ─────────────────────────────────────────────────── */

  .lp-toolbar {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 2px 8px;
    background: var(--ps-bg-secondary);
    border-bottom: 1px solid var(--ps-border);
    flex-shrink: 0;
    overflow-x: auto;
  }

  .lp-filters {
    display: flex;
    align-items: center;
    gap: 3px;
    flex-shrink: 0;
  }

  .lp-chip {
    display: inline-flex;
    align-items: center;
    padding: 1px 6px;
    font-size: 11px;
    border: 1px solid var(--ps-border);
    border-radius: 8px;
    cursor: pointer;
    color: var(--ps-fg-muted);
    background: transparent;
    transition: background-color 0.1s, border-color 0.1s, color 0.1s;
    user-select: none;
    white-space: nowrap;
  }

  .lp-chip:hover {
    background: var(--ps-bg-hover);
  }

  .lp-chip-active {
    background: var(--ps-bg-hover);
    border-color: var(--ps-accent);
    color: var(--ps-fg-primary);
  }

  .lp-chip-active.level-debug { border-color: #6b7280; }
  .lp-chip-active.level-info { border-color: var(--ps-accent); }
  .lp-chip-active.level-warning { border-color: var(--ps-warning); }
  .lp-chip-active.level-error { border-color: var(--ps-error); }

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

  .lp-search {
    width: 120px;
    padding: 1px 6px;
    font-size: 11px;
    font-family: var(--ps-font-family);
    color: var(--ps-fg-primary);
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: 3px;
    flex-shrink: 0;
  }

  .lp-search::placeholder { color: var(--ps-fg-muted); }
  .lp-search:focus {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: -1px;
  }

  .lp-count {
    font-size: 11px;
    color: var(--ps-fg-muted);
    white-space: nowrap;
    flex-shrink: 0;
  }

  .lp-btn {
    display: inline-flex;
    align-items: center;
    gap: 3px;
    padding: 1px 6px;
    font-size: 11px;
    font-family: var(--ps-font-family);
    color: var(--ps-fg-primary);
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: 3px;
    cursor: pointer;
    white-space: nowrap;
    flex-shrink: 0;
  }

  .lp-btn:hover { background: var(--ps-bg-hover); }

  .lp-badge {
    font-size: 9px;
    padding: 0 4px;
    background: var(--ps-accent);
    color: #fff;
    border-radius: 6px;
    font-weight: 600;
  }

  /* ── Log output ──────────────────────────────────────────────── */

  .lp-output {
    flex: 1;
    overflow-y: auto;
    overflow-x: hidden;
    font-family: var(--ps-font-family-mono, 'Menlo', 'Consolas', monospace);
    font-size: 11px;
    line-height: 1.4;
    background: var(--ps-bg-primary);
  }

  .lp-empty {
    padding: 8px 12px;
    color: var(--ps-fg-muted);
    font-family: var(--ps-font-family);
    font-size: 11px;
  }

  .lp-line {
    display: flex;
    align-items: baseline;
    gap: 5px;
    padding: 0 8px;
    white-space: nowrap;
  }

  .lp-line:hover { background: var(--ps-bg-hover); }

  .lp-line.level-error {
    background: color-mix(in srgb, var(--ps-error) 8%, transparent);
  }
  .lp-line.level-error:hover {
    background: color-mix(in srgb, var(--ps-error) 14%, transparent);
  }
  .lp-line.level-warning {
    background: color-mix(in srgb, var(--ps-warning) 5%, transparent);
  }

  .lp-ts {
    color: var(--ps-fg-muted);
    flex-shrink: 0;
  }

  .lp-level {
    flex-shrink: 0;
    width: 44px;
    text-align: center;
    font-size: 9px;
    font-weight: 600;
    padding: 0 3px;
    border-radius: 2px;
    letter-spacing: 0.03em;
  }

  .lp-level.level-debug {
    color: #9ca3af;
    background: color-mix(in srgb, #6b7280 15%, transparent);
  }
  .lp-level.level-info {
    color: var(--ps-accent);
    background: color-mix(in srgb, var(--ps-accent) 15%, transparent);
  }
  .lp-level.level-warning {
    color: var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
  }
  .lp-level.level-error {
    color: var(--ps-error);
    background: color-mix(in srgb, var(--ps-error) 15%, transparent);
  }

  .lp-source {
    flex-shrink: 0;
    width: 44px;
    text-align: center;
    font-size: 9px;
    font-weight: 500;
    color: var(--ps-fg-muted);
    padding: 0 3px;
    border: 1px solid var(--ps-border);
    border-radius: 2px;
  }

  .lp-module {
    flex-shrink: 0;
    font-size: 10px;
    font-weight: 500;
  }

  .lp-msg {
    color: var(--ps-fg-primary);
    overflow: hidden;
    text-overflow: ellipsis;
  }

  .lp-details {
    color: var(--ps-fg-muted);
    font-size: 10px;
    overflow: hidden;
    text-overflow: ellipsis;
  }
</style>
