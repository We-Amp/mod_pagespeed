<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script module lang="ts">
  import { SAVINGS_QUALIFIER } from "$lib/utils/serve-savings";
  import { ALERT_RULES, AlertTracker, RATE_ALERT_HOLD_MS } from "$lib/alerts";
  import { createAckStore } from "$lib/utils/finding-acks";

  // One tracker per browser tab. Acknowledgements are remembered across page
  // loads (finding-acks.ts) until the finding's condition clears.
  const tracker = new AlertTracker(ALERT_RULES, RATE_ALERT_HOLD_MS, createAckStore());
</script>

<script lang="ts">
  // Overview: the console's landing page -- is the module working, is the
  // optimizer daemon healthy, and what have they saved. It answers first:
  // one sentence with the state and the figures behind it, then a row of
  // tiles with the headline figures, then the findings and the breakdown by
  // type. One poll samples the module statistics and the daemon's health and
  // statistics together.
  import { usePolling } from "$lib/api/polling.svelte";
  import { isConnectionFailure } from "$lib/api/connection";
  import { ApiError, isUnavailable } from "$lib/api/client";
  import UnavailableCard from "$lib/UnavailableCard.svelte";
  import type { StatsResponse } from "$lib/api/types";
  import { healthSummary, num, type ActiveAlert, type AlertSnapshot } from "$lib/alerts";
  import EmptyState from "$lib/EmptyState.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import SavingsSplitLine from "$lib/SavingsSplitLine.svelte";
  import FindingsPanel from "$lib/FindingsPanel.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import ScopeChip from "$lib/ScopeChip.svelte";
  import { formatBytes, formatCount, formatDuration, formatPercent, formatSignedBytes } from "$lib/utils/format";
  import { moduleSavingsByType, optimizedCopyHitRate } from "$lib/utils/savings";
  import { serveSavingsView } from "$lib/utils/serve-savings";
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
    optimizerServes,
    optimizerStateText,
    OPTIMIZER_SETUP_DOC,
    overviewScopeLine,
    overviewVerdict,
    RESPONSE_HEADER_CHECK,
    SAVINGS_RELATION,
    serveShareJudgement,
    stripLevel,
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
  // When the server last answered the module read at all (an error answer
  // counts): the stamp is the age of the figures, this is the age of contact.
  let lastAnswerAt = $state<number | null>(null);

  // This mount's first sample is a baseline for rate rules: nothing was
  // sampled while the page was not shown.
  tracker.resetBaseline();
  let alerts = $state<ActiveAlert[]>(tracker.visible());
  let acknowledged = $state<ActiveAlert[]>(tracker.acknowledged());
  // The last sample's message-log read failed (404, 5xx, junk): the summary
  // must then say the findings are unavailable, never "No findings".
  let logUnavailable = $state(false);
  // ... because the server has turned the message log off (a state, not a fault).
  let logOff = $state(false);

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
        // The stamp is the age of the module figures: a sample whose module
        // read failed does not move it.
        if (sample.module.ok) {
          lastModule = sample.module.data;
          updatedAt = sample.at;
        }
        if (snap.daemon !== "transient" || daemonView === null) daemonView = snap;
        // The hosts the optimizer served feed the lens (the best-ranked source).
        lens.observe(hostSavingsView(snap.daemonStats?.serve_savings_by_host)?.hosts.map((r) => r.host) ?? [], 0);
        alerts = tracker.evaluate(snap, sample.at);
        acknowledged = tracker.acknowledged();
        logUnavailable = snap.messages === null;
        logOff = sample.messages !== undefined && !sample.messages.ok && isUnavailable(sample.messages.error);
        if (!sample.module.ok) {
          const status = sample.module.error instanceof ApiError ? sample.module.error.status : null;
          if (isConnectionFailure("/stats_json", status)) throw sample.module.error;
        }
        lastAnswerAt = sample.at;
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

  // The poll fails exactly when the module read lost the connection (see the
  // fetcher above). What this page last read then says nothing about now:
  // the health line and the optimizer's dot claim nothing until an answer.
  let serverDown = $derived(poll.error !== null);
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
  // The findings' count line; it closes the verdict sentence.
  let findings = $derived(healthSummary(alerts, lastModule !== null, acknowledged, logUnavailable, logOff));
  // The module's figures by type, and the optimizer's serves by type (whole server).
  let rows = $derived(vars ? moduleSavingsByType(vars) : null);
  let serveRows = $derived(serveSavingsView(daemonView?.daemonStats?.serve_savings)?.served ?? []);
  // The tiles need module figures; without them the two cards carry the page.
  let tiles = $derived(summary !== null && summary.active);
  // The whole-server console leads with the optimizer's transfer savings, in
  // the second tile. A per-host console never states them as this site's:
  // there they stay in the muted whole-server card, and the module's figure leads.
  let optimizerLeads = $derived(tiles && scope.isGlobal && daemonView?.daemon === "ok" && serveSavings !== null);
  let cacheTile = $derived(
    optimizerLeads || !(daemonView?.daemon === "not-configured" || summary === null || summary.inPlaceRequests === 0),
  );
  let judgement = $derived(serveShareJudgement(cacheRate, moduleSince?.warming ?? false));
  // The verdict reads this sample only: a failed module read states no figure.
  let verdict = $derived(
    overviewVerdict({
      wholeServer: scope.isGlobal,
      at: sampleAt ?? 0,
      down: serverDown,
      lastAnswerAt,
      module: summary !== null && moduleError === null ? { summary, startMs: moduleStart } : null,
      optimizer: {
        availability: daemonView?.daemon ?? "transient",
        stateWord: optimizer?.word ?? "",
        serves: daemonView?.daemon === "ok" ? optimizerServes(daemonView.daemonStats) : null,
        startMs: optimizerStart,
      },
    }),
  );
  let level = $derived(stripLevel(verdict.state, serverDown ? null : findings.level));
  let dot = $derived(level === "healthy" ? "ok" : level);
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
    <!-- The answer first: the state in a word, the figures behind it, the findings' count. -->
    <div class="health-strip health-strip-{dot}" data-testid="health-strip">
      <p class="verdict" data-testid="health-summary">
        <span class="status-dot status-dot-{dot}" data-testid="health-dot"></span><strong class="verdict-word"
          >{verdict.word}</strong
        >{verdict.text.slice(verdict.word.length)}{#if !serverDown}{" "}<span data-testid="findings-summary" role="status">{findings.text}</span>.{/if}
      </p>
      {#if alerts.length === 0 && findingsStamp && !serverDown}
        <p class="strip-meta" data-testid="findings-since">{findingsStamp}</p>
      {/if}
    </div>
    {#if logOff}
      <UnavailableCard testid="overview-log-unavailable" />
    {/if}

    {#snippet sinceModule(testid: string | undefined)}
      <p class="stat-tile-context" data-testid={testid} title={moduleSince?.iso ?? undefined}>
        {moduleSince?.text ?? "since restart"} (web server restart or reload)
        {#if moduleSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
      </p>
    {/snippet}

    {#snippet sinceOptimizer()}
      <p class="stat-tile-context" data-testid="since-optimizer" title={optimizerSince?.iso ?? undefined}>
        {optimizerSince?.text ?? "since restart"} (optimizer restart)
        {#if optimizerSince?.warming}<span class="warming" data-testid="warming-pill">warming up</span>{/if}
      </p>
    {/snippet}

    <!-- The optimizer's transfer savings: the second tile on the whole-server
         console, the muted whole-server card on a per-host console. -->
    {#snippet optimizerSavings(lead: boolean)}
      {#if serveSavings}
        <div data-testid="optimizer-savings">
          <p class="stat-tile-value" data-testid={lead ? "overview-lead-figure" : undefined}>
            {formatSignedBytes(serveSavings.saved)}
          </p>
          <p class="stat-tile-context">
            {optimizerSince?.warming
              ? `of ${formatBytes(serveSavings.original)} so far`
              : `${formatPercent(serveSavings.percent, serveSavings.saved)} of ${formatBytes(serveSavings.original)}`}
          </p>
        </div>
        <p class="stat-tile-context" data-testid="savings-qualifier">{SAVINGS_QUALIFIER}</p>
      {/if}
    {/snippet}

    <!-- What the module counted as served from the optimizer's cache, with its share. -->
    {#snippet servedShare(hero: boolean)}
      {#if summary}
        <div data-testid="module-served">
          {#if hero}
            <p class="stat-tile-value">{countText(summary.servedByOptimizer)}</p>
          {/if}
          {#if cacheRate && cacheRate.percent !== null}
            <p
              class="stat-tile-context"
              title={cacheRate.mode === "optimizable"
                ? "Share of cacheable resource requests (CSS, JavaScript, images) answered from the optimizer's cache; conditional and HEAD requests count as served"
                : "Share of all in-place requests answered from the optimizer's cache"}
            >
              {#if !hero}The module counted {countText(summary.servedByOptimizer)} served from the optimizer's cache:{/if}
              {moduleSince?.warming
                ? `of ${formatCount(cacheRate.total)}`
                : `${formatPercent(cacheRate.percent, cacheRate.served)} of ${formatCount(cacheRate.total)}`}
              {cacheRate.mode === "optimizable"
                ? "cacheable resource requests; conditional and HEAD requests count as served"
                : "in-place requests"}{moduleSince?.warming ? " so far" : ""}
            </p>
          {:else if !hero}
            <p class="stat-tile-context">The module counted {countText(summary.servedByOptimizer)} served from the optimizer's cache.</p>
          {/if}
        </div>
        {#if judgement}
          <p class="stat-tile-context" data-testid="serve-share-judgement">{judgement}</p>
        {/if}
      {/if}
    {/snippet}

    <!-- The optimizer's state: the fourth tile, or the optimizer card when there are no tiles. -->
    {#snippet optimizerState(valueClass: string)}
      {#if optimizer && daemonView}
        <p class={valueClass} data-testid="optimizer-state">
          <span
            class="status-dot"
            class:status-dot-ok={optimizer.ok && !serverDown}
            class:status-dot-warning={!optimizer.ok && !serverDown}
          ></span>{optimizer.word}{#if serverDown}{" "}· last known{/if}
        </p>
        <p class="stat-tile-context">Up {formatDuration(daemonView.health?.uptime_seconds)}</p>
        {#if optimizer.busy}
          <p class="stat-tile-context" data-testid="optimizer-busy">All worker threads busy.</p>
        {/if}
      {:else if availability}
        <p class={valueClass} data-testid="optimizer-state">
          <span
            class="status-dot"
            class:status-dot-warning={!serverDown && (daemonView?.daemon === "unreachable" || daemonView?.daemon === "unsupported")}
          ></span>{availability.word}{#if serverDown && daemonView?.daemon !== "transient"}{" "}· last known{/if}
        </p>
      {/if}
    {/snippet}

    {#if tiles && summary}
      <div class="stat-tiles" data-testid="overview-tiles">
        <section class="card stat-tile" data-testid="tile-rewriting" aria-labelledby="tile-rewriting-label">
          <h2 class="stat-tile-label" id="tile-rewriting-label">Saved by rewriting</h2>
          <div data-testid="module-bytes-saved">
            <p class="stat-tile-value" data-testid={optimizerLeads ? undefined : "overview-lead-figure"}>
              {formatBytes(summary.bytesSaved)}
            </p>
            {#if summary.savedPercent !== null && summary.originalBytes !== null}
              <p class="stat-tile-context">
                {moduleSince?.warming
                  ? `of ${formatBytes(summary.originalBytes)} so far`
                  : `${formatPercent(summary.savedPercent, summary.bytesSaved ?? 0)} of ${formatBytes(summary.originalBytes)}`}
                of CSS, JavaScript and images
              </p>
            {/if}
          </div>
          {@render sinceModule("since-module")}
          <a class="stat-tile-link" href="#/savings">Savings</a>
        </section>

        {#if cacheTile}
          <section class="card stat-tile" data-testid="tile-cache" aria-labelledby="tile-cache-label">
            {#if optimizerLeads}
              <h2 class="stat-tile-label" id="tile-cache-label">Saved on optimizer cache serves</h2>
              {@render optimizerSavings(true)}
              <p class="stat-tile-context" data-testid="savings-relation">{SAVINGS_RELATION}</p>
              {@render sinceOptimizer()}
            {:else}
              <h2 class="stat-tile-label" id="tile-cache-label">Served from the optimizer's cache</h2>
              {@render servedShare(true)}
              {@render sinceModule(undefined)}
            {/if}
            <a class="stat-tile-link" href="#/savings" data-testid="module-served-link">Optimizer cache savings</a>
          </section>
        {/if}

        <section class="card stat-tile" data-testid="tile-fetch-failures" aria-labelledby="tile-failures-label">
          <h2 class="stat-tile-label" id="tile-failures-label">Resource fetch failures</h2>
          <p class="stat-tile-value" data-testid="module-fetch-failures">{countText(summary.fetchFailures)}</p>
          {@render sinceModule(undefined)}
          <a class="stat-tile-link" href="#/logs?source=module&level=warning" data-testid="module-fetch-failures-link"
            >Module log</a
          >
        </section>

        <section class="card stat-tile" data-testid="tile-optimizer" aria-labelledby="tile-optimizer-label">
          <h2 class="stat-tile-label" id="tile-optimizer-label">
            Optimizer <ScopeChip label="whole server" shown={optimizerChip} />
          </h2>
          {@render optimizerState("stat-tile-value")}
          <a class="stat-tile-link" href="#/optimizer">Optimizer status</a>
        </section>
      </div>
    {/if}

    <FindingsPanel
      findings={alerts}
      {acknowledged}
      onAcknowledge={acknowledge}
      onUnacknowledge={unacknowledge}
    />

    <div class="cards">
      <section class="card reads-across-container" data-testid="module-card" aria-labelledby="module-heading">
        <h2 id="module-heading">Module <span class="version">{moduleVersion}</span> <ScopeChip label={moduleScopeLabel} shown={moduleChip} /></h2>
        {#if summary}
          <RefreshNotice error={moduleError} />
          {#if summary.active && rows}
            <table class="breakdown reads-across" data-testid="module-by-type">
              <caption>Saved by rewriting, by type</caption>
              <thead>
                <tr><th scope="col">Type</th><th scope="col">Saved</th><th scope="col">Of original</th><th scope="col">Saved %</th></tr>
              </thead>
              <tbody>
                {#each rows as row (row.key)}
                  <tr>
                    <th scope="row">{row.label}</th>
                    <td class="num">{formatBytes(row.saved)}{#if row.regressed} <span class="note">(no net savings)</span>{/if}</td>
                    <td class="num">{formatBytes(row.original)}</td>
                    <td class="num">{moduleSince?.warming ? "warming up" : formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          {:else}
            <!-- Nothing counted yet: what to do next, in order. -->
            <EmptyState
              testid="overview-empty"
              title="The module is running and ready."
              detail="Nothing has been optimized yet. Once this web server serves pages, this overview shows what the module and the optimizer are doing."
            />
            <h3 class="steps-title">Next steps</h3>
            <ol class="next-steps" data-testid="overview-next-steps">
              <li>Load a page of this site in a browser, twice: the first request starts the optimization.</li>
              <li>
                Check the response header. The module adds its own header to the pages it handles:
                <code class="command" data-testid="overview-header-check">{RESPONSE_HEADER_CHECK}</code>
              </li>
              <li>Come back here: the figures appear with the first optimized response.</li>
            </ol>
          {/if}
        {:else if moduleError}
          <LoadError
            error={moduleError}
            noun="the module statistics"
            onretry={() => poll.refresh()}
            testid="module-error"
          />
        {/if}
        <p class="more"><a href="#/statistics">All statistics</a></p>
      </section>

      <section
        class="card reads-across-container"
        class:card-muted={!scope.isGlobal}
        data-testid="optimizer-card"
        aria-labelledby="optimizer-heading"
      >
        <h2 id="optimizer-heading">
          Optimizer
          {#if typeof daemonView?.health?.version === "string" && daemonView.health.version}<span class="version">{daemonView.health.version}</span>{/if}
          <ScopeChip label="whole server" shown={optimizerChip} />
        </h2>
        {#if !scope.isGlobal}
          <p class="note" data-testid="optimizer-scope-note">
            Server-wide numbers — the per-host view is on the
            {#if wholeServerHref}<a href={wholeServerHref}>whole-server console</a>{:else}whole-server console{/if}.
          </p>
        {/if}
        {#if !tiles}
          {@render optimizerState("metric-value state")}
        {/if}
        {#if optimizer && daemonView}
          {#if serveSavings && !optimizerLeads}
            <p class="metric-label">Saved on served responses</p>
            {@render optimizerSavings(false)}
          {/if}
          {#if optimizerLeads}
            <!-- The module's own count of those serves, with its share. -->
            {@render servedShare(false)}
          {/if}
          {#if scope.isGlobal && serveRows.length > 0}
            <table class="breakdown reads-across card-table" data-testid="optimizer-by-type">
              <caption>Served from the optimizer's cache, by type</caption>
              <thead>
                <tr><th scope="col">Type</th><th scope="col">Responses</th><th scope="col">Saved</th><th scope="col">Of original</th><th scope="col">Saved %</th></tr>
              </thead>
              <tbody>
                {#each serveRows as row (row.key)}
                  <tr>
                    <th scope="row" data-label="Type">{row.label}</th>
                    <td class="num" data-label="Responses">{formatCount(row.hits)}</td>
                    <td class="num" data-label="Saved">{formatSignedBytes(row.saved)}</td>
                    <td class="num" data-label="Of original">{formatBytes(row.original)}</td>
                    <td class="num" data-label="Saved %">{optimizerSince?.warming ? "warming up" : formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          {/if}
          {#if serveSavings}
            <SavingsSplitLine split={serveSavings.split} />
          {/if}
          {#if !optimizerLeads}
            {@render sinceOptimizer()}
          {/if}
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
          {#if daemonView?.daemon === "unreachable"}
            <EmptyState
              testid="optimizer-empty"
              title={availability.detail}
              detail="This page asks again on every refresh."
            />
          {:else}
            <p class="note">{availability.detail}</p>
            {#if daemonView?.daemon === "not-configured"}
              <p class="more">
                <a href={OPTIMIZER_SETUP_DOC} target="_blank" rel="noopener noreferrer" data-testid="add-optimizer"
                  >Add the optimizer</a
                >
              </p>
            {/if}
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

  /* The verdict is one sentence at the reading size; its first word is the state. */
  .verdict {
    margin: 0;
  }

  .verdict-word {
    font-weight: 600;
  }

  .strip-meta {
    margin: var(--ps-space-xs) 0 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .stat-tiles {
    margin-bottom: var(--ps-space-md);
  }

  .cards {
    display: grid;
    /* The cards share the full width in equal columns; more cards, more
       columns. */
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-card-min)), 1fr));
    gap: var(--ps-space-md);
    /* The two cards are of a size: they share one height. */
  }

  .card h2:not(.stat-tile-label) {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--ps-space-sm);
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-type-card-title-size);
    font-weight: var(--ps-type-card-title-weight);
  }

  .version {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    font-weight: 400;
    color: var(--ps-text-secondary);
    overflow-wrap: anywhere;
  }

  .warming {
    margin-left: var(--ps-space-xs);
    font-weight: 600;
    color: var(--ps-warning-text);
    border: 1px solid var(--ps-warning);
    border-radius: 999px;
    padding: 0 var(--ps-space-sm);
    white-space: nowrap;
  }

  /* The by-type tables: columns at their content's width, a step apart. */
  .breakdown {
    border-collapse: collapse;
    font-size: var(--ps-font-size-sm);
  }

  .breakdown caption {
    padding-bottom: var(--ps-space-xs);
    text-align: left;
    font-size: var(--ps-type-label-size);
    font-weight: var(--ps-type-label-weight);
    color: var(--ps-text-secondary);
  }

  .breakdown th,
  .breakdown td {
    padding: var(--ps-space-xs) var(--ps-space-md) var(--ps-space-xs) 0;
    text-align: left;
    border-bottom: 1px solid var(--ps-border-light);
  }

  .breakdown td {
    white-space: nowrap;
  }

  .breakdown thead th {
    color: var(--ps-text-secondary);
    font-weight: 600;
  }

  .breakdown tbody th {
    font-weight: 400;
  }

  .state {
    margin: 0 0 var(--ps-space-xs);
    overflow-wrap: anywhere;
  }

  /* On a phone the card rows carry their own padding. */
  @media (max-width: 640px) {
    .breakdown.card-table th,
    .breakdown.card-table td {
      padding: var(--ps-space-xs) var(--ps-space-sm);
    }
  }

  .steps-title {
    margin: var(--ps-space-md) 0 var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
  }

  .next-steps {
    max-width: var(--ps-card-max);
    margin: 0;
    padding-left: var(--ps-space-lg);
    font-size: var(--ps-font-size-sm);
  }

  .next-steps li + li {
    margin-top: var(--ps-space-xs);
  }

  /* A command to copy: on its own line, wrapping instead of scrolling. */
  .command {
    display: block;
    margin-top: var(--ps-space-xs);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    overflow-wrap: anywhere;
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
</style>
