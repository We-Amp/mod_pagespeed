<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Daemon back-pressure: where the optimizer daemon sheds load — thread-pool
  // occupancy, dropped notifications, connection pressure, and the cache
  // cooldown list (/v1/daemon/stats + /v1/daemon/cooldowns).
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    fieldValue,
    isDaemonUnavailable,
    normalizeCooldowns,
  } from "$lib/utils/daemon";
  import type {
    DaemonCooldownsResponse,
    DaemonStatsResponse,
  } from "$lib/api/types";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);

  interface PressureData {
    stats: DaemonStatsResponse;
    cooldowns: DaemonCooldownsResponse | null;
  }

  // The cooldowns list is a detail view: if it alone fails (older daemon),
  // the pressure counters still render and the list reports itself unavailable.
  const pressure = usePolling<PressureData>(async () => {
    const stats = await api.daemonStats();
    let cooldowns: DaemonCooldownsResponse | null = null;
    try {
      cooldowns = await api.daemonCooldowns();
    } catch {
      cooldowns = null;
    }
    return { stats, cooldowns };
  }, 5000);

  let unreachable = $derived(isDaemonUnavailable(pressure.error));

  let cooldownEntries = $derived.by(() => {
    const entries = normalizeCooldowns(pressure.data?.cooldowns);
    // Most time remaining first — the entries the operator is waiting on.
    return [...entries].sort(
      (a, b) => (b.remaining_seconds ?? 0) - (a.remaining_seconds ?? 0),
    );
  });

  function formatSeconds(seconds: number | undefined): string {
    if (seconds === undefined || !Number.isFinite(seconds)) return "\u2014";
    return `${seconds.toLocaleString()} s`;
  }

  function toggleAutoRefresh() {
    if (pressure.autoRefresh) {
      pressure.stop();
    } else {
      pressure.start();
    }
  }
</script>

<div class="page">
  <div class="header">
    <h1>Daemon Back-pressure</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {pressure.autoRefresh ? "Pause" : "Resume"} Auto-Refresh
      </button>
      <button class="btn btn-primary" onclick={() => pressure.refresh()}>
        Refresh Now
      </button>
    </div>
  </div>

  {#if pressure.loading}
    <p class="loading">Loading daemon back-pressure...</p>
  {:else if pressure.error && !pressure.data}
    {#if unreachable}
      <div class="empty-state" data-testid="daemon-unreachable">
        <p class="empty-title">Daemon Unreachable</p>
        <p class="empty-description">
          The module could not reach the optimizer daemon ({pressure.error.message}).
          The daemon may be stopped or not configured, or this build does not
          serve the daemon endpoints; the module keeps serving without it.
          This panel populates once the daemon is reachable.
        </p>
      </div>
    {:else}
      <p class="error">{pressure.error.message}</p>
    {/if}
  {:else if pressure.data}
    <RefreshNotice error={pressure.error} />

    <section class="daemon-section">
      <h2>Load</h2>
      <div class="info-grid">
        <div class="info-item">
          <span class="info-label">Thread pool</span>
          <span class="info-value">
            {fieldValue(pressure.data.stats.thread_pool?.inflight)} of {fieldValue(pressure.data.stats.thread_pool?.size)} busy
          </span>
        </div>
        <div class="info-item">
          <span class="info-label">Connections</span>
          <span class="info-value">
            {fieldValue(pressure.data.stats.connections?.active)} of {fieldValue(pressure.data.stats.connections?.max)}
          </span>
        </div>
        <div class="info-item">
          <span class="info-label">Notifications received</span>
          <span class="info-value">{fieldValue(pressure.data.stats.notifications?.received)}</span>
        </div>
        <div class="info-item">
          <span class="info-label">Skipped (duplicate)</span>
          <span class="info-value">{fieldValue(pressure.data.stats.notifications?.skipped_dedup)}</span>
        </div>
        <div class="info-item">
          <span class="info-label">Skipped (in-flight)</span>
          <span class="info-value">{fieldValue(pressure.data.stats.notifications?.skipped_inflight)}</span>
        </div>
      </div>
    </section>

    <section class="daemon-section">
      <h2>Cache Cooldowns</h2>
      {#if pressure.data.cooldowns === null}
        <p class="empty">The cooldown list is unavailable on this daemon.</p>
      {:else if cooldownEntries.length === 0}
        <p class="empty">No URLs in cooldown.</p>
      {:else}
        <div class="table-wrapper">
          <table>
            <thead>
              <tr>
                <th>URL</th>
                <th>Reason</th>
                <th class="numeric">Remaining</th>
                <th class="numeric">Duration</th>
              </tr>
            </thead>
            <tbody>
              <!-- Keyed by index: entry.url is optional and untrusted, so
                   duplicate or absent URLs must not throw. -->
              {#each cooldownEntries as entry, i (i)}
                <tr>
                  <td class="name-cell">{entry.url ?? "\u2014"}</td>
                  <td>{entry.reason ?? "\u2014"}</td>
                  <td class="value-cell">{formatSeconds(entry.remaining_seconds)}</td>
                  <td class="value-cell">{formatSeconds(entry.duration_seconds)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
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

  .controls {
    display: flex;
    gap: var(--ps-space-sm);
  }

  .daemon-section {
    margin-bottom: var(--ps-space-xl);
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  .info-grid {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .info-item {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .info-label {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-text-tertiary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .info-value {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    color: var(--ps-text);
  }

  .table-wrapper {
    overflow-x: auto;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
  }

  table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }

  thead {
    background: var(--ps-bg-tertiary);
    position: sticky;
    top: 0;
  }

  th {
    text-align: left;
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
    white-space: nowrap;
  }

  th.numeric {
    text-align: right;
  }

  td {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border-light);
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  tbody tr:hover {
    background: var(--ps-surface-hover);
  }

  .name-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .empty {
    color: var(--ps-text-tertiary);
  }

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
  }

  .btn-primary {
    background: var(--ps-primary);
    color: var(--ps-text-inverse);
    border-color: var(--ps-primary);
  }

  .btn-primary:hover {
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
  }

  .empty-state {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-lg);
    border: 1px dashed var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .empty-title {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-sm);
  }

  .empty-description {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-tertiary);
    line-height: 1.5;
  }

  @media (max-width: 600px) {
    .header {
      flex-direction: column;
      align-items: flex-start;
    }
  }
</style>
