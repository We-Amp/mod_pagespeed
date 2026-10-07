<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script module lang="ts">
  import { liveSeries, pushSample, SAVINGS_GROUP, SAVINGS_SERIES } from "$lib/utils/live-series";

  // Registered once per tab: the store survives navigating between console
  // pages; a reload starts afresh. Nothing is persisted.
  liveSeries.register(SAVINGS_GROUP, SAVINGS_SERIES);
</script>

<script lang="ts">
  // Savings: "how much am I saving?" -- the module's own rewrite savings
  // and the optimizer's cache-serve savings, labelled for what they are and
  // never silently added together, plus a live chart accumulated from the
  // poll this page already runs.
  import { usePolling } from "$lib/api/polling.svelte";
  import { isConnectionFailure } from "$lib/api/connection";
  import { ApiError } from "$lib/api/client";
  import type { StatsResponse } from "$lib/api/types";
  import { num, type AlertSnapshot } from "$lib/alerts";
  import PageHeader from "$lib/PageHeader.svelte";
  import ScopeChip from "$lib/ScopeChip.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import TimeSeriesChart, { SERIES_PALETTE } from "$lib/TimeSeriesChart.svelte";
  import { bytesRateTicks, chartWindowLabel } from "$lib/utils/chart-summary";
  import { formatBytes, formatBytesRate, formatCount, formatPercent } from "$lib/utils/format";
  import { useConsole } from "$lib/api/context";
  import {
    availabilityText,
    MIN_OPTIMIZER_VERSION,
    moduleSummary,
    overviewScopeLine,
    sampleOverview,
    toSnapshot,
    variablesOf,
  } from "$lib/utils/overview";
  import {
    daemonSavedRaw,
    moduleSavedRaw,
    moduleSavingsByType,
    moduleSavingsTotal,
    notYetOptimized,
    optimizedCopyHitRate,
    savingsSplit,
    splitSummaryText,
  } from "$lib/utils/savings";
  import { serveSavingsView } from "$lib/utils/serve-savings";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import { consoleHostName, hostRow, hostSavingsView, siteRow } from "$lib/utils/host-savings";
  import { sinceLabel } from "$lib/utils/since";
  import { globalConsoleHref } from "$lib/utils/urls-api";
  import { scopeChipShown } from "$lib/utils/scope-chip";
  import { toRatesPerSecond } from "$lib/utils/graph-series";

  const { api, scope, lens } = useConsole();

  // Latest good module statistics and latest settled optimizer view, so one
  // failed or busy refresh does not blank what the page already shows.
  let lastModule = $state<StatsResponse | null>(null);
  let daemonView = $state<AlertSnapshot | null>(null);
  let updatedAt = $state<number | null>(null);

  // Per-sample bookkeeping runs inside the fetcher, not in an $effect (same
  // contract as the Overview page): each sample appends to the live series
  // and throws exactly when the module read itself lost the connection.
  const poll = usePolling(
    () =>
      sampleOverview(api).then((sample) => {
        const snap = toSnapshot(sample, daemonView);
        updatedAt = sample.at;
        if (sample.module.ok) lastModule = sample.module.data;
        if (snap.daemon !== "transient" || daemonView === null) daemonView = snap;
        // The hosts the optimizer served feed the lens (the best-ranked source).
        lens.observe(hostSavingsView(snap.daemonStats?.serve_savings_by_host)?.hosts.map((r) => r.host) ?? [], 0);
        const moduleRaw =
          sample.module.ok && sample.module.data.variables
            ? moduleSavedRaw(variablesOf(sample.module.data))
            : null;
        const optimizerRaw = snap.daemonStats ? daemonSavedRaw(snap.daemonStats.serve_savings) : null;
        // Gap-aware: a paused tab or a long backoff breaks the line.
        pushSample(liveSeries, SAVINGS_GROUP, sample.at / 1000, [moduleRaw, optimizerRaw], 5);
        if (!sample.module.ok) {
          const status = sample.module.error instanceof ApiError ? sample.module.error.status : null;
          if (isConnectionFailure("/stats_json", status)) throw sample.module.error;
        }
        return sample;
      }),
    5000,
  );

  let moduleError = $derived.by(() => {
    if (poll.error) return poll.error;
    const m = poll.data?.module;
    return m && !m.ok ? m.error : null;
  });
  let vars = $derived(lastModule ? variablesOf(lastModule) : null);
  let summary = $derived(vars ? moduleSummary(vars) : null);
  let rows = $derived(vars ? moduleSavingsByType(vars) : null);
  let total = $derived(rows ? moduleSavingsTotal(rows) : null);
  let hitRate = $derived(vars ? optimizedCopyHitRate(vars) : null);
  let scopeLine = $derived(overviewScopeLine(scope.isGlobal, scope.config));
  let serveView = $derived(serveSavingsView(daemonView?.daemonStats?.serve_savings));
  // Serve savings per host: the lens host on the whole-server console, the
  // console's own host on a per-vhost one (never a lens there).
  let byHost = $derived(hostSavingsView(daemonView?.daemonStats?.serve_savings_by_host));
  let lensHost = $derived(activeLensHost(scope, lens));
  let ownHost = $derived(scope.isGlobal ? null : consoleHostName(scope.host));
  // The lens host's row on the whole-server console; on a per-vhost one the
  // row the module marks for it (or, from an older module, the row of the
  // console's own name) -- labelled with that row's own name.
  let site = $derived(scope.isGlobal ? null : siteRow(byHost, ownHost));
  let focusRow = $derived(lensHost !== null ? hostRow(byHost, lensHost) : (site?.row ?? null));
  let focusHost = $derived(lensHost ?? site?.host ?? null);
  let optimizerRaw = $derived(daemonSavedRaw(daemonView?.daemonStats?.serve_savings));
  let optimizerRegressed = $derived(optimizerRaw !== null && optimizerRaw < 0);
  // Each card states its own time base: module counters restart with the
  // web server, optimizer counters with the optimizer. A start of 0 or less
  // is "unknown", never the epoch.
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
  let moduleScopeLabel = $derived<"this host" | "whole server">(scope.isGlobal ? "whole server" : "this host");
  // The module's rewrites do not follow the host lens; the optimizer card's
  // totals cover the whole server (its host row says "this host" itself).
  let moduleChip = $derived(scopeChipShown(scope.isGlobal, scope.isGlobal, lensHost !== null));
  let optimizerChip = $derived(scopeChipShown(true, scope.isGlobal, lensHost !== null));
  let wholeServerHref = $derived(globalConsoleHref(api.basePath, "#/savings"));
  let split = $derived(
    savingsSplit(daemonView?.daemonStats?.serve_savings, daemonView?.daemonStats?.verdicts),
  );
  let pending = $derived(notYetOptimized(daemonView?.daemonStats ?? null));
  let encodingsNote = $derived.by(() => {
    if (!split.some((s) => s.totalServes > 0)) return null;
    if (!split.some((s) => s.encodingsKnown)) {
      return "Compressed copies: no data (this optimizer version does not report them).";
    }
    const encodedServes = split.reduce((acc, s) => acc + s.segments[2].count, 0);
    if (encodedServes === 0) {
      return "Compressed copies are stored but not served by this server; compression happens downstream.";
    }
    return null;
  });
  let availability = $derived(daemonView && daemonView.daemon !== "ok" ? availabilityText(daemonView.daemon) : null);

  const NO_TRAFFIC: Record<string, string> = {
    css: "No CSS seen yet.",
    js: "No JavaScript seen yet.",
    image: "No images seen yet.",
  };

  function toggleAutoRefresh() {
    if (poll.autoRefresh) poll.stop();
    else poll.start();
  }

  const CHART_LABELS: Record<string, string> = {
    "module.bytes_saved": "Module rewrites",
    "optimizer.bytes_saved": "Optimizer cache serves",
  };

  function latestSeriesValue(data: ReadonlyArray<number | null>): string {
    for (let i = data.length - 1; i >= 0; --i) {
      const v = data[i];
      if (v !== null && Number.isFinite(v)) return formatBytesRate(v);
    }
    return "—";
  }

  let chartView = $derived.by(() => {
    // Re-derive after each completed sample; the store itself is not reactive.
    const sampled = poll.data;
    const view = liveSeries.get(SAVINGS_GROUP);
    if (sampled === undefined && view === null) return null;
    if (view === null || view.timestamps.length < 2) return null;
    return {
      timestamps: view.timestamps,
      series: view.names.map((name, i) => ({
        label: CHART_LABELS[name] ?? name,
        // Bytes saved per second: the rate view divides by the seconds
        // between samples, so poll-spacing jitter is not a chart artefact.
        data: toRatesPerSecond(view.timestamps, view.series[i]),
      })),
    };
  });
</script>

<div class="page">
  <PageHeader
    title="Savings"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => poll.refresh(),
    }}
  />
  <p class="scope-line" data-testid="savings-scope">{scopeLine}</p>
  {#if poll.loading}
    <p class="loading">Loading savings...</p>
  {:else}
    <div class="cards">
      <section class="card reads-across-container" data-testid="savings-module-card" aria-labelledby="savings-module-heading">
        <h2 id="savings-module-heading">Module rewrites <ScopeChip label={moduleScopeLabel} shown={moduleChip} /></h2>
        {#if rows && total && summary}
          <RefreshNotice error={moduleError} />
          {#if summary.active || total.saved > 0}
            <dl class="figures">
              <div class="figure">
                <dt class="metric-label">Bytes saved by rewriting</dt>
                <dd class="metric-value num" data-testid="savings-module-total">
                  {formatBytes(total.saved)}
                  {#if total.percent !== null}
                    <span class="detail metric-context"
                      >{moduleSince?.warming ? "—" : formatPercent(total.percent, total.saved)} of {formatBytes(total.original)}</span
                    >
                  {/if}
                </dd>
              </div>
            </dl>
            <p class="since" data-testid="since-module" title={moduleSince?.iso ?? undefined}>
              {moduleSince?.text ?? "since restart"} (web server restart)
              {#if moduleSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
            </p>
            {#if rows.some((row) => row.regressed)}
              <p class="note" data-testid="savings-module-regression">
                At least one content type came out larger than the original; those bytes
                count as no net savings above.
              </p>
            {/if}
            <table class="breakdown reads-across" data-testid="savings-module-table">
              <thead>
                <tr><th>Type</th><th>Saved</th><th>Of original</th><th>Saved %</th></tr>
              </thead>
              <tbody>
                {#each rows as row (row.key)}
                  <tr>
                    <td>{row.label}</td>
                    <td class="num">{formatBytes(row.saved)}{#if row.regressed} <span class="note">(no net savings)</span>{/if}</td>
                    <td class="num">{formatBytes(row.original)}</td>
                    <td class="num">{formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          {:else}
            <div class="onboarding" data-testid="savings-empty">
              <p class="onboarding-title">No traffic yet.</p>
              <p>Once this server serves pages, this page shows what the module's
                rewriting and the optimizer's cache save.</p>
            </div>
          {/if}
        {:else if moduleError}
          <p class="error" role="alert" data-testid="savings-module-error">
            Could not read the module statistics: {moduleError.message}
          </p>
        {/if}
        <p class="more"><a href="#/statistics">All statistics</a></p>
      </section>

      <section
        class="card reads-across-container"
        class:card--muted={!scope.isGlobal && focusRow === null}
        data-testid="savings-optimizer-card"
        aria-labelledby="savings-optimizer-heading"
      >
        <h2 id="savings-optimizer-heading">Optimizer cache serves <ScopeChip label="whole server" shown={optimizerChip} /></h2>
        {#if !scope.isGlobal && focusRow === null}
          <p class="note" data-testid="optimizer-scope-note">
            Server-wide numbers — the per-host view is on the
            {#if wholeServerHref}<a href={wholeServerHref}>whole-server console</a>{:else}whole-server console{/if}.
          </p>
        {/if}
        {#if focusHost !== null && daemonView?.daemon === "ok" && !daemonView.daemonStatsUnsupported && (byHost !== null || lensHost !== null)}
          <div class="host-figures" data-testid={lensHost !== null ? "savings-lens-host" : "savings-this-host"}>
            {#if byHost === null}
              <p class="note" data-testid="savings-host-unsupported">This optimizer version does not report savings per host.</p>
            {:else if focusRow === null}
              <p class="note" data-testid="savings-host-none">No serves recorded for {focusHost} since the optimizer started.</p>
            {:else}
              <dl class="figures">
                <div class="figure">
                  <dt class="metric-label">Saved on responses served for {focusRow.host} <ScopeChip label="this host" /></dt>
                  <dd class="metric-value num">
                    {formatBytes(focusRow.saved)}
                    <span class="detail metric-context"
                      >{formatPercent(focusRow.percent, focusRow.saved)} of {formatBytes(focusRow.original)} · {formatCount(focusRow.hits)}
                      {focusRow.hits === 1 ? "response" : "responses"}</span
                    >
                  </dd>
                </div>
              </dl>
            {/if}
          </div>
        {/if}
        {#if daemonView?.daemon === "ok"}
          {#if daemonView.belowFloor}
            <p class="note" data-testid="savings-below-floor">
              This optimizer version is older than {MIN_OPTIMIZER_VERSION}, the oldest this
              console supports. Update the optimizer package.
            </p>
          {/if}
          {#if daemonView.daemonStatsUnsupported}
            <p class="note" data-testid="savings-optimizer-outdated">
              This optimizer version does not provide statistics. Update the optimizer package.
            </p>
          {:else if serveView && serveView.total}
            <!-- The headline figures: side by side once the card is wide. -->
            <div class="headlines">
            <div class="headline">
            <dl class="figures">
              <div class="figure">
                <dt class="metric-label">Saved on served responses</dt>
                <dd class="metric-value num" data-testid="savings-optimizer-total">
                  {formatBytes(serveView.total.saved)}
                  <span class="detail metric-context"
                    >{optimizerSince?.warming ? "—" : formatPercent(serveView.total.percent, serveView.total.saved)} of {formatBytes(
                      serveView.total.original,
                    )}</span
                  >
                </dd>
              </div>
            </dl>
            <p class="since" data-testid="since-optimizer" title={optimizerSince?.iso ?? undefined}>
              {optimizerSince?.text ?? "since restart"} (optimizer restart)
              {#if optimizerSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
            </p>
            </div>
            {#if hitRate && hitRate.percent !== null}
              <div class="headline">
              <!-- Computed from module counters, so it carries the module's time base. -->
              <dl class="figures">
                <div class="figure">
                  <dt
                    class="metric-label"
                    title={hitRate.mode === "optimizable"
                      ? "Share of cacheable resource requests (CSS, JavaScript, images) answered from the optimizer's cache; conditional and HEAD requests count as served"
                      : "Share of all in-place requests answered from the optimizer's cache"}
                  >
                    Optimized-copy hit rate{#if hitRate.mode === "all-in-place"}{" "}<span class="label-qualifier">(of all in-place requests)</span>{/if}
                  </dt>
                  <dd class="metric-value num" data-testid="savings-hit-rate">
                    {moduleSince?.warming ? "—" : formatPercent(hitRate.percent, hitRate.served)}
                    <span class="detail metric-context"
                      >{formatCount(hitRate.served)} of {formatCount(hitRate.total)}{hitRate.mode === "optimizable"
                        ? " cacheable resource requests; conditional and HEAD requests count as served"
                        : " in-place requests"}</span
                    >
                  </dd>
                </div>
              </dl>
              <p class="since" data-testid="since-hit-rate" title={moduleSince?.iso ?? undefined}>
                {moduleSince?.text ?? "since restart"} (web server restart)
                {#if moduleSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
              </p>
              </div>
            {/if}
            </div>
            {#if hitRate && hitRate.mode === "optimizable" && (hitRate.excluded ?? 0) > 0}
              <p
                class="note"
                data-testid="savings-not-optimizable"
                title="Fonts, icons and other types the optimizer does not rewrite, responses it does not record (compressed by the origin, not storable, oversized), and requests whose type was never learned"
              >
                +{formatCount(hitRate.excluded)} requests not optimizable or not recorded
              </p>
            {/if}
            {#if optimizerRegressed}
              <p class="note" data-testid="savings-optimizer-regression">
                The optimizer served more bytes than the originals on some responses;
                those bytes count as no net savings above.
              </p>
            {/if}
            <table class="breakdown reads-across" data-testid="savings-optimizer-table">
              <thead>
                <tr><th>Type</th><th>Saved</th><th>Of original</th><th>Saved %</th></tr>
              </thead>
              <tbody>
                {#each serveView.served as row (row.key)}
                  <tr>
                    <td>{row.label}</td>
                    <td class="num">{formatBytes(row.saved)}</td>
                    <td class="num">{formatBytes(row.original)}</td>
                    <td class="num">{formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
            {#if scope.isGlobal && lensHost === null && byHost !== null && byHost.hosts.length > 0}
              <h3 class="subhead">By host</h3>
              <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
              <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
              <div class="table-scroll" role="region" aria-label="Savings by host" tabindex="0">
                <table class="breakdown reads-across" data-testid="savings-by-host">
                  <thead>
                    <tr><th>Host</th><th>Responses</th><th>Saved</th><th>Of original</th><th>Saved %</th></tr>
                  </thead>
                  <tbody>
                    {#each byHost.hosts as row (row.host)}
                      <tr>
                        <td class="host-cell">{row.host}</td>
                        <td class="num">{formatCount(row.hits)}</td>
                        <td class="num">{formatBytes(row.saved)}</td>
                        <td class="num">{formatBytes(row.original)}</td>
                        <td class="num">{formatPercent(row.percent, row.saved)}</td>
                      </tr>
                    {/each}
                    {#if byHost.other}
                      <tr data-testid="savings-by-host-other">
                        <td class="host-cell">Other hosts and serves without a host</td>
                        <td class="num">{formatCount(byHost.other.hits)}</td>
                        <td class="num">{formatBytes(byHost.other.saved)}</td>
                        <td class="num">{formatBytes(byHost.other.original)}</td>
                        <td class="num">{formatPercent(byHost.other.percent, byHost.other.saved)}</td>
                      </tr>
                    {/if}
                  </tbody>
                </table>
              </div>
            {/if}
            <div class="split-types">
              {#each split as s (s.key)}
                <div class="split" data-testid="savings-split-{s.key}">
                  <h3>{s.label}</h3>
                  {#if s.hasData}
                    <div
                      class="split-bar"
                      role="img"
                      aria-label="{s.label}: {s.segments
                        .map((seg) => `${seg.label} ${formatCount(seg.count)}`)
                        .join(', ')}"
                    >
                      {#each s.segments as seg (seg.key)}
                        {#if seg.count > 0}
                          <span class="split-seg split-{seg.key}" style:flex-grow={seg.count}></span>
                        {/if}
                      {/each}
                    </div>
                    {#if s.summary}
                      <p class="split-summary num" data-testid="savings-split-summary">
                        {splitSummaryText(s.summary, s.bytesBasis)}
                      </p>
                    {/if}
                    <dl class="split-figures">
                      <div>
                        <dt><span class="split-swatch split-{s.segments[0].key}" aria-hidden="true"></span>{s.segments[0].label}</dt>
                        <dd class="num">
                          {formatCount(s.segments[0].count)} requests{#if s.verdict}{" · "}{formatCount(
                              s.verdict.count,
                            )}
                            {s.verdict.count === 1 ? "resource" : "resources"}, {formatBytes(s.verdict.bytes)} original{/if}
                        </dd>
                      </div>
                      <div>
                        <dt><span class="split-swatch split-{s.segments[1].key}" aria-hidden="true"></span>{s.segments[1].label}</dt>
                        <dd class="num">
                          {formatCount(s.segments[1].count)} requests
                        </dd>
                      </div>
                      <div>
                        <dt><span class="split-swatch split-{s.segments[2].key}" aria-hidden="true"></span>{s.segments[2].label}</dt>
                        <dd class="num">
                          {formatCount(s.segments[2].count)} requests{#if s.segments[2].bytes > 0}{" · "}{formatBytes(
                              s.segments[2].bytes,
                            )} sent{/if}
                        </dd>
                      </div>
                    </dl>
                  {:else}
                    <p class="note">{NO_TRAFFIC[s.key]}</p>
                  {/if}
                </div>
              {/each}
            </div>
            {#if encodingsNote}
              <p class="note" data-testid="savings-encodings-note">{encodingsNote}</p>
            {/if}
            {#if pending !== null && pending > 0}
              <p class="note" data-testid="savings-pending">
                Not yet optimized (pending or cold, across all types): {formatCount(pending)} requests
              </p>
            {/if}
            {#if serveView.notServed.length > 0}
              <p class="note" data-testid="savings-optimizer-not-served">
                Nothing served yet for: {serveView.notServed.join(", ")}.
              </p>
            {/if}
          {:else}
            <p class="note" data-testid="savings-optimizer-none">
              The optimizer has not served anything yet.
            </p>
          {/if}
        {:else if availability}
          <p class="note" data-testid="savings-optimizer-unavailable">
            {availability.word}. {availability.detail}
          </p>
        {/if}
        <p class="more"><a href="#/optimizer?section=cache">Optimizer cache</a></p>
      </section>
    </div>

    <section class="card chart-card" data-testid="savings-chart-card" aria-labelledby="savings-chart-heading">
      <h2 id="savings-chart-heading" data-testid="savings-chart-window">
        Savings — {chartView ? chartWindowLabel(chartView.timestamps) : "collecting samples"}
      </h2>
      {#if chartView}
        <div class="chart-legend" data-testid="savings-chart-legend">
          {#each chartView.series as s, i (s.label)}
            <span class="chart-legend-item">
              <span class="dot" style:background="var({SERIES_PALETTE[i % SERIES_PALETTE.length]})"></span>
              {s.label}
              <span class="legend-value num">{latestSeriesValue(s.data)}</span>
            </span>
          {/each}
        </div>
        <TimeSeriesChart
          title="Savings rate (bytes per second)"
          timestamps={chartView.timestamps}
          series={chartView.series}
          height={220}
          showLegend={false}
          tickValues={bytesRateTicks}
          formatValue={formatBytesRate}
        />
      {:else}
        <p class="note" data-testid="savings-chart-warming">
          The chart fills in as samples arrive, one per refresh.
        </p>
      {/if}
    </section>
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
    /* A card keeps its own height, not its tallest neighbour's. */
    align-items: start;
  }
  .card {
    min-width: 0;
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-surface);
  }
  .card h2 {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-lg);
  }
  .chart-card {
    margin-top: var(--ps-space-md);
    overflow: hidden;
  }
  .chart-legend {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-sm) var(--ps-space-lg);
    margin-bottom: var(--ps-space-xs);
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }
  .chart-legend-item {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex: none;
  }
  .legend-value {
    color: var(--ps-text);
  }
  /* Headline figures sit side by side once their card is wide enough. */
  .headlines {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-figure-min)), 1fr));
    column-gap: var(--ps-space-lg);
  }
  .figures {
    display: grid;
    gap: var(--ps-space-sm);
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
  .label-qualifier {
    font-weight: 400;
  }
  .breakdown {
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }
  .breakdown th, .breakdown td {
    /* Columns sit at their content's width, a step apart. */
    padding: var(--ps-space-xs) var(--ps-space-md) var(--ps-space-xs) 0;
    text-align: left;
    border-bottom: 1px solid var(--ps-border-light);
  }
  .breakdown th {
    color: var(--ps-text-secondary);
    font-weight: 600;
  }
  .note {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
  .onboarding-title { font-weight: 600; }
  .more {
    margin: var(--ps-space-sm) 0 0;
    font-size: var(--ps-font-size-sm);
  }
  .more a { color: var(--ps-primary); }
  .loading { color: var(--ps-text-secondary); }
  .error { color: var(--ps-error); }
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
  .split-types {
    display: grid;
    gap: var(--ps-space-md);
    margin-top: var(--ps-space-md);
  }
  .split h3 {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    margin: 0 0 var(--ps-space-xs);
  }
  /* The track shows only in the gaps, so neighbouring segments separate
     even when their colours are close. */
  .split-bar {
    display: flex;
    gap: 2px;
    height: 12px;
    border-radius: var(--ps-border-radius);
    overflow: hidden;
    background: var(--ps-surface);
    margin: 0 0 var(--ps-space-xs);
  }
  .split-seg {
    min-width: 2px;
  }
  .split-already-optimal {
    background: var(--ps-split-optimal);
  }
  .split-optimized-served {
    background: var(--ps-text);
  }
  .split-served-encoded {
    background: var(--ps-split-encoded);
  }
  .split-swatch {
    display: inline-block;
    width: 0.75em;
    height: 0.75em;
    margin-right: var(--ps-space-xs);
    border-radius: 2px;
    vertical-align: baseline;
  }
  .split-figures {
    margin: 0;
    display: grid;
    gap: var(--ps-space-xs);
  }
  .split-figures dt {
    font-size: var(--ps-font-size-xs);
    font-weight: 500;
    color: var(--ps-text-secondary);
  }
  .split-figures dd {
    margin: 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text);
  }
  .split-summary {
    margin: 0 0 var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text);
  }

  .host-figures {
    margin-bottom: var(--ps-space-md);
    padding-bottom: var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border-light);
  }

  .subhead {
    margin: var(--ps-space-md) 0 var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
  }

  .table-scroll {
    overflow-x: auto;
  }

  .host-cell {
    overflow-wrap: anywhere;
  }
</style>
