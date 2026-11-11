<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import type { ConfigResponse } from "$lib/api/types";
  import LoadError from "$lib/LoadError.svelte";
  import EmptyState from "$lib/EmptyState.svelte";
  import UnavailableCard, { DOCS_URL } from "$lib/UnavailableCard.svelte";
  import { isUnavailable } from "$lib/api/client";
  import PageHeader from "$lib/PageHeader.svelte";
  import Tabs from "$lib/Tabs.svelte";
  import { displayHost, scopeLine } from "$lib/utils/config-scope";
  import { useConsole } from "$lib/api/context";

  const { api } = useConsole();

  let data = $state<ConfigResponse | null>(null);
  let error = $state<Error | string | null>(null);
  let loading = $state(true);
  // The server has turned this view off: said once, not retried.
  let off = $state(false);
  let updatedAt = $state<number | null>(null);
  // Which text the config-wrapper shows: the server-wide config, or the
  // options actually in effect for the request that hit this vhost.
  type ConfigView = "server" | "effective";
  let view = $state<ConfigView>("server");
  const CONFIG_TABS: ReadonlyArray<{ id: ConfigView; label: string }> = [
    { id: "server", label: "Server config" },
    { id: "effective", label: "Effective for this request" },
  ];

  async function fetchConfig() {
    loading = true;
    error = null;
    try {
      data = await api.getConfig();
      updatedAt = Date.now();
    } catch (err) {
      if (isUnavailable(err)) off = true;
      else error = err instanceof Error ? err : String(err);
    } finally {
      loading = false;
    }
  }

  // Fetch on mount.
  fetchConfig();

  let hasEffectiveConfig = $derived(data?.effective_config !== undefined);

  let configText = $derived(
    view === "effective" && data?.effective_config !== undefined
      ? data.effective_config
      : (data?.config ?? (data as Record<string, unknown>)?.raw ?? ""),
  );
</script>

<div class="page">
  <PageHeader
    title="Configuration"
    subtitle={scopeLine(data ? { scope: data.scope, host: displayHost(data.host, window.location.host) } : {})}
    updatedAt={updatedAt}
  >
    {#snippet toolbar()}
      {#if !off}
        <button type="button" class="btn btn-secondary" onclick={fetchConfig} disabled={loading}>
          {loading ? "Loading..." : "Refresh"}
        </button>
      {/if}
    {/snippet}
  </PageHeader>

  {#if off}
    <UnavailableCard testid="config-unavailable" />
  {:else if loading && !data}
    <p class="loading">Loading configuration...</p>
  {:else if error}
    <LoadError {error} noun="the configuration" onretry={fetchConfig} />
  {:else if configText}
    {#if hasEffectiveConfig}
      <Tabs
        tabs={CONFIG_TABS}
        active={view}
        label="Configuration view"
        idPrefix="config"
        panelId="config-panel"
        onselect={(id) => (view = id as ConfigView)}
      />
    {/if}
    <div
      role="tabpanel"
      id="config-panel"
      aria-labelledby={hasEffectiveConfig ? `config-tab-${view}` : undefined}
    >
      <!-- A box that may scroll is a named region that takes keyboard focus (WCAG 2.1.1). -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div class="config-wrapper" role="region" aria-label={hasEffectiveConfig && view === "effective" ? "Effective configuration" : "Server configuration"} tabindex="0">
        <pre class="config-block">{configText}</pre>
      </div>
    </div>
  {:else}
    <EmptyState
      testid="config-empty"
      title="The server sent no configuration text."
      detail="The answer arrived, but it was empty. Refresh asks again; if it stays empty, the web server's error log may say why."
      action={{ label: "About the admin console", href: DOCS_URL }}
    />
  {/if}
</div>

<style>
  .config-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
    max-height: 80vh;
  }

  .config-block {
    padding: var(--ps-space-md);
    margin: 0;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
    line-height: 1.6;
    white-space: pre-wrap;
    word-break: break-word;
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
  }

  .loading {
    color: var(--ps-text-secondary);
  }
</style>
