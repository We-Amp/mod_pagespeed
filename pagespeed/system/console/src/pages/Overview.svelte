<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script module lang="ts">
  import { ALERT_RULES, AlertTracker, RATE_ALERT_HOLD_MS } from "$lib/alerts";
  import { createAckStore } from "$lib/utils/finding-acks";

  // One tracker per browser tab. Acknowledgements are remembered across page
  // loads (finding-acks.ts) until the finding's condition clears.
  const tracker = new AlertTracker(ALERT_RULES, RATE_ALERT_HOLD_MS, createAckStore());
</script>

<script lang="ts">
  // Overview: the console's landing page -- is the module working, is the
  // optimizer daemon healthy, and what have they saved. One poll samples the
  // module statistics and the daemon's health and statistics together.
  import { usePolling } from "$lib/api/polling.svelte";
  import { isConnectionFailure } from "$lib/api/connection";
  import { ApiError } from "$lib/api/client";
  import type { StatsResponse } from "$lib/api/types";
  import { healthSummary, num, type ActiveAlert, type AlertSnapshot } from "$lib/alerts";
  import EmptyState from "$lib/EmptyState.svelte";
  import FindingsPanel from "$lib/FindingsPanel.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import ScopeChip from "$lib/ScopeChip.svelte";
  import { formatBytes, formatCount, formatDuration, formatPercent } from "$lib/utils/format";
  import { optimizedCopyHitRate } from "$lib/utils/savings";
  import { hostSavingsView } from "$lib/utils/host-savings";
  import { sinceLabel } from "$lib/utils/since";
  import { globalConsoleHref } from "$lib/utils/urls-api";
  import { scopeChipShown } from "$lib/utils/scope-chip";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import { useConsole } from "$lib/api/context";
  import {
    availabilityText,
    countText,
    daemonServeSavings,
    findingsStampText,
    MIN_OPTIMIZER_VERSION,
    moduleSummary,
    optimizerStateText,
    overviewScopeLine,
    sampleOverview,
    toSnapshot,
    variablesOf,
  } from "$lib/utils/overview";

  const { api, scope, lens } = useConsole();
  const moduleVersion = import.meta.env.VITE_APP_VERSION ?? "dev";

  // Latest good module statistics and latest settled optimizer view, so one
  // failed or busy refresh does not blank what the page already shows.
  let lastModule = $state<StatsResponse | null>(null);
  let daemonView = $state<AlertSnapshot | null>(null);
  let updatedAt = $state<number | null>(null);

  // This mount's first sample is a baseline for rate rules: nothing was
  // sampled while the page was not shown.
  tracker.resetBaseline();
  let alerts = $state<ActiveAlert[]>(tracker.visible());
  let acknowledged = $state<ActiveAlert[]>(tracker.acknowledged());
  // The last sample's message-log read failed (404, 5xx, junk): the summary
  // must then say the findings are unavailable, never "No findings".
  let logUnavailable = $state(false);

  // Per-sample bookkeeping runs inside the fetcher, not in an $effect: the
  // fetcher is called from the poll timer (or the Refresh button), outside any
  // reactive context, so these writes cannot re-arm the poll. A transient
  // optimizer read re-uses the previous view's block (toSnapshot's
  // carry-forward), so a 429 from another tab neither blanks the figures nor
  // clears a firing alert. The sample is always booked first (the last good
  // view stays, the alert rules see a transient sample); the fetcher then
  // throws exactly when the module read itself lost the connection --
  // isConnectionFailure covers no answer at all and a gateway's 502/503/504
  // in front of the module -- so the poller counts a failure and backs off
  // the same way every single-leaf page does.
  const poll = usePolling(
    () =>
      sampleOverview(api).then((sample) => {
        const snap = toSnapshot(sample, daemonView, moduleVersion);
        updatedAt = sample.at;
        if (sample.module.ok) lastModule = sample.module.data;
        if (snap.daemon !== "transient" || daemonView === null) daemonView = snap;
        // The hosts the optimizer served feed the lens (the best-ranked source).
        lens.observe(hostSavingsView(snap.daemonStats?.serve_savings_by_host)?.hosts.map((r) => r.host) ?? [], 0);
        alerts = tracker.evaluate(snap, sample.at);
        acknowledged = tracker.acknowledged();
        logUnavailable = snap.messages === null;
        if (!sample.module.ok) {
          const status = sample.module.error instanceof ApiError ? sample.module.error.status : null;
          if (isConnectionFailure("/stats_json", status)) throw sample.module.error;
        }
        return sample;
      }),
    5000,
  );

  function toggleAutoRefresh() {
    if (poll.autoRefresh) poll.stop();
    else poll.start();
  }

  function acknowledge(id: string): void {
    tracker.acknowledge(id);
    alerts = tracker.visible();
    acknowledged = tracker.acknowledged();
  }

  function unacknowledge(id: string): void {
    tracker.unacknowledge(id);
    alerts = tracker.visible();
    acknowledged = tracker.acknowledged();
  }

  let moduleError = $derived.by(() => {
    if (poll.error) return poll.error;
    const m = poll.data?.module;
    return m && !m.ok ? m.error : null;
  });
  let vars = $derived(lastModule ? variablesOf(lastModule) : null);
  let summary = $derived(vars ? moduleSummary(vars) : null);
  let cacheRate = $derived(vars ? optimizedCopyHitRate(vars) : null);
  // Each card states its own time base (a start of 0 or less is unknown).
  let sampleAt = $derived(poll.data?.at ?? null);
  let moduleStart = $derived.by(() => {
    const v = vars === null ? undefined : num(vars, "process_start_ms");
    return v !== undefined && v > 0 ? v : null;
  });
  let optimizerStart = $derived.by(() => {
    const v = num(daemonView?.daemonStats ?? null, "started_at_ms");
    return v !== undefined && v > 0 ? v : null;
  });
  let moduleSince = $derived(sampleAt !== null ? sinceLabel(moduleStart, sampleAt) : null);
  let optimizerSince = $derived(sampleAt !== null ? sinceLabel(optimizerStart, sampleAt) : null);
  let findingsStamp = $derived(
    findingsStampText(moduleSince, daemonView?.daemon === "ok" ? optimizerSince : null),
  );
  let moduleScopeLabel = $derived<"this host" | "whole server">(scope.isGlobal ? "whole server" : "this host");
  // Neither card follows the host lens: the module's figures cover the
  // console's scope, the optimizer's the whole server.
  let lensActive = $derived(activeLensHost(scope, lens) !== null);
  let moduleChip = $derived(scopeChipShown(scope.isGlobal, scope.isGlobal, lensActive));
  let optimizerChip = $derived(scopeChipShown(true, scope.isGlobal, lensActive));
  let wholeServerHref = $derived(globalConsoleHref(api.basePath, "#/savings"));
  let scopeLine = $derived(overviewScopeLine(scope.isGlobal, scope.config));
  let optimizer = $derived(daemonView?.daemon === "ok" ? optimizerStateText(daemonView.health) : null);
  let availability = $derived(daemonView && daemonView.daemon !== "ok" ? availabilityText(daemonView.daemon) : null);
  let serveSavings = $derived(daemonServeSavings(daemonView?.daemonStats ?? null));
  let health = $derived(healthSummary(alerts, lastModule !== null, acknowledged, logUnavailable));
</script>

<div class="page">
  <PageHeader
    title="Overview"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => poll.refresh(),
    }}
  />
  <p class="scope-line" data-testid="overview-scope">{scopeLine}</p>
  {#if poll.loading}
    <p class="loading">Loading overview...</p>
  {:else}
    <p class="health health-{health.level}" data-testid="health-summary" role="status">{health.text}</p>
    {#if alerts.length === 0 && findingsStamp}
      <p class="findings-since" data-testid="findings-since">{findingsStamp}</p>
    {/if}
    <FindingsPanel
      findings={alerts}
      {acknowledged}
      onAcknowledge={acknowledge}
      onUnacknowledge={unacknowledge}
    />
    <div class="cards">
      <section class="card" data-testid="module-card" aria-labelledby="module-heading">
        <h2 id="module-heading">Module <span class="version">{moduleVersion}</span> <ScopeChip label={moduleScopeLabel} shown={moduleChip} /></h2>
        {#if summary}
          <RefreshNotice error={moduleError} />
          {#if summary.active}
            <dl class="figures">
              <div class="figure">
                <dt class="metric-label">Bytes saved by optimization</dt>
                <dd class="metric-value num" data-testid="module-bytes-saved">
                  {formatBytes(summary.bytesSaved)}
                  {#if summary.savedPercent !== null && summary.originalBytes !== null}
                    <span class="detail metric-context"
                      >{moduleSince?.warming ? "—" : formatPercent(summary.savedPercent, summary.bytesSaved ?? 0)} of {formatBytes(
                        summary.originalBytes,
                      )}</span
                    >
                  {/if}
                </dd>
              </div>
              {#if !(daemonView?.daemon === "not-configured" || summary.inPlaceRequests === 0)}
                <div class="figure">
                  <dt class="metric-label">Served from the optimizer's cache</dt>
                  <dd class="metric-value num" data-testid="module-served">
                    <a
                      href="#/savings"
                      data-testid="module-served-link"
                      title={cacheRate?.mode === "optimizable"
                        ? "Share of cacheable resource requests (CSS, JavaScript, images) answered from the optimizer's cache; conditional and HEAD requests count as served"
                        : "Share of all in-place requests answered from the optimizer's cache"}
                    >
                      {countText(summary.servedByOptimizer)}
                      {#if cacheRate && cacheRate.percent !== null}
                        <span class="detail metric-context"
                          >{moduleSince?.warming ? "—" : formatPercent(cacheRate.percent, cacheRate.served)} of {formatCount(
                            cacheRate.total,
                          )}
                          {cacheRate.mode === "optimizable"
                            ? "cacheable resource requests; conditional and HEAD requests count as served"
                            : "in-place requests"}</span
                        >
                      {/if}
                    </a>
                  </dd>
                </div>
              {/if}
              <div class="figure">
                <dt class="metric-label">Resource fetch failures</dt>
                <dd class="metric-value num" data-testid="module-fetch-failures"><a href="#/logs?source=module&level=warning">{countText(summary.fetchFailures)}</a></dd>
              </div>
            </dl>
            <p class="since" data-testid="since-module" title={moduleSince?.iso ?? undefined}>
              {moduleSince?.text ?? "since restart"} (web server restart)
              {#if moduleSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
            </p>
          {:else}
            <EmptyState
              testid="overview-empty"
              title="The module is running and ready."
              detail="Nothing has been optimized yet. Once this web server serves pages, this overview shows what the module and the optimizer are doing."
            />
          {/if}
        {:else if moduleError}
          <p class="error" role="alert" data-testid="module-error">
            Could not read the module statistics: {moduleError.message}
          </p>
        {/if}
        <p class="more"><a href="#/statistics">All statistics</a></p>
      </section>

      <section
        class="card"
        class:card--muted={!scope.isGlobal}
        data-testid="optimizer-card"
        aria-labelledby="optimizer-heading"
      >
        <h2 id="optimizer-heading">
          Optimizer daemon
          {#if typeof daemonView?.health?.version === "string" && daemonView.health.version}<span class="version">{daemonView.health.version}</span>{/if}
          <ScopeChip label="whole server" shown={optimizerChip} />
        </h2>
        {#if !scope.isGlobal}
          <p class="note" data-testid="optimizer-scope-note">
            Server-wide numbers — the per-host view is on the
            {#if wholeServerHref}<a href={wholeServerHref}>whole-server console</a>{:else}whole-server console{/if}.
          </p>
        {/if}
        {#if optimizer && daemonView}
          <p class="state" class:state-ok={optimizer.ok} class:state-warn={!optimizer.ok} data-testid="optimizer-state">{optimizer.word}</p>
          {#if optimizer.busy}
            <p class="note" data-testid="optimizer-busy">All worker threads busy.</p>
          {/if}
          <dl class="figures">
            <div class="figure">
              <dt class="metric-label">Uptime</dt>
              <dd class="metric-value num">{formatDuration(daemonView.health?.uptime_seconds)}</dd>
            </div>
            {#if serveSavings}
              <div class="figure">
                <dt class="metric-label">Saved on served responses</dt>
                <dd class="metric-value num" data-testid="optimizer-savings">
                  {formatBytes(serveSavings.saved)}
                  <span class="detail metric-context"
                    >{optimizerSince?.warming ? "—" : formatPercent(serveSavings.percent, serveSavings.saved)} of {formatBytes(
                      serveSavings.original,
                    )}</span
                  >
                </dd>
              </div>
            {/if}
          </dl>
          <p class="since" data-testid="since-optimizer" title={optimizerSince?.iso ?? undefined}>
            {optimizerSince?.text ?? "since restart"} (optimizer restart)
            {#if optimizerSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
          </p>
          {#if daemonView.belowFloor}
            <p class="note" data-testid="optimizer-below-floor">
              This optimizer version is older than {MIN_OPTIMIZER_VERSION}, the oldest this console
              supports. Update the optimizer package.
            </p>
          {/if}
          {#if daemonView.daemonStatsUnsupported}
            <p class="note" data-testid="optimizer-outdated">
              This optimizer version does not provide statistics, so its savings and the alerts
              that need them are unavailable. Update the optimizer package.
            </p>
          {/if}
        {:else if availability}
          <p
            class="state"
            class:state-warn={daemonView?.daemon === "unreachable" || daemonView?.daemon === "unsupported"}
            data-testid="optimizer-state"
          >{availability.word}</p>
          {#if daemonView?.daemon === "unreachable"}
            <EmptyState
              testid="optimizer-empty"
              title={availability.detail}
              detail="This page asks again on every refresh."
            />
          {:else}
            <p class="note">{availability.detail}</p>
          {/if}
        {/if}
        <p class="more"><a href="#/optimizer">Optimizer status</a></p>
      </section>
    </div>
  {/if}
</div>

<style>
  .scope-line {
    margin: 0 0 var(--ps-space-md);
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .cards {
    display: grid;
    /* The cards share the full width in equal columns; more cards, more
       columns. */
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-card-min)), 1fr));
    gap: var(--ps-space-md);
    /* The two cards are of a size: they share one height. */
  }

  .card {
    min-width: 0;
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-surface);
  }

  .card h2 {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--ps-space-sm);
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-lg);
  }

  .version {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    font-weight: 400;
    color: var(--ps-text-secondary);
    overflow-wrap: anywhere;
  }

  /* The headline figures sit side by side once their card is wide enough. */
  .figures {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-figure-min)), 1fr));
    gap: var(--ps-space-sm) var(--ps-space-lg);
    margin: var(--ps-space-sm) 0;
  }

  .figure dd {
    margin: 0;
    overflow-wrap: anywhere;
  }

  .detail {
    display: block;
    font-weight: 400;
  }

  .since {
    margin: var(--ps-space-xs) 0 0;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }

  .warming {
    margin-left: var(--ps-space-xs);
    font-weight: 600;
    color: var(--ps-warning-text);
    border: 1px solid var(--ps-warning);
    border-radius: 999px;
    padding: 0 var(--ps-space-sm);
  }

  .card--muted {
    background: var(--ps-bg-secondary);
    border: 1px dashed var(--ps-secondary);
    opacity: var(--ps-muted-opacity);
  }

  /* Status text stays in --ps-text (AA contrast); the colour is on the dot. */
  .state {
    margin: 0 0 var(--ps-space-sm);
    font-weight: 600;
    color: var(--ps-text);
    overflow-wrap: anywhere;
  }

  .state::before {
    content: "";
    display: inline-block;
    width: 0.6em;
    height: 0.6em;
    margin-right: 0.4em;
    border-radius: 50%;
    background: var(--ps-text-secondary);
  }

  .state-ok::before {
    background: var(--ps-success);
  }

  .state-warn::before {
    background: var(--ps-warning);
  }

  .note {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .more {
    margin: var(--ps-space-sm) 0 0;
    font-size: var(--ps-font-size-sm);
  }

  .more a {
    color: var(--ps-primary);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
  }

  /* Text stays in --ps-text (AA contrast); the colour is on the left border. */
  .health {
    margin: 0 0 var(--ps-space-md);
    padding-left: var(--ps-space-sm);
    border-left: 4px solid var(--ps-text-secondary);
    font-weight: 600;
    color: var(--ps-text);
  }

  .health-healthy {
    border-left-color: var(--ps-success);
  }

  .health-warning {
    border-left-color: var(--ps-warning);
  }

  .health-error {
    border-left-color: var(--ps-error);
  }

  .health-info {
    border-left-color: var(--ps-primary);
  }

  .findings-since {
    margin: calc(-1 * var(--ps-space-sm)) 0 var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
</style>
