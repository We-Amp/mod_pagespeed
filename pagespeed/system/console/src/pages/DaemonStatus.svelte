<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Daemon status: health of the optimizer daemon via the module's read-only
  // /v1/daemon/ proxy. The daemon is an optional companion — a 502 means it is
  // unreachable or not configured, which is a normal operating state rendered
  // as an honest empty state, not an error.
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    fieldValue,
    formatUptime,
    isDaemonUnavailable,
    objectEntries,
  } from "$lib/utils/daemon";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const health = usePolling(() => api.daemonHealth(), 5000);

  let unreachable = $derived(isDaemonUnavailable(health.error));

  type BadgeColor = "success" | "warning" | "neutral";
  let statusColor = $derived.by((): BadgeColor => {
    const status = health.data?.status;
    if (!status) return "neutral";
    return status === "ok" ? "success" : "warning";
  });

  let checks = $derived(objectEntries(health.data?.checks));
  let browser = $derived(health.data?.browser);

  function toggleAutoRefresh() {
    if (health.autoRefresh) {
      health.stop();
    } else {
      health.start();
    }
  }
</script>

<div class="page">
  <div class="header">
    <h1>Daemon Status</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {health.autoRefresh ? "Pause" : "Resume"} Auto-Refresh
      </button>
      <button class="btn btn-primary" onclick={() => health.refresh()}>
        Refresh Now
      </button>
    </div>
  </div>

  {#if health.loading}
    <p class="loading">Loading daemon health...</p>
  {:else if health.error && !health.data}
    {#if unreachable}
      <div class="empty-state" data-testid="daemon-unreachable">
        <p class="empty-title">Daemon Unreachable</p>
        <p class="empty-description">
          The module could not reach the optimizer daemon ({health.error.message}).
          The daemon may be stopped or not configured, or this build does not
          serve the daemon endpoints; the module keeps serving without it.
          This panel populates once the daemon is reachable.
        </p>
      </div>
    {:else}
      <p class="error">{health.error.message}</p>
    {/if}
  {:else if health.data}
    <RefreshNotice error={health.error} />

    <div class="status-card">
      <div class="status-row">
        <span class="status-badge badge-{statusColor}" role="status">
          {health.data.status ?? "unknown"}
        </span>
        {#if health.data.ready !== undefined}
          <span class="ready-flag">
            {health.data.ready ? "ready" : "not ready"}
          </span>
        {/if}
      </div>
      <div class="info-grid">
        <div class="info-item">
          <span class="info-label">Version</span>
          <span class="info-value">{health.data.version ?? "\u2014"}</span>
        </div>
        <div class="info-item">
          <span class="info-label">Commit</span>
          <span class="info-value">{health.data.git_commit ?? "\u2014"}</span>
        </div>
        <div class="info-item">
          <span class="info-label">Uptime</span>
          <span class="info-value">{formatUptime(health.data.uptime_seconds)}</span>
        </div>
        <div class="info-item">
          <span class="info-label">Connections</span>
          <span class="info-value">
            {fieldValue(health.data.connections?.active)} of {fieldValue(health.data.connections?.max)}
          </span>
        </div>
        <div class="info-item">
          <span class="info-label">In-flight requests</span>
          <span class="info-value">{fieldValue(health.data.inflight)}</span>
        </div>
      </div>
    </div>

    {#if checks.length > 0}
      <section class="daemon-section">
        <h2>Checks</h2>
        <div class="table-wrapper">
          <table>
            <thead>
              <tr>
                <th>Check</th>
                <th>Result</th>
              </tr>
            </thead>
            <tbody>
              {#each checks as [name, value] (name)}
                <tr>
                  <td class="name-cell">{name}</td>
                  <td class="value-cell">{fieldValue(value)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </section>
    {/if}

    {#if browser}
      <section class="daemon-section">
        <h2>Browser Analysis</h2>
        <div class="info-grid">
          <div class="info-item">
            <span class="info-label">Enabled</span>
            <span class="info-value">{fieldValue(browser.enabled)}</span>
          </div>
          {#if browser.enabled}
            {#if browser.chrome_running !== undefined}
              <div class="info-item">
                <span class="info-label">Browser running</span>
                <span class="info-value">{fieldValue(browser.chrome_running)}</span>
              </div>
            {/if}
            {#if browser.chrome_consecutive_failures !== undefined}
              <div class="info-item">
                <span class="info-label">Consecutive failures</span>
                <span class="info-value">{fieldValue(browser.chrome_consecutive_failures)}</span>
              </div>
            {/if}
            {#if browser.chrome_restart_delay_ms !== undefined}
              <div class="info-item">
                <span class="info-label">Restart delay</span>
                <span class="info-value">{fieldValue(browser.chrome_restart_delay_ms)} ms</span>
              </div>
            {/if}
          {/if}
        </div>
      </section>
    {/if}
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

  .status-card {
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
    margin-bottom: var(--ps-space-lg);
  }

  .status-row {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-md);
  }

  .status-badge {
    display: inline-block;
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-radius: var(--ps-border-radius);
    font-weight: 700;
    font-size: var(--ps-font-size-sm);
  }

  .badge-success {
    background: var(--ps-success);
    color: #ffffff;
  }

  .badge-warning {
    background: var(--ps-warning);
    color: #000000;
  }

  .badge-neutral {
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }

  .ready-flag {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
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
  }

  th {
    text-align: left;
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-weight: 600;
    border-bottom: 2px solid var(--ps-border);
    white-space: nowrap;
  }

  td {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border-light);
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  .name-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
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
