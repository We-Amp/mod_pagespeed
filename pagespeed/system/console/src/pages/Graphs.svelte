<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script module lang="ts">
  /** The statistics log is reread at most this often: it gains one sample
      per logging interval, so faster rereads buy nothing. */
  export const LOG_REFETCH_MS = 60_000;
  /** Matching counters are revealed in pages of this many cards. */
  export const PAGE_SIZE = 48;
</script>

<script lang="ts">
  // Graphs: every numeric counter as a time series. Every range first
  // draws the module's statistics log for that range (reread at most once
  // a minute) and merges the live ring buffer this page's statistics poll
  // feeds into the same series: short ranges gain a dense live tail, wide
  // ranges get their last hour densified. Counters are drawn as rates per
  // second once the module names its gauges (or the viewer asks); gauges
  // are always raw, and a counter reset (a restart) is a gap, not a spike.
  import { usePolling } from "$lib/api/polling.svelte";
  import type { TimeRangeParams } from "$lib/api/types";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import TimeSeriesChart from "$lib/TimeSeriesChart.svelte";
  import { bytesRateTicks } from "$lib/utils/chart-summary";
  import ViewSwitch from "$lib/ViewSwitch.svelte";
  import { formatBytesRate, formatCount, formatPercent } from "$lib/utils/format";
  import {
    formatRate,
    graphTitle,
    latestForDisplay,
    loadDeltaView,
    saveDeltaView,
    seriesForDisplay,
    type Sample,
  } from "$lib/utils/graph-series";
  import { mergeSeries, sliceSince } from "$lib/utils/merge-series";
  import {
    graphsView,
    isGraphsAbsent,
    perVhostConsoleHref,
    type GraphsView,
  } from "$lib/utils/graphs-view";
  import { liveSeries, pushSample, SAVINGS_GROUP } from "$lib/utils/live-series";
  import type { DaemonStatsResponse } from "$lib/api/types";
  import EmptyState from "$lib/EmptyState.svelte";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import { router } from "$lib/router.svelte";
  import {
    CURATED_GROUP,
    CURATED_SERIES,
    countersHref,
    curatedCharts,
    curatedFocus,
    curatedFromLog,
    curatedSample,
    keepFocus,
    mergeCurated,
    parseCountersParam,
    type CuratedChart,
    type CuratedUnit,
  } from "$lib/utils/graphs-curated";
  import { consoleHostName, hostSavingsView, siteRow } from "$lib/utils/host-savings";
  import { variablesOf } from "$lib/utils/overview";
  import { useConsole } from "$lib/api/context";

  const { api, scope, lens } = useConsole();
  // A chart's plot keeps sensible proportions on a wide window: it grows
  // with its width (width over height) from 160 px up to this height.
  const CHART_ASPECT = 3.5;
  const CHART_MAX_HEIGHT = 320;

  // The default charts' live group (idempotent across mounts).
  liveSeries.register(CURATED_GROUP, CURATED_SERIES);

  // counters=all: every counter, as before; counters=a,b: the default
  // charts plus those counters; no parameter: the default charts.
  const counters = parseCountersParam(router.params.get("counters"));
  const showAll = counters.all;
  let pinned = $state<string[]>(counters.pinned);
  let lensHost = $derived(activeLensHost(scope, lens));
  /** A per-vhost console's site once the optimizer reports savings per host (set by the poll). */
  let siteHost = $state<string | null>(null);
  // Whose optimizer savings: the lens host, or the site a per-vhost console
  // is given (the module's marker, or its own name from an older module);
  // otherwise the whole server.
  let focusHost = $derived(lensHost ?? siteHost);

  let timeRange = $state("15");
  /** null: the viewer never chose — the default follows the reply's gauges field. */
  let deltaChoice = $state<boolean | null>(loadDeltaView());
  let search = $state("");
  let shownCount = $state(PAGE_SIZE);
  /** The module's gauge names; null while (and when) the reply omits the list. */
  let gaugeNames = $state<string[] | null>(null);
  let logView = $state<GraphsView | null>(null);

  // Fetch bookkeeping, deliberately not reactive: the read budget must not
  // depend on renders.
  let logFetchedAt = 0;
  let logRequested = true;

  let updatedAt = $state<number | null>(null);

  const timeRangeOptions = [
    { label: "Last 5 minutes", value: "5" },
    { label: "Last 15 minutes", value: "15" },
    { label: "Last 30 minutes", value: "30" },
    { label: "Last 1 hour", value: "60" },
    { label: "Last 6 hours", value: "360" },
    { label: "Last 24 hours", value: "1440" },
  ];

  function buildParams(): TimeRangeParams {
    const minutes = parseInt(timeRange, 10);
    const now = Date.now();
    return { start: now - minutes * 60 * 1000, end: now };
  }

  /** The delta view: the viewer's choice, or on exactly when the module names its gauges. */
  let deltaView = $derived(deltaChoice ?? (gaugeNames !== null));
  let gauges = $derived(new Set(gaugeNames ?? []));

  // One poll, two sources: every sample feeds the live ring buffer from
  // stats_json; the statistics log is (re)read when the range changes, on
  // an explicit Refresh, and otherwise at most once a minute. A range
  // change invalidates the poller: an answer for the old range still in
  // flight is dropped, and the new range is read as soon as that pending
  // read settles.
  const graphs = usePolling<number>(async () => {
    // The optimizer's figures ride along for the default charts; an
    // optimizer that does not answer leaves its two charts without data.
    const daemonRead: Promise<DaemonStatsResponse | null> = api.daemonStats().catch(() => null);
    const stats = await api.getStats();
    const daemon = await daemonRead;
    const at = Date.now() / 1000;
    const byHost = hostSavingsView(daemon?.serve_savings_by_host);
    if (byHost !== null) lens.observe(byHost.hosts.map((r) => r.host), 0);
    // A per-vhost console's site: the module's marker, or its own name.
    siteHost = !scope.isGlobal && byHost !== null ? siteRow(byHost, consoleHostName(scope.host)).host : null;
    // The sample is recorded with the host it was read for, so a later
    // change of host never draws it under the new one.
    const sampleHost = activeLensHost(scope, lens) ?? siteHost;
    pushSample(liveSeries, CURATED_GROUP, at, curatedSample(variablesOf(stats), daemon, sampleHost), 5);
    curatedFocus.record(at, sampleHost);
    for (const [name, value] of Object.entries(variablesOf(stats))) {
      liveSeries.register(name, ["value"]);
      liveSeries.push(name, at, [value]);
    }
    gaugeNames = Array.isArray(stats.gauges)
      ? stats.gauges.filter((x): x is string => typeof x === "string")
      : null;
    if (logRequested || Date.now() - logFetchedAt >= LOG_REFETCH_MS) {
      // Record the attempt before the fetch: a missing endpoint must not
      // be retried on every statistics poll.
      logRequested = false;
      logFetchedAt = Date.now();
      try {
        logView = graphsView(await api.getGraphs(buildParams()));
      } catch (e) {
        if (!(e instanceof Error) || !isGraphsAbsent(e)) throw e;
        logView = { kind: "absent", detail: "" };
      }
    }
    updatedAt = Date.now();
    return at;
  }, 5000);

  const vhostHref = perVhostConsoleHref(api.basePath);

  function changeRange(value: string) {
    timeRange = value;
    logRequested = true;
    graphs.invalidate();
  }

  function toggleDeltaView() {
    deltaChoice = !deltaView;
    saveDeltaView(deltaChoice);
  }

  function toggleAutoRefresh() {
    if (graphs.autoRefresh) {
      graphs.stop();
    } else {
      graphs.start();
    }
  }

  interface ChartCard {
    name: string;
    title: string;
    /** Epoch seconds: log history and live tail merged and sorted. */
    timestamps: number[];
    series: Sample[];
    liveSince: number | null;
    liveOnly: boolean;
    /** True when the card draws rates per second (formats values with the unit). */
    isRate: boolean;
  }

  interface GraphsPage {
    /** Sorted, filtered names with at least one point in the selected range. */
    names: string[];
    /** The merged cards for the rendered page of `names`. */
    charts: ChartCard[];
  }

  // The charts to draw. Re-derives after every completed sample by
  // referencing graphs.data (the live store itself is not reactive), and
  // whenever the log view, the range, the filter, the toggle or the gauges
  // list changes. Only samples inside the selected range count: the window
  // moves with each completed sample, and a log reply fetched up to a minute
  // ago can carry points that have since left it. The names are listed
  // cheaply first; only the rendered page of cards is merged and shaped.
  let view = $derived.by<GraphsPage | null>(() => {
    const sampled = graphs.data;
    if (sampled === undefined || sampled === null) return null;
    const sinceSec = sampled - parseInt(timeRange, 10) * 60;
    const q = search.toLowerCase();
    const lv: GraphsView | null = logView;
    // The log's values keyed by name, timestamps converted to seconds. Every
    // logged series shares the one timestamp array, so the window check
    // looks at its newest entry only.
    const logged = new Map<string, Sample[]>();
    let logTimestampsSec: number[] = [];
    if (lv !== null && lv.kind === "data") {
      logTimestampsSec = lv.timestamps.map((t) => t / 1000);
      for (const g of lv.graphs) logged.set(g.name, g.data);
    }
    /** True when the named logged series has a sample inside the window. */
    function logInWindow(name: string): boolean {
      const data = logged.get(name);
      if (data === undefined) return false;
      const n = Math.min(logTimestampsSec.length, data.length);
      return n > 0 && logTimestampsSec[n - 1] >= sinceSec;
    }
    const names = new Set<string>([...liveSeries.names(), ...logged.keys()]);
    names.delete(SAVINGS_GROUP);
    names.delete(CURATED_GROUP);
    const wanted = showAll ? null : new Set(pinned);
    const listed: string[] = [];
    for (const name of [...names].sort()) {
      if (wanted !== null && !wanted.has(name)) continue;
      if (showAll && q && !name.toLowerCase().includes(q)) continue;
      const liveNewest = liveSeries.newestTimestamp(name);
      if ((liveNewest !== null && liveNewest >= sinceSec) || logInWindow(name)) listed.push(name);
    }
    const charts: ChartCard[] = [];
    for (const name of listed.slice(0, shownCount)) {
      const log = sliceSince(logTimestampsSec, logged.get(name) ?? [], sinceSec);
      const liveView = liveSeries.get(name);
      const live =
        liveView === null
          ? { timestamps: [], values: [] }
          : sliceSince(liveView.timestamps, liveView.series[0], sinceSec);
      const merged = mergeSeries(log.timestamps, log.values, live.timestamps, live.values);
      if (merged.timestamps.length === 0) continue;
      const gauge = gauges.has(name);
      charts.push({
        name,
        title: graphTitle(name, deltaView, gauge),
        timestamps: merged.timestamps,
        series: seriesForDisplay(merged.timestamps, merged.values, deltaView, gauge),
        liveSince: merged.liveSince,
        // Live-only whenever the log contributed no points to this series;
        // when the log is absent, unwritten or range-empty the page-level
        // notice above the grid says why.
        liveOnly: merged.liveOnly,
        isRate: deltaView && !gauge,
      });
    }
    return { names: listed, charts };
  });

  // The default charts over the selected range: the statistics log's module
  // history merged with the live tail. The optimizer savings chart plots
  // only the samples read for the current focus host: after a change of
  // host it shows a gap and starts again. Charts without a value are listed
  // in one empty state instead of drawn empty.
  let curated = $derived.by(() => {
    const sampled = graphs.data;
    if (showAll || sampled === undefined || sampled === null) return null;
    const sinceSec = sampled - parseInt(timeRange, 10) * 60;
    const merged = mergeCurated(curatedFromLog(logView), liveSeries.get(CURATED_GROUP), sinceSec);
    const charts = curatedCharts(merged.timestamps, keepFocus(merged.timestamps, merged.raw, curatedFocus, focusHost));
    return { shown: charts.filter((c) => c.hasData), hidden: charts.filter((c) => !c.hasData) };
  });

  // "Add a counter": up to twelve counters matching the field, not shown yet.
  let addMatches = $derived.by(() => {
    const sampled = graphs.data;
    const q = search.trim().toLowerCase();
    if (showAll || q === "" || sampled === undefined || sampled === null) return [];
    const all = new Set<string>([...liveSeries.names(), ...(logView?.kind === "data" ? logView.graphs.map((g) => g.name) : [])]);
    all.delete(SAVINGS_GROUP);
    all.delete(CURATED_GROUP);
    return [...all].filter((n) => n.toLowerCase().includes(q) && !pinned.includes(n)).sort().slice(0, 12);
  });

  function setPinned(next: string[]): void {
    pinned = next;
    history.replaceState(history.state, "", countersHref(next, window.location.hash));
  }

  function curatedTitle(chart: CuratedChart): string {
    // The host the optimizer savings follow: the lens, or a per-vhost console's own.
    return chart.id === "optimizer_saved" && focusHost !== null ? `${chart.title} — ${focusHost}` : chart.title;
  }

  function curatedFormat(unit: CuratedUnit): (v: number) => string {
    switch (unit) {
      case "bytes-per-second":
        return formatBytesRate;
      case "percent":
        return (v) => formatPercent(v);
      case "per-second":
        return formatRate;
      default:
        return (v) => formatCount(v);
    }
  }

  let rendered = $derived(view?.charts ?? []);
  let remaining = $derived(Math.max(0, (view?.names.length ?? 0) - shownCount));

  /** The plottable values of a series, with gaps dropped. */
  function plotted(series: Sample[]): number[] {
    return series.filter((v): v is number => v !== null && Number.isFinite(v));
  }

  function formatSample(v: number | null, isRate: boolean): string {
    if (v === null) return "—";
    return isRate ? formatRate(v) : formatCount(v);
  }
</script>
<div class="page">
  <PageHeader
    title="Graphs"
    updatedAt={updatedAt}
    refresh={{
      autoRefresh: graphs.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      onRefresh: () => {
        logRequested = true;
        void graphs.refresh();
      },
    }}
  >
    {#snippet toolbar()}
      <label class="control-label field-label">
        Time range
        <select class="control field" value={timeRange} onchange={(e) => changeRange(e.currentTarget.value)}>
          {#each timeRangeOptions as opt (opt.value)}
            <option value={opt.value}>{opt.label}</option>
          {/each}
        </select>
      </label>
      <label class="control-label field-check">
        <input type="checkbox" checked={deltaView} onchange={toggleDeltaView} />
        Rates per second
      </label>
      <input
        class="control field field-search"
        type="search"
        aria-label={showAll ? "Filter graphs" : "Add a counter"}
        placeholder={showAll ? "Filter graphs" : "Add a counter"}
        bind:value={search}
        oninput={() => (shownCount = PAGE_SIZE)}
      />
    {/snippet}
  </PageHeader>

  <ViewSwitch
    label="Statistics views"
    views={[
      { label: "Table", href: "#/statistics", current: false },
      { label: "Graphs", href: "#/graphs", current: true },
    ]}
  />
  <RefreshNotice error={graphs.error} />

  {#if logView !== null && logView.kind === "absent"}
    <p class="note" data-testid="graphs-absent">
      This build does not serve graphs data{logView.detail === "" ? "." : `: ${logView.detail}.`}
    </p>
  {:else if logView !== null && logView.kind === "no-log"}
    <p class="note" data-testid="graphs-no-log">
      Statistics logging is off or has not written yet, so there is no history to draw.
      The charts below fill in from the live poll.
    </p>
    {#if scope.isGlobal && vhostHref !== null}
      <p class="note" data-testid="graphs-global-idle">
        This is the aggregate console. A virtual host with traffic has its own view:
        <a href={vhostHref}>per-host graphs</a>.
      </p>
    {/if}
  {:else if logView !== null && logView.kind === "empty-range"}
    <div data-testid="graphs-empty-range">
      <p class="note">No logged samples in this time range; the charts below draw the live poll.</p>
      {#if scope.isGlobal && vhostHref !== null}
        <p class="note" data-testid="graphs-global-idle">
          This is the aggregate console. A virtual host with traffic has its own view:
          <a href={vhostHref}>per-host graphs</a>.
        </p>
      {/if}
    </div>
  {/if}

  {#if !showAll}
    {#if lensHost !== null}
      <p class="note" data-testid="curated-lens-note">
        The optimizer savings chart follows the host lens ({lensHost}); the module's charts cover the whole server.
      </p>
    {/if}
    {#if !scope.isGlobal && focusHost === null && curated !== null && curated.shown.some((c) => c.id === "optimizer_saved")}
      <p class="note" data-testid="curated-whole-server-note">The optimizer savings chart covers the whole server.</p>
    {/if}
    {#if addMatches.length > 0}
      <ul class="add-list control-group" data-testid="graphs-add" aria-label="Counters to add">
        {#each addMatches as name (name)}
          <li><button type="button" class="control btn btn-secondary btn-compact" aria-label="Add {name}" onclick={() => { setPinned([...pinned, name]); search = ""; }}>+ {name}</button></li>
        {/each}
      </ul>
    {/if}
    {#if curated !== null}
      <section class="curated" aria-labelledby="curated-heading">
        <h2 id="curated-heading" class="section-title">At a glance</h2>
        {#if curated.shown.length > 0}
          <div class="charts">
            {#each curated.shown as chart (chart.id)}
              {@const title = curatedTitle(chart)}
              {@const format = curatedFormat(chart.unit)}
              {@const latest = latestForDisplay(chart.series)}
              <section class="card" data-testid="curated-{chart.id}">
                <h3 class="card-title">{title}</h3>
                <TimeSeriesChart
                  {title}
                  timestamps={chart.timestamps}
                  series={[{ label: title, data: chart.series }]}
                  height={160}
                  aspect={CHART_ASPECT}
                  maxHeight={CHART_MAX_HEIGHT}
                  showLegend={false}
                  formatValue={format}
                  tickValues={chart.unit === "bytes-per-second" ? bytesRateTicks : undefined}
                />
                <p class="summary">Latest: <span data-testid="curated-latest">{latest === null ? "—" : format(latest)}</span></p>
              </section>
            {/each}
          </div>
        {/if}
        {#if curated.hidden.length > 0}
          <EmptyState
            testid="curated-hidden"
            title={curated.shown.length === 0 ? "No data for the default charts yet" : "Some default charts have no data yet"}
            detail={`Drawn once they have a value: ${curated.hidden.map((c) => c.title).join(", ")}. Rates need two samples, one per refresh.`}
          />
        {/if}
      </section>
    {/if}
    <p class="note"><a href={countersHref(["all"], "#/graphs")} data-testid="graphs-show-all">Show every counter</a></p>
  {:else}
    <p class="note"><a href="#/graphs" data-testid="graphs-show-default">Back to the default charts</a></p>
  {/if}

  {#if view === null}
    <p class="loading">Loading graphs...</p>
  {:else if view.names.length === 0}
    {#if !showAll}
      <!-- The default view with nothing pinned: the curated section above is the page. -->
    {:else if search === ""}
      <p class="note" data-testid="graphs-live-warming">
        The charts fill in as samples arrive, one per refresh.
      </p>
    {:else}
      <p class="note" data-testid="graphs-no-match">No counter matches this filter.</p>
    {/if}
  {:else}
    <div class="charts">
      {#each rendered as chart (chart.name)}
        {@const values = plotted(chart.series)}
        <section class="card" data-testid="graph-card-{chart.name}">
          <h2 class="card-title">{chart.title}</h2>
          {#if !showAll}
            <button type="button" class="control btn btn-secondary btn-compact remove" aria-label="Remove {chart.name}" onclick={() => setPinned(pinned.filter((n) => n !== chart.name))}>Remove</button>
          {/if}
          <TimeSeriesChart
            title={chart.title}
            timestamps={chart.timestamps}
            series={[{ label: chart.title, data: chart.series }]}
            height={160}
                  aspect={CHART_ASPECT}
                  maxHeight={CHART_MAX_HEIGHT}
            showLegend={false}
            formatValue={chart.isRate ? formatRate : undefined}
          />
          <p class="summary">
            Latest: <span data-testid="graph-latest">{formatSample(latestForDisplay(chart.series), chart.isRate)}</span>
            · Min: {values.length === 0 ? "—" : formatSample(Math.min(...values), chart.isRate)}
            · Max: {values.length === 0 ? "—" : formatSample(Math.max(...values), chart.isRate)}
          </p>
          {#if chart.liveSince !== null}
            <p class="note" data-testid="graph-live-since">
              Live since {new Date(chart.liveSince * 1000).toLocaleTimeString()}.
            </p>
          {/if}
          {#if chart.liveOnly}
            <p class="note" data-testid="graph-live-only">No logged history for this counter.</p>
          {/if}
          <details>
            <summary>Raw data</summary>
            <table class="raw">
              <thead><tr><th>Time</th><th>Value</th></tr></thead>
              <tbody>
                {#each chart.timestamps as ts, i (ts)}
                  <tr>
                    <td>{new Date(ts * 1000).toLocaleTimeString()}</td>
                    <td>{formatSample(chart.series[i] ?? null, chart.isRate)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </details>
        </section>
      {/each}
    </div>
    {#if remaining > 0}
      <button
        type="button"
        class="control btn btn-secondary show-more"
        data-testid="graphs-show-more"
        onclick={() => (shownCount += PAGE_SIZE)}
      >
        Show {PAGE_SIZE} more ({remaining} remaining)
      </button>
    {/if}
  {/if}
</div>

<style>
  .curated {
    margin-bottom: var(--ps-space-lg);
  }
  /* The note about charts without data sits below the charts, not on them. */
  .curated :global(.empty-state) {
    margin-top: var(--ps-space-md);
  }
  .section-title {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-base);
    font-weight: 600;
  }
  .add-list {
    margin: 0 0 var(--ps-space-md);
    padding: 0;
    list-style: none;
  }
  .add-list .control {
    font-family: var(--ps-font-mono);
    white-space: normal;
    overflow-wrap: anywhere;
    max-width: 100%;
  }
  /* Charts share the full width, at most three to a row: a wide window
     makes them larger (their height follows their width), not flatter. */
  .charts {
    display: grid;
    grid-template-columns: repeat(
      auto-fit,
      minmax(max(320px, calc((100% - 2 * var(--ps-space-md)) / 3)), 1fr)
    );
    gap: var(--ps-space-md);
  }
  .card {
    min-width: 0;
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-surface);
  }
  .card-title {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    overflow-wrap: anywhere;
  }
  .summary {
    margin: var(--ps-space-xs) 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
  .raw {
    width: 100%;
    border-collapse: collapse;
    font-size: var(--ps-font-size-xs);
    font-family: var(--ps-font-mono);
  }
  .raw th, .raw td {
    text-align: left;
    padding: 0 var(--ps-space-sm) 0 0;
  }
  .note {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }
  .loading { color: var(--ps-text-secondary); }
  .control:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }
  .control-label {
    color: var(--ps-text-secondary);
  }
  .show-more { margin-top: var(--ps-space-md); }
</style>
