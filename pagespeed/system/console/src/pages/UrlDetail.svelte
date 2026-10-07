<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // URL Detail: what the optimizer holds for one cached entry (path + host
  // + scheme) — its variants, their sizes/quality/freshness, and image
  // previews. Hidden route, reached from the URLs page and from the Cache
  // lookup. One composite poller (health for the version floor + the
  // entry's alternates); allSettled never rejects, so a failed leaf shows
  // its explanation instead of a stale-data notice — with one exception: a
  // busy (429) alternates answer is rethrown so the poller's busy rule
  // keeps the last view (the table and the previews stay) instead of
  // blanking the page until the next poll.
  import { usePolling } from "$lib/api/polling.svelte";
  import { isBusyAnswer } from "$lib/api/poller";
  import { useConsole } from "$lib/api/context";
  import { router } from "$lib/router.svelte";
  import { ApiError } from "$lib/api/client";
  import LoadError from "$lib/LoadError.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import SortableHeader from "$lib/SortableHeader.svelte";
  import FormatBadge from "$lib/FormatBadge.svelte";
  import QualityBadge from "$lib/QualityBadge.svelte";
  import ContentClassBadge from "$lib/ContentClassBadge.svelte";
  import QueuedImage from "$lib/QueuedImage.svelte";
  import ImageDiff from "$lib/ImageDiff.svelte";
  import {
    cooldownReasonLabel,
    daemonUnavailableReason,
    errorCode,
    isDaemonUnavailable,
    wholeServerConsoleOnlyError,
  } from "$lib/utils/daemon";
  import { formatBytes, formatCount, formatIsoTitle, formatRelative } from "$lib/utils/format";
  import { knownPerVhostScope } from "$lib/utils/config-scope";
  import { MIN_OPTIMIZER_VERSION, optimizerFloor } from "$lib/utils/overview";
  import { computeUrlStatus } from "$lib/utils/url-status";
  import { findMatchingOriginal, formatSavingsPercent } from "$lib/utils/quality";
  import { createLimiter, PREVIEW_CONCURRENCY } from "$lib/utils/image-queue";
  import type { DiffMode } from "$lib/utils/image-diff";
  import { displayUrl, globalConsoleUrlsHref, parseDetailKey } from "$lib/utils/urls-api";
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
  // Every content-leaf image on this page (previews and the diff) loads
  // through this one limiter: never more than the module's two slots.
  const previewLimiter = createLimiter(PREVIEW_CONCURRENCY);

  let sort = $state<VariantSortState>(DEFAULT_VARIANT_SORT);
  let updatedAt = $state<number | null>(null);
  // "W×H" per variant id, measured on load (the daemon exposes byte
  // sizes, not pixel dimensions).
  let dims = $state<Record<string, string>>({});

  const poll = usePolling(
    () =>
      key === null
        ? Promise.resolve(null)
        : Promise.allSettled([
            api.daemonHealth(),
            // cache/alternates is whole-server only (403
            // whole_server_console_only on a per-vhost console). Once
            // /config has settled the scope, asking again would only
            // reproduce the same 403 -- skip the request and settle this
            // slot locally instead.
            knownPerVhostScope(scope.config, scope.isGlobal)
              ? Promise.reject(wholeServerConsoleOnlyError())
              : api.daemonCacheAlternates(key),
          ]).then((results) => {
            const alternates = results[1];
            if (alternates.status === "rejected" && isBusyAnswer(alternates.reason))
              throw alternates.reason;
            // A real 403 reached before /config settled the scope (the
            // race on first load): terminal for this page's lifetime --
            // one poller, its own stop(), never a second mechanism -- so
            // it never asks again.
            if (
              alternates.status === "rejected" &&
              alternates.reason instanceof Error &&
              errorCode(alternates.reason) === "whole_server_console_only"
            ) {
              poll.stop();
            }
            updatedAt = Date.now();
            return results;
          }),
    10000,
  );
  if (key === null) poll.stop(); // the misuse state never polls

  let healthData = $derived(
    poll.data?.[0]?.status === "fulfilled" ? poll.data[0].value : null,
  );
  let altResult = $derived(poll.data?.[1]);
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
  let globalHref = $derived(globalConsoleUrlsHref(api.basePath));

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
          }
    }
  />

  {#if key === null}
    <div class="empty-state" data-testid="url-detail-misuse">
      <p class="empty-title">No URL to inspect</p>
      <p class="empty-description">
        This view needs a cached entry's path, host and scheme. Open it from
        the <a href="#/urls">URLs page</a> or the Cache page's lookup.
      </p>
    </div>
  {:else if poll.loading}
    <p class="loading">Loading the cached variants...</p>
  {:else if altError && !altData}
    {#if errorCode(altError) === "whole_server_console_only"}
      <div class="empty-state" data-testid="url-detail-per-vhost">
        <p class="empty-title">Whole-server console only</p>
        <p class="empty-description">
          The URL index is shared by every virtual host on this server, so
          this per-host console does not show it.
          {#if globalHref !== null}
            View it on the <a href={globalHref}>whole-server console</a>.
          {/if}
        </p>
      </div>
    {:else if belowFloor && altError instanceof ApiError && (altError.status === 404 || altError.status === 501)}
      <div class="empty-state" data-testid="url-detail-below-floor">
        <p class="empty-title">The optimizer is too old for this view</p>
        <p class="empty-description">
          The optimizer is {typeof healthData?.version === "string" ? healthData.version : "an older version"},
          below the minimum {MIN_OPTIMIZER_VERSION} for the URL index.
        </p>
      </div>
    {:else if altError instanceof ApiError && altError.status === 404}
      {#if errorCode(altError) === "not_in_index"}
        <div class="empty-state" data-testid="url-detail-not-found">
          <p class="empty-title">Not in the optimizer's index</p>
          <p class="empty-description">
            The optimizer holds nothing for <code>{shown}</code>. It records
            URLs as it optimizes them.
          </p>
        </div>
      {:else}
        <div class="empty-state" data-testid="url-detail-no-support">
          <p class="empty-title">The URL index is not available here</p>
          <p class="empty-description">
            This module version cannot show the optimizer's cached-URL
            index; the rest of the console works as before.
          </p>
        </div>
      {/if}
    {:else if isDaemonUnavailable(altError)}
      <div class="empty-state" data-testid="url-detail-unreachable">
        <p class="empty-title">Daemon Unreachable</p>
        <p class="empty-reason">{daemonUnavailableReason(altError)}</p>
        <p class="empty-description">
          The module could not reach the optimizer daemon
          ({altError.message}). The page populates once it answers.
        </p>
      </div>
    {:else}
      <LoadError message={altError.message} />
    {/if}
  {:else if altData}
    <p class="detail-url" data-testid="url-detail-url"><code>{shown}</code></p>
    <div class="info-grid">
      <div class="info-item">
        <span class="info-label">Status</span>
        <span class="info-value" data-testid="url-detail-status">{statusLabel(status)}</span>
      </div>
      <div class="info-item">
        <span class="info-label">Variants</span>
        <span class="info-value" data-testid="url-detail-count">{rows.length}</span>
      </div>
    </div>

    {#if cooldown}
      {@const reason = cooldownReasonLabel(cooldown.reason)}
      <p class="cooldown-banner" data-testid="url-detail-cooldown">
        In cooldown{#if reason !== null}{` (${reason})`}{/if}{#if typeof cooldown.remaining_seconds === "number" && Number.isFinite(cooldown.remaining_seconds)}{` — ${Math.max(
              0,
              Math.round(cooldown.remaining_seconds),
            )}s remaining`}{/if}.
      </p>
    {/if}

    <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <div class="table-wrapper" role="region" aria-label="Variants" tabindex="0">
      <table data-testid="variants-table" class="card-table">
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
                  <QualityBadge score={row.quality} />
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
        <h2 id="previews-heading">Variant previews</h2>
        <div class="preview-grid" data-testid="variant-previews">
          {#each previews as row (row.rowKey)}
            <figure class="preview-card" data-testid="preview-{row.id}">
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
            <div class="compare-panel" data-testid="compare-panel">
              <h3>Variant {compareRow.id} against its original</h3>
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

    <details class="raw" data-testid="url-detail-raw">
      <summary>Raw metadata</summary>
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <pre class="raw-pre" role="region" aria-label="Raw metadata" tabindex="0">{JSON.stringify(altData, null, 2)}</pre>
    </details>
  {/if}
</div>

<style>
  .previews-section {
    margin-top: var(--ps-space-lg);
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  .info-grid {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
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

  .info-label {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-text-secondary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .info-value {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    color: var(--ps-text);
  }

  .table-wrapper {
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
    position: sticky;
    top: 0;
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
  }

  tbody tr:nth-child(even) {
    background: var(--ps-bg-secondary);
  }

  tbody tr:hover {
    background: var(--ps-surface-hover);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .empty-state {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-lg);
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

  .detail-url {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }
  .mono {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
  }
  th.num,
  td.num {
    text-align: right;
    font-variant-numeric: tabular-nums;
  }
  .sentinel {
    font-style: italic;
    color: var(--ps-text-secondary);
  }
  .cooldown-banner {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-warning);
    border-radius: var(--ps-border-radius);
    background: color-mix(in srgb, var(--ps-warning) 12%, var(--ps-bg));
    color: var(--ps-warning-text);
  }
  .preview-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(220px, 1fr));
    gap: var(--ps-space-md);
  }
  .preview-card {
    margin: 0;
    padding: var(--ps-space-sm);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
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
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
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
