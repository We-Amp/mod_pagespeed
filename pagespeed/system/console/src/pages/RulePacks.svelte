<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Rule packs: the module's rule-pack status -- the configuration, every
  // pack of the scope in load order, the rule_pack_* counters and the
  // bounded tail of the decision log -- read by ONE poll of the admin
  // handler's rule_packs leaf as JSON (rule-packs.ts). The server-rendered
  // page stays as the fallback and for scripts (?raw=1).
  //
  // Which pack's decision log is loaded is the hash's pack= (a control
  // navigates, so the page mounts with its one poller, as Logs does); the
  // filters on the loaded tail run in the browser. On the whole-server
  // console the host lens narrows the tail's rows to one host.
  //
  // A pack file the server has not read yet reports state "none" with no
  // id: that is "still starting", never "no packs configured", and the page
  // asks again at its normal cadence.
  //
  // Every string below is configuration or log text (decision paths and
  // hosts are visitor-controlled): shown as text nodes, never as links.
  import { usePolling } from "$lib/api/polling.svelte";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import EmptyState from "$lib/EmptyState.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import UnavailableCard from "$lib/UnavailableCard.svelte";
  import { isUnavailable } from "$lib/api/client";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import { formatCount } from "$lib/utils/format";
  import { serverPageHref } from "$lib/utils/nav";
  import {
    NO_FILTERS,
    distinctValues,
    filterRows,
    rulePacksView,
    tailPackOptions,
    tailStatusText,
    withQuery,
    type RulePacksView,
  } from "$lib/utils/rule-packs";

  const { api, scope, lens } = useConsole();

  const POLL_MS = 5000;
  // The tail's pack, from the address; null: the server's default (the first loaded pack).
  const requestedPack = router.params.get("pack") || null;

  let updatedAt = $state<number | null>(null);
  const poll = usePolling(async () => {
    const view = rulePacksView(await api.getRulePacks(requestedPack ?? undefined));
    lens.observe(view.log.rows.map((r) => r.host), 2);
    updatedAt = Date.now();
    return view;
  }, POLL_MS);

  let view = $derived<RulePacksView | null>(poll.data ?? null);

  // Filters of the loaded tail (client side).
  let actionFilter = $state("");
  let reasonFilter = $state("");
  let ruleFilter = $state("");
  let packFilter = $state("");

  let lensHost = $derived(activeLensHost(scope, lens));
  let tailRows = $derived(view?.log.rows ?? []);
  let shown = $derived(
    filterRows(tailRows, { ...NO_FILTERS, pack: packFilter, action: actionFilter, reason: reasonFilter, rule: ruleFilter, host: lensHost }),
  );
  let packChoices = $derived(distinctValues(tailRows, "pack_id"));
  let actionChoices = $derived(distinctValues(tailRows, "action"));
  let reasonChoices = $derived(distinctValues(tailRows, "reason"));
  let tailPacks = $derived(view ? tailPackOptions(view.packs) : []);
  let tailNote = $derived(view ? tailStatusText(view.log.status, view.log.error) : null);
  let filtering = $derived(packFilter !== "" || actionFilter !== "" || reasonFilter !== "" || ruleFilter.trim() !== "" || lensHost !== null);

  // The loaded tail's pack as the server resolved it (the default, or ?pack=).
  let tailPack = $derived(view?.log.pack ?? requestedPack);

  let serverPage = serverPageHref(api.basePath, "rule_packs");
  let rawHref = $derived(withQuery(serverPage, { raw: "1", pack: tailPack }));
  let pageHref = $derived(withQuery(serverPage, { pack: tailPack }));

  function selectTailPack(id: string) {
    router.navigate(id === "" ? "#/rule-packs" : `#/rule-packs?pack=${encodeURIComponent(id)}`);
  }

  function clearFilters() {
    packFilter = "";
    actionFilter = "";
    reasonFilter = "";
    ruleFilter = "";
  }

  function timeOf(iso: string | null | undefined): string {
    return iso ? iso.replace("T", " ").replace(/Z$/, " UTC") : "—";
  }
</script>

<div class="page">
  <PageHeader
    title="Rule packs"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: POLL_MS,
      onToggle: () => (poll.autoRefresh ? poll.stop() : poll.start()),
      onRefresh: () => poll.refresh(),
    }}
  >
  </PageHeader>

  <p class="scope-note" data-testid="rp-scope-note">
    {#if scope.isGlobal}
      The rule packs of the whole-server configuration, and the decisions every virtual host logged.
    {:else}
      The rule packs of this virtual host{scope.host ? ` (${scope.host})` : ""}, and the decisions it logged.
    {/if}
    <a href={pageHref} data-testid="rp-server-page">Server page</a>
  </p>

  {#if poll.loading}
    <p class="loading">Loading the rule packs...</p>
  {:else if isUnavailable(poll.error)}
    <UnavailableCard testid="rule-packs-unavailable" />
  {:else if poll.error && view === null}
    <LoadError message={poll.error.message} />
  {:else if view !== null}
    <RefreshNotice error={poll.error} />

    <section class="block" aria-labelledby="rp-config-heading" data-testid="rp-config">
      <h2 id="rp-config-heading">Configuration</h2>
      <div class="info-grid info-grid--panel">
        <div class="info-item">
          <span class="metric-label">Rule pack filter</span>
          <span class="metric-value" data-testid="rp-filter-enabled">{view.filterEnabled ? "on" : "off"}</span>
        </div>
        <div class="info-item">
          <span class="metric-label">RulePackMode</span>
          <span class="metric-value mono" data-testid="rp-mode">{view.mode || "—"}</span>
        </div>
        <div class="info-item">
          <span class="metric-label">RulePackRequireSigned</span>
          <span class="metric-value" data-testid="rp-require-signed">{view.requireSigned ? "on" : "off"}</span>
        </div>
        <div class="info-item info-wide">
          <span class="metric-label">RulePackFile (load order)</span>
          {#if view.files.length === 0}
            <span class="metric-value" data-testid="rp-files">none</span>
          {:else}
            <ol class="files" data-testid="rp-files">
              {#each view.files as file, i (i)}
                <li class="mono">{file}</li>
              {/each}
            </ol>
          {/if}
        </div>
      </div>
    </section>

    <section class="block" aria-labelledby="rp-packs-heading" data-testid="rp-packs">
      <h2 id="rp-packs-heading">Packs</h2>
      {#if view.noneConfigured}
        <EmptyState
          testid="rp-none-configured"
          title="No rule pack is configured"
          detail={view.filterEnabled
            ? "RulePackFile is not set, so the rule pack filter has nothing to apply."
            : "RulePackFile is not set. Set it to a pack file and enable the rule pack filter to use rule packs."}
        />
      {:else if view.notLoadedYet}
        <p class="source-note" data-testid="rp-not-loaded-yet">
          This server process has not read its rule pack {view.files.length === 1 ? "file" : "files"} yet. The
          packs appear here once it has; the page asks again every few seconds.
        </p>
      {/if}
      {#if view.overlapWarnings.length > 0}
        <ul class="warnings" data-testid="rp-overlap-warnings" aria-label="Overlap warnings">
          {#each view.overlapWarnings as warning, i (i)}
            <li>{warning}</li>
          {/each}
        </ul>
      {/if}
      {#if view.packs.length > 0}
        <ol class="pack-list">
          {#each view.packs as pack, i (i)}
            <li class="pack-card" data-testid="rp-pack" data-state={pack.rawState} data-pack-id={pack.id ?? ""}>
              <div class="pack-head">
                <span class="pack-pos">#{pack.position + 1}</span>
                <span class="pack-id mono">{pack.id ?? "(no pack read)"}</span>
                {#if pack.version}<span class="pack-version mono">{pack.version}</span>{/if}
                <span class="state-badge tone-{pack.state.tone}" title={pack.state.explain}>{pack.state.label}</span>
              </div>
              <p class="pack-file mono">{pack.file}</p>
              {#if pack.state.explain && pack.state.tone !== "success"}
                <p class="pack-explain">{pack.state.explain}</p>
              {/if}
              {#if pack.lastError}
                <p class="pack-error" data-testid="rp-pack-error">Last error: {pack.lastError}</p>
              {/if}
              {#if pack.warnings.length > 0}
                <ul class="warnings" data-testid="rp-pack-warnings" aria-label="Warnings for {pack.id ?? pack.file}">
                  {#each pack.warnings as warning, w (w)}
                    <li>{warning}</li>
                  {/each}
                </ul>
              {/if}
              <dl class="facts">
                <dt>Publisher</dt>
                <dd>{pack.publisher ?? "—"}</dd>
                <dt>Signature</dt>
                <dd>{pack.keyId ? `verified with key ${pack.keyId}` : "not verified"}</dd>
                <dt>SHA-256</dt>
                <dd class="mono">{pack.sha256 ?? "—"}</dd>
                <dt>Last event</dt>
                <dd>{timeOf(pack.lastEventAt)}</dd>
                {#if pack.license}
                  <dt>Licence</dt>
                  <dd data-testid="rp-license">
                    {pack.license.label || "—"}{pack.license.expires ? `, expires ${pack.license.expires}` : ""}{pack.license.terms ? `; ${pack.license.terms}` : ""}
                  </dd>
                {/if}
              </dl>
            </li>
          {/each}
        </ol>
      {/if}
    </section>

    <section class="block" aria-labelledby="rp-counters-heading" data-testid="rp-counters">
      <h2 id="rp-counters-heading">Counters</h2>
      {#if view.counters === null}
        <p class="empty">This server reports no rule pack counters.</p>
      {:else}
        <p class="section-note">Counted across {scope.isGlobal ? "the server" : "the server (not per pack)"}; the decision log below breaks decisions down per pack.</p>
        <div class="tiles">
          {#each view.counters as counter (counter.name)}
            <div class="tile" data-testid="rp-counter" data-counter={counter.name}>
              <span class="metric-label mono">{counter.name.replace(/^rule_pack_/, "")}</span>
              <span class="metric-value num">{formatCount(counter.value)}</span>
            </div>
          {/each}
        </div>
      {/if}
    </section>

    <section class="block" aria-labelledby="rp-tail-heading" data-testid="rp-tail">
      <h2 id="rp-tail-heading">Decisions</h2>
      <p class="section-note">
        {#if view.log.date}The latest decisions of {view.log.date} (UTC), newest first{:else}The latest decisions, newest first{/if}{view.log.limit > 0 ? `, at most ${formatCount(view.log.limit)}` : ""}.
        <a href={rawHref} data-testid="rp-raw-log">Raw log</a>
      </p>
      {#if tailNote}
        <p class="source-note" data-testid="rp-tail-note">{tailNote}</p>
      {/if}
      <div class="controls control-row">
        {#if tailPacks.length > 1}
          <label class="control-group field-label" data-testid="rp-tail-pack">
            Log of pack
            <select
              class="field"
              value={tailPack ?? ""}
              onchange={(e) => selectTailPack(e.currentTarget.value)}
            >
              {#each tailPacks as id (id)}
                <option value={id}>{id}</option>
              {/each}
            </select>
          </label>
        {/if}
        <label class="control-group field-label">
          Pack
          <select class="field" bind:value={packFilter} data-testid="rp-filter-pack">
            <option value="">All</option>
            {#each packChoices as id (id)}<option value={id}>{id}</option>{/each}
          </select>
        </label>
        <label class="control-group field-label">
          Action
          <select class="field" bind:value={actionFilter} data-testid="rp-filter-action">
            <option value="">All</option>
            {#each actionChoices as a (a)}<option value={a}>{a}</option>{/each}
          </select>
        </label>
        <label class="control-group field-label">
          Reason
          <select class="field" bind:value={reasonFilter} data-testid="rp-filter-reason">
            <option value="">All</option>
            {#each reasonChoices as r (r)}<option value={r}>{r}</option>{/each}
          </select>
        </label>
        <div class="control-group">
          <input
            class="field field-search"
            type="search"
            placeholder="Filter by rule"
            aria-label="Filter by rule"
            data-testid="rp-filter-rule"
            bind:value={ruleFilter}
          />
          <span class="count field-text" data-testid="rp-count">Showing {formatCount(shown.length)} of {formatCount(tailRows.length)}</span>
          {#if filtering}
            <button type="button" class="btn btn-secondary" onclick={clearFilters} data-testid="rp-clear">Clear filters</button>
          {/if}
        </div>
      </div>
      {#if lensHost !== null}
        <p class="section-note" data-testid="rp-lens">Decisions for {lensHost} (host lens).</p>
      {/if}

      {#if tailRows.length === 0 && tailNote === null}
        <EmptyState testid="rp-tail-empty" title="No decisions logged yet" detail="Decisions appear here as pages are evaluated against the loaded packs." />
      {:else if tailRows.length > 0 && shown.length === 0}
        <EmptyState testid="rp-tail-none-match" title="No decisions match the filters" />
      {:else if shown.length > 0}
        <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <div class="table-wrapper" role="region" aria-label="Decision log" tabindex="0">
          <table data-testid="rp-decisions">
            <thead>
              <tr>
                <th scope="col">Time (UTC)</th>
                <th scope="col">Pack</th>
                <th scope="col">Host</th>
                <th scope="col">Path</th>
                <th scope="col">Rule</th>
                <th scope="col">Action</th>
                <th scope="col">Reason</th>
              </tr>
            </thead>
            <tbody>
              {#each shown as row, i (i)}
                <tr data-testid="rp-decision">
                  <td class="mono nowrap">{timeOf(row.at)}</td>
                  <td class="mono">{row.pack_id ?? ""}{row.pack_version ? `@${row.pack_version}` : ""}</td>
                  <td class="mono">{row.host ?? ""}</td>
                  <td class="mono wrap">{row.path ?? ""}</td>
                  <td class="mono">{row.rule ?? ""}</td>
                  <td>{row.action ?? ""}</td>
                  <td>{row.reason ?? ""}{row.defect ? ` (${row.defect})` : ""}{row.claimed_by ? ` by ${row.claimed_by}` : ""}</td>
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
  .scope-note,
  .section-note,
  .source-note {
    margin: 0 0 var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .source-note {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-left: 3px solid var(--ps-border);
    background: var(--ps-bg-secondary);
  }

  .loading,
  .empty {
    color: var(--ps-text-secondary);
  }

  .block {
    margin-bottom: var(--ps-space-xl);
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  .mono {
    font-family: var(--ps-font-mono);
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
    min-width: 0;
  }

  .info-wide {
    flex-basis: 100%;
  }

  .files {
    margin: 0;
    padding-left: 1.4em;
    font-size: var(--ps-font-size-sm);
    overflow-wrap: anywhere;
  }

  .warnings {
    margin: 0 0 var(--ps-space-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm) var(--ps-space-xs) 1.6em;
    border-left: 3px solid var(--ps-warning);
    background: var(--ps-bg-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .pack-list {
    list-style: none;
    margin: 0;
    padding: 0;
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-card-max)), 1fr));
    gap: var(--ps-space-md);
  }

  .pack-card {
    min-width: 0;
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
  }

  .pack-head {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .pack-pos {
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .pack-id {
    font-weight: 700;
    overflow-wrap: anywhere;
  }

  .pack-version {
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .pack-file {
    margin: var(--ps-space-xs) 0;
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
    overflow-wrap: anywhere;
  }

  .pack-explain {
    margin: 0 0 var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .pack-error {
    margin: 0 0 var(--ps-space-xs);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-left: 3px solid var(--ps-error);
    font-size: var(--ps-font-size-sm);
    overflow-wrap: anywhere;
  }

  .state-badge {
    margin-left: auto;
    padding: 2px var(--ps-space-sm);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    font-weight: 700;
  }

  .tone-success {
    background: var(--ps-success-text);
    color: var(--ps-text-inverse);
  }

  .tone-warning {
    background: var(--ps-warning);
    color: var(--ps-warning-contrast-text);
  }

  .tone-error {
    background: var(--ps-error);
    color: var(--ps-text-inverse);
  }

  .tone-neutral {
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }

  .facts {
    display: grid;
    grid-template-columns: max-content minmax(0, 1fr);
    gap: 2px var(--ps-space-md);
    margin: var(--ps-space-sm) 0 0;
    font-size: var(--ps-font-size-sm);
  }

  .facts dt {
    color: var(--ps-text-secondary);
  }

  .facts dd {
    margin: 0;
    overflow-wrap: anywhere;
  }

  .tiles {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-md);
  }

  .tile {
    display: flex;
    flex-direction: column;
    gap: 2px;
    min-width: 9em;
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg-secondary);
  }

  .controls {
    margin-bottom: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border-light);
    border-radius: var(--ps-border-radius);
  }

  .count {
    font-size: var(--ps-font-size-xs);
  }

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

  td {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-bottom: 1px solid var(--ps-border-light);
    vertical-align: top;
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  .nowrap {
    white-space: nowrap;
  }

  .wrap {
    overflow-wrap: anywhere;
  }
</style>
