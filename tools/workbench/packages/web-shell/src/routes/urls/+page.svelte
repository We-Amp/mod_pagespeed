<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onMount, onDestroy } from 'svelte';
  import type {
    ApiTransport,
    ConnectionManager,
    ConnectionState,
    CacheUrlEntry,
    CacheUrlsResponse,
    AlternateInfo,
    CacheAlternatesResponse,
    CacheSelectResponse,
    CacheSelectAlternateEntry,
    CooldownInfo,
    CooldownListEntry,
  } from '@pagespeed/api-client';
  import {
    createCacheClient,
    DEVICE_PRESETS,
    ImageFormat,
    ViewportClass,
    PixelDensity,
    SaveData,
    TransferEncoding,
    encodeMask,
    decodeMask,
    formatMask,
    formatBytes,
    formatImageFormat,
    formatViewport,
    formatDensity,
    formatSaveData,
    formatEncoding,
  } from '@pagespeed/api-client';
  import type { Readable } from 'svelte/store';
  import QualityBadge from '#lib/components/QualityBadge.svelte';
  import SizeComparisonBar from '#lib/components/SizeComparisonBar.svelte';
  import ContentClassBadge from '#lib/components/ContentClassBadge.svelte';
  import FormatBadge from '#lib/components/FormatBadge.svelte';
  import QualityDistributionBar from '#lib/components/QualityDistribution.svelte';
  import { computeQualityDistribution, formatToMimeType, findMatchingOriginal } from '#lib/quality.ts';
  import UrlGroupManager from '#lib/components/UrlGroupManager.svelte';
  import HelpIcon from '#lib/components/HelpIcon.svelte';
  import Tooltip from '#lib/components/Tooltip.svelte';
  import { loadUrlGroups, addUrlToGroup } from '#lib/url-groups.ts';
  import type { UrlGroup } from '#lib/url-groups.ts';
  import { computeUrlStatus } from '#lib/url-status.ts';

  // -- Context ---------------------------------------------------------------

  const transport = getContext<ApiTransport>('api');
  const connectionManager = getContext<ConnectionManager>('connection');
  const cache = createCacheClient(transport);

  const connState: Readable<ConnectionState> = connectionManager.state;

  // -- Reactive state --------------------------------------------------------

  let connected = $state(false);

  // URL list state
  let urls = $state<CacheUrlEntry[]>([]);
  let totalUrls = $state(0);
  let currentPage = $state(0);
  let pageSize = $state(50);
  let searchFilter = $state('');
  let loading = $state(false);
  let error = $state<string | null>(null);

  // Inspector state
  let selectedUrl = $state<string | null>(null);
  let selectedHostname = $state<string | null>(null);
  let selectedScheme = $state<string | null>(null);
  let selectedSite = $state<string | null>(null);
  let alternates = $state<AlternateInfo[]>([]);
  let loadingAlternates = $state(false);
  let alternatesError = $state<string | null>(null);

  // Mask builder state
  let maskFormat = $state(ImageFormat.Original);
  let maskViewport = $state(ViewportClass.Desktop);
  let maskDensity = $state(PixelDensity.Standard);
  let maskSaveData = $state(SaveData.Off);
  let maskEncoding = $state(TransferEncoding.Identity);

  // Simulation result
  let simulationResult = $state<CacheSelectResponse | null>(null);
  let simulating = $state(false);
  let simulationError = $state<string | null>(null);

  // Action feedback
  let actionMessage = $state<string | null>(null);
  let actionError = $state<string | null>(null);
  let purgeConfirm = $state(false);
  let reprocessConfirm = $state(false);
  let clearCacheConfirm = $state(false);
  let clearingCache = $state(false);

  // Per-URL optimization status (lazy-loaded from alternates).
  import type { UrlStatus } from '#lib/url-status.ts';
  let urlStatuses = $state<Map<string, UrlStatus>>(new Map());

  // Sort state for alternates table.
  type SortColumn = 'id' | 'format' | 'viewport' | 'size' | 'quality' | 'hits';
  type SortDir = 'asc' | 'desc';
  let sortColumn = $state<SortColumn>('id');
  let sortDir = $state<SortDir>('asc');

  // Expandable detail panel state.
  let expandedAlternateId = $state<number | null>(null);
  let expandedContent = $state<{
    type: 'image' | 'text' | 'sentinel' | 'loading' | 'error';
    blobUrl?: string;
    text?: string;
    mimeType?: string;
    sentinelData?: string[];
  } | null>(null);

  // Monotonic id for the in-flight detail request. Fast-clicking alternates
  // starts overlapping async loads; only the newest may apply its result.
  let detailRequestSeq = 0;

  function toggleSort(col: SortColumn) {
    if (sortColumn === col) {
      sortDir = sortDir === 'asc' ? 'desc' : 'asc';
    } else {
      sortColumn = col;
      sortDir = col === 'size' || col === 'hits' || col === 'quality' ? 'desc' : 'asc';
    }
  }

  function sortIndicator(col: SortColumn): string {
    if (sortColumn !== col) return '';
    return sortDir === 'asc' ? ' \u25B2' : ' \u25BC';
  }

  // URL group state
  let urlGroups = $state<UrlGroup[]>([]);
  let addToGroupOpen = $state(false);
  let addToGroupMessage = $state<string | null>(null);

  // Auto-refresh state
  let autoRefresh = $state(false);
  let autoRefreshTimerId: ReturnType<typeof setInterval> | undefined;
  const AUTO_REFRESH_INTERVAL_MS = 3000;

  // Cooldown state
  let cooldownInfo = $state<CooldownInfo | null>(null);
  let activeCooldowns = $state<Map<string, CooldownInfo>>(new Map());
  let cooldownTick = $state(0);
  let cooldownTimerId: ReturnType<typeof setInterval> | undefined;
  // Store local timestamps when cooldown data was fetched so we can
  // derive remaining seconds client-side without re-fetching.
  // Separate timestamps for detail view (single URL) and list view
  // (bulk fetch) since they are fetched at different times.
  let detailCooldownFetchedAt = $state(0);
  let listCooldownFetchedAt = $state(0);

  // -- Subscriptions ---------------------------------------------------------

  const unsubConn = connState.subscribe((s) => {
    connected = s === 'connected';
  });

  onDestroy(() => {
    clearTimeout(debounceTimer);
    stopAutoRefresh();
    stopCooldownTimer();
    unsubConn();
    if (expandedContent?.blobUrl) URL.revokeObjectURL(expandedContent.blobUrl);
  });

  // -- Derived values --------------------------------------------------------

  let computedMask = $derived(
    encodeMask({
      format: maskFormat,
      viewport: maskViewport,
      density: maskDensity,
      saveData: maskSaveData,
      encoding: maskEncoding,
    }),
  );

  let totalPages = $derived(Math.max(1, Math.ceil(totalUrls / pageSize)));

  /** SSIMULACRA2 quality distribution across all alternates with scores. */
  let qualityDist = $derived(
    computeQualityDistribution(
      alternates
        .filter((a) => a.ssimulacra2_score != null)
        .map((a) => a.ssimulacra2_score!),
    ),
  );

  /** Sorted and deduplicated alternates for the table. */
  let sortedAlternates = $derived.by(() => {
    // Deduplicate by alternate_id (keep last occurrence — most recent write).
    const seen = new Map<number, typeof alternates[0]>();
    for (const alt of alternates) {
      seen.set(alt.alternate_id, alt);
    }
    const deduped = [...seen.values()];
    deduped.sort((a, b) => {
      let cmp = 0;
      switch (sortColumn) {
        case 'id': cmp = a.alternate_id - b.alternate_id; break;
        case 'format': cmp = (a.format ?? '').localeCompare(b.format ?? ''); break;
        case 'viewport': cmp = (a.viewport ?? '').localeCompare(b.viewport ?? ''); break;
        case 'size': cmp = a.size - b.size; break;
        case 'quality':
          cmp = (a.ssimulacra2_score ?? -1) - (b.ssimulacra2_score ?? -1);
          break;
        case 'hits': cmp = a.hit_count - b.hit_count; break;
      }
      return sortDir === 'asc' ? cmp : -cmp;
    });
    return deduped;
  });

  /** Find the original-format alternate to use as size baseline. */
  let originalAlternate = $derived(
    alternates.find(
      (a) => a.format === 'original' && !a.is_sentinel,
    ),
  );

  /** SVG alternates for the SVG detail panel. */
  let svgAlternates = $derived(
    alternates.filter((a) => a.format === 'svg' && !a.is_sentinel),
  );

  /** Whether any alternate has a content class that indicates SVG candidacy. */
  let hasSvgCandidate = $derived(
    alternates.some(
      (a) =>
        a.content_class === 'illustration' || a.content_class === 'icon',
    ),
  );

  let debounceTimer: ReturnType<typeof setTimeout> | undefined;

  // -- Cooldown helpers ------------------------------------------------------

  function cooldownLabel(reason: string): string {
    switch (reason) {
      case 'processing': return 'Optimizing';
      case 'write_failure': return 'Retry Pending';
      case 'revalidation': return 'Revalidating';
      default: return reason;
    }
  }

  function cooldownTooltip(reason: string): string {
    switch (reason) {
      case 'processing': return 'This URL is currently being optimized by the worker.';
      case 'write_failure': return 'A write conflict occurred. The worker will retry after the cooldown expires.';
      case 'revalidation': return 'External CSS was not yet cached. The worker will re-process shortly.';
      default: return 'Processing cooldown active.';
    }
  }

  function cooldownColorClass(reason: string): string {
    switch (reason) {
      case 'processing': return 'processing';
      case 'write_failure': return 'write-failure';
      case 'revalidation': return 'revalidation';
      default: return 'processing';
    }
  }

  /** Compute remaining seconds for a cooldown given fetch timestamp. */
  function cooldownRemaining(cd: CooldownInfo, fetchedAt?: number): number {
    const ts = fetchedAt ?? detailCooldownFetchedAt;
    const elapsed = Math.floor((Date.now() - ts) / 1000);
    return Math.max(0, cd.remaining_seconds - elapsed);
  }

  async function fetchCooldowns() {
    try {
      const resp = await cache.getCooldowns();
      if (!resp.enabled) return;
      const now = Date.now();
      listCooldownFetchedAt = now;
      const map = new Map<string, CooldownInfo>();
      for (const entry of resp.cooldowns) {
        const key = `${entry.hostname}|${entry.url}`;
        map.set(key, {
          reason: entry.reason,
          remaining_seconds: entry.remaining_seconds,
          duration_seconds: entry.duration_seconds,
        });
      }
      activeCooldowns = map;
    } catch {
      // Ignore — cooldowns are optional.
    }
  }

  function startCooldownTimer() {
    if (cooldownTimerId) return;
    cooldownTimerId = setInterval(() => {
      cooldownTick++;
      // When the selected URL's cooldown expires, auto-refresh alternates
      // and stop the timer to avoid leaking.
      if (cooldownInfo && cooldownRemaining(cooldownInfo) <= 0) {
        cooldownInfo = null;
        stopCooldownTimer();
        if (selectedUrl && selectedHostname) fetchAlternates();
      }
    }, 1000);
  }

  function stopCooldownTimer() {
    if (cooldownTimerId) {
      clearInterval(cooldownTimerId);
      cooldownTimerId = undefined;
    }
  }

  // -- Auto-refresh helpers --------------------------------------------------

  function startAutoRefresh() {
    if (autoRefreshTimerId || !autoRefresh) return;
    autoRefreshTimerId = setInterval(() => {
      if (selectedUrl && selectedHostname && !loadingAlternates) {
        silentRefreshAlternates();
      } else if (!selectedUrl && !loading) {
        fetchUrls();
      }
    }, AUTO_REFRESH_INTERVAL_MS);
  }

  function stopAutoRefresh() {
    if (autoRefreshTimerId) {
      clearInterval(autoRefreshTimerId);
      autoRefreshTimerId = undefined;
    }
  }

  /** Re-fetch alternates without showing the loading spinner. */
  async function silentRefreshAlternates() {
    if (!selectedUrl || !selectedHostname) return;
    try {
      const resp = await cache.getAlternates({
        url: selectedUrl,
        hostname: selectedHostname,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
      alternates = resp.alternates;
      alternatesError = null;
      if (resp.cooldown) {
        cooldownInfo = resp.cooldown;
        detailCooldownFetchedAt = Date.now();
        startCooldownTimer();
      } else {
        cooldownInfo = null;
      }
      if (selectedUrl && selectedHostname) {
        const key = `${selectedHostname}|${selectedUrl}`;
        const status = computeUrlStatus(resp.alternates);
        if (status) {
          urlStatuses = new Map(urlStatuses).set(key, status);
        }
      }
    } catch {
      // Silent refresh — do not overwrite alternates or show errors.
    }
  }

  // -- Data fetching ---------------------------------------------------------

  async function fetchUrls() {
    loading = true;
    error = null;
    try {
      const params: { offset?: number; limit?: number; hostname?: string } = {
        offset: currentPage * pageSize,
        limit: pageSize,
      };
      if (searchFilter.trim()) {
        params.hostname = searchFilter.trim();
      }
      const [urlsResp] = await Promise.all([
        cache.getUrls(params),
        fetchCooldowns(),
      ]);
      urls = urlsResp.urls;
      totalUrls = urlsResp.total;
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
      urls = [];
      totalUrls = 0;
    } finally {
      loading = false;
    }
  }

  async function fetchAlternates() {
    if (!selectedUrl || !selectedHostname) return;
    loadingAlternates = true;
    alternatesError = null;
    simulationResult = null;
    try {
      const resp = await cache.getAlternates({
        url: selectedUrl,
        hostname: selectedHostname,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
      alternates = resp.alternates;
      // Extract cooldown info from the alternates response.
      if (resp.cooldown) {
        cooldownInfo = resp.cooldown;
        detailCooldownFetchedAt = Date.now();
        startCooldownTimer();
      } else {
        cooldownInfo = null;
      }
      // Cache per-URL optimization status for the list view badge.
      if (selectedUrl && selectedHostname) {
        const key = `${selectedHostname}|${selectedUrl}`;
        const status = computeUrlStatus(resp.alternates);
        if (status) {
          urlStatuses = new Map(urlStatuses).set(key, status);
        }
      }
    } catch (e) {
      alternatesError = e instanceof Error ? e.message : String(e);
      alternates = [];
    } finally {
      loadingAlternates = false;
    }
  }

  // -- Actions ---------------------------------------------------------------

  function selectUrlEntry(entry: CacheUrlEntry) {
    selectedUrl = entry.url;
    selectedHostname = entry.hostname;
    selectedScheme = entry.scheme ?? null;
    selectedSite = entry.site || null;
    simulationResult = null;
    actionMessage = null;
    actionError = null;
    purgeConfirm = false;
    reprocessConfirm = false;
    startAutoRefresh();
    fetchAlternates();
  }

  function backToList() {
    selectedUrl = null;
    selectedHostname = null;
    selectedScheme = null;
    selectedSite = null;
    alternates = [];
    simulationResult = null;
    actionMessage = null;
    actionError = null;
    purgeConfirm = false;
    reprocessConfirm = false;
    cooldownInfo = null;
    stopAutoRefresh();
    stopCooldownTimer();
  }

  function handleSearchInput() {
    clearTimeout(debounceTimer);
    debounceTimer = setTimeout(() => {
      currentPage = 0;
      fetchUrls();
    }, 300);
  }

  function prevPage() {
    if (currentPage > 0) {
      currentPage--;
      fetchUrls();
    }
  }

  function nextPage() {
    if (currentPage < totalPages - 1) {
      currentPage++;
      fetchUrls();
    }
  }

  function applyPreset(event: Event) {
    const select = event.target as HTMLSelectElement;
    const idx = parseInt(select.value, 10);
    if (isNaN(idx) || idx < 0) return;
    const preset = DEVICE_PRESETS[idx];
    if (!preset) return;
    maskFormat = preset.components.format;
    maskViewport = preset.components.viewport;
    maskDensity = preset.components.density;
    maskSaveData = preset.components.saveData;
    maskEncoding = preset.components.encoding;
  }

  async function simulateSelection() {
    if (!selectedUrl || !selectedHostname) return;
    simulating = true;
    simulationError = null;
    try {
      simulationResult = await cache.selectAlternate({
        url: selectedUrl,
        hostname: selectedHostname,
        mask: computedMask,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
    } catch (e) {
      simulationError = e instanceof Error ? e.message : String(e);
      simulationResult = null;
    } finally {
      simulating = false;
    }
  }

  async function purgeUrl() {
    if (!selectedUrl || !selectedHostname) return;
    actionMessage = null;
    actionError = null;
    try {
      const resp = await cache.purgeUrl({
        url: selectedUrl,
        hostname: selectedHostname,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
      actionMessage = `Purged ${resp.deleted} variant(s).`;
      purgeConfirm = false;
      fetchAlternates();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
      purgeConfirm = false;
    }
  }

  async function reprocessUrl() {
    if (!selectedUrl || !selectedHostname) return;
    actionMessage = null;
    actionError = null;
    try {
      const resp = await cache.reprocessUrl({
        url: selectedUrl,
        hostname: selectedHostname,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
      actionMessage = resp.reprocess_enqueued ? 'Reprocess enqueued.' : 'Reprocess not available.';
      reprocessConfirm = false;
      fetchAlternates();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
      reprocessConfirm = false;
    }
  }

  async function clearAllCache() {
    clearingCache = true;
    actionMessage = null;
    actionError = null;
    try {
      const resp = await cache.purgeAll();
      actionMessage = `Cache volume reset. ${resp.urls_cleared} tracked URL(s) cleared.`;
      clearCacheConfirm = false;
      // Reset local state.
      urls = [];
      totalUrls = 0;
      selectedUrl = null;
      selectedHostname = null;
      selectedScheme = null;
      selectedSite = null;
      alternates = [];
      urlStatuses = new Map();
      // Refresh.
      fetchUrls();
    } catch (e) {
      actionError = e instanceof Error ? e.message : String(e);
      clearCacheConfirm = false;
    } finally {
      clearingCache = false;
    }
  }

  // -- URL Group actions -----------------------------------------------------

  function handleAddToGroup(groupId: string) {
    if (!selectedUrl || !selectedHostname) return;
    urlGroups = addUrlToGroup(groupId, {
      url: selectedUrl,
      hostname: selectedHostname,
      ...(selectedSite ? { site: selectedSite } : {}),
    });
    addToGroupOpen = false;
    addToGroupMessage = `Added to group.`;
    setTimeout(() => {
      addToGroupMessage = null;
    }, 3000);
  }

  function refreshUrlGroups() {
    urlGroups = loadUrlGroups();
  }

  // -- Detail panel ----------------------------------------------------------

  async function toggleDetail(alt: AlternateInfo) {
    // Collapse if already expanded.
    if (expandedAlternateId === alt.alternate_id) {
      if (expandedContent?.blobUrl) URL.revokeObjectURL(expandedContent.blobUrl);
      expandedAlternateId = null;
      expandedContent = null;
      return;
    }

    // A new expand supersedes any in-flight load. Bump the sequence so a slower
    // earlier response can detect that it is stale and bail without clobbering
    // this one or leaking a blob URL.
    const seq = ++detailRequestSeq;

    // Revoke the currently-displayed blob URL before replacing the content.
    if (expandedContent?.blobUrl) URL.revokeObjectURL(expandedContent.blobUrl);

    expandedAlternateId = alt.alternate_id;
    expandedContent = { type: 'loading' };

    if (!selectedUrl || !selectedHostname) return;

    try {
      const resp = await cache.getContent({
        url: selectedUrl,
        hostname: selectedHostname,
        alternate_id: alt.alternate_id,
        ...(selectedScheme ? { scheme: selectedScheme } : {}),
        ...(selectedSite ? { site: selectedSite } : {}),
      });
      if (seq !== detailRequestSeq) return; // superseded by a newer expand
      const ct = resp.headers.get('content-type') || '';

      if (alt.is_sentinel) {
        // Parse sentinel text content as structured data.
        const text = await resp.text();
        if (seq !== detailRequestSeq) return;
        expandedContent = {
          type: 'sentinel',
          text,
          sentinelData: text.split('\n').filter((l) => l.trim()),
        };
      } else if (ct.startsWith('image/svg+xml') || alt.format === 'svg' || ct.startsWith('image/')) {
        // Render every image, including SVG, via a blob-backed <img>. An <img>
        // source cannot execute embedded <script>/on* handlers, so cached SVG
        // (attacker-influenceable: user uploads, compromised origin assets)
        // cannot run script in the console origin, where the API token lives.
        // Never feed cached bytes to {@html}.
        const blob = await resp.blob();
        // Create the object URL only once this is still the active request, so a
        // superseded load never leaks a blob URL (the blob itself is GC'd).
        if (seq !== detailRequestSeq) return;
        const blobUrl = URL.createObjectURL(blob);
        expandedContent = { type: 'image', blobUrl, mimeType: ct };
      } else {
        // HTML, CSS, JS, or other text content.
        const text = await resp.text();
        if (seq !== detailRequestSeq) return;
        expandedContent = {
          type: 'text',
          text: text.length > 50000 ? text.slice(0, 50000) + '\n... (truncated)' : text,
          mimeType: ct,
        };
      }
    } catch (e) {
      if (seq !== detailRequestSeq) return;
      expandedContent = {
        type: 'error',
        text: e instanceof Error ? e.message : String(e),
      };
    }
  }

  function formatTimestamp(ms: number): string {
    if (!ms) return '--';
    return new Date(ms).toLocaleString();
  }

  // -- Helpers ---------------------------------------------------------------

  function formatHex(n: number): string {
    return '0x' + n.toString(16).toUpperCase().padStart(2, '0');
  }

  /** Decode an 8-bit alternate ID into its capability mask components. */
  function describeAlternateId(alt: AlternateInfo): string {
    const parts: string[] = [];
    if (alt.is_sentinel) {
      parts.push(`Sentinel: ${alt.sentinel_name ?? 'unknown'}`);
      parts.push(`ID: ${formatHex(alt.alternate_id)}`);
      parts.push(`Size: ${formatBytes(alt.size)}`);
      return parts.join('\n');
    }
    parts.push(`Format: ${(alt.format ?? 'unknown').toUpperCase()}`);
    parts.push(`Viewport: ${alt.viewport ?? 'unknown'}`);
    parts.push(`Density: ${alt.density ?? '?'}`);
    parts.push(`Save-Data: ${alt.save_data ? 'On' : 'Off'}`);
    const enc = alt.encoding === 'identity' ? 'Uncompressed' : (alt.encoding ?? '?');
    parts.push(`Encoding: ${enc}`);
    parts.push('');
    parts.push('Bit layout: [1:0] format, [3:2] viewport,');
    parts.push('[4] density, [5] save-data, [7:6] encoding');
    return parts.join('\n');
  }

  function truncateUrl(url: string, maxLen: number = 80): string {
    if (url.length <= maxLen) return url;
    return url.slice(0, maxLen - 3) + '...';
  }

  // -- Lifecycle -------------------------------------------------------------

  onMount(() => {
    urlGroups = loadUrlGroups();
    if (connected) {
      fetchUrls();
    }
  });

  // Fetch when connection comes up.
  $effect(() => {
    if (connected && !selectedUrl) {
      fetchUrls();
    }
  });

  // Manage auto-refresh timer when toggle or detail view changes.
  $effect(() => {
    if (autoRefresh && connected) {
      startAutoRefresh();
    } else {
      stopAutoRefresh();
    }
  });
</script>

{#if !connected}
  <div class="page-center">
    <div class="ps-card center-card">
      <div class="ps-spinner" aria-label="Connecting to URL inspector"></div>
      <p class="center-text">Waiting for worker connection...</p>
      <span class="ps-badge ps-badge-warning">Disconnected</span>
    </div>
  </div>
{:else if selectedUrl && selectedHostname}
  <!-- ═══════════════════════════════════════════════════════════════
       URL Inspector Detail View
       ═══════════════════════════════════════════════════════════════ -->
  <div class="inspector">
    <div class="inspector-header">
      <button class="ps-btn ps-btn-ghost back-btn" onclick={backToList}>
        &larr; Back to URL list
      </button>
      <div class="inspector-url-info">
        <h1 class="inspector-title" title={selectedUrl}><a href={selectedUrl} target="_blank" rel="noopener noreferrer">{truncateUrl(selectedUrl, 120)}</a></h1>
        <span class="ps-badge">{selectedHostname}</span>
        {#if (selectedScheme ?? 'https') === 'http'}<span class="ps-badge ps-badge-warning" title="Served over plain HTTP">http</span>{/if}
      </div>
      <div class="inspector-actions">
        {#if purgeConfirm}
          <span class="confirm-prompt">Purge all variants?</span>
          <button class="ps-btn ps-btn-primary" onclick={purgeUrl}>Confirm</button>
          <button class="ps-btn ps-btn-ghost" onclick={() => (purgeConfirm = false)}>Cancel</button>
        {:else}
          <button class="ps-btn ps-btn-secondary" onclick={() => (purgeConfirm = true)} title="Delete all cached variants for this URL. Reversible — variants will be regenerated on next request.">Purge</button>
        {/if}
        {#if reprocessConfirm}
          <span class="confirm-prompt">Purge and re-queue?</span>
          <button class="ps-btn ps-btn-primary" onclick={reprocessUrl}>Confirm</button>
          <button class="ps-btn ps-btn-ghost" onclick={() => (reprocessConfirm = false)}>Cancel</button>
        {:else}
          <button class="ps-btn ps-btn-secondary" onclick={() => (reprocessConfirm = true)} title="Purge all variants and queue the URL for immediate re-optimization.">Reprocess</button>
        {/if}
        <div class="add-to-group-wrapper">
          <button
            class="ps-btn ps-btn-secondary"
            onclick={() => {
              urlGroups = loadUrlGroups();
              addToGroupOpen = !addToGroupOpen;
            }}
          >
            Add to Group
          </button>
          {#if addToGroupOpen}
            <div class="add-to-group-dropdown">
              {#if urlGroups.length === 0}
                <span class="dropdown-empty">No groups. Create one on the URL Groups panel below.</span>
              {:else}
                {#each urlGroups as group}
                  <button
                    class="dropdown-item"
                    onclick={() => handleAddToGroup(group.id)}
                  >
                    {group.name}
                    <span class="dropdown-meta">
                      ({group.urls.length} URL{group.urls.length === 1 ? '' : 's'})
                    </span>
                  </button>
                {/each}
              {/if}
            </div>
          {/if}
        </div>
        {#if addToGroupMessage}
          <span class="ps-badge ps-badge-success">{addToGroupMessage}</span>
        {/if}
      </div>
    </div>

    {#if actionMessage}
      <div class="action-feedback ps-badge ps-badge-success">{actionMessage}</div>
    {/if}
    {#if actionError}
      <div class="action-feedback ps-badge ps-badge-error">{actionError}</div>
    {/if}

    <!-- Cooldown Banner -->
    {#if cooldownInfo}
      {@const remaining = cooldownRemaining(cooldownInfo)}
      {#if remaining > 0}
        <!-- Force reactive update on timer tick -->
        <div class="cooldown-banner cooldown-banner-{cooldownColorClass(cooldownInfo.reason)}" data-tick={cooldownTick}>
          <span class="cooldown-banner-icon">&#9202;</span>
          <span class="cooldown-banner-text">
            {cooldownLabel(cooldownInfo.reason)} ({remaining}s remaining)
          </span>
          {#if !reprocessConfirm}
            <button class="ps-btn ps-btn-secondary ps-btn-sm" onclick={() => (reprocessConfirm = true)}>
              Reprocess Now
            </button>
          {/if}
        </div>
      {/if}
    {/if}

    <!-- Alternates Table -->
    <div class="ps-card">
      <div class="section-title-row">
        <h2 class="section-title">Variants <HelpIcon tooltip="Each cached URL can have multiple variants — one per device/format/encoding combination. The original content plus all optimized variants are stored separately, identified by capability mask." /></h2>
        <div class="auto-refresh-toggle">
          <label class="auto-refresh-label" title="Automatically refresh alternates every {AUTO_REFRESH_INTERVAL_MS / 1000} seconds">
            <input type="checkbox" bind:checked={autoRefresh} class="auto-refresh-checkbox" />
            <span class="auto-refresh-text">
              {#if autoRefresh}
                <span class="auto-refresh-dot"></span>
                Auto-refresh
              {:else}
                Auto-refresh
              {/if}
            </span>
          </label>
        </div>
      </div>
      {#if loadingAlternates}
        <div class="loading-row">
          <div class="ps-spinner" aria-label="Loading alternates"></div>
          <span>Loading alternates...</span>
        </div>
      {:else if alternatesError}
        <p class="error-text">{alternatesError}</p>
      {:else if alternates.length === 0}
        <div class="empty-alternates">
          <p class="empty-text">No variants found for this URL.</p>
          <p class="empty-hint">This usually means the worker hasn't processed this URL yet (first-request race condition).</p>
          {#if reprocessConfirm}
            <div class="empty-reprocess-confirm">
              <span class="confirm-prompt">Queue this URL for processing?</span>
              <button class="ps-btn ps-btn-primary" onclick={reprocessUrl}>Confirm</button>
              <button class="ps-btn ps-btn-ghost" onclick={() => (reprocessConfirm = false)}>Cancel</button>
            </div>
          {:else}
            <button class="ps-btn ps-btn-primary" onclick={() => (reprocessConfirm = true)}>
              Reprocess Now
            </button>
          {/if}
        </div>
      {:else}
        <div class="table-scroll">
          <table class="data-table">
            <thead>
              <tr>
                <th scope="col" class="num-col sortable" onclick={() => toggleSort('id')}>ID <HelpIcon tooltip="8-bit alternate identifier encoding the capability mask: bits [1:0] image format, [3:2] viewport class, [4] pixel density, [5] save-data, [7:6] transfer encoding. Hover a row's ID for decoded values." />{sortIndicator('id')}</th>
                <th scope="col" class="sortable" onclick={() => toggleSort('format')}>Format{sortIndicator('format')}</th>
                <th scope="col" class="sortable" onclick={() => toggleSort('viewport')}>Viewport{sortIndicator('viewport')}</th>
                <th scope="col">Density</th>
                <th scope="col">Save-Data</th>
                <th scope="col">Encoding</th>
                <th scope="col">Content Type</th>
                <th scope="col" class="num-col sortable" onclick={() => toggleSort('size')}>Size{sortIndicator('size')}</th>
                <th scope="col">Size Comparison</th>
                <th scope="col" class="sortable" onclick={() => toggleSort('quality')}>Quality{sortIndicator('quality')}</th>
                <th scope="col">Class</th>
                <th scope="col" class="num-col sortable" onclick={() => toggleSort('hits')}>Hits{sortIndicator('hits')}</th>
                <th scope="col">Sentinel <HelpIcon tooltip="Internal metadata entries (e.g., Early Hints, Browser Profile) stored alongside content variants. Not served to clients." /></th>
              </tr>
            </thead>
            <tbody>
              {#each sortedAlternates as alt}
                <tr
                  class="alt-row"
                  class:alt-selected={simulationResult !== null &&
                    simulationResult.best_index >= 0 &&
                    simulationResult.alternates[simulationResult.best_index]?.alternate_id === alt.alternate_id}
                  class:alt-expanded={expandedAlternateId === alt.alternate_id}
                  onclick={() => toggleDetail(alt)}
                >
                  <td class="num-col mono">
                    <Tooltip text={describeAlternateId(alt)}>
                      <span class="expand-arrow" class:expanded={expandedAlternateId === alt.alternate_id}>{'\u25B6'}</span>
                      {formatHex(alt.alternate_id)}
                    </Tooltip>
                  </td>
                  <td>
                    {#if alt.is_sentinel}
                      <span class="ps-badge ps-badge-info">{alt.sentinel_name ?? 'Sentinel'}</span>
                    {:else}
                      <FormatBadge format={alt.format ?? ''} />
                      {#if alt.needs_revalidation}
                        <span class="revalidation-badge" title="This HTML variant was processed without cached CSS. It will be re-optimized on next request.">Revalidating</span>
                      {/if}
                    {/if}
                  </td>
                  <td>{alt.is_sentinel ? '--' : (alt.viewport ?? '--')}</td>
                  <td>{alt.is_sentinel ? '--' : (alt.density ?? '--')}</td>
                  <td>{alt.is_sentinel ? '--' : (alt.save_data ? 'On' : 'Off')}</td>
                  <td>{alt.is_sentinel ? '--' : (alt.encoding === 'identity' ? 'Uncompressed' : (alt.encoding ?? '--'))}</td>
                  <td class="mono" title={alt.is_sentinel ? '' : (alt.origin_content_type || alt.content_type || '')}>{alt.is_sentinel ? '--' : formatToMimeType(alt.format ?? '', alt.origin_content_type || alt.content_type)}</td>
                  <td class="num-col mono">{formatBytes(alt.size)}</td>
                  <td class="size-col">
                    {#if alt.is_sentinel}
                      <span class="no-data">--</span>
                    {:else if alt.original_size != null && alt.original_size > 0}
                      <SizeComparisonBar
                        originalSize={alt.original_size}
                        optimizedSize={alt.size}
                        format={alt.format ?? ''}
                      />
                    {:else if alt.format !== 'original'}
                      {@const matchedOriginal = findMatchingOriginal(alt, alternates)}
                      {#if matchedOriginal}
                        <SizeComparisonBar
                          originalSize={matchedOriginal.size}
                          optimizedSize={alt.size}
                          format={alt.format ?? ''}
                        />
                      {:else if originalAlternate}
                        <SizeComparisonBar
                          originalSize={originalAlternate.size}
                          optimizedSize={alt.size}
                          format={alt.format ?? ''}
                        />
                      {:else}
                        <span class="no-data">--</span>
                      {/if}
                    {:else}
                      <span class="no-data">--</span>
                    {/if}
                  </td>
                  <td>
                    {#if alt.ssimulacra2_score != null}
                      <QualityBadge score={alt.ssimulacra2_score} showLabel={false} />
                    {:else}
                      <span class="no-data">--</span>
                    {/if}
                  </td>
                  <td>
                    {#if alt.content_class}
                      <ContentClassBadge contentClass={alt.content_class} />
                      {#if alt.content_class === 'illustration' || alt.content_class === 'icon'}
                        <span class="svg-candidate-badge" title="Good candidate for SVG auto-vectorization">SVG Candidate</span>
                      {/if}
                    {:else}
                      <span class="no-data">--</span>
                    {/if}
                  </td>
                  <td class="num-col mono">{alt.hit_count}</td>
                  <td>
                    {#if alt.is_sentinel}
                      <span class="ps-badge ps-badge-info">{alt.sentinel_name ?? 'Sentinel'}</span>
                    {/if}
                  </td>
                </tr>
                {#if expandedAlternateId === alt.alternate_id}
                  <tr class="detail-row">
                    <td colspan="13">
                      <div class="detail-panel">
                        <div class="detail-meta">
                          <div class="detail-meta-grid">
                            <span class="detail-label">Alternate ID</span>
                            <span class="detail-value mono">{formatHex(alt.alternate_id)}</span>
                            {#if alt.is_sentinel}
                              <span class="detail-label">Type</span>
                              <span class="detail-value"><span class="ps-badge ps-badge-info">{alt.sentinel_name ?? 'Sentinel'}</span></span>
                            {/if}
                            {#if !alt.is_sentinel}
                              <span class="detail-label">Content Type</span>
                              <span class="detail-value mono">{alt.content_type || '--'}</span>
                              <span class="detail-label" title="The Content-Type header from the origin server response.">Origin Content-Type</span>
                              <span class="detail-value mono">{alt.origin_content_type || '--'}</span>
                              <span class="detail-label">Flags</span>
                              <span class="detail-value mono">{formatHex(alt.flags ?? 0)}{alt.needs_revalidation ? ' (needs revalidation)' : ''}</span>
                            {/if}
                            <span class="detail-label">Last Access</span>
                            <span class="detail-value">{formatTimestamp(alt.last_access)}</span>
                            <span class="detail-label" title="Number of times this variant was served from cache by nginx.">Hit Count</span>
                            <span class="detail-value mono">{alt.hit_count}</span>
                            <span class="detail-label">Size</span>
                            <span class="detail-value mono">{formatBytes(alt.size)}</span>
                            {#if !alt.is_sentinel && alt.cache_inserted_at}
                              <span class="detail-label" title="When this variant was first written to cache.">Cached At</span>
                              <span class="detail-value">{formatTimestamp(alt.cache_inserted_at * 1000)}</span>
                              {@const age = Math.floor(Date.now() / 1000) - alt.cache_inserted_at}
                              {@const maxAge = alt.origin_s_maxage || alt.origin_max_age || 0}
                              {@const remaining = maxAge - age}
                              <span class="detail-label" title="Cache freshness status based on origin Cache-Control headers and elapsed time.">Freshness</span>
                              <span class="detail-value">
                                {#if maxAge === 0}
                                  {#if alt.origin_cc_flags && alt.origin_cc_flags & 0x10}
                                    <span class="freshness-badge heuristic">Heuristic (public)</span>
                                  {:else if alt.origin_cc_flags && alt.origin_cc_flags & 0x01}
                                    <span class="freshness-badge stale">no-cache</span>
                                  {:else}
                                    <span class="freshness-badge stale">No max-age</span>
                                  {/if}
                                {:else if remaining > 0}
                                  <span class="freshness-badge fresh">Fresh ({Math.floor(remaining / 60)}m remaining)</span>
                                {:else}
                                  <span class="freshness-badge stale">Stale</span>
                                {/if}
                              </span>
                            {/if}
                            {#if !alt.is_sentinel && alt.origin_cc_flags !== undefined}
                              <span class="detail-label">Origin Cache-Control</span>
                              <span class="detail-value">
                                {#if !(alt.origin_cc_flags & 0x200)}
                                  <span class="freshness-badge no-cc">No Cache-Control</span>
                                {:else}
                                  <span class="mono">
                                    {#if alt.origin_max_age}max-age={alt.origin_max_age}{/if}
                                    {#if alt.origin_cc_flags & 0x01} no-cache{/if}
                                    {#if alt.origin_cc_flags & 0x02} must-revalidate{/if}
                                    {#if alt.origin_cc_flags & 0x04} no-store{/if}
                                    {#if alt.origin_cc_flags & 0x08} private{/if}
                                    {#if alt.origin_cc_flags & 0x10} public{/if}
                                    {#if alt.origin_cc_flags & 0x20} immutable{/if}
                                    {#if alt.origin_cc_flags & 0x40} s-maxage={alt.origin_s_maxage}{/if}
                                    {#if alt.origin_cc_flags & 0x80} proxy-revalidate{/if}
                                    {#if alt.origin_cc_flags & 0x100} no-transform{/if}
                                  </span>
                                {/if}
                              </span>
                            {/if}
                            {#if alt.version}
                              <span class="detail-label" title="Wire format version of the stored metadata (v3-v6). Older versions lack newer fields.">Metadata Version</span>
                              <span class="detail-value mono">v{alt.version}</span>
                            {/if}
                          </div>
                        </div>
                        <div class="detail-content">
                          {#if expandedContent?.type === 'loading'}
                            <div class="loading-row">
                              <div class="ps-spinner" aria-label="Loading content"></div>
                              <span>Loading content...</span>
                            </div>
                          {:else if expandedContent?.type === 'error'}
                            <p class="error-text">{expandedContent.text}</p>
                          {:else if expandedContent?.type === 'image' && expandedContent.blobUrl}
                            <div class="detail-image-preview">
                              <img src={expandedContent.blobUrl} alt="Cached content preview" class="preview-image" />
                            </div>
                          {:else if expandedContent?.type === 'sentinel' && expandedContent.sentinelData}
                            <div class="detail-sentinel">
                              <ul class="sentinel-list">
                                {#each expandedContent.sentinelData as line}
                                  <li class="sentinel-item mono">{line}</li>
                                {/each}
                              </ul>
                            </div>
                          {:else if expandedContent?.type === 'text' && expandedContent.text != null}
                            <div class="detail-code-preview">
                              <pre class="code-block"><code>{expandedContent.text}</code></pre>
                            </div>
                          {/if}
                        </div>
                      </div>
                    </td>
                  </tr>
                {/if}
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </div>

    <!-- SVG Detail Panel -->
    {#if svgAlternates.length > 0}
      <div class="ps-card">
        <h2 class="section-title">SVG Vectorization Details <HelpIcon tooltip="Details for SVG auto-vectorized alternates. SVG variants are resolution-independent and served as a single alternate (Desktop/1x/Identity). The vectorization pipeline uses VTracer to convert raster images to scalable vector graphics." /></h2>
        {#each svgAlternates as svgAlt}
          <div class="svg-detail-section">
            <div class="svg-detail-grid">
              <span class="svg-detail-label">Alternate ID</span>
              <span class="svg-detail-value mono">{formatHex(svgAlt.alternate_id)}</span>

              <span class="svg-detail-label">Format</span>
              <span class="svg-detail-value"><FormatBadge format="SVG" /></span>

              <span class="svg-detail-label">SVG Size</span>
              <span class="svg-detail-value mono">{formatBytes(svgAlt.size)}</span>

              {#if svgAlt.original_size != null && svgAlt.original_size > 0}
                <span class="svg-detail-label">Original Size</span>
                <span class="svg-detail-value mono">{formatBytes(svgAlt.original_size)}</span>

                <span class="svg-detail-label">Size Reduction</span>
                <span class="svg-detail-value mono">
                  {#if svgAlt.size < svgAlt.original_size}
                    {((1 - svgAlt.size / svgAlt.original_size) * 100).toFixed(1)}% smaller
                  {:else}
                    {((svgAlt.size / svgAlt.original_size - 1) * 100).toFixed(1)}% larger
                  {/if}
                </span>
              {:else if originalAlternate}
                <span class="svg-detail-label">Original Size</span>
                <span class="svg-detail-value mono">{formatBytes(originalAlternate.size)}</span>

                <span class="svg-detail-label">Size Reduction</span>
                <span class="svg-detail-value mono">
                  {#if svgAlt.size < originalAlternate.size}
                    {((1 - svgAlt.size / originalAlternate.size) * 100).toFixed(1)}% smaller
                  {:else}
                    {((svgAlt.size / originalAlternate.size - 1) * 100).toFixed(1)}% larger
                  {/if}
                </span>
              {/if}

              <span class="svg-detail-label">Content Type</span>
              <span class="svg-detail-value mono">{svgAlt.origin_content_type || svgAlt.content_type}</span>

              <span class="svg-detail-label">Viewport</span>
              <span class="svg-detail-value">{svgAlt.viewport} (resolution-independent)</span>

              <span class="svg-detail-label">Hit Count</span>
              <span class="svg-detail-value mono">{svgAlt.hit_count}</span>

              {#if svgAlt.content_class}
                <span class="svg-detail-label">Content Class</span>
                <span class="svg-detail-value">
                  <ContentClassBadge contentClass={svgAlt.content_class} />
                </span>
              {/if}
            </div>
            <p class="svg-detail-note">
              SVG alternates use a single resolution-independent variant (Desktop/1x/Identity).
              The vectorization pipeline: Decode, Analyze, Evaluate, Preprocess, VTracer (Rust FFI), Sanitize, Size Gate.
            </p>
          </div>
        {/each}
      </div>
    {/if}

    <!-- SVG Candidacy Banner -->
    {#if hasSvgCandidate && svgAlternates.length === 0}
      <div class="ps-card svg-candidacy-card">
        <h2 class="section-title">SVG Candidacy</h2>
        <p class="svg-candidacy-text">
          Content analysis classified this image as an <strong>illustration</strong> or <strong>icon</strong>,
          making it a good candidate for SVG auto-vectorization. An SVG variant may be generated
          by the worker if vectorization is enabled and the fidelity gate passes.
        </p>
      </div>
    {/if}

    <!-- Quality Distribution -->
    {#if qualityDist.total > 0}
      <div class="ps-card">
        <h2 class="section-title">Quality Distribution <HelpIcon tooltip="Distribution of SSIMULACRA2 perceptual quality scores across all optimized alternates for this URL. Scores: 80+ imperceptible, 70-80 very good, 60-70 good, below 60 acceptable for bandwidth-constrained scenarios." /></h2>
        <p class="quality-dist-desc">
          SSIMULACRA2 score distribution across {qualityDist.total} optimized alternate{qualityDist.total === 1 ? '' : 's'}.
        </p>
        <QualityDistributionBar distribution={qualityDist} />
      </div>
    {/if}

    <!-- Mask Builder Panel -->
    <div class="ps-card">
      <h2 class="section-title">Device Simulation <HelpIcon tooltip="Simulate how the cache selects a variant for a specific device. Build a capability mask by choosing format, viewport, density, encoding, and save-data — then click Simulate to see which alternate would be served." /></h2>
      <div class="mask-builder">
        <div class="mask-field">
          <label class="mask-label" for="preset-select">Preset</label>
          <select id="preset-select" class="ps-select" onchange={applyPreset}>
            <option value="-1">-- Select preset --</option>
            {#each DEVICE_PRESETS as preset, i}
              <option value={i}>{preset.name}</option>
            {/each}
          </select>
        </div>

        <div class="mask-field">
          <label class="mask-label" for="format-select">Image Format</label>
          <select id="format-select" class="ps-select" bind:value={maskFormat}>
            <option value={ImageFormat.Original}>Original</option>
            <option value={ImageFormat.WebP}>WebP</option>
            <option value={ImageFormat.AVIF}>AVIF</option>
            <option value={ImageFormat.SVG}>SVG</option>
          </select>
        </div>

        <div class="mask-field">
          <label class="mask-label" for="viewport-select">Viewport</label>
          <select id="viewport-select" class="ps-select" bind:value={maskViewport}>
            <option value={ViewportClass.Mobile}>Mobile</option>
            <option value={ViewportClass.Tablet}>Tablet</option>
            <option value={ViewportClass.Desktop}>Desktop</option>
          </select>
        </div>

        <div class="mask-field">
          <label class="mask-label" for="density-select">Density</label>
          <select id="density-select" class="ps-select" bind:value={maskDensity}>
            <option value={PixelDensity.Standard}>1x</option>
            <option value={PixelDensity.High}>2x+</option>
          </select>
        </div>

        <div class="mask-field">
          <label class="mask-label" for="savedata-select">Save-Data</label>
          <select id="savedata-select" class="ps-select" bind:value={maskSaveData}>
            <option value={SaveData.Off}>Off</option>
            <option value={SaveData.On}>On</option>
          </select>
        </div>

        <div class="mask-field">
          <label class="mask-label" for="encoding-select">Encoding</label>
          <select id="encoding-select" class="ps-select" bind:value={maskEncoding}>
            <option value={TransferEncoding.Identity}>Identity</option>
            <option value={TransferEncoding.Gzip}>Gzip</option>
            <option value={TransferEncoding.Brotli}>Brotli</option>
          </select>
        </div>
      </div>

      <div class="mask-display">
        <span class="mask-computed">
          Mask: {formatHex(computedMask)} ({computedMask})
        </span>
        <span class="mask-human">{formatMask(computedMask)}</span>
      </div>

      <button
        class="ps-btn ps-btn-primary simulate-btn"
        onclick={simulateSelection}
        disabled={simulating || alternates.length === 0}
      >
        {#if simulating}
          <div class="ps-spinner" aria-label="Simulating"></div>
          Simulating...
        {:else}
          Simulate Selection
        {/if}
      </button>
    </div>

    <!-- Simulation Results -->
    {#if simulationError}
      <div class="ps-card">
        <h2 class="section-title">Simulation Error</h2>
        <p class="error-text">{simulationError}</p>
      </div>
    {/if}

    {#if simulationResult}
      {@const bestAlt = simulationResult.best_index >= 0 ? simulationResult.alternates[simulationResult.best_index] : null}
      <div class="ps-card">
        <h2 class="section-title">Simulation Result</h2>
        <div class="sim-summary">
          <div class="sim-kv">
            <span class="sim-label">Selected Alternate</span>
            <span class="sim-value mono">{bestAlt ? formatHex(bestAlt.alternate_id) : 'none'}</span>
          </div>
          {#if bestAlt?.content_type}
            <div class="sim-kv">
              <span class="sim-label">Content Type</span>
              <span class="sim-value mono">{bestAlt.content_type}</span>
            </div>
          {/if}
          {#if bestAlt}
            <div class="sim-kv">
              <span class="sim-label">Size</span>
              <span class="sim-value mono">{formatBytes(bestAlt.size)}</span>
            </div>
          {/if}
          <div class="sim-kv">
            <span class="sim-label">Best Score</span>
            <span class="sim-value mono score-total">{simulationResult.best_score}</span>
          </div>
        </div>

        <h3 class="subsection-title">Per-Alternate Scores <HelpIcon tooltip="Each alternate is scored against the requested capability mask. The highest-scoring alternate is served. Format match dominates (+1000), with viewport, encoding, density, and save-data as tiebreakers." /></h3>
        <table class="data-table breakdown-table">
          <thead>
            <tr>
              <th scope="col" class="num-col">ID</th>
              <th scope="col">Type</th>
              <th scope="col" class="num-col">Size</th>
              <th scope="col" class="num-col">Score</th>
            </tr>
          </thead>
          <tbody>
            {#each simulationResult.alternates as entry, i}
              <tr class:alt-selected={i === simulationResult.best_index}>
                <td class="num-col mono">{formatHex(entry.alternate_id)}</td>
                <td class="mono">{entry.is_sentinel ? entry.sentinel_name ?? 'sentinel' : entry.content_type ?? ''}</td>
                <td class="num-col mono">{formatBytes(entry.size)}</td>
                <td class="num-col mono">{entry.score}</td>
              </tr>
            {/each}
          </tbody>
        </table>
        <p class="scoring-legend">
          Higher total score = better match for the requested capability mask.
          Viewport (+2000) dominates, then format match (+1000); encoding (+60), density (+40),
          and save-data (+20) are tiebreakers. Original content scores +100 as a fallback.
        </p>
      </div>
    {/if}
  </div>
{:else}
  <!-- ═══════════════════════════════════════════════════════════════
       URL List View
       ═══════════════════════════════════════════════════════════════ -->
  <div class="url-list">
    <div class="list-header">
      <h1 class="list-title">URL Inspector</h1>
      <div class="list-actions">
        {#if clearCacheConfirm}
          <span class="confirm-label">Clear all cached content?</span>
          <button class="ps-btn ps-btn-danger" onclick={clearAllCache} disabled={clearingCache}>
            {clearingCache ? 'Clearing...' : 'Confirm'}
          </button>
          <button class="ps-btn ps-btn-ghost" onclick={() => (clearCacheConfirm = false)} disabled={clearingCache}>Cancel</button>
        {:else}
          <button
            class="ps-btn ps-btn-secondary"
            onclick={() => (clearCacheConfirm = true)}
            disabled={totalUrls === 0}
            title="Delete all cached content. Variants will be regenerated on next request."
          >Clear Cache</button>
        {/if}
      </div>
    </div>

    {#if actionMessage && !selectedUrl}
      <div class="action-feedback ps-badge ps-badge-success">{actionMessage}</div>
    {/if}
    {#if actionError && !selectedUrl}
      <div class="action-feedback ps-badge ps-badge-error">{actionError}</div>
    {/if}

    <div class="search-bar">
      <input
        type="text"
        class="ps-input"
        placeholder="Filter by hostname..."
        bind:value={searchFilter}
        oninput={handleSearchInput}
      />
    </div>

    {#if loading}
      <div class="loading-row">
        <div class="ps-spinner" aria-label="Loading URLs"></div>
        <span>Loading cached URLs...</span>
      </div>
    {:else if error}
      <div class="ps-card">
        <p class="error-text">{error}</p>
        <button class="ps-btn ps-btn-secondary" onclick={fetchUrls}>Retry</button>
      </div>
    {:else if urls.length === 0}
      <div class="ps-card empty-card">
        <p class="empty-text">No cached URLs found.</p>
        {#if searchFilter.trim()}
          <p class="empty-hint">Try clearing the filter or using a different hostname.</p>
        {/if}
      </div>
    {:else}
      <div class="ps-card table-card">
        <div class="table-scroll">
          <table class="data-table url-table">
            <thead>
              <tr>
                <th scope="col">URL</th>
                <th scope="col">Hostname</th>
                <th scope="col" class="num-col">Variants</th>
                <th scope="col">Status</th>
              </tr>
            </thead>
            <tbody>
              {#each urls as entry}
                {@const statusKey = `${entry.hostname}|${entry.url}`}
                {@const status = urlStatuses.get(statusKey)}
                {@const entryCooldown = activeCooldowns.get(statusKey)}
                <tr class="url-row" onclick={() => selectUrlEntry(entry)}>
                  <td class="url-cell" title={entry.url}>{#if (entry.scheme ?? 'https') === 'http'}<span class="scheme-badge scheme-http" title="HTTP (not HTTPS)">http</span>{/if}{truncateUrl(entry.url)}</td>
                  <td>{entry.hostname}</td>
                  <td class="num-col mono">{entry.alternate_count}</td>
                  <td>
                    {#if status}
                      <span class="status-badge status-{status}">
                        {status === 'complete' ? 'Complete' : status === 'partial' ? 'Partial' : status === 'original-only' ? 'Original Only' : 'Revalidating'}
                      </span>
                    {:else}
                      <span class="no-data">--</span>
                    {/if}
                    {#if entryCooldown}
                      <span
                        class="cooldown-badge {cooldownColorClass(entryCooldown.reason)}"
                        title={cooldownTooltip(entryCooldown.reason)}
                      >
                        {cooldownLabel(entryCooldown.reason)}
                      </span>
                    {/if}
                  </td>
                </tr>
              {/each}
            </tbody>
          </table>
        </div>
      </div>

      <div class="pagination">
        <button
          class="ps-btn ps-btn-secondary"
          onclick={prevPage}
          disabled={currentPage <= 0}
        >
          Prev
        </button>
        <span class="page-info">
          Showing page {currentPage + 1} of {totalPages} ({totalUrls} total)
        </span>
        <button
          class="ps-btn ps-btn-secondary"
          onclick={nextPage}
          disabled={currentPage >= totalPages - 1}
        >
          Next
        </button>
      </div>
    {/if}

    <!-- URL Groups Section -->
    <div class="url-groups-section">
      <div class="ps-card">
        <UrlGroupManager />
      </div>
    </div>
  </div>
{/if}

<style>
  /* ── Shared layout ──────────────────────────────────────────────── */

  .page-center {
    display: flex;
    align-items: center;
    justify-content: center;
    min-height: 60vh;
    padding: var(--ps-space-xl);
  }

  .center-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 320px;
    text-align: center;
  }

  .center-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  .loading-row {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-lg);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
  }

  .error-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-error);
    margin: 0 0 var(--ps-space-sm);
  }

  .empty-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
  }

  .empty-hint {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    margin: var(--ps-space-xs) 0 0;
  }

  .empty-card {
    text-align: center;
    padding: var(--ps-space-xl);
  }

  .mono {
    font-family: var(--ps-font-mono);
  }

  /* ── Shared table styles ────────────────────────────────────────── */

  .table-scroll {
    overflow-x: auto;
  }

  .table-card {
    overflow: hidden;
  }

  .data-table {
    width: 100%;
    border-collapse: collapse;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .data-table th {
    text-align: left;
    font-weight: 500;
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-bottom: 1px solid var(--ps-border);
    white-space: nowrap;
  }

  .data-table td {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    color: var(--ps-fg-primary);
    border-bottom: 1px solid var(--ps-border);
  }

  .data-table tbody tr:last-child td {
    border-bottom: none;
  }

  .num-col {
    text-align: right;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .sortable {
    cursor: pointer;
    user-select: none;
  }

  .sortable:hover {
    color: var(--ps-accent);
  }

  /* ── URL List View ──────────────────────────────────────────────── */

  .url-list {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1200px;
  }

  .list-header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
  }

  .list-actions {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  .confirm-label {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .list-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
  }

  .search-bar {
    margin-bottom: var(--ps-space-md);
    max-width: 400px;
  }

  .url-table .url-row {
    cursor: pointer;
    transition: background-color 0.1s ease;
  }

  .url-table .url-row:hover {
    background: var(--ps-bg-hover);
  }

  .url-cell {
    max-width: 500px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .scheme-badge {
    display: inline-block;
    font-size: 0.65em;
    font-weight: 600;
    text-transform: uppercase;
    padding: 1px 4px;
    border-radius: 3px;
    margin-right: 4px;
    vertical-align: middle;
    line-height: 1.4;
  }
  .scheme-http {
    background: var(--ps-warning-bg, #fef3cd);
    color: var(--ps-warning-text, #856404);
    border: 1px solid var(--ps-warning-border, #ffc107);
  }

  .pagination {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: var(--ps-space-md);
    margin-top: var(--ps-space-lg);
  }

  .page-info {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  /* ── Inspector Detail View ──────────────────────────────────────── */

  .inspector {
    padding: var(--ps-space-lg) var(--ps-space-xl);
    max-width: 1200px;
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-lg);
  }

  .inspector-header {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
  }

  .back-btn {
    align-self: flex-start;
  }

  .inspector-url-info {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    flex-wrap: wrap;
  }

  .inspector-title {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0;
    word-break: break-all;
  }

  .inspector-title a {
    color: inherit;
    text-decoration: none;
  }

  .inspector-title a:hover {
    text-decoration: underline;
  }

  .inspector-actions {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    flex-wrap: wrap;
  }

  .confirm-prompt {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-warning);
    font-weight: 500;
  }

  .action-feedback {
    align-self: flex-start;
  }

  .section-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md);
  }

  .subsection-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-fg-secondary);
    margin: var(--ps-space-lg) 0 var(--ps-space-sm);
  }

  /* Alternate row highlighting */
  .alt-selected {
    background: color-mix(in srgb, var(--ps-accent) 12%, transparent);
  }

  /* ── Mask Builder ───────────────────────────────────────────────── */

  .mask-builder {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(160px, 1fr));
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
  }

  .mask-field {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .mask-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
  }

  .mask-display {
    display: flex;
    align-items: center;
    gap: var(--ps-space-lg);
    padding: var(--ps-space-sm) 0;
    margin-bottom: var(--ps-space-sm);
  }

  .mask-computed {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    font-weight: 600;
    color: var(--ps-fg-primary);
  }

  .mask-human {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .simulate-btn {
    align-self: flex-start;
  }

  /* ── Simulation Results ─────────────────────────────────────────── */

  .sim-summary {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-sm);
  }

  .sim-kv {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .sim-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .sim-value {
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
  }

  .score-total {
    font-weight: 600;
    color: var(--ps-accent);
  }

  .breakdown-table {
    max-width: 400px;
  }

  .scoring-legend {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    margin: var(--ps-space-sm) 0 0;
    line-height: 1.5;
  }

  /* ── Expandable row + detail panel ────────────────────────────────── */

  .alt-row {
    cursor: pointer;
    transition: background-color 0.1s ease;
  }

  .alt-row:hover {
    background: var(--ps-bg-hover);
  }

  .alt-expanded {
    background: color-mix(in srgb, var(--ps-accent) 6%, transparent);
  }

  .expand-arrow {
    display: inline-block;
    font-size: 8px;
    margin-right: var(--ps-space-xs);
    transition: transform 0.15s ease;
    color: var(--ps-fg-muted);
  }

  .expand-arrow.expanded {
    transform: rotate(90deg);
  }

  .revalidation-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: 10px;
    font-weight: 600;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
    color: var(--ps-warning);
    margin-left: var(--ps-space-xs);
    vertical-align: middle;
  }

  .freshness-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: 10px;
    font-weight: 600;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
  }

  .freshness-badge.fresh {
    background: color-mix(in srgb, var(--ps-success) 15%, transparent);
    color: var(--ps-success);
  }

  .freshness-badge.stale {
    background: color-mix(in srgb, var(--ps-error) 15%, transparent);
    color: var(--ps-error);
  }

  .freshness-badge.no-cc {
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
    color: var(--ps-warning);
  }

  .freshness-badge.heuristic {
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
    color: var(--ps-warning);
  }

  .detail-row td {
    padding: 0 !important;
    border-bottom: 1px solid var(--ps-border);
  }

  .detail-panel {
    display: grid;
    grid-template-columns: 280px 1fr;
    gap: var(--ps-space-md);
    padding: var(--ps-space-md) var(--ps-space-lg);
    background: var(--ps-bg-secondary);
    border-top: 1px solid var(--ps-border);
  }

  .detail-meta-grid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: var(--ps-space-xs) var(--ps-space-md);
    align-items: baseline;
  }

  .detail-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    white-space: nowrap;
  }

  .detail-value {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-primary);
    word-break: break-all;
  }

  .detail-content {
    min-height: 60px;
    max-height: 400px;
    overflow: auto;
  }

  .detail-image-preview {
    display: flex;
    justify-content: center;
    align-items: flex-start;
    padding: var(--ps-space-sm);
  }

  .preview-image {
    max-width: 100%;
    max-height: 360px;
    object-fit: contain;
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
  }

  .detail-code-preview {
    overflow: auto;
  }

  .code-block {
    margin: 0;
    padding: var(--ps-space-sm);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    line-height: 1.5;
    white-space: pre-wrap;
    word-break: break-all;
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
  }

  .detail-sentinel {
    padding: var(--ps-space-sm);
  }

  .sentinel-list {
    list-style: none;
    margin: 0;
    padding: 0;
  }

  .sentinel-item {
    padding: var(--ps-space-xs) 0;
    border-bottom: 1px solid var(--ps-border);
    font-size: var(--ps-font-size-sm);
  }

  .sentinel-item:last-child {
    border-bottom: none;
  }

  @media (max-width: 767px) {
    .detail-panel {
      grid-template-columns: 1fr;
    }
  }

  /* ── Quality inspector additions ─────────────────────────────────── */

  .size-col {
    min-width: 180px;
  }

  .no-data {
    color: var(--ps-fg-muted);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  /* ── Per-URL status badges ─────────────────────────────────────── */

  .status-badge {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 4px;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    white-space: nowrap;
  }

  .status-complete {
    background: color-mix(in srgb, var(--ps-success) 15%, transparent);
    color: var(--ps-success);
  }

  .status-partial {
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
    color: var(--ps-warning);
  }

  .status-original-only {
    background: color-mix(in srgb, var(--ps-error) 15%, transparent);
    color: var(--ps-error);
  }

  .status-revalidating {
    background: color-mix(in srgb, var(--ps-accent) 15%, transparent);
    color: var(--ps-accent);
  }

  .quality-dist-desc {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    margin: 0 0 var(--ps-space-md);
  }

  /* ── Add to Group dropdown ────────────────────────────────────────── */

  .add-to-group-wrapper {
    position: relative;
  }

  .add-to-group-dropdown {
    position: absolute;
    top: 100%;
    left: 0;
    z-index: 100;
    min-width: 220px;
    padding: var(--ps-space-xs) 0;
    background: var(--ps-bg-primary);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-radius-sm);
    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
    margin-top: var(--ps-space-xs);
  }

  .dropdown-empty {
    display: block;
    padding: var(--ps-space-sm) var(--ps-space-md);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  .dropdown-item {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    width: 100%;
    padding: var(--ps-space-xs) var(--ps-space-md);
    background: none;
    border: none;
    cursor: pointer;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
    text-align: left;
  }

  .dropdown-item:hover {
    background: var(--ps-bg-hover);
  }

  .dropdown-meta {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
  }

  /* ── SVG Detail Panel ──────────────────────────────────────────── */

  .svg-detail-section {
    padding: var(--ps-space-md) 0;
    border-bottom: 1px solid var(--ps-border);
  }

  .svg-detail-section:last-child {
    border-bottom: none;
  }

  .svg-detail-grid {
    display: grid;
    grid-template-columns: auto 1fr;
    gap: var(--ps-space-xs) var(--ps-space-lg);
    align-items: baseline;
  }

  .svg-detail-label {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
    white-space: nowrap;
  }

  .svg-detail-value {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
  }

  .svg-detail-note {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted);
    font-style: italic;
    margin: var(--ps-space-sm) 0 0;
    line-height: 1.5;
  }

  /* ── SVG Candidacy ──────────────────────────────────────────────── */

  .svg-candidate-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: 10px;
    font-weight: 600;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    background: var(--ps-quality-illustration-bg);
    color: var(--ps-quality-illustration-fg);
    margin-left: var(--ps-space-xs);
    vertical-align: middle;
  }

  .svg-candidacy-card {
    border-left: 3px solid var(--ps-quality-illustration-fg);
  }

  .svg-candidacy-text {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0;
    line-height: 1.6;
  }

  /* ── Empty alternates CTA ────────────────────────────────────────── */

  .empty-alternates {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    padding: var(--ps-space-xl) var(--ps-space-lg);
    text-align: center;
  }

  .empty-reprocess-confirm {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
  }

  /* ── URL Groups section ──────────────────────────────────────────── */

  .url-groups-section {
    margin-top: var(--ps-space-xl);
    padding-top: var(--ps-space-lg);
    border-top: 1px solid var(--ps-border);
  }

  /* ── Cooldown badges & banner ───────────────────────────────────── */

  .cooldown-badge {
    display: inline-flex;
    align-items: center;
    padding: 1px var(--ps-space-sm);
    font-family: var(--ps-font-family);
    font-size: 10px;
    font-weight: 600;
    line-height: 1.5;
    border-radius: 10px;
    white-space: nowrap;
    margin-left: var(--ps-space-xs);
    vertical-align: middle;
  }

  .cooldown-badge.processing {
    color: var(--ps-accent);
    background: color-mix(in srgb, var(--ps-accent) 15%, transparent);
  }

  .cooldown-badge.write-failure {
    color: var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 15%, transparent);
  }

  .cooldown-badge.revalidation {
    color: var(--ps-info, var(--ps-accent));
    background: color-mix(in srgb, var(--ps-info, var(--ps-accent)) 15%, transparent);
  }

  .cooldown-banner {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-radius: var(--ps-radius-sm);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .cooldown-banner-processing {
    border-left: 3px solid var(--ps-accent);
    background: color-mix(in srgb, var(--ps-accent) 8%, transparent);
    color: var(--ps-fg-primary);
  }

  .cooldown-banner-write-failure {
    border-left: 3px solid var(--ps-warning);
    background: color-mix(in srgb, var(--ps-warning) 8%, transparent);
    color: var(--ps-fg-primary);
  }

  .cooldown-banner-revalidation {
    border-left: 3px solid var(--ps-info, var(--ps-accent));
    background: color-mix(in srgb, var(--ps-info, var(--ps-accent)) 8%, transparent);
    color: var(--ps-fg-primary);
  }

  .cooldown-banner-icon {
    font-size: var(--ps-font-size-lg);
  }

  .cooldown-banner-text {
    flex: 1;
    font-weight: 500;
  }

  /* ── Auto-refresh ──────────────────────────────────────────────── */

  .section-title-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-md);
  }

  .section-title-row .section-title {
    margin-bottom: 0;
  }

  .auto-refresh-toggle {
    flex-shrink: 0;
  }

  .auto-refresh-label {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
    cursor: pointer;
    user-select: none;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-secondary);
  }

  .auto-refresh-checkbox {
    accent-color: var(--ps-accent);
    cursor: pointer;
  }

  .auto-refresh-text {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .auto-refresh-dot {
    display: inline-block;
    width: 6px;
    height: 6px;
    border-radius: 50%;
    background: var(--ps-success);
    animation: auto-refresh-pulse 2s ease-in-out infinite;
  }

  @keyframes auto-refresh-pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.3; }
  }

  /* ── Responsive ─────────────────────────────────────────────────── */

  @media (max-width: 767px) {
    .url-list,
    .inspector {
      padding: var(--ps-space-md);
    }

    .mask-builder {
      grid-template-columns: 1fr 1fr;
    }

    .sim-summary {
      grid-template-columns: 1fr 1fr;
    }

    .inspector-url-info {
      flex-direction: column;
      align-items: flex-start;
    }

    .pagination {
      flex-direction: column;
      gap: var(--ps-space-sm);
    }
  }
</style>
