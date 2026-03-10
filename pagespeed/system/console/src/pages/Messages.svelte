<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const messages = usePolling(() => api.getMessages(), 5000);

  let showError = $state(true);
  let showWarning = $state(true);
  let showInfo = $state(true);
  let showFatal = $state(true);

  const severityOrder: Record<string, number> = {
    fatal: 0,
    error: 1,
    warning: 2,
    info: 3,
  };

  /**
   * Parse a timestamp from the message text.
   * Messages typically start with "[Tue, 10 Mar 2026 08:09:07 GMT]".
   * Returns epoch ms or 0 if unparseable.
   */
  function parseTimestampFromMessage(msg: { timestamp?: number; message: string }): number {
    // If the API already provides a numeric timestamp, use it.
    if (msg.timestamp) {
      return msg.timestamp > 1e12 ? msg.timestamp : msg.timestamp * 1000;
    }
    // Otherwise extract from the message text: "[Thu, 01 Jan 2026 00:00:00 GMT]"
    const match = msg.message.match(/^\[([^\]]+)\]/);
    if (match) {
      const parsed = Date.parse(match[1]);
      if (!isNaN(parsed)) return parsed;
    }
    return 0;
  }

  let filteredMessages = $derived.by(() => {
    if (!messages.data?.messages) return [];
    const visibleSeverities = new Set<string>();
    if (showFatal) visibleSeverities.add("fatal");
    if (showError) visibleSeverities.add("error");
    if (showWarning) visibleSeverities.add("warning");
    if (showInfo) visibleSeverities.add("info");

    return messages.data.messages
      .filter((m) => visibleSeverities.has(m.severity))
      .map((m) => ({ ...m, _ts: parseTimestampFromMessage(m) }))
      .sort((a, b) => {
        // Most recent first.
        if (a._ts !== b._ts) return b._ts - a._ts;
        return (severityOrder[a.severity] ?? 99) - (severityOrder[b.severity] ?? 99);
      });
  });

  let severityCounts = $derived.by(() => {
    const counts: Record<string, number> = { fatal: 0, error: 0, warning: 0, info: 0 };
    if (!messages.data?.messages) return counts;
    for (const m of messages.data.messages) {
      counts[m.severity] = (counts[m.severity] ?? 0) + 1;
    }
    return counts;
  });

  function toggleAutoRefresh() {
    if (messages.autoRefresh) {
      messages.stop();
    } else {
      messages.start();
    }
  }

  function formatTime(ts: number): string {
    if (!ts) return "";
    // ts is already in ms (parseTimestampFromMessage normalises).
    return new Date(ts).toLocaleString();
  }

  function severityClass(severity: string): string {
    switch (severity) {
      case "fatal":
        return "severity-fatal";
      case "error":
        return "severity-error";
      case "warning":
        return "severity-warning";
      case "info":
        return "severity-info";
      default:
        return "";
    }
  }
</script>

<div class="page">
  <div class="header">
    <h1>Messages</h1>
    <div class="controls">
      <button class="btn btn-secondary" onclick={toggleAutoRefresh}>
        {messages.autoRefresh ? "Pause" : "Resume"} Auto-Refresh
      </button>
      <button class="btn btn-primary" onclick={() => messages.refresh()}>
        Refresh Now
      </button>
    </div>
  </div>

  {#if messages.loading}
    <p class="loading">Loading messages...</p>
  {:else if messages.error}
    <p class="error">{messages.error.message}</p>
  {:else}
    <div class="filters">
      <span class="filter-label">Filter by severity:</span>
      <label class="filter-checkbox">
        <input type="checkbox" bind:checked={showFatal} />
        <span class="badge severity-fatal">Fatal ({severityCounts.fatal})</span>
      </label>
      <label class="filter-checkbox">
        <input type="checkbox" bind:checked={showError} />
        <span class="badge severity-error">Error ({severityCounts.error})</span>
      </label>
      <label class="filter-checkbox">
        <input type="checkbox" bind:checked={showWarning} />
        <span class="badge severity-warning">Warning ({severityCounts.warning})</span>
      </label>
      <label class="filter-checkbox">
        <input type="checkbox" bind:checked={showInfo} />
        <span class="badge severity-info">Info ({severityCounts.info})</span>
      </label>
      <span class="count">Showing {filteredMessages.length} of {messages.data?.messages?.length ?? 0}</span>
    </div>

    <div class="messages-list">
      {#each filteredMessages as msg, i (i)}
        <div class="message-row {severityClass(msg.severity)}">
          <div class="message-meta">
            <span class="severity-tag {severityClass(msg.severity)}">{msg.severity.toUpperCase()}</span>
            <span class="message-time">{formatTime(msg._ts)}</span>
          </div>
          <div class="message-text">{msg.message}</div>
        </div>
      {:else}
        <p class="empty">No messages match the selected filters.</p>
      {/each}
    </div>
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

  .filters {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border-light);
    border-radius: var(--ps-border-radius);
  }

  .filter-label {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-text-secondary);
  }

  .filter-checkbox {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    cursor: pointer;
    font-size: var(--ps-font-size-sm);
  }

  .filter-checkbox input {
    cursor: pointer;
  }

  .count {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
    margin-left: auto;
  }

  .badge {
    font-size: var(--ps-font-size-xs);
    padding: 1px var(--ps-space-sm);
    border-radius: var(--ps-border-radius);
    font-weight: 600;
  }

  .messages-list {
    display: flex;
    flex-direction: column;
    gap: 1px;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: hidden;
    max-height: 70vh;
    overflow-y: auto;
  }

  .message-row {
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg);
    border-left: 3px solid transparent;
  }

  .message-row:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  .message-row.severity-fatal {
    border-left-color: #8b0000;
    background: color-mix(in srgb, #8b0000 5%, var(--ps-bg));
  }

  .message-row.severity-error {
    border-left-color: var(--ps-error);
    background: color-mix(in srgb, var(--ps-error) 5%, var(--ps-bg));
  }

  .message-row.severity-warning {
    border-left-color: var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 5%, var(--ps-bg));
  }

  .message-row.severity-info {
    border-left-color: var(--ps-primary);
  }

  .message-meta {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-xs);
  }

  .severity-tag {
    font-size: var(--ps-font-size-xs);
    font-weight: 700;
    padding: 1px var(--ps-space-sm);
    border-radius: var(--ps-border-radius);
    text-transform: uppercase;
  }

  .severity-tag.severity-fatal {
    color: #ffffff;
    background: #8b0000;
  }

  .severity-tag.severity-error {
    color: #ffffff;
    background: var(--ps-error);
  }

  .severity-tag.severity-warning {
    color: #000000;
    background: var(--ps-warning);
  }

  .severity-tag.severity-info {
    color: var(--ps-text-secondary);
    background: var(--ps-bg-tertiary);
  }

  .message-time {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
  }

  .message-text {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    line-height: 1.4;
    white-space: pre-wrap;
    word-break: break-word;
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

  .empty {
    color: var(--ps-text-tertiary);
    text-align: center;
    padding: var(--ps-space-xl);
  }

  @media (max-width: 600px) {
    .header {
      flex-direction: column;
      align-items: flex-start;
    }

    .filters {
      flex-direction: column;
      align-items: flex-start;
    }

    .count {
      margin-left: 0;
    }
  }
</style>
