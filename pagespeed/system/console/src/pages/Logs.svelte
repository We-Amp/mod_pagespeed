<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Logs: the module's message log and the optimizer daemon's log ring on
  // one timeline, newest first (timeline.ts), read by ONE composite poll
  // every 5 seconds: message_history from its cursor (mergeMessages) and
  // the optimizer's ring from its cursor with catch-up reads
  // (readLogPages). Each source fails on its own and says so above the
  // list; the poller sees an error only when the module's log cannot be
  // read and nothing is buffered, or when that read is busy.
  //
  // One view keeps the old Optimizer Logs page exactly: the optimizer only
  // with grouping off (source=optimizer&group=0) is the raw stream -- oldest
  // to newest, following the newest entry until the reader scrolls up, a
  // "Jump to newest" that counts what arrived since, the restart/gap/shed
  // markers inline, and a 3-second poll of the optimizer's ring alone. The
  // view is a function of the hash; the source and grouping controls
  // navigate, so the page mounts with its view's one poller.
  //
  // Per-sample bookkeeping (both cursors and buffers) lives INSIDE the
  // fetcher, never in an $effect (polling-discipline.test.ts).
  //
  // The optimizer's log is whole-server only: once /config has settled the
  // scope a per-vhost console never asks for it, and a real 403 before that
  // switches it off for this page's lifetime.
  //
  // Both logs carry visitor-controlled bytes (URLs, user agents): text
  // renders as text nodes; only an http(s) URL that parses becomes a link
  // (linkify.ts), the module's messages and the optimizer's lines alike.
  import { tick } from "svelte";
  import { usePolling } from "$lib/api/polling.svelte";
  import { isBusyAnswer } from "$lib/api/poller";
  import { useConsole } from "$lib/api/context";
  import { ApiError } from "$lib/api/client";
  import { router } from "$lib/router.svelte";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import EmptyState from "$lib/EmptyState.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import RefreshNotice from "$lib/RefreshNotice.svelte";
  import { knownPerVhostScope } from "$lib/utils/config-scope";
  import { daemonUnavailableReason, errorCode, isDaemonUnavailable, wholeServerConsoleOnlyError } from "$lib/utils/daemon";
  import { formatCount, formatIsoTitle } from "$lib/utils/format";
  import { linkSegments } from "$lib/utils/linkify";
  import { hostsInText, textNamesHost } from "$lib/utils/host-lens";
  import {
    EMPTY_LOG_BUFFER,
    countUnseen,
    displayLogText,
    filterLogRows,
    formatLogTimestamp,
    globalConsoleLogsHref,
    readLogPages,
    visibleLogRows,
    type LogBuffer,
  } from "$lib/utils/logs-buffer";
  import { mergeMessages, type MessagesState } from "$lib/utils/message-cursor";
  import { MAX_GROUP_ENTRIES } from "$lib/utils/message-groups";
  import {
    TIMELINE_LEVELS,
    TIMELINE_RENDER_CAP,
    filterEntries,
    groupTimeline,
    isKnownTimelineLevel,
    levelCounts,
    moduleEntries,
    optimizerEntries,
    parseSourceFilter,
    streamNotes,
    timelineLevelFilter,
    type SourceFilter,
  } from "$lib/utils/timeline";

  const { api, scope, lens } = useConsole();

  // Cap on how many module messages are kept client-side across polls.
  const MAX_RETAINED_MESSAGES = 2000;

  // The view, from the hash (presets from a link; the controls below
  // navigate to change them). A per-vhost console shows the module's
  // messages only.
  const source: SourceFilter = scope.isGlobal ? parseSourceFilter(router.params.get("source")) : "module";
  const grouped = router.params.get("group") !== "0";
  const raw = source === "optimizer" && !grouped;
  const POLL_MS = raw ? 3000 : 5000;

  // Both buffers are replaced wholesale by each fold, never deep-proxied.
  let messages: MessagesState = $state.raw({ items: [], next: 0 });
  let optimizer: LogBuffer = $state.raw(EMPTY_LOG_BUFFER);
  let moduleError = $state.raw<Error | null>(null);
  let optimizerError = $state.raw<Error | null>(null);
  // A real whole_server_console_only answer: never ask the optimizer again.
  let optimizerOff = $state(false);
  let updatedAt = $state<number | null>(null);

  // The raw view only (the old page's rules): follow pins the list to the
  // newest entry until the reader scrolls up; unfollowed, the render window
  // stays anchored where the reader left it, and the jump button counts
  // what arrived since.
  let follow = $state(true);
  let anchorId: number | null = $state(null);
  let unseenFrom: number | null = $state(null);
  let logEl: HTMLDivElement | undefined = $state();

  function asError(reason: unknown): Error {
    return reason instanceof Error ? reason : new Error(String(reason));
  }

  const poll = usePolling(async () => {
    const previousNextId = optimizer.nextId;
    const askOptimizer = !optimizerOff && !knownPerVhostScope(scope.config, scope.isGlobal);
    const [mod, opt] = await Promise.allSettled([
      // The raw view reads the optimizer's ring alone, as the old page did.
      raw ? Promise.resolve(null) : api.getMessages(messages.next || undefined),
      askOptimizer
        ? readLogPages((since) => api.daemonLogs(since), optimizer)
        : Promise.reject(wholeServerConsoleOnlyError()),
    ]);
    if (mod.status === "fulfilled") {
      if (mod.value !== null) {
        messages = mergeMessages(messages, mod.value, MAX_RETAINED_MESSAGES);
        // Hosts named in new lines feed the lens (the lowest rank: anyone can
        // put a URL into a log line).
        lens.observe((mod.value.messages ?? []).flatMap((m) => (typeof m?.message === "string" ? hostsInText(m.message) : [])), 2);
      }
      moduleError = null;
    } else if (!isBusyAnswer(asError(mod.reason))) {
      moduleError = asError(mod.reason);
    }
    if (opt.status === "fulfilled") {
      optimizer = opt.value.state;
      lens.observe(
        opt.value.state.rows.flatMap((row) => (row.kind === "entry" && row.id >= previousNextId ? hostsInText(row.message) : [])),
        2,
      );
      optimizerError = null;
      // Pin after Svelte renders the fold: a fetch-completion DOM write, not
      // an $effect (the old page's rule).
      if (raw && follow) void pinToNewestWhenRendered();
    } else {
      const err = asError(opt.reason);
      if (errorCode(err) === "whole_server_console_only") optimizerOff = true;
      if (!isBusyAnswer(err)) optimizerError = err;
      // The raw view's one source is busy: the poller's one-second retry,
      // with the current view kept.
      if (raw && isBusyAnswer(err)) throw err;
    }
    if (mod.status === "rejected") {
      const err = asError(mod.reason);
      // The poller's rules apply to the page's primary source: a busy read
      // retries soon; a failure with nothing at all to show is the page's.
      if (isBusyAnswer(err)) throw err;
      if (messages.items.length === 0 && optimizer.rows.length === 0) throw err;
    }
    updatedAt = Date.now();
    return updatedAt;
  }, POLL_MS);

  async function pinToNewestWhenRendered(attempts = 3): Promise<void> {
    // On the very first page the list mounts only once the poller's own
    // "loading" flag flips, a later microtask than this fetcher's return:
    // retry across a few ticks (the old page's rule).
    for (let i = 0; i < attempts; i++) {
      await tick();
      if (!follow) return;
      if (logEl) {
        logEl.scrollTop = logEl.scrollHeight;
        return;
      }
    }
  }

  // The level filter is a preset from the link (level=) and the operator's
  // to change from there; the raw view shows every level unless the link
  // names one, as the old page did.
  const levelParam = router.params.get("level");
  const levelKnown = isKnownTimelineLevel(levelParam);
  let enabledLevels = $state(timelineLevelFilter(raw && levelParam === null ? "debug" : levelParam));
  let text = $state("");
  let expanded = $state<Record<string, boolean>>({});

  let perVhost = $derived(optimizerOff || knownPerVhostScope(scope.config, scope.isGlobal));
  let lensHost = $derived(activeLensHost(scope, lens));
  let allEntries = $derived([...moduleEntries(messages.items), ...optimizerEntries(optimizer.rows)]);
  let sourceEntries = $derived(source === "both" ? allEntries : allEntries.filter((e) => e.source === source));
  let counts = $derived(levelCounts(sourceEntries));
  let filtered = $derived(filterEntries(allEntries, { source, levels: enabledLevels, text, host: lensHost }));
  let rows = $derived(groupTimeline(filtered, grouped));
  let visibleRows = $derived(rows.slice(0, TIMELINE_RENDER_CAP));
  let notes = $derived(streamNotes(optimizer.rows));
  let globalHref = $derived(globalConsoleLogsHref(api.basePath, levelParam));
  // The raw view's rows: the optimizer's buffer in arrival order, markers kept.
  let rawRows = $derived(
    filterLogRows(optimizer.rows, enabledLevels, text).filter(
      // Markers stay (they say where the stream broke); entries must name the lens host.
      (row) => lensHost === null || row.kind !== "entry" || textNamesHost(row.message, lensHost),
    ),
  );
  let rawVisible = $derived(visibleLogRows(rawRows, follow ? null : anchorId));
  let unseen = $derived(countUnseen(rawRows, follow ? null : unseenFrom));

  const SOURCES: ReadonlyArray<{ value: SourceFilter; label: string }> = [
    { value: "both", label: "Both" },
    { value: "module", label: "Module" },
    { value: "optimizer", label: "Optimizer" },
  ];

  /** This view at another level: the source and the grouping kept (the raw view stays raw). */
  function levelHref(level: string): string {
    const params = new URLSearchParams({ source, level });
    if (!grouped) params.set("group", "0");
    return `#/logs?${params.toString()}`;
  }

  /** Another view of this page: the link's level kept, the source and the grouping as asked. */
  function viewHref(nextSource: SourceFilter, nextGrouped: boolean): string {
    const params = new URLSearchParams();
    if (levelParam !== null) params.set("level", levelParam);
    if (scope.isGlobal && nextSource !== "both") params.set("source", nextSource);
    if (!nextGrouped) params.set("group", "0");
    const query = params.toString();
    return query === "" ? "#/logs" : `#/logs?${query}`;
  }

  function onLogScroll() {
    if (!logEl) return;
    const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 40;
    if (atBottom === follow) return;
    if (atBottom) {
      follow = true;
      anchorId = null;
      unseenFrom = null;
    } else {
      anchorId = rawVisible.length > 0 ? rawVisible[0].id : null;
      unseenFrom = optimizer.nextId;
      follow = false;
    }
  }

  function jumpToNewest() {
    follow = true;
    anchorId = null;
    unseenFrom = null;
    void tick().then(() => logEl?.scrollTo({ top: logEl.scrollHeight }));
  }

  function levelBadgeClass(level: string): string {
    return isKnownTimelineLevel(level) ? `level-badge-${level}` : "level-badge-debug";
  }

  function entriesText(n: number): string {
    return n === 1 ? "1 entry" : `${formatCount(n)} entries`;
  }

  function expandLabel(count: number): string {
    return count > MAX_GROUP_ENTRIES ? `Show the newest ${MAX_GROUP_ENTRIES} entries` : `Show ${count} entries`;
  }

  function toggle(key: string) {
    expanded = { ...expanded, [key]: !expanded[key] };
  }
</script>

<!-- Log text is visitor-influenced: every segment renders as a text node;
     only an http(s) URL that parses becomes a link (linkify.ts). Kept on one
     line: the text keeps its whitespace (pre-wrap). -->
{#snippet linked(value: string)}{#each linkSegments(value) as segment, i (i)}{#if segment.kind === "link"}<a class="log-link" href={segment.href} target="_blank" rel="noopener noreferrer nofollow" referrerpolicy="no-referrer">{segment.text}</a>{:else}{segment.text}{/if}{/each}{/snippet}

<div class="page">
  <PageHeader
    title="Logs"
    {updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: POLL_MS,
      onToggle: () => (poll.autoRefresh ? poll.stop() : poll.start()),
      onRefresh: () => poll.refresh(),
    }}
  />

  <p class="scope-note" data-testid="logs-scope-note">
    {#if raw}
      The optimizer daemon's own recent log, oldest to newest; the list follows
      the newest entry until you scroll up.
    {:else}
      The module's messages (every virtual host of this server) and the
      optimizer daemon's own recent log, newest first.
    {/if}
  </p>

  {#if lensHost !== null}
    <p class="filter-note" data-testid="logs-lens">
      Entries that name {lensHost} (host lens); entries that name no host are hidden while the lens is on.
    </p>
  {/if}

  {#if levelParam}
    <p class="filter-note" data-testid="logs-filter-note">
      {#if levelKnown}
        Showing {levelParam} entries and anything more severe.
      {:else}
        Showing all entries.
      {/if}
      <a href={levelHref("debug")}>Show all</a>
    </p>
  {/if}

  {#if poll.loading}
    <p class="loading">Loading logs...</p>
  {:else if poll.error && allEntries.length === 0}
    <LoadError message={poll.error.message} />
  {:else}
    <RefreshNotice error={poll.error} />

    {#if moduleError && source !== "optimizer"}
      <p class="source-note" data-testid="logs-module-error">The module's messages could not be read: {moduleError.message}</p>
    {/if}
    {#if perVhost}
      <p class="source-note" data-testid="logs-per-vhost">
        The optimizer's log covers every virtual host on this server, so this
        per-host console shows the module's messages only.
        {#if globalHref !== null}
          See both on the <a href={globalHref}>whole-server console</a>.
        {/if}
      </p>
    {:else if source !== "module" && optimizerError}
      {#if optimizerError instanceof ApiError && optimizerError.status === 404}
        <p class="source-note" data-testid="logs-no-support">
          This module version cannot show the optimizer's log; the module's messages are below.
        </p>
      {:else if optimizerError instanceof ApiError && optimizerError.status === 501}
        <p class="source-note" data-testid="logs-no-support">
          This optimizer version does not provide logs. Update the optimizer package.
        </p>
      {:else if errorCode(optimizerError) === "response_too_large"}
        <p class="source-note" data-testid="logs-too-large">
          The optimizer answered with a log page larger than the console accepts. The optimizer
          itself is running; the page asks again on the next refresh.
        </p>
      {:else if isDaemonUnavailable(optimizerError)}
        <p class="source-note" data-testid="logs-unreachable">
          The optimizer's log: optimizer unreachable ({daemonUnavailableReason(optimizerError)}).
          Its entries appear here once it answers.
        </p>
      {:else}
        <p class="source-note" data-testid="logs-optimizer-error">The optimizer's log could not be read: {optimizerError.message}</p>
      {/if}
    {/if}
    {#if source !== "module" && !raw}
      {#if notes.restarts > 0}
        <p class="source-note" data-testid="logs-restart">
          The optimizer restarted {notes.restarts === 1 ? "once" : `${formatCount(notes.restarts)} times`} while this
          page was open; its earlier entries are kept.
        </p>
      {/if}
      {#if notes.dropped > 0}
        <p class="source-note" data-testid="logs-gap">
          {entriesText(notes.dropped)} dropped: the optimizer logged faster than this page reads,
          and its log ring moved on.
        </p>
      {/if}
      {#if notes.notRecorded > 0}
        <p class="source-note" data-testid="logs-shed">
          {entriesText(notes.notRecorded)} not recorded: the optimizer was logging faster than it could keep.
        </p>
      {/if}
    {/if}

    <div class="controls control-row">
      {#if scope.isGlobal}
        <fieldset class="choice control-group" data-testid="logs-source-filter">
          <legend>Source</legend>
          {#each SOURCES as option (option.value)}
            <label class="field-check">
              <input
                type="radio"
                name="logs-source"
                value={option.value}
                checked={source === option.value}
                onchange={() => router.navigate(viewHref(option.value, grouped))}
              />
              {option.label}
            </label>
          {/each}
        </fieldset>
      {/if}
      <fieldset class="choice control-group" data-testid="logs-level-filter">
        <legend>Level</legend>
        {#each TIMELINE_LEVELS as level (level)}
          <label class="field-check">
            <input type="checkbox" bind:checked={enabledLevels[level]} />
            <span class="level-badge level-badge-{level}">{level} ({formatCount(counts[level])})</span>
          </label>
        {/each}
      </fieldset>
      <label class="toggle field-check">
        <input type="checkbox" checked={grouped} onchange={() => router.navigate(viewHref(source, !grouped))} />
        Group repeats
      </label>
      <div class="control-group">
      <input
        class="field field-search"
        type="search"
        placeholder="Filter the entries"
        aria-label="Filter the entries"
        data-testid="logs-search"
        bind:value={text}
      />
      <span class="count field-text">Showing {formatCount(filtered.length)} of {formatCount(sourceEntries.length)}</span>
      </div>
    </div>

    {#if allEntries.length === 0 && raw && optimizerError}
      <!-- The raw view reads the optimizer only: the note above says why it is empty. -->
    {:else if allEntries.length === 0}
      <EmptyState
        testid="logs-empty"
        title="No log entries yet"
        detail="Nothing has been logged since the server and the optimizer started; entries appear here as they happen."
      />
    {:else if rows.length === 0}
      <EmptyState
        testid="logs-none-match"
        title="No entries match the filters"
        detail={enabledLevels.info ? "" : "Info and debug entries are hidden; include them to see everything."}
        action={{ label: "Show all levels", href: levelHref("debug") }}
      />
    {:else if raw}
      <!-- The raw optimizer stream, as the old Optimizer Logs page showed it:
           oldest to newest, following the newest entry until the reader
           scrolls up, a marker where the stream broke. A scrolling region
           must take keyboard focus to be scrollable without a mouse (WCAG
           2.1.1); role="log" is not interactive, so Svelte's tabindex check
           does not know that. -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div
        class="timeline log-stream"
        role="log"
        aria-live="off"
        aria-label="Optimizer log entries"
        tabindex="0"
        data-testid="logs-timeline"
        bind:this={logEl}
        onscroll={onLogScroll}
      >
        {#each rawVisible as row (row.id)}
          {#if row.kind === "gap"}
            <div class="log-marker" data-testid="logs-gap">
              {entriesText(Math.max(0, row.toSeq - row.fromSeq - 1))} dropped: the optimizer logged faster than this
              page reads, and its log ring moved on.
            </div>
          {:else if row.kind === "restart"}
            <div class="log-marker" data-testid="logs-restart">
              The optimizer restarted; the entries above are from before the restart.
            </div>
          {:else if row.kind === "shed"}
            <div class="log-marker" data-testid="logs-shed">
              {entriesText(row.count)} not recorded: the optimizer was logging faster than it could keep.
            </div>
          {:else}
            <div class="log-line" data-testid="log-group" data-source="optimizer">
              <span class="log-time">{formatLogTimestamp(row.ts)}</span>
              <span class="log-level level-badge {levelBadgeClass(row.level)}">{row.level}</span>
              {#if row.source || row.module}
                <span class="log-where">{displayLogText(row.source)}{row.source && row.module ? "/" : ""}{displayLogText(row.module)}</span>
              {/if}
              <span class="log-text">{@render linked(displayLogText(row.message))}</span>
            </div>
          {/if}
        {/each}
      </div>
      {#if rawRows.length > rawVisible.length}
        <p class="section-note" data-testid="logs-render-cap">
          Showing {formatCount(rawVisible.length)} of {formatCount(rawRows.length)} rows; narrow the filters, or jump to
          the newest, to see others.
        </p>
      {/if}
      {#if !follow}
        <button type="button" class="btn btn-primary btn-jump" data-testid="logs-jump" onclick={jumpToNewest}>
          Jump to newest{unseen > 0 ? ` (${formatCount(unseen)} new)` : ""}
        </button>
      {/if}
    {:else}
      <!-- A scrolling region must take keyboard focus to be scrollable
           without a mouse (WCAG 2.1.1). -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div class="timeline" data-testid="logs-timeline" role="region" aria-label="Log entries" tabindex="0">
        {#each visibleRows as row (row.key)}
          <div class="log-row log-row--{isKnownTimelineLevel(row.level) ? row.level : 'debug'}" data-testid="log-group" data-source={row.source}>
            <div class="log-meta">
              <span class="source-badge">{row.source === "module" ? "Module" : "Optimizer"}</span>
              <span class="level-badge {levelBadgeClass(row.level)}">{row.level}</span>
              {#if row.lastTs !== null}
                <time class="log-time" datetime={formatIsoTitle(row.lastTs)} title={formatIsoTitle(row.lastTs)}>{formatLogTimestamp(row.lastTs)}</time>
              {/if}
              {#if row.count > 1}
                <span class="log-repeats" data-testid="log-repeats">×{formatCount(row.count)}</span>
              {/if}
              {#if row.where}
                <span class="log-where">{row.where}</span>
              {/if}
            </div>
            <div class="log-text">{@render linked(row.text)}</div>
            {#if row.count > 1}
              <button
                type="button"
                class="log-expand"
                aria-expanded={expanded[row.key] === true}
                onclick={() => toggle(row.key)}
              >{expanded[row.key] ? "Hide entries" : expandLabel(row.count)}</button>
              {#if expanded[row.key]}
                <ol class="log-entries" data-testid="log-entries">
                  {#each row.entries as entry, i (i)}
                    <li>
                      <span class="entry-time">{entry.ts === null ? "—" : formatLogTimestamp(entry.ts)}</span>
                      <span class="entry-text">{@render linked(entry.text)}</span>
                    </li>
                  {/each}
                </ol>
              {/if}
            {/if}
          </div>
        {/each}
      </div>
      {#if rows.length > visibleRows.length}
        <p class="section-note" data-testid="logs-render-cap">
          Showing the newest {formatCount(visibleRows.length)} of {formatCount(rows.length)} rows; narrow the filters to see others.
        </p>
      {/if}
    {/if}
  {/if}
</div>

<style>
  .scope-note,
  .filter-note,
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

  .loading {
    color: var(--ps-text-secondary);
  }

  .controls {
    margin-bottom: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border: 1px solid var(--ps-border-light);
    border-radius: var(--ps-border-radius);
  }

  .choice {
    margin: 0;
    padding: 0;
    border: none;
  }

  .choice legend {
    float: left;
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-text-secondary);
  }

  .choice label,
  .toggle {
    color: var(--ps-text);
  }

  .count {
    font-size: var(--ps-font-size-xs);
  }

  .timeline {
    display: flex;
    flex-direction: column;
    gap: 1px;
    max-height: 70vh;
    overflow-y: auto;
    overflow-x: hidden;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
  }

  .timeline:focus-visible {
    outline: 2px solid var(--ps-primary);
    outline-offset: 2px;
  }

  /* The raw optimizer stream keeps the old Optimizer Logs page's line layout. */
  .log-stream {
    display: block;
    max-height: 60vh;
    padding: var(--ps-space-xs) 0;
    background: var(--ps-bg-secondary);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .log-line {
    display: flex;
    align-items: baseline;
    gap: var(--ps-space-sm);
    padding: 1px var(--ps-space-sm);
    /* A visible boundary between rows, so a wrapped continuation line can
       never be mistaken for the start of another entry. */
    border-bottom: 1px solid var(--ps-border);
  }

  .log-line .log-time,
  .log-line .log-level,
  .log-line .log-where {
    flex-shrink: 0;
    white-space: nowrap;
  }

  .log-line .log-level {
    font-weight: 600;
    min-width: 5.5em;
  }

  .log-line .log-text {
    flex: 1;
  }

  .log-marker {
    text-align: center;
    color: var(--ps-text-secondary);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-top: 1px dashed var(--ps-border);
    border-bottom: 1px dashed var(--ps-border);
    margin: var(--ps-space-xs) 0;
  }

  .btn-jump {
    margin-top: var(--ps-space-sm);
  }

  .log-row {
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: var(--ps-bg);
    border-left: 3px solid transparent;
  }

  .log-row:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  .log-row--fatal {
    border-left-color: var(--ps-severity-fatal-bg);
  }

  .log-row--error {
    border-left-color: var(--ps-error);
  }

  .log-row--warning {
    border-left-color: var(--ps-warning);
  }

  .log-row--info {
    border-left-color: var(--ps-primary);
  }

  .log-meta {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
    margin-bottom: var(--ps-space-xs);
  }

  .source-badge {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    padding: 0 var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: 999px;
    color: var(--ps-text-secondary);
  }

  .log-time,
  .log-repeats,
  .log-where,
  .entry-time {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }

  /* Long lines (URLs up to 4 KiB) wrap instead of being cut off. */
  .log-text,
  .entry-text {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    line-height: 1.4;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    min-width: 0;
  }

  .log-link {
    color: var(--ps-primary);
  }

  .log-expand {
    margin-top: var(--ps-space-xs);
    padding: 0;
    border: none;
    background: none;
    color: var(--ps-primary);
    font-size: var(--ps-font-size-xs);
    text-decoration: underline;
    cursor: pointer;
  }

  .log-entries {
    margin: var(--ps-space-xs) 0 0;
    padding-left: var(--ps-space-lg);
    display: grid;
    gap: 2px;
  }

  .log-entries li {
    display: flex;
    gap: var(--ps-space-sm);
    min-width: 0;
  }

  @media (max-width: 600px) {
    .controls {
      flex-direction: column;
      align-items: flex-start;
    }

    .count {
      margin-left: 0;
    }

    .log-entries li {
      flex-wrap: wrap;
    }

    .log-line {
      flex-wrap: wrap;
    }
  }
</style>
