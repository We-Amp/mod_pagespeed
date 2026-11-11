<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // URL Detail: the answer to "what happened to this URL?" (path + host +
  // scheme), on every console. Hidden route, reached from "Inspect a URL"
  // on Overview and URLs, from the URLs list, a log line and the Cache
  // lookup. The answer has two halves:
  // - the optimizer's (whole-server console): the entry's variants, their
  //   sizes/quality/freshness and image previews, or that it holds nothing,
  //   and a cooldown;
  // - the module's (every console): its cache lookup, a link to the log
  //   entries that name the URL, and the usual reasons a URL is not
  //   optimized.
  // A per-host console shows the module's half as the answer and says in
  // one line what the whole-server console adds.
  //
  // One composite poller (health for the version floor, the entry's
  // alternates, the cooldown list and the module's lookup; the logs are
  // linked, never read here);
  // allSettled never rejects, so a failed leaf shows its explanation
  // instead of a stale-data notice — with one exception: a busy (429)
  // alternates answer is rethrown so the poller's busy rule keeps the last
  // view (the table and the previews stay) instead of blanking the page
  // until the next poll.
  import { usePolling } from "$lib/api/polling.svelte";
  import { isBusyAnswer } from "$lib/api/poller";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";
  import { ApiError, isUnavailable } from "$lib/api/client";
  import UnavailableCard from "$lib/UnavailableCard.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import InspectUrlForm from "$lib/InspectUrlForm.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import SortableHeader from "$lib/SortableHeader.svelte";
  import FormatBadge from "$lib/FormatBadge.svelte";
  import QualityBadge from "$lib/QualityBadge.svelte";
  import ContentClassBadge from "$lib/ContentClassBadge.svelte";
  import QueuedImage from "$lib/QueuedImage.svelte";
  import ImageDiff from "$lib/ImageDiff.svelte";
  import {
    errorCode,
    isDaemonUnavailable,
    normalizeCooldowns,
    optimizerAbsence,
    optimizerUnavailableNote,
    wholeServerConsoleOnlyError,
  } from "$lib/utils/daemon";
  import { formatBytes, formatCount, formatIsoTitle, formatRelative } from "$lib/utils/format";
  import { knownPerVhostScope } from "$lib/utils/config-scope";
  import { MIN_OPTIMIZER_VERSION, optimizerFloor } from "$lib/utils/overview";
  import { computeUrlStatus } from "$lib/utils/url-status";
  import { findMatchingOriginal, formatSavingsPercent } from "$lib/utils/quality";
  import { createLimiter, PREVIEW_CONCURRENCY } from "$lib/utils/image-queue";
  import type { DiffMode } from "$lib/utils/image-diff";
  import { detailHref, displayUrl, globalConsoleHref, parseDetailKey } from "$lib/utils/urls-api";
  import {
    cooldownFor,
    cooldownText,
    lookupIsFinal,
    logsHrefFor,
    moduleLookup,
    urlReasons,
    urlVerdict,
    wayBack,
    wholeServerLine,
  } from "$lib/utils/url-inspect";
  import type { CacheEntryResponse } from "$lib/api/types";
  import {
    DEFAULT_VARIANT_SORT,
    nextVariantSortState,
    originalContentId,
    previewable,
    sentinelLabel,
    sortVariantRows,
    statusLabel,
    toVariantRows,
    type VariantRow,
    type VariantSortState,
  } from "$lib/utils/url-detail";

  const { api, scope } = useConsole();
  const key = parseDetailKey(router.params);
  const shown = key === null ? "" : displayUrl(key);
  // Where the reader came from, as it was when this page opened; kept in
  // this history entry, so returning here with Back keeps the same way back.
  const cameFrom: string = history.state?.urlBack ?? router.previous;
  history.replaceState({ ...history.state, urlBack: cameFrom }, "");
  let back = $derived(wayBack(cameFrom, scope.isGlobal));
  // Every content-leaf image on this page (previews and the diff) loads
  // through this one limiter: never more than the module's two slots.
  const previewLimiter = createLimiter(PREVIEW_CONCURRENCY);

  let sort = $state<VariantSortState>(DEFAULT_VARIANT_SORT);
  let updatedAt = $state<number | null>(null);
  // "W×H" per variant id, measured on load (the daemon exposes byte
  // sizes, not pixel dimensions).
  let dims = $state<Record<string, string>>({});

  // Per-read bookkeeping, kept by the fetcher itself (never an $effect):
  // - a real refusal of the optimizer's record (403 before /config settled
  //   the scope, or the leaf turned off) is final for this page's lifetime:
  //   later reads settle that slot locally, and the module's half keeps
  //   refreshing;
  // - the module runs one cache lookup at a time: a busy answer keeps the
  //   last one, and an answer that cannot change (an original URL, another
  //   host's URL, which is a refusal) is not asked for again.
  let optimizerRefusal: Error | null = null;
  let lastLookup: PromiseSettledResult<CacheEntryResponse> | undefined;
  let finalLookup: PromiseSettledResult<CacheEntryResponse> | undefined;

  const poll = usePolling(
    () => {
      if (key === null) return Promise.resolve(null);
      // The optimizer's record and its cooldown list are whole-server only
      // (403 whole_server_console_only on a per-vhost console). Once
      // /config has settled the scope, asking again would only reproduce
      // the same 403 -- skip the requests and settle the slots locally.
      const perVhost = knownPerVhostScope(scope.config, scope.isGlobal);
      const refusal = optimizerRefusal ?? (perVhost ? wholeServerConsoleOnlyError() : null);
      return Promise.allSettled([
        api.daemonHealth(),
        refusal !== null ? Promise.reject(refusal) : api.daemonCacheAlternates(key),
        refusal !== null ? Promise.reject(refusal) : api.daemonCooldowns(),
        finalLookup === undefined ? api.getCacheEntry(shown) : null,
      ] as const).then((results) => {
        const alternates = results[1];
        if (alternates.status === "rejected" && isBusyAnswer(alternates.reason))
          throw alternates.reason;
        if (
          alternates.status === "rejected" &&
          alternates.reason instanceof Error &&
          (errorCode(alternates.reason) === "whole_server_console_only" || isUnavailable(alternates.reason))
        ) {
          optimizerRefusal = alternates.reason;
        }
        const lookup = finalLookup ?? (results[3] as PromiseSettledResult<CacheEntryResponse>);
        const busy = lookup.status === "rejected" && lookup.reason instanceof Error && isBusyAnswer(lookup.reason);
        if (!busy) lastLookup = lookup;
        if (lookupIsFinal(moduleLookup(lookup).kind)) finalLookup = lookup;
        updatedAt = Date.now();
        return { health: results[0], alternates, cooldowns: results[2], lookup: lastLookup ?? lookup };
      });
    },
    10000,
  );
  if (key === null) poll.stop(); // the misuse state never polls

  let healthData = $derived(
    poll.data?.health.status === "fulfilled" ? poll.data.health.value : null,
  );
  let healthError = $derived<Error | null>(
    poll.data?.health.status === "rejected" && poll.data.health.reason instanceof Error ? poll.data.health.reason : null,
  );
  let altResult = $derived(poll.data?.alternates);
  let altData = $derived(altResult?.status === "fulfilled" ? altResult.value : null);
  let altError = $derived<Error | null>(
    altResult?.status === "rejected" ? altResult.reason : null,
  );
  let belowFloor = $derived(
    healthData !== null && optimizerFloor(healthData.version) === "below",
  );
  let rows = $derived(sortVariantRows(toVariantRows(altData), sort));
  let previews = $derived(rows.filter(previewable));

  // The variant whose before/after diff is open (one at a time).
  let compareId = $state<number | null>(null);
  let diffMode = $state<DiffMode>("slider");

  // The matching original's alternate id for a variant, or null (the
  // variant is itself an original or a sentinel, or no original bytes are
  // cached).
  function originalIdFor(row: VariantRow): number | null {
    if (row.isSentinel || row.format === "original" || row.id === null) return null;
    const original = findMatchingOriginal(row.raw, rows.map((r) => r.raw));
    const id = original?.alternate_id;
    if (typeof id === "number" && Number.isInteger(id) && id >= 0) return id;
    // No same-shaped original entry: compare against the cached original
    // bytes the optimizer keeps as its "original_content" record.
    return originalContentId(rows);
  }

  let compareRow = $derived(previews.find((r) => r.id === compareId) ?? null);
  let compareOriginalId = $derived(compareRow === null ? null : originalIdFor(compareRow));
  // Status over the normalized rows' raw entries (non-objects dropped).
  let status = $derived(computeUrlStatus(rows.map((r) => r.raw)));
  let cooldown = $derived(
    altData !== null && typeof altData.cooldown === "object" && altData.cooldown !== null
      ? altData.cooldown
      : null,
  );

  // The module's half, and what the page says around it.
  let lookup = $derived(moduleLookup(poll.data?.lookup));
  // A URL the optimizer holds nothing for can still be in its cooldown list.
  let listedCooldown = $derived(
    key !== null && poll.data?.cooldowns.status === "fulfilled"
      ? cooldownFor(normalizeCooldowns(poll.data.cooldowns.value), key)
      : null,
  );
  let reasons = $derived(urlReasons(cooldown ?? listedCooldown));
  let perVhost = $derived(errorCode(altError) === "whole_server_console_only");
  // A server without an optimizer: an ordinary state, on either console.
  let noOptimizer = $derived(
    errorCode(altError) === "daemon_not_configured" ||
      ["not-configured", "module-unsupported"].includes(optimizerAbsence(healthError) ?? ""),
  );
  // The optimizer's log is the whole-server console's, and only where an optimizer is connected.
  let optimizerLogShown = $derived(
    altData !== null || (altError !== null && !perVhost && !noOptimizer && !isUnavailable(altError)),
  );
  let wholeServer = $derived(wholeServerLine(noOptimizer));
  let notInIndex = $derived(errorCode(altError) === "not_in_index");
  let verdict = $derived(
    urlVerdict({
      record: altData === null ? null : { rows, status },
      notInIndex,
      access:
        altData !== null || notInIndex
          ? "read"
          : noOptimizer
            ? "none"
            : perVhost
              ? "not-here"
              : altError !== null && (isUnavailable(altError) || isDaemonUnavailable(altError))
                ? "not-answering"
                : "unreadable",
      cooldown: cooldown ?? listedCooldown,
      lookup,
    }),
  );
  // The reasons follow whenever the verdict is not "optimized".
  let reasonsShown = $derived(verdict.kind !== "optimized");
  // This URL's page on the whole-server console (null when the admin path is renamed).
  let globalHref = $derived(key === null ? null : globalConsoleHref(api.basePath, detailHref(key)));

  function onsort(sortKey: VariantSortState["key"]) {
    sort = nextVariantSortState(sort, sortKey);
  }

  function onready(id: number, img: HTMLImageElement) {
    dims = { ...dims, [String(id)]: `${img.naturalWidth}×${img.naturalHeight}` };
  }

  // The preview helpers only run in the loaded branch, where `key` is set.
  function previewSrc(id: number): string {
    return key === null ? "" : api.daemonCacheContentUrl(key, id);
  }
  function previewProbe(id: number): Promise<number> {
    return key === null ? Promise.resolve(0) : api.daemonCacheContentStatus(key, id);
  }

  function altText(row: VariantRow): string {
    return row.format === "original"
      ? `Original variant ${row.id} of ${shown}`
      : `Optimized ${(row.format || "unknown").toUpperCase()} variant ${row.id} of ${shown}`;
  }
</script>

<div class="page">
  <p class="note"><a href={back.href} data-testid="url-back"><span aria-hidden="true">←</span> Back to {back.label}</a></p>
  <PageHeader
    title="URL Detail"
    updatedAt={updatedAt}
    refresh={
      // Without a URL there is nothing to refresh, so no controls at all.
      key === null
        ? undefined
        : {
            autoRefresh: poll.autoRefresh,
            intervalMs: 10000,
            onToggle: () => (poll.autoRefresh ? poll.stop() : poll.start()),
            onRefresh: () => poll.refresh(),
            failedParts: () => [
              healthError,
              altError,
              poll.data?.lookup.status === "rejected" ? poll.data.lookup.reason : null,
            ],
          }
    }
  />

  {#if key === null}
    <div class="empty-state" data-testid="url-detail-misuse">
      <p class="empty-title">No URL to inspect</p>
      <p class="empty-detail">
        This view needs a cached entry's path, host and scheme. Open it from
        the <a href="#/urls">URLs page</a> or the Cache page's lookup.
      </p>
    </div>
    <InspectUrlForm />
  {:else if poll.loading}
    <p class="loading">Looking up this URL...</p>
  {:else if poll.data}
    <p class="detail-url" data-testid="url-detail-url"><code>{shown}</code></p>
    <!-- The answer first, in words; the evidence follows. -->
    <p
      class="health-strip"
      class:health-strip-ok={verdict.kind === "optimized"}
      class:health-strip-info={verdict.kind === "not-yet"}
      data-testid="url-verdict"
      data-kind={verdict.kind}
    ><strong>{verdict.word}</strong>{verdict.text}</p>
    {#if altData}
      {@render optimizerRecord()}
    {:else if altError && !perVhost && !noOptimizer}
      {@render optimizerNote(altError)}
    {/if}
    {#if optimizerLogShown}
      <p class="note optimizer-log" data-testid="url-optimizer-log">
        <a href={logsHrefFor(key, "optimizer")}>Optimizer log entries that name this URL</a>
      </p>
    {/if}

    <div class="answer">
      <section class="card" data-testid="url-module" aria-labelledby="url-module-heading">
        <h2 class="card-title" id="url-module-heading">The module's answer</h2>
        <ul class="facts">
          <li data-testid="url-module-lookup">{lookup.text}</li>
          <li data-testid="url-module-log">
            <a href={logsHrefFor(key, "module")}>Module log entries that name this URL</a>
          </li>
        </ul>
      </section>

      {#if reasonsShown}
        <section class="card" aria-labelledby="url-reasons-heading">
          <h2 class="card-title" id="url-reasons-heading">Usual reasons a URL is not optimized</h2>
          <ul class="reasons" data-testid="url-reasons">
            {#each reasons as reason (reason.id)}
              <li data-testid="url-reason-{reason.id}">
                <strong>{reason.title}.</strong>
                {#if reason.evidence !== null}
                  <strong data-testid="url-reason-evidence">{reason.evidence}</strong>
                {/if}
                {reason.text}
                <a href={reason.doc.href} target="_blank" rel="noopener noreferrer">{reason.doc.label}</a>
              </li>
            {/each}
          </ul>
        </section>
      {/if}
    </div>

    {#if perVhost || noOptimizer}
      <p class="note whole-server" data-testid="url-detail-per-vhost">
        {wholeServer.text}
        {#if wholeServer.link && globalHref !== null}
          Open this URL on the <a href={globalHref}>whole-server console</a>.
        {/if}
      </p>
    {/if}

    {#if altData}
      <details class="raw" data-testid="url-detail-raw">
        <summary>Raw metadata</summary>
        <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
        <pre class="raw-pre" role="region" aria-label="Raw metadata" tabindex="0">{JSON.stringify(altData, null, 2)}</pre>
      </details>
    {/if}
  {/if}
</div>

<!-- Why the optimizer's record of the URL is not shown (the whole-server console's half). -->
{#snippet optimizerNote(altError: Error)}
    {#if isUnavailable(altError)}
      <UnavailableCard testid="url-detail-unavailable" />
    {:else if belowFloor && altError instanceof ApiError && (altError.status === 404 || altError.status === 501)}
      <div class="empty-state" data-testid="url-detail-below-floor">
        <p class="empty-title">The optimizer is too old for this view</p>
        <p class="empty-detail">
          The optimizer is {typeof healthData?.version === "string" ? healthData.version : "an older version"},
          below the minimum {MIN_OPTIMIZER_VERSION} for the URL index.
        </p>
      </div>
    {:else if altError instanceof ApiError && altError.status === 404}
      {#if errorCode(altError) === "not_in_index"}
        <div class="empty-state" data-testid="url-detail-not-found">
          <p class="empty-title">Not in the optimizer's index</p>
          <p class="empty-detail">
            The optimizer holds nothing for <code>{shown}</code>. It records
            URLs as it optimizes them.
          </p>
        </div>
      {:else}
        <div class="empty-state" data-testid="url-detail-no-support">
          <p class="empty-title">The URL index is not available here</p>
          <p class="empty-detail">
            This module version cannot show the optimizer's cached-URL
            index; the rest of the console works as before.
          </p>
        </div>
      {/if}
    {:else if isDaemonUnavailable(altError)}
      <div class="empty-state" data-testid="url-detail-unreachable">
        <p class="empty-title">{optimizerUnavailableNote(altError).title}</p>
        <p class="empty-detail">{optimizerUnavailableNote(altError).detail}</p>
      </div>
    {:else}
      <LoadError error={altError} noun="this URL's variants" onretry={() => poll.refresh()} />
    {/if}
{/snippet}

<!-- What the optimizer holds for the URL: its variants and their previews. -->
{#snippet optimizerRecord()}
    <div class="info-grid card">
      <div class="info-item">
        <span class="metric-label">Status</span>
        <span class="info-value" data-testid="url-detail-status">{statusLabel(status)}</span>
      </div>
      <div class="info-item">
        <span class="metric-label">Variants</span>
        <span class="info-value" data-testid="url-detail-count">{rows.length}</span>
      </div>
    </div>

    {#if cooldown && verdict.kind !== "not-yet"}
      <p class="notice notice-warning cooldown-banner" data-testid="url-detail-cooldown">{cooldownText(cooldown)}</p>
    {/if}

    <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <div class="table-frame variants-frame" role="region" aria-label="Variants" tabindex="0">
      <table data-testid="variants-table" class="data card-table">
        <thead>
          <tr>
            <th scope="col" class="num">ID</th>
            <SortableHeader label="Format" active={sort.key === "format"} ascending={sort.asc} onsort={() => onsort("format")} />
            <th scope="col">Viewport</th>
            <th scope="col">Density</th>
            <th scope="col">Save-data</th>
            <th scope="col">Encoding</th>
            <th scope="col">Type</th>
            <SortableHeader label="Size" active={sort.key === "size"} ascending={sort.asc} onsort={() => onsort("size")} align="right" />
            <SortableHeader label="Quality" active={sort.key === "quality"} ascending={sort.asc} onsort={() => onsort("quality")} align="right" />
            <SortableHeader label="Hits" active={sort.key === "hits"} ascending={sort.asc} onsort={() => onsort("hits")} align="right" />
            <SortableHeader label="Last access" active={sort.key === "lastAccess"} ascending={sort.asc} onsort={() => onsort("lastAccess")} align="right" />
            <th scope="col" class="num">Cached</th>
          </tr>
        </thead>
        <tbody>
          {#each rows as row (row.rowKey)}
            <tr>
              <td class="num" data-label="ID">{row.id ?? "—"}</td>
              <td data-label="Format">
                {#if row.isSentinel}
                  <span class="sentinel">{sentinelLabel(row.sentinelName)}</span>
                {:else}
                  <FormatBadge format={row.format} />
                  {#if row.contentClass}
                    <ContentClassBadge contentClass={row.contentClass} />
                  {/if}
                {/if}
              </td>
              <td data-label="Viewport">{row.viewport || "—"}</td>
              <td data-label="Density">{row.density || "—"}</td>
              <td data-label="Save-data">{row.saveData ? "yes" : "no"}</td>
              <td data-label="Encoding">{row.encoding || "—"}</td>
              <td class="mono" data-label="Type">{row.mimeType}</td>
              <td class="num" data-label="Size">{row.size === null ? "—" : formatBytes(row.size)}</td>
              <td class="num" data-label="Quality">
                {#if row.quality !== null}
                  <QualityBadge score={row.quality} showLabel={false} />
                {:else}
                  —
                {/if}
              </td>
              <td class="num" data-label="Hits">{formatCount(row.hits)}</td>
              <td class="num" data-label="Last access">
                {#if row.lastAccessMs !== null}
                  <time
                    datetime={formatIsoTitle(row.lastAccessMs)}
                    title={formatIsoTitle(row.lastAccessMs)}
                  >{formatRelative(row.lastAccessMs, Date.now())}</time>
                {:else}
                  —
                {/if}
              </td>
              <td class="num" data-label="Cached">
                {#if row.cachedAtMs !== null}
                  <time
                    datetime={formatIsoTitle(row.cachedAtMs)}
                    title={formatIsoTitle(row.cachedAtMs)}
                  >{formatRelative(row.cachedAtMs, Date.now())}</time>
                {:else}
                  —
                {/if}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>

    {#if previews.length > 0}
      <section class="previews-section" aria-labelledby="previews-heading">
        <h2 class="section-title" id="previews-heading">Variant previews</h2>
        <div class="preview-grid" data-testid="variant-previews">
          {#each previews as row (row.rowKey)}
            <figure class="card preview-card" data-testid="preview-{row.id}">
              <QueuedImage
                src={previewSrc(row.id ?? 0)}
                alt={altText(row)}
                limiter={previewLimiter}
                probe={() => previewProbe(row.id ?? 0)}
                onready={(img) => onready(row.id ?? 0, img)}
              />
              <figcaption>
                <FormatBadge format={row.format} />
                {#if row.size !== null}
                  <span class="preview-size">{formatBytes(row.size)}</span>
                {/if}
                {#if row.originalSize !== null && row.format !== "original"}
                  <span class="preview-savings">{formatSavingsPercent(row.originalSize, row.size ?? 0)}</span>
                {/if}
                {#if originalIdFor(row) !== null}
                  <button
                    type="button"
                    class="btn btn-secondary btn-compact compare-button"
                    data-testid="compare-{row.id}"
                    aria-pressed={compareId === row.id}
                    onclick={() => (compareId = compareId === row.id ? null : row.id)}
                  >{compareId === row.id ? "Hide comparison" : "Compare with original"}</button>
                {/if}
                {#if dims[String(row.id)]}
                  <span class="preview-dims">{dims[String(row.id)]}</span>
                {/if}
              </figcaption>
            </figure>
          {/each}
        </div>
        {#if compareRow !== null && compareRow.id !== null && compareOriginalId !== null}
          {#key compareRow.id}
            <div class="card compare-panel" data-testid="compare-panel">
              <h3 class="card-title">Variant {compareRow.id} against its original</h3>
              <div class="compare-modes control-group" role="group" aria-label="Comparison mode">
                <button
                  type="button"
                  class="btn btn-secondary"
                  aria-pressed={diffMode === "slider"}
                  onclick={() => (diffMode = "slider")}
                >Slider</button>
                <button
                  type="button"
                  class="btn btn-secondary"
                  aria-pressed={diffMode === "blink"}
                  onclick={() => (diffMode = "blink")}
                >Blink</button>
              </div>
              <ImageDiff
                beforeSrc={previewSrc(compareOriginalId)}
                afterSrc={previewSrc(compareRow.id)}
                beforeAlt={`Original variant ${compareOriginalId} of ${shown}`}
                afterAlt={altText(compareRow)}
                limiter={previewLimiter}
                beforeProbe={() => previewProbe(compareOriginalId ?? 0)}
                afterProbe={() => previewProbe(compareRow?.id ?? 0)}
                mode={diffMode}
              />
            </div>
          {/key}
        {/if}
      </section>
    {/if}

{/snippet}

<style>
  .previews-section {
    margin-top: var(--ps-space-lg);
  }

  .cooldown-banner {
    margin-top: var(--ps-space-md);
  }

  .variants-frame {
    /* Twelve columns: a tighter step between them keeps the table inside its
       frame on a desktop, with wider glyphs too. */
    --ps-cell-pad-x: var(--ps-space-sm);
    margin-top: var(--ps-space-md);
  }

  /* A plain header may wrap, to keep a twelve-column table in its frame. */
  .variants-frame thead th {
    white-space: normal;
  }

  .raw {
    margin-top: var(--ps-space-sm);
  }

  /* The module's answer and the reasons: side by side where there is room. */
  .answer {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(min(100%, var(--ps-card-min)), 1fr));
    align-items: start;
    gap: var(--ps-space-md);
    margin-top: var(--ps-space-md);
  }

  .facts,
  .reasons {
    margin: 0;
    padding-left: var(--ps-space-lg);
    font-size: var(--ps-font-size-sm);
    line-height: 1.5;
  }

  .facts li + li,
  .reasons li + li {
    margin-top: var(--ps-space-sm);
  }

  .whole-server,
  .optimizer-log {
    margin-top: var(--ps-space-md);
  }

  .detail-url {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }
  .mono {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
  }
  .sentinel {
    font-style: italic;
    color: var(--ps-text-secondary);
  }
  .preview-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: var(--ps-space-md);
  }
  .preview-card {
    margin: 0;
    padding: var(--ps-space-sm);
  }
  .preview-card :global(img) {
    max-width: 100%;
    max-height: 240px;
    height: auto;
    display: block;
    margin: 0 auto;
    /* Transparent pixels need the checkerboard to be visible. */
    background: repeating-conic-gradient(var(--ps-border) 0% 25%, var(--ps-bg-secondary) 0% 50%) 50% / 16px 16px;
  }
  .preview-card figcaption {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
    align-items: center;
    margin-top: var(--ps-space-sm);
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
  }
  .compare-panel {
    margin-top: var(--ps-space-md);
  }
  .compare-modes {
    margin-bottom: var(--ps-space-sm);
  }
  .raw-pre {
    font-size: var(--ps-font-size-xs);
    font-family: var(--ps-font-mono);
    overflow-x: auto;
  }
</style>
