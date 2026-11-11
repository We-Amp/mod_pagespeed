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
  // state rendered as an explanation, never an error. When no read answers,
  // the page is that one explanation, named by its reason (daemon.ts), and
  // no section is drawn. The retired Daemon
  // Status, Daemon Back-pressure and Daemon Cache pages redirect here with a
  // section= parameter (redirects.ts).
  import { tick } from "svelte";
  import { usePolling } from "$lib/api/polling.svelte";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";
  import EmptyState from "$lib/EmptyState.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import HtmlCachePanel from "$lib/HtmlCachePanel.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import {
    checkLabel,
    checkResult,
    counterRows,
    daemonUnavailableReason,
    fieldValue,
    isDaemonUnavailable,
    normalizeCooldowns,
    objectEntries,
    optimizerAbsence,
    optimizerAbsenceText,
  } from "$lib/utils/daemon";
  import { formatBytes, formatCount, formatDuration, formatPercent, formatSignedBytes } from "$lib/utils/format";
  import { foldStatus, optimizerSection, sampleStatus, statusView, type StatusSample } from "$lib/utils/optimizer-status";
  import { SAVINGS_QUALIFIER, serveSavingsView, splitCells } from "$lib/utils/serve-savings";
  import SavingsSplitLine from "$lib/SavingsSplitLine.svelte";
  import { SPLIT_TEXT } from "$lib/data/split-text";
  import { TERMS } from "$lib/data/terms";
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
  // No leaf answered and each says "no optimizer here": the one reason the page gives.
  let absence = $derived(view.allUnavailable ? optimizerAbsence(view.healthError, view.statsError) : null);
  let absenceText = $derived(absence === null ? null : optimizerAbsenceText(absence));

  // The dot repeats the status word in colour: ok, anything else, or none.
  let statusTone = $derived.by((): "ok" | "warning" | null => {
    const status = view.health?.status;
    if (!status) return null;
    return status === "ok" ? "ok" : "warning";
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
      failedParts: () => [view.healthError, view.statsError],
    }}
  />
  <RefreshNotice error={poll.error} />

  {#if absence !== null && absenceText !== null}
    <div data-testid="optimizer-absent" data-reason={absence}>
      <EmptyState
        testid="daemon-unreachable"
        title={absenceText.title}
        detail={absenceText.detail}
        action={absenceText.doc}
      />
    </div>
  {:else}
  <div class="daemon-sections">
  <section class="daemon-section" id="optimizer-health" aria-labelledby="optimizer-health-heading">
    <h2 class="section-title" id="optimizer-health-heading" data-page-focus={section === "health" ? "" : undefined}>Health</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's health...</p>
    {:else if view.health}
      <div class="card status-card">
        <div class="status-row">
          <span class="status-word" role="status"><span
              class="status-dot"
              class:status-dot-ok={statusTone === "ok"}
              class:status-dot-warning={statusTone === "warning"}
              aria-hidden="true"
            ></span>{view.health.status ?? "unknown"}</span>
          {#if view.health.ready !== undefined}
            <span class="ready-flag">
              {view.health.ready ? "ready" : "not ready"}
            </span>
          {/if}
        </div>
        <div class="info-grid">
          <div class="info-item">
            <span class="info-label metric-label">Version</span>
            <span class="info-value metric-value mono-value">{view.health.version ? (view.health.version.startsWith("v") ? view.health.version : `v${view.health.version}`) : "—"}</span>
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
        <h3 class="card-title">Checks</h3>
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-frame" role="region" aria-label="Checks" tabindex="0">
          <table class="data">
            <thead>
              <tr>
                <th scope="col">Check</th>
                <th scope="col">Result</th>
              </tr>
            </thead>
            <tbody>
              {#each checks as [name, value] (name)}
                {@const result = checkResult(value)}
                <tr>
                  <th scope="row" class="name-cell" title={name}>{checkLabel(name)}</th>
                  <td class="check-cell">
                    <span class="check"><span
                      class="status-dot"
                      class:status-dot-ok={result.pass === true}
                      class:status-dot-error={result.pass === false}
                      aria-hidden="true"
                    ></span>{result.text}</span>
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}

      {#if browser}
        <h3 class="card-title">Browser analysis</h3>
        <div class="info-grid card">
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
        <p class="muted" data-testid="health-unavailable">Health is not available: {daemonUnavailableReason(view.healthError)}.</p>
      {:else}
        <LoadError error={view.healthError} noun="the optimizer's health" onretry={() => poll.refresh()} />
      {/if}
    {/if}
  </section>

  <section class="daemon-section" id="optimizer-load" aria-labelledby="optimizer-load-heading">
    <h2 class="section-title" id="optimizer-load-heading" data-page-focus={section === "load" ? "" : undefined}>Load</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's load...</p>
    {:else if view.stats}
      <div class="info-grid card">
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

      <h3 class="card-title">Cache cooldowns</h3>
      {#if view.cooldowns === null}
        <p class="muted">The cooldown list is unavailable on this optimizer.</p>
      {:else if cooldownEntries.length === 0}
        <p class="muted">No URLs in cooldown.</p>
      {:else}
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-frame" role="region" aria-label="Cache cooldowns" tabindex="0">
          <table class="data">
            <thead>
              <tr>
                <th scope="col">URL</th>
                <th scope="col">Reason</th>
                <th scope="col" class="num">Remaining</th>
                <th scope="col" class="num">Duration</th>
              </tr>
            </thead>
            <tbody>
              <!-- Keyed by index: entry.url is optional and untrusted, so
                   duplicate or absent URLs must not throw. -->
              {#each cooldownEntries as entry, i (i)}
                <tr>
                  <th scope="row" class="name-cell">{entry.url ?? "—"}</th>
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
        <EmptyState
          testid="statistics-unavailable"
          title="Statistics are not available: {daemonUnavailableReason(view.statsError)}."
          detail="The load and cache sections fill in once the optimizer answers this read."
        />
      {:else}
        <LoadError error={view.statsError} noun="the optimizer's statistics" onretry={() => poll.refresh()} />
      {/if}
    {/if}
  </section>

  <section class="daemon-section" id="optimizer-cache" aria-labelledby="optimizer-cache-heading">
    <h2 class="section-title" id="optimizer-cache-heading" data-page-focus={section === "cache" ? "" : undefined}>Cache</h2>
    {#if poll.loading}
      <p class="loading">Loading the optimizer's cache statistics...</p>
    {:else if view.stats}
      <div class="info-grid card">
        <div class="info-item">
          <span class="info-label metric-label">Cache entries</span>
          <span class="info-value metric-value num">{fieldValue(view.stats.cache?.entries)}</span>
        </div>
        <div class="info-item">
          <span class="info-label metric-label">Cache size</span>
          <span class="info-value metric-value num">{formatBytes(view.stats.cache?.size_bytes)}</span>
        </div>
      </div>

      <h3 class="card-title" id="serve-savings-heading">{TERMS.savedByOptimizerCache}</h3>
      <p class="note" data-testid="serve-savings-source">
        Recorded by this web server's module each time it serves an optimized
        response from the optimizer's cache. {SAVINGS_QUALIFIER}
      </p>
      {#if savings === null}
        <p class="muted" data-testid="serve-savings-absent">This optimizer version does not report what its cache saved.</p>
      {:else}
        {#if savings.served.length > 0}
          <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
          <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
          <div class="table-frame" role="region" aria-label={TERMS.savedByOptimizerCache} tabindex="0">
            <table class="data" data-testid="serve-savings-table" aria-labelledby="serve-savings-heading">
              <thead>
                <tr>
                  <th scope="col">Content</th>
                  <th scope="col" class="num">Responses</th>
                  <th scope="col" class="num">Original</th>
                  <th scope="col" class="num">Served</th>
                  <th scope="col" class="num">Saved</th>
                  {#if savings.total?.split?.available}
                    <th scope="col" class="num">{SPLIT_TEXT.encodingLabel}</th>
                    <th scope="col" class="num">{SPLIT_TEXT.rewriteLabel}</th>
                    {#if savings.total.split.coverage < 1}
                      <th scope="col" class="num">{SPLIT_TEXT.unsplitLabel}</th>
                    {/if}
                  {/if}
                </tr>
              </thead>
              <tbody>
                {#each savings.served as row (row.key)}
                  <tr>
                    <th scope="row" class="name-cell">{row.label}</th>
                    <td class="value-cell">{formatCount(row.hits)}</td>
                    <td class="value-cell">{formatBytes(row.original)}</td>
                    <td class="value-cell">{formatBytes(row.served)}</td>
                    <td class="value-cell">{formatSignedBytes(row.saved)}{row.percent !== null ? ` (${formatPercent(row.percent)})` : ""}</td>
                    {#if savings.total?.split?.available}
                      {@const cells = splitCells(row.split)}
                      <td class="value-cell">{cells.encoding}</td>
                      <td class="value-cell">{cells.rewrite}</td>
                      {#if savings.total.split.coverage < 1}
                        <td class="value-cell">{cells.unsplit}</td>
                      {/if}
                    {/if}
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
                    <td class="value-cell">{formatSignedBytes(savings.total.saved)} ({formatPercent(savings.total.percent)})</td>
                    {#if savings.total.split?.available}
                      {@const cells = splitCells(savings.total.split)}
                      <td class="value-cell">{cells.encoding}</td>
                      <td class="value-cell">{cells.rewrite}</td>
                      {#if savings.total.split.coverage < 1}
                        <td class="value-cell">{cells.unsplit}</td>
                      {/if}
                    {/if}
                  </tr>
                </tfoot>
              {/if}
            </table>
          </div>
          {#if savings.total}
            <SavingsSplitLine split={savings.total.split} />
          {/if}
        {:else}
          <p class="muted" data-testid="serve-savings-none">
            No optimized responses have been served from the optimizer's cache yet.
          </p>
        {/if}
        {#if savings.notServed.length > 0}
          <p class="note" data-testid="serve-savings-not-served">Not served from the optimizer's cache on this server so far: {savings.notServed.join(", ")}.</p>
        {/if}
        <details class="raw" data-testid="serve-savings-raw">
          <summary>Raw counters</summary>
          <table class="data-raw">
            <thead>
              <tr><th scope="col">Counter</th><th scope="col">Value</th></tr>
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
      <p class="muted" data-testid="cache-unavailable">The optimizer's statistics are not available; see Load above.</p>
    {/if}
  </section>
  <HtmlCachePanel />
  </div>
  {/if}
</div>

<style>
  .status-card {
    margin-bottom: var(--ps-space-lg);
  }

  .status-row {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-md);
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

  .raw {
    margin-top: var(--ps-space-sm);
  }
</style>
