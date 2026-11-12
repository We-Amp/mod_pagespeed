<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { tick } from "svelte";
  import type { ConfigResponse } from "$lib/api/types";
  import LoadError from "$lib/LoadError.svelte";
  import EmptyState from "$lib/EmptyState.svelte";
  import UnavailableCard from "$lib/UnavailableCard.svelte";
  import { DOCS_URL } from "$lib/data/product-facts-console";
  import { isUnavailable } from "$lib/api/client";
  import type { RefreshOutcome } from "$lib/api/poller";
  import PageHeader from "$lib/PageHeader.svelte";
  import Tabs from "$lib/Tabs.svelte";
  import { displayHost, scopeLine } from "$lib/utils/config-scope";
  import { configView, differingLines, parseConfigText } from "$lib/utils/config-text";
  import { settledCount } from "$lib/utils/filter-status";
  import { useConsole } from "$lib/api/context";

  const { api } = useConsole();

  let data = $state<ConfigResponse | null>(null);
  let error = $state<Error | string | null>(null);
  let loading = $state(true);
  // The server has turned this view off: said once, not retried.
  let off = $state(false);
  let updatedAt = $state<number | null>(null);
  // Which text the page shows: the configuration of the server context that
  // answered (the whole server, or this site on a per-host console), or the
  // options in effect for this request.
  type ConfigView = "server" | "effective";
  let view = $state<ConfigView>("server");
  let site = $derived(data?.scope === "vhost");
  let configTabs = $derived([
    { id: "server", label: site ? "This site" : "Whole server" },
    { id: "effective", label: "Effective" },
  ]);
  // The first text, as the sentences name it.
  let serverNoun = $derived(site ? "this site's text" : "the whole-server text");
  const EFFECTIVE_NOUN = "the text in effect for this request";

  let query = $state("");

  // Asked for, never on a timer: the text changes only with the configuration.
  async function fetchConfig(): Promise<RefreshOutcome> {
    // A failure on screen stays until the answer arrives: the notice, and
    // the focus on its "Try again", survive the retry.
    loading = true;
    try {
      data = await api.getConfig();
      error = null;
      updatedAt = Date.now();
      return "ok";
    } catch (err) {
      if (isUnavailable(err)) off = true;
      else error = err instanceof Error ? err : String(err);
      return off ? "off" : "failed";
    } finally {
      loading = false;
    }
  }

  // Fetch on mount.
  fetchConfig();

  let hasEffectiveConfig = $derived(data?.effective_config !== undefined);

  let serverText = $derived(String(data?.config ?? (data as Record<string, unknown> | null)?.raw ?? ""));
  let effectiveText = $derived(data?.effective_config ?? "");
  let configText = $derived(view === "effective" && hasEffectiveConfig ? effectiveText : serverText);

  let parsedServer = $derived(parseConfigText(serverText));
  let parsedEffective = $derived(parseConfigText(effectiveText));
  let parsed = $derived(view === "effective" && hasEffectiveConfig ? parsedEffective : parsedServer);
  // The lines of the text on screen that the other tab's text does not have.
  let differing = $derived(
    hasEffectiveConfig
      ? view === "effective"
        ? differingLines(parsedEffective, parsedServer)
        : differingLines(parsedServer, parsedEffective)
      : new Set<number>(),
  );
  let shown = $derived(configView(parsed, query));
  let narrowed = $derived(query.trim() !== "");
  let countText = $derived(
    narrowed
      ? `${shown.shown} of ${shown.total} ${shown.total === 1 ? "line" : "lines"}`
      : `${shown.total} ${shown.total === 1 ? "line" : "lines"}`,
  );
  let regionLabel = $derived(
    hasEffectiveConfig && view === "effective"
      ? "Effective configuration"
      : site
        ? "This site's configuration"
        : "Whole-server configuration",
  );

  // The count, read out once the box is quiet (never on a tab change).
  let countStatus = $state("");
  const announceCount = settledCount((text) => (countStatus = text));

  let copyStatus = $state("");
  // The whole text of the tab on screen, not only the lines a filter shows.
  async function copyText() {
    // Emptied first, so a second Copy is announced again.
    copyStatus = "";
    await tick();
    try {
      await navigator.clipboard.writeText(configText);
      copyStatus = "Copied";
      return;
    } catch {
      // The browser refuses the clipboard on a console opened over plain HTTP.
    }
    const area = document.createElement("textarea");
    area.value = configText;
    area.setAttribute("readonly", "");
    area.className = "visually-hidden";
    // Selecting moves the focus; it goes back to the button afterwards.
    const focused = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    document.body.append(area);
    area.select();
    let copied = false;
    try {
      copied = document.execCommand("copy");
    } catch {
      // Not copied: said below.
    }
    area.remove();
    focused?.focus();
    copyStatus = copied ? "Copied" : "Not copied: select the text and copy it";
  }
</script>

<!-- A line the other tab's text lacks: the word, never the colour alone, a space apart from the line. -->
{#snippet mark(line: number)}{#if differing.has(line)}{" "}<span class="badge badge-tint-primary">differs</span>{/if}{/snippet}

{#snippet textLines(lines: ReadonlyArray<{ text: string; line: number }>)}
  <div class="config-lines">
    {#each lines as l (l.line)}
      <div class="config-line">{l.text}{@render mark(l.line)}</div>
    {/each}
  </div>
{/snippet}

<div class="page">
  <PageHeader
    title="Configuration"
    subtitle={scopeLine(data ? { scope: data.scope, host: displayHost(data.host, window.location.host) } : {})}
    updatedAt={updatedAt}
    refresh={off ? undefined : { onRefresh: fetchConfig }}
  >
    {#snippet toolbar()}
      {#if configText.trim()}
        <div class="control-group">
          <input
            type="search"
            class="field field-search config-search"
            placeholder="Filter lines..."
            aria-label="Filter configuration lines"
            data-testid="config-search"
            bind:value={query}
            oninput={() => {
              copyStatus = "";
              announceCount(() => countText);
            }}
          />
          <span class="field-text" data-testid="config-count">{countText}</span>
          <span class="visually-hidden" role="status" data-testid="config-count-status">{countStatus}</span>
        </div>
        <div class="control-group">
          <button type="button" class="btn btn-secondary" data-testid="config-copy" onclick={copyText}>Copy text</button>
          <span role="status" class="field-text" data-testid="config-copy-status">{copyStatus}</span>
        </div>
      {/if}
    {/snippet}
  </PageHeader>

  {#if off}
    <UnavailableCard testid="config-unavailable" />
  {:else if error}
    <LoadError {error} noun="the configuration" onretry={fetchConfig} />
  {:else if loading && !data}
    <p class="loading">Loading configuration...</p>
  {:else if configText.trim()}
    {#if hasEffectiveConfig}
      <Tabs
        tabs={configTabs}
        active={view}
        label="Configuration view"
        idPrefix="config"
        panelId="config-panel"
        onselect={(id) => {
          view = id as ConfigView;
          copyStatus = "";
        }}
      />
    {/if}
    <div
      role="tabpanel"
      id="config-panel"
      aria-labelledby={hasEffectiveConfig ? `config-tab-${view}` : undefined}
    >
      {#if hasEffectiveConfig}
        <div class="config-compare">
          {#if differing.size === 0}
            <p class="note" data-testid="config-same">{site ? "This site's" : "The whole-server"} text and {EFFECTIVE_NOUN} are the same.</p>
          {:else}
            <p class="note" data-testid="config-differs">
              {differing.size} {differing.size === 1 ? "line" : "lines"} not in {view === "effective" ? serverNoun : EFFECTIVE_NOUN},
              marked "differs".
            </p>
          {/if}
        </div>
      {/if}
      <!-- A box that may scroll is a named region that takes keyboard focus (WCAG 2.1.1). -->
      <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
      <div class="config-wrapper" role="region" aria-label={regionLabel} tabindex="0">
        <div class="config-block" data-testid="config-text" data-structured={parsed.structured}>
          {#if shown.blocks.length === 0}
            <p class="note config-none" data-testid="config-no-match">
              No line matches "{query.trim()}".
            </p>
          {:else if parsed.structured}
            {#each shown.blocks as block, b (b)}
              <section class="config-section">
                {#if block.kind === "filters"}
                  <h2 class="minor-title">Filters <span class="muted">({block.filters.length})</span></h2>
                  {#if block.filters.length === 0}
                    <p class="note">None set.</p>
                  {:else}
                    <ul class="config-filters" data-testid="config-filters">
                      {#each block.filters as f (f.line)}
                        <li>
                          <code class="config-id">{f.id}</code>
                          <span>{f.name}</span>
                          {@render mark(f.line)}
                        </li>
                      {/each}
                    </ul>
                  {/if}
                {:else if block.kind === "options"}
                  <h2 class="minor-title">Options <span class="muted">({block.options.length})</span></h2>
                  {#if block.options.length === 0}
                    <p class="note">None set.</p>
                  {:else}
                    <table class="data data-dense card-table config-options" data-testid="config-options">
                      <thead>
                        <tr><th scope="col">Option</th><th scope="col">ID</th><th scope="col">Value</th></tr>
                      </thead>
                      <tbody>
                        {#each block.options as o (o.line)}
                          <tr>
                            <th scope="row" data-label="Option">{o.name}</th>
                            <td data-label="ID"><code class="config-id">{o.id}</code></td>
                            <td data-label="Value">
                              <code class="config-value">{o.value}</code>
                              {@render mark(o.line)}
                            </td>
                          </tr>
                        {/each}
                      </tbody>
                    </table>
                  {/if}
                {:else}
                  {#if block.title !== null}<h2 class="minor-title">{block.title}</h2>{/if}
                  {#if block.lines.length === 0}
                    <p class="note">None set.</p>
                  {:else}
                    {@render textLines(block.lines)}
                  {/if}
                {/if}
              </section>
            {/each}
          {:else if narrowed && shown.blocks[0].kind === "text"}
            <!-- Text of another shape: the lines that pass, or all of it, blank lines included. -->
            {@render textLines(shown.blocks[0].lines)}
          {:else}
            {@render textLines(parsed.lines.map((text, line) => ({ text, line })))}
          {/if}
        </div>
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
  .config-search {
    width: 16rem;
    max-width: 100%;
  }

  .config-compare {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--ps-space-xs) var(--ps-space-md);
    margin-bottom: var(--ps-space-sm);
  }

  .config-compare p {
    margin: 0;
  }

  .config-wrapper {
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    overflow: auto;
    max-height: 80vh;
  }

  .config-block {
    padding: var(--ps-space-md);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
  }

  .config-section + .config-section {
    margin-top: var(--ps-space-lg);
  }

  .config-lines,
  .config-id,
  .config-value {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-sm);
  }

  .config-line {
    min-height: 1.6em;
    line-height: 1.6;
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .config-value {
    overflow-wrap: anywhere;
  }

  .config-filters {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(min(16rem, 100%), 1fr));
    gap: var(--ps-space-xs) var(--ps-space-md);
    margin: 0;
    padding: 0;
    list-style: none;
  }

  /* Id, name, mark: a long name wraps in its own column, never under the id. */
  .config-filters li {
    display: grid;
    grid-template-columns: minmax(3ch, auto) minmax(0, 1fr) auto;
    align-items: baseline;
    column-gap: var(--ps-space-sm);
  }

  .config-filters .config-id {
    color: var(--ps-text-secondary);
  }

  /* A long option name or value wraps; on a phone the cells are cards. */
  .config-options {
    background: var(--ps-surface);
    overflow-wrap: anywhere;
  }
</style>
