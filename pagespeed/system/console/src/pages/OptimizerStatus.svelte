<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Optimizer status: the optimizer daemon's health, load and cache on one
  // page, read through the module's read-only /v1/daemon/ proxy by ONE
  // composite poll (health, statistics, cooldowns; optimizer-status.ts).
  // Each section renders from its own read. An unavailable read (404: this
  // module serves no daemon endpoints; 501: this optimizer lacks the
  // endpoint; 502: unreachable; 503: not configured) is a normal operating
  // state rendered as an explanation, never an error. The retired Daemon
  // Status, Daemon Back-pressure and Daemon Cache pages redirect here with a
  // section= parameter (redirects.ts).
  import { tick } from "svelte";
  import { usePolling } from "$lib/api/polling.svelte";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    checkResult,
    counterRows,
    daemonUnavailableReason,
    fieldValue,
    isDaemonUnavailable,
    normalizeCooldowns,
    objectEntries,
  } from "$lib/utils/daemon";
  import { formatBytes, formatCount, formatDuration, formatPercent } from "$lib/utils/format";
  import { foldStatus, optimizerSection, sampleStatus, statusView, type StatusSample } from "$lib/utils/optimizer-status";
  import { serveSavingsView } from "$lib/utils/serve-savings";
  import { hostSavingsView } from "$lib/utils/host-savings";

  const { api, lens } = useConsole();

  // A section= link (the retired pages' redirects, the Savings page's link)
  // brings its section into view once the first answer has rendered, and
  // names its heading for the shell's focus move (data-page-focus).
  const section = optimizerSection(router.params.get("section"));
  let scrollPending = section !== null;

  let updatedAt = $state<number | null>(null);
  // The previous folded sample, so a busy read keeps its last answer. Plain
  // bookkeeping inside the fetcher, never reactive.
  let previous: StatusSample | null = null;
  const poll = usePolling(async () => {
    const folded = foldStatus(previous, await sampleStatus(api));
    previous = folded;
    if (folded.stats.ok) lens.observe(hostSavingsView(folded.stats.data.serve_savings_by_host)?.hosts.map((r) => r.host) ?? [], 0);
    updatedAt = Date.now();
    if (scrollPending) {
      scrollPending = false;
      void scrollToSectionWhenRendered();
    }
    return folded;
  }, 5000);

  // The first answer renders a microtask or two after the fetcher returns;
  // wait across a few ticks (the Logs page's pin uses the same idiom), then
  // bring the section into view.
  async function scrollToSectionWhenRendered(attempts = 3): Promise<void> {
    for (let i = 0; i < attempts && poll.loading; i++) await tick();
    await tick();
    document.getElementById(`optimizer-${section}`)?.scrollIntoView({ block: "start" });
  }

  let view = $derived(statusView(poll.data ?? null));
  let statsUnavailable = $derived(view.statsError !== null && isDaemonUnavailable(view.statsError));

  type BadgeColor = "success" | "warning" | "neutral";
  let statusColor = $derived.by((): BadgeColor => {
    const status = view.health?.status;
    if (!status) return "neutral";
    return status === "ok" ? "success" : "warning";
  });
  let checks = $derived(objectEntries(view.health?.checks));
  let browser = $derived(view.health?.browser);
  // Most time remaining first — the entries the operator is waiting on.
  let cooldownEntries = $derived(
    [...normalizeCooldowns(view.cooldowns)].sort((a, b) => (b.remaining_seconds ?? 0) - (a.remaining_seconds ?? 0)),
  );
  let savings = $derived(serveSavingsView(view.stats?.serve_savings));

  function toggleAutoRefresh() {
    if (poll.autoRefresh) {
      poll.stop();
    } else {
      poll.start();
    }
  }
</script>

<div class="page">
  <PageHeader
    title="Optimizer status"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => poll.refresh(),
    }}
  />
  <RefreshNotice error={poll.error} />

  {#if view.allUnavailable && view.unavailableError}
    <div class="empty-state" data-testid="daemon-unreachable">
      <p class="empty-title">Optimizer unreachable</p>
      <p class="empty-reason">{daemonUnavailableReason(view.unavailableError)}</p>
      <p class="empty-description">
        The module could not reach the optimizer daemon ({view.unavailableError.message}).
        The daemon may be stopped or not configured, or this build does not
        serve the daemon endpoints; the module keeps serving without it.
        This page populates once the daemon is reachable.
      </p>
    </div>
  {/if}

  <div class="daemon-sections">
  <section class="daemon-section" id="optimizer-health" aria-labelledby="optimizer-health-heading">
    <h2 id="optimizer-health-heading" data-page-focus={section === "health" ? "" : undefined}>Health</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's health...</p>
    {:else if view.health}
      <div class="status-card">
        <div class="status-row">
          <span class="status-badge badge-{statusColor}" role="status">
            {view.health.status ?? "unknown"}
          </span>
          {#if view.health.ready !== undefined}
            <span class="ready-flag">
              {view.health.ready ? "ready" : "not ready"}
            </span>
          {/if}
        </div>
        <div class="info-grid">
          <div class="info-item">
            <span class="info-label metric-label">Version</span>
            <span class="info-value metric-value mono-value">{view.health.version ?? "—"}</span>
          </div>
          <div class="info-item">
            <span class="info-label metric-label">Commit</span>
            <span class="info-value metric-value mono-value">{view.health.git_commit ?? "—"}</span>
          </div>
          <div class="info-item">
            <span class="info-label metric-label">Uptime</span>
            <span class="info-value metric-value num">{formatDuration(view.health.uptime_seconds)}</span>
          </div>
          <div class="info-item">
            <span class="info-label metric-label">Connections</span>
            <span class="info-value metric-value num">
              {fieldValue(view.health.connections?.active)} of {fieldValue(view.health.connections?.max)}
            </span>
          </div>
          <div class="info-item">
            <span class="info-label metric-label">In-flight requests</span>
            <span class="info-value metric-value num">{fieldValue(view.health.inflight)}</span>
          </div>
        </div>
      </div>

      {#if checks.length > 0}
        <h3>Checks</h3>
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-wrapper" role="region" aria-label="Checks" tabindex="0">
          <table>
            <thead>
              <tr>
                <th>Check</th>
                <th>Result</th>
              </tr>
            </thead>
            <tbody>
              {#each checks as [name, value] (name)}
                {@const result = checkResult(value)}
                <tr>
                  <td class="name-cell">{name}</td>
                  <td class="check-cell">
                    <span class="check" class:check-pass={result.pass === true} class:check-fail={result.pass === false}>{result.text}</span>
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}

      {#if browser}
        <h3>Browser analysis</h3>
        <div class="info-grid">
          <div class="info-item">
            <span class="info-label metric-label">Enabled</span>
            <span class="info-value metric-value num">{fieldValue(browser.enabled)}</span>
          </div>
          {#if browser.enabled}
            {#if browser.chrome_running !== undefined}
              <div class="info-item">
                <span class="info-label metric-label">Browser running</span>
                <span class="info-value metric-value num">{fieldValue(browser.chrome_running)}</span>
              </div>
            {/if}
            {#if browser.chrome_consecutive_failures !== undefined}
              <div class="info-item">
                <span class="info-label metric-label">Consecutive failures</span>
                <span class="info-value metric-value num">{fieldValue(browser.chrome_consecutive_failures)}</span>
              </div>
            {/if}
            {#if browser.chrome_restart_delay_ms !== undefined}
              <div class="info-item">
                <span class="info-label metric-label">Restart delay</span>
                <span class="info-value metric-value num">{fieldValue(browser.chrome_restart_delay_ms)} ms</span>
              </div>
            {/if}
          {/if}
        </div>
      {/if}
    {:else if view.healthError && !view.allUnavailable}
      {#if isDaemonUnavailable(view.healthError)}
        <p class="empty" data-testid="health-unavailable">Health is not available: {daemonUnavailableReason(view.healthError)}.</p>
      {:else}
        <LoadError message={view.healthError.message} />
      {/if}
    {/if}
  </section>

  <section class="daemon-section" id="optimizer-load" aria-labelledby="optimizer-load-heading">
    <h2 id="optimizer-load-heading" data-page-focus={section === "load" ? "" : undefined}>Load</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's load...</p>
    {:else if view.stats}
      <div class="info-grid info-grid--panel">
        <div class="info-item">
          <span class="info-label metric-label">Thread pool</span>
          <span class="info-value metric-value num">
            {fieldValue(view.stats.thread_pool?.inflight)} of {fieldValue(view.stats.thread_pool?.size)} busy
          </span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Connections</span>
          <span class="info-value metric-value num">
            {fieldValue(view.stats.connections?.active)} of {fieldValue(view.stats.connections?.max)}
          </span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Notifications received</span>
          <span class="info-value metric-value num">{fieldValue(view.stats.notifications?.received)}</span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Skipped (duplicate)</span>
          <span class="info-value metric-value num">{fieldValue(view.stats.notifications?.skipped_dedup)}</span>
          <span class="info-hint">Notifications for URLs the optimizer had already handled, or is holding back for a while (for example after a failed write). Not an error; the optimizer's log names the reason for each.</span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Skipped (in-flight)</span>
          <span class="info-value metric-value num">{fieldValue(view.stats.notifications?.skipped_inflight)}</span>
          <span class="info-hint">Notifications for URLs the optimizer was already working on.</span>
        </div>
      </div>

      <h3>Cache cooldowns</h3>
      {#if view.cooldowns === null}
        <p class="empty">The cooldown list is unavailable on this daemon.</p>
      {:else if cooldownEntries.length === 0}
        <p class="empty">No URLs in cooldown.</p>
      {:else}
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-wrapper" role="region" aria-label="Cache cooldowns" tabindex="0">
          <table>
            <thead>
              <tr>
                <th>URL</th>
                <th>Reason</th>
                <th class="num">Remaining</th>
                <th class="num">Duration</th>
              </tr>
            </thead>
            <tbody>
              <!-- Keyed by index: entry.url is optional and untrusted, so
                   duplicate or absent URLs must not throw. -->
              {#each cooldownEntries as entry, i (i)}
                <tr>
                  <td class="name-cell">{entry.url ?? "—"}</td>
                  <td>{entry.reason ?? "—"}</td>
                  <td class="value-cell">{formatDuration(entry.remaining_seconds)}</td>
                  <td class="value-cell">{formatDuration(entry.duration_seconds)}</td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    {:else if view.statsError && !view.allUnavailable}
      {#if statsUnavailable}
        <div class="empty-state" data-testid="daemon-unreachable">
          <p class="empty-title">Optimizer unreachable</p>
          <p class="empty-reason">{daemonUnavailableReason(view.statsError)}</p>
          <p class="empty-description">
            The module could not read the optimizer's statistics ({view.statsError.message}).
            The load and cache sections populate once it answers.
          </p>
        </div>
      {:else}
        <LoadError message={view.statsError.message} />
      {/if}
    {/if}
  </section>

  <section class="daemon-section" id="optimizer-cache" aria-labelledby="optimizer-cache-heading">
    <h2 id="optimizer-cache-heading" data-page-focus={section === "cache" ? "" : undefined}>Cache</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's cache statistics...</p>
    {:else if view.stats}
      <div class="info-grid info-grid--panel">
        <div class="info-item">
          <span class="info-label metric-label">Cache entries</span>
          <span class="info-value metric-value num">{fieldValue(view.stats.cache?.entries)}</span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Cache size</span>
          <span class="info-value metric-value num">{formatBytes(view.stats.cache?.size_bytes)}</span>
        </div>
      </div>

      <h3 id="serve-savings-heading">Serve savings</h3>
      <p class="section-note" data-testid="serve-savings-source">
        Recorded by this web server's module each time it serves an optimized
        response from the optimizer's cache.
      </p>
      {#if savings === null}
        <p class="empty" data-testid="serve-savings-absent">This optimizer version reports no serve savings.</p>
      {:else}
        {#if savings.served.length > 0}
          <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
          <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
          <div class="table-wrapper" role="region" aria-label="Serve savings" tabindex="0">
            <table data-testid="serve-savings-table" aria-labelledby="serve-savings-heading">
              <thead>
                <tr>
                  <th scope="col">Content</th>
                  <th scope="col" class="num">Responses</th>
                  <th scope="col" class="num">Original</th>
                  <th scope="col" class="num">Served</th>
                  <th scope="col" class="num">Saved</th>
                </tr>
              </thead>
              <tbody>
                {#each savings.served as row (row.key)}
                  <tr>
                    <th scope="row" class="name-cell">{row.label}</th>
                    <td class="value-cell">{formatCount(row.hits)}</td>
                    <td class="value-cell">{formatBytes(row.original)}</td>
                    <td class="value-cell">{formatBytes(row.served)}</td>
                    <td class="value-cell">{formatBytes(row.saved)}{row.percent !== null ? ` (${formatPercent(row.percent)})` : ""}</td>
                  </tr>
                {/each}
              </tbody>
              {#if savings.total}
                {@const totalHits = savings.served.reduce((acc, row) => acc + row.hits, 0)}
                {@const totalServed = savings.served.reduce((acc, row) => acc + row.served, 0)}
                <tfoot>
                  <tr data-testid="serve-savings-total">
                    <th scope="row" class="name-cell">Total</th>
                    <td class="value-cell">{formatCount(totalHits)}</td>
                    <td class="value-cell">{formatBytes(savings.total.original)}</td>
                    <td class="value-cell">{formatBytes(totalServed)}</td>
                    <td class="value-cell">{formatBytes(savings.total.saved)} ({formatPercent(savings.total.percent)})</td>
                  </tr>
                </tfoot>
              {/if}
            </table>
          </div>
        {:else}
          <p class="empty" data-testid="serve-savings-none">
            No optimized responses have been served from the optimizer's cache yet.
          </p>
        {/if}
        {#if savings.notServed.length > 0}
          <p class="section-note" data-testid="serve-savings-not-served">Not served from the optimizer's cache on this server so far: {savings.notServed.join(", ")}.</p>
        {/if}
        <details class="raw" data-testid="serve-savings-raw">
          <summary>Raw counters</summary>
          <table class="raw-table">
            <thead>
              <tr><th>Counter</th><th>Value</th></tr>
            </thead>
            <tbody>
              {#each counterRows(view.stats.serve_savings, "serve_savings") as row (row.name)}
                <tr><td>{row.name}</td><td>{row.value}</td></tr>
              {/each}
            </tbody>
          </table>
        </details>
      {/if}
    {:else if view.statsError && !view.allUnavailable}
      <p class="empty" data-testid="cache-unavailable">The optimizer's statistics are not available; see Load above.</p>
    {/if}
  </section>
  </div>
</div>

<style>
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
    text-transform: capitalize;
  }

  .badge-success {
    background: var(--ps-success-text);
    color: var(--ps-text-inverse);
  }

  .badge-warning {
    background: var(--ps-warning);
    color: var(--ps-warning-contrast-text);
  }

  .badge-neutral {
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }

  .ready-flag {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    text-transform: capitalize;
  }

  /* One column on a laptop; on a wide window the sections sit side by
     side instead of stretching their figures across the screen. */
  .daemon-sections {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-card-max)), 1fr));
    align-items: start;
    column-gap: var(--ps-space-xl);
  }

  .daemon-section {
    min-width: 0;
    margin-bottom: var(--ps-space-xl);
    scroll-margin-top: calc(var(--ps-topbar-height) + var(--ps-space-md));
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  h3 {
    font-size: var(--ps-font-size-base);
    font-weight: 600;
    margin: var(--ps-space-lg) 0 var(--ps-space-sm) 0;
  }

  .info-grid {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
  }

  .info-grid--panel {
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

  .info-hint {
    max-width: 28ch;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }

  .section-note {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  /* A wide table scrolls inside its frame and never widens the page: the
     zero width keeps it out of the page's minimum width on a phone, and
     min-width fills the column. */
  .table-wrapper {
    width: 0;
    min-width: 100%;
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

  th.num {
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

  .check-cell {
    font-family: var(--ps-font-mono);
  }

  .value-cell {
    font-family: var(--ps-font-mono);
    text-align: right;
    white-space: nowrap;
  }

  .check::before {
    content: "";
    display: inline-block;
    width: 0.6em;
    height: 0.6em;
    margin-right: 0.4em;
    border-radius: 50%;
    background: var(--ps-text-secondary);
  }

  .check-pass::before {
    background: var(--ps-success);
  }

  .check-fail::before {
    background: var(--ps-error);
  }

  .empty {
    color: var(--ps-text-secondary);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .empty-state {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-lg);
    margin-bottom: var(--ps-space-lg);
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

  .empty-reason {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-sm);
  }

  .empty-description {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    line-height: 1.5;
  }

  .raw {
    margin-top: var(--ps-space-sm);
  }

  .raw-table {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--ps-font-size-xs);
    font-family: var(--ps-font-mono);
  }

  .raw-table th,
  .raw-table td {
    text-align: left;
    padding-right: var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .raw-table th {
    color: var(--ps-text-secondary);
  }
</style>
