<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // The HTML cache report, from the module's own counters (html-cache.ts holds
  // the arithmetic and the wording). A read that fails shows nothing here: the
  // statistics page reports that on its own. The mode is read from the
  // configuration once; until that read succeeds it is inferred, and a failed
  // read is tried again only after a growing wait, not on every poll.
  import { usePolling } from "$lib/api/polling.svelte";
  import { useConsole } from "$lib/api/context";
  import { formatCount } from "$lib/utils/format";
  import {
    HTML_CACHE_TEXT as text,
    configRetryDelayMs,
    htmlCacheMode,
    htmlCacheView,
    type HtmlCacheMode,
  } from "$lib/utils/html-cache";

  const { api } = useConsole();
  let mode: HtmlCacheMode = null;
  let configFailures = 0;
  let configRetryAt = 0;
  const poll = usePolling(async () => {
    if (mode === null && Date.now() >= configRetryAt) {
      mode = await api.getConfig().then(htmlCacheMode, () => null);
      if (mode === null) {
        configFailures += 1;
        configRetryAt = Date.now() + configRetryDelayMs(configFailures);
      }
    }
    return htmlCacheView((await api.getStats()).variables, mode);
  });
  const view = $derived(poll.data);
</script>

{#if view}
  <section class="daemon-section" id="optimizer-html-cache" aria-labelledby="optimizer-html-cache-heading">
    <h2 id="optimizer-html-cache-heading" class="section-title">{text.heading}</h2>
    {#if view.state === "off"}
      <p class="note" data-testid="html-cache-off">{text.off}</p>
    {:else if view.state === "waiting"}
      <p class="note" data-testid="html-cache-waiting">{text.waiting}</p>
    {:else if view.state === "unavailable"}
      <p class="note" data-testid="html-cache-unavailable">{text.unavailable}</p>
    {:else}
      <p class="note">{text.intro}</p>
      <ul data-testid="html-cache-totals">
        <li>{text.evaluated}: {formatCount(view.evaluated)}</li>
        <li>{text.wouldStore}: {formatCount(view.wouldStore)}{#if view.wouldStorePercent !== null} ({view.wouldStorePercent}%){/if}</li>
        <li>{text.refused}: {formatCount(view.refused)}</li>
        {#if view.unavailable > 0}<li>{text.unavailableCount}: {formatCount(view.unavailable)}</li>{/if}
      </ul>
      {#if view.reasons.length > 0}
        <table aria-label={text.reasons}>
          <thead><tr><th scope="col">{text.reason}</th><th scope="col">{text.count}</th></tr></thead>
          <tbody>
            {#each view.reasons as row (row.name)}
              <tr><td>{row.name}</td><td>{formatCount(row.count)}</td></tr>
            {/each}
          </tbody>
        </table>
      {/if}
    {/if}
  </section>
{/if}
