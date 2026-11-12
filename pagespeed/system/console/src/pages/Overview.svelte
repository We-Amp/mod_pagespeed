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
  import InspectUrlForm from "$lib/InspectUrlForm.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import ScopeChip from "$lib/ScopeChip.svelte";
  import { formatBytes, formatCount, formatDuration, formatPercent, formatSignedBytes } from "$lib/utils/format";
  import { moduleSavingsByType, optimizedCopyHitRate, servedOptimizedText } from "$lib/utils/savings";
  import { TERMS } from "$lib/data/terms";
  import { serveSavingsView } from "$lib/utils/serve-savings";
  import { hostSavingsView } from "$lib/utils/host-savings";
  import { globalConsoleHref } from "$lib/utils/urls-api";
  import { scopeChipShown } from "$lib/utils/scope-chip";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import { useConsole } from "$lib/api/context";
  import {
    availabilityText,
    countText,
    daemonServeSavings,
    MIN_OPTIMIZER_VERSION,
    moduleSummary,
    findingsMark,
    optimizerServes,
    optimizerStateText,
    OPTIMIZER_SETUP_DOC,
    overviewScopeLine,
    overviewVerdict,
    RESPONSE_HEADER_CHECK,
    SAVINGS_RELATION,
    SERVED_COUNTS_RELATION,
    serveShareJudgement,
    timeBase,
    verdictLevel,
    type TimeBase,
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
  // One wording for a time base, in the sentence and in the tiles (overview.ts).
  let moduleBase = $derived(timeBase("module", moduleStart, sampleAt ?? 0));
  let optimizerBase = $derived(timeBase("optimizer", optimizerStart, sampleAt ?? 0));
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
  // This sample's module read failed (the server answered, or not at all):
  // the figures on the page, if any, are the last good ones.
  let moduleStale = $derived(moduleError !== null);
  // The findings' count line; it closes the verdict sentence.
  let findings = $derived(healthSummary(alerts, lastModule !== null && !moduleStale, acknowledged, logUnavailable, logOff));
  // A finding that needs action is marked in words next to the count; the
  // strip's own colour stays with the verdict's word.
  let mark = $derived(findingsMark(findings.level, alerts.some((a) => a.severity !== "info")));
  // The module's figures by type, and the optimizer's serves by type (whole server).
  let rows = $derived(vars ? moduleSavingsByType(vars) : null);
  let serveRows = $derived(serveSavingsView(daemonView?.daemonStats?.serve_savings)?.served ?? []);
  // The whole-server console leads with the optimizer's transfer savings, in
  // the second tile. A per-host console never states them as this site's:
  // there they stay in the muted whole-server card, and the module's figure leads.
  let optimizerFigures = $derived(scope.isGlobal && daemonView?.daemon === "ok" && serveSavings !== null);
  // The tiles show whenever there is a figure to lead with: module activity,
  // or the optimizer's serves right after a web server restart. Without one
  // (no traffic yet, or no module figures at all) the two cards carry the page.
  let tiles = $derived(summary !== null && (summary.active || optimizerFigures));
  let optimizerLeads = $derived(tiles && optimizerFigures);
  // No traffic in this console's scope: the first-run steps.
  let idle = $derived(summary !== null && !tiles);
  let cacheTile = $derived(
    optimizerLeads || !(daemonView?.daemon === "not-configured" || summary === null || summary.inPlaceRequests === 0),
  );
  let judgement = $derived(serveShareJudgement(cacheRate, moduleBase.warming));
  // The verdict reads this sample only: a failed module read states no figure.
  let verdict = $derived(
    overviewVerdict({
      wholeServer: scope.isGlobal,
      at: sampleAt ?? 0,
      down: serverDown,
      lastAnswerAt,
      module: summary !== null && !moduleStale ? { summary, startMs: moduleStart } : null,
      staleFiguresAt: lastModule !== null ? updatedAt : null,
      optimizer: {
        availability: daemonView?.daemon ?? "transient",
        stateWord: optimizer?.word ?? "",
        serves: daemonView?.daemon === "ok" ? optimizerServes(daemonView.daemonStats) : null,
        startMs: optimizerStart,
      },
    }),
  );
  let level = $derived(verdictLevel(verdict.state));
  // The verdict of a failed module read already says the statistics could
  // not be read: the findings count would only say it again.
  let findingsSaid = $derived(moduleStale && lastModule !== null && alerts.length === 0);

  // The findings count is a link to the findings, which follow the tiles
  // (two screens down on a phone): it scrolls there and moves focus.
  let findingsEl = $state<HTMLElement | null>(null);
  function showFindings(event: MouseEvent): void {
    event.preventDefault();
    const el = findingsEl;
    if (el === null) return;
    el.scrollIntoView({ block: "start" });
    // The findings take the focus for this jump only: once the focus moves
    // on they are plain content again, so a click on a finding's text (to
    // select it) does not focus them.
    el.tabIndex = -1;
    el.addEventListener("blur", () => el.removeAttribute("tabindex"), { once: true });
    el.focus({ preventScroll: true });
  }
</script>

<div class="page">
  <PageHeader
    title="Overview"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: 5000,
      onToggle: toggleAutoRefresh,
      // The optimizer not answering is a notice on this page, so it is part of the answer.
      onRefresh: () => poll.refresh(),
      failedParts: () => [moduleError, daemonView?.daemon === "unreachable"],
    }}
  />
  <p class="note scope-line" data-testid="overview-scope">{scopeLine}</p>
  {#if poll.loading}
    <p class="loading">Loading overview...</p>
  {:else}
    <!-- The answer first: the state in a word, the figures behind it, the findings' count. -->
    <div class="health-strip" class:health-strip-ok={level === "ok"} class:health-strip-info={level === "info"} data-testid="health-strip">
      <p class="verdict" data-testid="health-summary">
        <span
          class="status-dot"
          class:status-dot-ok={level === "ok"}
          class:status-dot-info={level === "info"}
          data-testid="health-dot"
        ></span><strong class="verdict-word">{verdict.word}</strong>{verdict.text.slice(verdict.word.length)}{#if !serverDown && !findingsSaid}{" "}{#if mark}<span
              class="level-badge level-badge-{mark.level}"
              data-testid="findings-mark">{mark.word}</span
            >{" "}{/if}<span data-testid="findings-summary" role="status"
            >{#if alerts.length > 0}<a href="#/overview" data-testid="findings-link" onclick={showFindings}>{findings.text}</a
              >{:else}{findings.text}{/if}</span
          >.{/if}
      </p>
    </div>
    {#if logOff}
      <UnavailableCard testid="overview-log-unavailable" />
    {/if}

    <!-- A source's time base, in the page's one wording, with its warming mark. -->
    {#snippet since(base: TimeBase, testid: string)}
      <span data-testid={testid} title={base.iso ?? undefined}>{base.text}</span>{#if base.warming}{" "}<span
          class="badge badge-outline-warning"
          data-testid="warming-pill">warming up</span
        >{/if}
    {/snippet}

    <!-- The optimizer's transfer savings: the second tile on the whole-server
         console (the leading figure), the muted whole-server card on a per-host console. -->
    {#snippet optimizerSavings(lead: boolean)}
      {#if serveSavings}
        <div data-testid="optimizer-savings">
          <p class={lead ? "stat-tile-value" : "metric-value figure"} data-testid={lead ? "overview-lead-figure" : undefined}>
            {formatSignedBytes(serveSavings.saved)}
          </p>
          <p class={lead ? "stat-tile-context" : "note figure-context"}>
            {optimizerBase.warming
              ? `of ${formatBytes(serveSavings.original)} so far,`
              : `${formatPercent(serveSavings.percent, serveSavings.saved)} of ${formatBytes(serveSavings.original)}`}
            {@render since(optimizerBase, "since-optimizer")}
          </p>
        </div>
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
            {@const words = servedOptimizedText(cacheRate.mode)}
            <p class={hero ? "stat-tile-context" : "note"} title={words.tooltip}>
              {#if !hero}The module counted {countText(summary.servedByOptimizer)} responses served optimized:{/if}
              {moduleBase.warming
                ? `of ${formatCount(cacheRate.total)}`
                : `${formatPercent(cacheRate.percent, cacheRate.served)} of ${formatCount(cacheRate.total)}`}
              {words.of}{moduleBase.warming ? " so far" : ""}
            </p>
          {:else if !hero}
            <p class="note">The module counted {countText(summary.servedByOptimizer)} responses served optimized.</p>
          {/if}
          {#if !hero}<p class="note" data-testid="served-counts-relation">{SERVED_COUNTS_RELATION}</p>{/if}
        </div>
        {#if judgement}
          <p class={hero ? "stat-tile-context" : "note"} data-testid="serve-share-judgement">{judgement}</p>
        {/if}
      {/if}
    {/snippet}

    <!-- The optimizer's state: the fourth tile, or the optimizer card when there
         are no tiles. The value is the state word alone; "last known" is context. -->
    {#snippet optimizerState(valueClass: string, contextClass: string)}
      {#if optimizer && daemonView}
        <div class="state" data-testid="optimizer-state">
          <p class={valueClass}>
            <span
              class="status-dot"
              class:status-dot-ok={optimizer.ok && !serverDown}
              class:status-dot-warning={!optimizer.ok && !serverDown}
            ></span>{optimizer.word}
          </p>
          {#if serverDown}
            <p class={contextClass} data-testid="optimizer-last-known">Last known state</p>
          {/if}
        </div>
        <p class={contextClass}>Up {formatDuration(daemonView.health?.uptime_seconds)}</p>
        {#if optimizer.busy}
          <p class={contextClass} data-testid="optimizer-busy">All optimizer threads busy.</p>
        {/if}
      {:else if availability}
        <div class="state" data-testid="optimizer-state">
          <p class={valueClass}>
            <span
              class="status-dot"
              class:status-dot-warning={!serverDown && (daemonView?.daemon === "unreachable" || daemonView?.daemon === "unsupported")}
            ></span>{availability.word}
          </p>
          {#if serverDown && daemonView?.daemon !== "transient"}
            <p class={contextClass} data-testid="optimizer-last-known">Last known state</p>
          {/if}
        </div>
      {/if}
    {/snippet}

    {#if tiles && summary}
      <!-- Stale figures say so above them, with the reason. While the server
           is down the connection banner and the verdict already say it. -->
      <RefreshNotice error={serverDown ? null : moduleError} />
      <div class="stat-tiles" data-testid="overview-tiles">
        <section class="card stat-tile" data-testid="tile-rewriting" aria-labelledby="tile-rewriting-label">
          <h2 class="stat-tile-label" id="tile-rewriting-label">{TERMS.savedByRewriting}</h2>
          <div data-testid="module-bytes-saved">
            <p class="stat-tile-value" data-testid={optimizerLeads ? undefined : "overview-lead-figure"}>
              {formatBytes(summary.bytesSaved)}
            </p>
            <p class="stat-tile-context">
              {#if summary.savedPercent !== null && summary.originalBytes !== null}
                {moduleBase.warming
                  ? `of ${formatBytes(summary.originalBytes)} so far,`
                  : `${formatPercent(summary.savedPercent, summary.bytesSaved ?? 0)} of ${formatBytes(summary.originalBytes)}`}
              {/if}
              {@render since(moduleBase, "since-module")}
            </p>
          </div>
          <a class="stat-tile-link" href="#/savings">Savings</a>
        </section>

        {#if cacheTile}
          <section class="card stat-tile" data-testid="tile-cache" aria-labelledby="tile-cache-label">
            {#if optimizerLeads}
              <h2 class="stat-tile-label" id="tile-cache-label">{TERMS.savedByOptimizerCache}</h2>
              {@render optimizerSavings(true)}
              <p class="stat-tile-context" data-testid="savings-relation">{SAVINGS_RELATION}</p>
            {:else}
              <h2 class="stat-tile-label" id="tile-cache-label">{TERMS.servedOptimized}</h2>
              {@render servedShare(true)}
            {/if}
            <a class="stat-tile-link" href="#/savings" data-testid="module-served-link">Savings</a>
          </section>
        {/if}

        <section class="card stat-tile" data-testid="tile-fetch-failures" aria-labelledby="tile-failures-label">
          <h2 class="stat-tile-label" id="tile-failures-label">Resource fetch failures</h2>
          <p class="stat-tile-value" data-testid="module-fetch-failures">{countText(summary.fetchFailures)}</p>
          <a class="stat-tile-link" href="#/logs?source=module&level=warning" data-testid="module-fetch-failures-link"
            >Module log</a
          >
        </section>

        <!-- Named apart from the Optimizer card below: two landmarks, two names. -->
        <section class="card stat-tile" data-testid="tile-optimizer" aria-label="Optimizer state">
          <h2 class="stat-tile-label">
            Optimizer <ScopeChip label="whole server" shown={optimizerChip} />
          </h2>
          {@render optimizerState("stat-tile-value", "stat-tile-context")}
          <a class="stat-tile-link" href="#/optimizer">Optimizer status</a>
        </section>
      </div>
    {/if}

    <!-- "Why is this URL not optimized?" starts here, on every console. -->
    <InspectUrlForm />

    <div class="findings-anchor" bind:this={findingsEl} data-testid="findings-anchor">
      <FindingsPanel
        findings={alerts}
        {acknowledged}
        onAcknowledge={acknowledge}
        onUnacknowledge={unacknowledge}
      />
    </div>

    <div class="cards">
      <section class="card reads-across-container" data-testid="module-card" aria-labelledby="module-heading">
        <h2 class="card-title" id="module-heading">Module <span class="version">{moduleVersion}</span> <ScopeChip label={moduleScopeLabel} shown={moduleChip} /></h2>
        {#if summary}
          {#if !tiles}
            <RefreshNotice error={serverDown ? null : moduleError} />
          {/if}
          {#if summary.active && rows}
            <table class="data-inline breakdown reads-across card-table" data-testid="module-by-type">
              <caption>{TERMS.savedByRewriting}, by type</caption>
              <thead>
                <tr><th scope="col">Type</th><th scope="col">Saved</th><th scope="col">Of original</th><th scope="col">Saved %</th></tr>
              </thead>
              <tbody>
                {#each rows as row (row.key)}
                  <tr>
                    <th scope="row" data-label="Type">{row.label}</th>
                    <td class="num" data-label="Saved">{formatBytes(row.saved)}{#if row.regressed} <span class="note">(no net savings)</span>{/if}</td>
                    <td class="num" data-label="Of original">{formatBytes(row.original)}</td>
                    <td class="num" data-label="Saved %">{moduleBase.warming ? "warming up" : formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          {:else if idle}
            <!-- Nothing counted yet: what to do next, in order. -->
            <EmptyState
              testid="overview-empty"
              title="The module is running and ready."
              detail="Nothing has been optimized yet. Once this web server serves pages, this overview shows what the module and the optimizer are doing."
            />
            <h3 class="minor-title steps-title">Next steps</h3>
            <ol class="next-steps" data-testid="overview-next-steps">
              <li>Load a page of this site in a browser, twice: the first request starts the optimization.</li>
              <li>
                Check the response header. The module adds its own header to the pages it handles:
                <code class="command" data-testid="overview-header-check">{RESPONSE_HEADER_CHECK}</code>
              </li>
              <li>Come back here: the figures appear with the first optimized response.</li>
            </ol>
          {:else}
            <!-- The optimizer has served; the module's own counters started over. -->
            <p class="note" data-testid="module-nothing-yet">The module has rewritten nothing {moduleBase.text}.</p>
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
        <h2 class="card-title" id="optimizer-heading">
          Optimizer
          {#if typeof daemonView?.health?.version === "string" && daemonView.health.version}<span class="version">{daemonView.health.version}</span>{/if}
          <ScopeChip label="whole server" shown={optimizerChip} />
        </h2>
        {#if !scope.isGlobal}
          <p class="note" data-testid="optimizer-scope-note">
            Server-wide numbers. The per-host view is on the
            {#if wholeServerHref}<a href={wholeServerHref}>whole-server console</a>{:else}whole-server console{/if}.
          </p>
        {/if}
        {#if !tiles}
          {@render optimizerState("metric-value figure", "note")}
        {/if}
        {#if optimizer && daemonView}
          {#if serveSavings && !optimizerLeads}
            <p class="metric-label">{TERMS.savedByOptimizerCache}</p>
            {@render optimizerSavings(false)}
          {:else if !serveSavings}
            <p class="note">Counters {@render since(optimizerBase, "since-optimizer")}</p>
          {/if}
          {#if serveSavings}
            <p class="note" data-testid="savings-qualifier">{SAVINGS_QUALIFIER}</p>
          {/if}
          {#if optimizerLeads}
            <!-- The module's own count of those serves, with its share. -->
            {@render servedShare(false)}
          {/if}
          {#if scope.isGlobal && serveRows.length > 0}
            <table class="data-inline breakdown reads-across card-table" data-testid="optimizer-by-type">
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
                    <td class="num" data-label="Saved %">{optimizerBase.warming ? "warming up" : formatPercent(row.percent, row.saved)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          {/if}
          {#if serveSavings}
            <SavingsSplitLine split={serveSavings.split} />
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

  /* The verdict is one sentence at the reading size; its first word is the state. */
  .verdict {
    margin: 0;
  }

  .verdict-word {
    font-weight: 600;
  }

  /* The findings follow the tiles; the count in the sentence jumps here,
     clear of the top bar. */
  .findings-anchor {
    scroll-margin-top: calc(var(--ps-topbar-height) + var(--ps-space-md));
  }

  /* Focus lands here only from the count's link, by keyboard or pointer:
     the ring shows where the link went. */
  .findings-anchor:focus {
    outline: 2px solid var(--ps-primary);
    outline-offset: 2px;
    border-radius: var(--ps-border-radius-lg);
  }

  /* The state word and, while the server is down, the line that says it is the last known one. */
  .state {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
    min-width: 0;
  }

  .state p {
    margin: 0;
  }

  .figure {
    margin: 0;
    overflow-wrap: anywhere;
  }

  .figure-context {
    margin-top: 0;
  }

  .scope-line {
    margin-bottom: var(--ps-space-md);
  }

  .cards {
    display: grid;
    /* The cards share the full width in equal columns; more cards, more
       columns. A card is never narrower than its by-type table needs
       (five columns of figures, also in a wide system font). */
    grid-template-columns: repeat(auto-fit, minmax(min(100%, 30rem), 1fr));
    gap: var(--ps-space-md);
    /* The two cards are of a size: they share one height. */
  }

  .version {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    font-weight: 400;
    color: var(--ps-text-secondary);
    overflow-wrap: anywhere;
  }

  .steps-title {
    margin-top: var(--ps-space-md);
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

</style>
