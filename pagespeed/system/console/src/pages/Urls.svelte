<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // URLs: the optimizer's cached-URL index, one page at a time — the
  // index-level answer to "what happened to URL X?". Whole-server console
  // only: the module gates the leaves (403 on a per-vhost console) and a
  // module without them answers 404; both render as explanations.
  //
  // One composite poller (health + cooldowns + the urls page); the page
  // keys its state off the urls answer. allSettled never rejects, so a
  // failed leaf shows its explanation instead of a stale-data notice — with
  // one exception: a busy (429) urls answer is rethrown so the poller's
  // busy rule applies (keep the last view, retry; poller.ts), rather than
  // blanking the table behind an error until the next poll.
  import { onDestroy } from "svelte";
  import { usePolling } from "$lib/api/polling.svelte";
  import { activeLensHost } from "$lib/host-lens.svelte";
  import { TERMS } from "$lib/data/terms";
  import { isBusyAnswer } from "$lib/api/poller";
  import { useConsole } from "$lib/api/context";
  import { ApiError, isUnavailable } from "$lib/api/client";
  import UnavailableCard from "$lib/UnavailableCard.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import InspectUrlForm from "$lib/InspectUrlForm.svelte";
  import PageHeader from "$lib/PageHeader.svelte";
  import SortableHeader from "$lib/SortableHeader.svelte";
  import { formatCount } from "$lib/utils/format";
  import {
    cooldownReasonLabel,
    errorCode,
    isDaemonUnavailable,
    optimizerUnavailableNote,
    normalizeCooldowns,
    wholeServerConsoleOnlyError,
  } from "$lib/utils/daemon";
  import { knownPerVhostScope } from "$lib/utils/config-scope";
  import { MIN_OPTIMIZER_VERSION, optimizerFloor } from "$lib/utils/overview";
  import {
    URLS_PAGE_SIZE,
    buildUrlRows,
    detailHref,
    globalConsoleUrlsHref,
    nextUrlSortState,
    pageWindow,
    urlsViewFromParams,
    urlsViewParams,
    type UrlSortState,
  } from "$lib/utils/urls-api";
  import { router } from "$lib/router.svelte";
  import { replaceViewParams } from "$lib/utils/view-address";

  const { api, scope, lens } = useConsole();

  // The page, filter and sort live in the address too (written in place),
  // so a URL's page can send the reader back to the same view.
  const linked = urlsViewFromParams(router.params);
  let offset = $state(linked.offset);
  let search = $state(linked.q);
  let sort = $state<UrlSortState>(linked.sort);
  const remember = () => replaceViewParams(urlsViewParams({ offset, q: search, sort }));
  let updatedAt = $state<number | null>(null);
  let lensHost = $derived(activeLensHost(scope, lens));

  const poll = usePolling(
    () =>
      Promise.allSettled([
        api.daemonHealth(),
        api.daemonCooldowns(),
        // cache/urls is whole-server only (403 whole_server_console_only on
        // a per-vhost console). Once /config has settled the scope, asking
        // again would only reproduce the same 403 -- skip the request and
        // settle this slot locally instead.
        knownPerVhostScope(scope.config, scope.isGlobal)
          ? Promise.reject(wholeServerConsoleOnlyError())
          : api.daemonCacheUrls(offset, URLS_PAGE_SIZE, activeLensHost(scope, lens) ?? undefined),
      ]).then((results) => {
        const urls = results[2];
        // The index's hosts feed the lens (served or indexed by the optimizer).
        if (urls.status === "fulfilled" && Array.isArray(urls.value?.urls)) {
          lens.observe(urls.value.urls.map((entry) => entry?.hostname), 1);
        }
        if (urls.status === "rejected" && isBusyAnswer(urls.reason)) throw urls.reason;
        // A real 403 reached before /config settled the scope (the race on
        // first load): terminal for this page's lifetime -- one poller, its
        // own stop(), never a second mechanism -- so it never asks again.
        if (
          urls.status === "rejected" &&
          urls.reason instanceof Error &&
          (errorCode(urls.reason) === "whole_server_console_only" || isUnavailable(urls.reason))
        ) {
          poll.stop();
        }
        updatedAt = Date.now();
        return results;
      }),
    10000,
  );

  // A new lens host starts the index again from its first page.
  onDestroy(
    lens.onChange(() => {
      offset = 0;
      remember();
      poll.invalidate();
    }),
  );

  let urlsResult = $derived(poll.data?.[2]);
  let urlsData = $derived(
    urlsResult?.status === "fulfilled" ? urlsResult.value : null,
  );
  let urlsError = $derived<Error | null>(
    urlsResult?.status === "rejected" ? urlsResult.reason : null,
  );
  let healthData = $derived(
    poll.data?.[0]?.status === "fulfilled" ? poll.data[0].value : null,
  );
  let cooldowns = $derived(
    normalizeCooldowns(
      poll.data?.[1]?.status === "fulfilled" ? poll.data[1].value : null,
    ),
  );

  // Rows of the current page, filtered and sorted CLIENT-SIDE (server-side
  // filter/sort does not exist); the note under the table says so. Under a
  // lens only that host's rows: the previous host's page, still shown until
  // the new answer arrives, never appears under the new lens.
  let rows = $derived(
    buildUrlRows(urlsData ?? undefined, cooldowns, search, sort, lensHost),
  );
  let pageCount = $derived(
    urlsData && Array.isArray(urlsData.urls) ? urlsData.urls.length : 0,
  );
  // The window spans the rows actually shown: entries the row builder drops
  // as garbage do not count toward it.
  let win = $derived(pageWindow(urlsData ?? undefined, offset, rows.length));

  // The next page starts where the daemon says it does: its pages need not
  // hold URLS_PAGE_SIZE rows. offset + URLS_PAGE_SIZE when it says nothing.
  let nextOffset = $derived.by(() => {
    const raw = urlsData?.next_offset;
    return typeof raw === "number" && Number.isInteger(raw) && raw > offset
      ? raw
      : offset + URLS_PAGE_SIZE;
  });

  let belowFloor = $derived(
    healthData !== null && optimizerFloor(healthData.version) === "below",
  );
  let globalHref = $derived(globalConsoleUrlsHref(api.basePath));

  function goToPage(requested: number) {
    offset = Math.max(0, requested);
    remember();
    poll.invalidate();
  }

  function onsort(key: UrlSortState["key"]) {
    sort = nextUrlSortState(sort, key);
    remember();
  }
</script>

<div class="page">
  <PageHeader
    title="URLs"
    updatedAt={updatedAt}
    refresh={{
      autoRefresh: poll.autoRefresh,
      intervalMs: 10000,
      onToggle: () => (poll.autoRefresh ? poll.stop() : poll.start()),
      onRefresh: () => poll.refresh(),
      failedParts: () => [urlsError],
    }}
  >
    {#snippet toolbar()}
      <input
        class="field field-search"
        type="search"
        placeholder="Filter this page by URL or host"
        aria-label="Filter this page by URL or host"
        data-testid="urls-search"
        value={search}
        oninput={(e) => {
          search = e.currentTarget.value;
          remember();
        }}
      />
    {/snippet}
  </PageHeader>

  {#if lensHost !== null}
    <p class="note" data-testid="urls-lens">{TERMS.hostFilter}: URLs of {lensHost} only.</p>
  {/if}

  <!-- A URL goes straight to its page: the list below shows one page of the index at a time. -->
  <InspectUrlForm />

  {#if poll.loading}
    <p class="loading">Loading the URL index...</p>
  {:else if urlsError && !urlsData}
    {#if errorCode(urlsError) === "whole_server_console_only"}
      <div class="empty-state" data-testid="urls-per-vhost">
        <p class="empty-title">Whole-server console only</p>
        <p class="empty-detail">
          The URL index is shared by every virtual host on this server, so
          this per-host console does not show it.
          {#if globalHref !== null}
            View it on the <a href={globalHref}>whole-server console</a>.
          {/if}
        </p>
      </div>
    {:else if isUnavailable(urlsError)}
      <UnavailableCard testid="urls-unavailable" />
    {:else if urlsError instanceof ApiError && (urlsError.status === 404 || urlsError.status === 501)}
      <div class="empty-state" data-testid="urls-no-support">
        <p class="empty-title">The URL index is not available here</p>
        <p class="empty-detail">
          {#if urlsError.status === 404}
            This module version cannot show the optimizer's cached-URL
            index; the rest of the console works as before.
          {:else}
            The connected optimizer does not serve the cached-URL index.
          {/if}
          {#if belowFloor && typeof healthData?.version === "string"}
            The optimizer is {healthData.version}, below the minimum
            {MIN_OPTIMIZER_VERSION} for the index endpoints.
          {/if}
        </p>
      </div>
    {:else if isDaemonUnavailable(urlsError)}
      <div class="empty-state" data-testid="urls-unreachable">
        <p class="empty-title">{optimizerUnavailableNote(urlsError).title}</p>
        <p class="empty-detail">{optimizerUnavailableNote(urlsError).detail}</p>
      </div>
    {:else}
      <LoadError error={urlsError} noun="the URL list" onretry={() => poll.refresh()} />
    {/if}
  {:else if urlsData}
    {#if pageCount === 0}
      <div class="empty-state" data-testid="urls-empty">
        <p class="empty-title">No URLs recorded yet</p>
        <p class="empty-detail">
          The optimizer records URLs here as it optimizes them.
        </p>
      </div>
    {:else}
      <!-- A table that may scroll sideways is a named region that takes keyboard focus (WCAG 2.1.1). -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div class="table-frame reads-across-container" role="region" aria-label="URL index" tabindex="0">
        <table data-testid="urls-table" class="data card-table reads-across">
          <thead>
            <tr>
              <SortableHeader label="URL" active={sort.key === "url"} ascending={sort.asc} onsort={() => onsort("url")} />
              <SortableHeader label="Host" active={sort.key === "host"} ascending={sort.asc} onsort={() => onsort("host")} />
              <SortableHeader label="Variants" active={sort.key === "variants"} ascending={sort.asc} onsort={() => onsort("variants")} align="right" />
              <th scope="col">Cooldown</th>
            </tr>
          </thead>
          <tbody>
            {#each rows as row (row.rowKey)}
              <tr>
                <th scope="row" class="url-cell" data-label="URL">
                  {#if row.key !== null}
                    <a class="url-text" href={detailHref(row.key)}>{row.display}</a>
                  {:else}
                    {row.display}
                    <span class="url-note">(cannot be opened: incomplete entry)</span>
                  {/if}
                </th>
                <td data-label="Host">{row.host}</td>
                <td class="num" data-label="Variants">{formatCount(row.variants)}</td>
                <td data-label="Cooldown">
                  {#if row.cooldown}
                    <span class="badge badge-tint-warning">{cooldownReasonLabel(row.cooldown.reason) ?? "In cooldown"}</span>
                  {:else}
                    —
                  {/if}
                </td>
              </tr>
            {/each}
          </tbody>
        </table>
      </div>
      {#if rows.length === 0}
        <p class="muted no-match" data-testid="urls-no-match">No URLs on this page match the filter.</p>
      {/if}
      <div class="pager control-group" data-testid="urls-pager">
        <button class="btn btn-secondary" data-testid="urls-prev" disabled={offset === 0} onclick={() => goToPage(offset - URLS_PAGE_SIZE)}>Previous</button>
        <span class="pager-window field-text">
          {win.from}–{win.to}{win.total !== null ? ` of ${win.total}` : ""}
        </span>
        <button class="btn btn-secondary" data-testid="urls-next" disabled={!win.hasMore} onclick={() => goToPage(nextOffset)}>Next</button>
      </div>
      <p class="note" data-testid="urls-filter-note">
        Filtering and sorting cover the current page of the index only.
      </p>
    {/if}
  {/if}
</div>

<style>
  .no-match {
    padding: var(--ps-space-md);
    text-align: center;
  }

  .url-cell {
    font-family: var(--ps-font-mono);
    word-break: break-all;
  }

  /* A URL keeps its own width up to a maximum and wraps within it, so the
     column neither squeezes it nor pushes the short columns away. */
  .url-text {
    display: block;
    width: max-content;
    max-width: var(--ps-url-column-max);
  }

  /* The phone's card rows: the URL simply wraps in the card. (640px: the
     card layout's breakpoint.) */
  @media (max-width: 640px) {
    .url-text {
      width: auto;
      max-width: none;
    }

    /* The card row's "URL" label stays one word beside a long URL. */
    .url-cell::before {
      flex-shrink: 0;
      word-break: normal;
    }
  }

  .url-note {
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-xs);
  }

  .pager {
    margin: var(--ps-space-md) 0;
  }

  .pager-window {
    font-size: var(--ps-font-size-sm);
    font-variant-numeric: tabular-nums;
  }
</style>
