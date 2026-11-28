<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // The HTML cache panel, from the module's own counters (html-cache.ts holds
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
    htmlCacheServeUnavailable,
    htmlCacheView,
    type HtmlCacheMode,
  } from "$lib/utils/html-cache";

  const { api } = useConsole();
  let mode: HtmlCacheMode = null;
  let configFailures = 0;
  let configRetryAt = 0;
  // daemon_html_serve_unavailable at the previous poll: it only grows, so a
  // rise between two polls is what says serving is blocked now.
  let previousServeUnavailable: number | null = null;
  const poll = usePolling(async () => {
    if (mode === null && Date.now() >= configRetryAt) {
      mode = await api.getConfig().then(htmlCacheMode, () => null);
      if (mode === null) {
        configFailures += 1;
        configRetryAt = Date.now() + configRetryDelayMs(configFailures);
      }
    }
    const variables = (await api.getStats()).variables;
    const result = htmlCacheView(variables, mode, previousServeUnavailable);
    previousServeUnavailable = htmlCacheServeUnavailable(variables);
    return result;
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
      <p class="note" data-testid="html-cache-mode">
        {text.mode}: {view.mode === null ? text.modeUnknown : text.modeNames[view.mode]}{view.modeInferred ? ` ${text.inferred}` : ""}.
        {view.mode === "serve-variants" ? text.introServe : text.intro}
      </p>
      {#if view.mode === "serve-variants"}<p class="note" data-testid="html-cache-agent-optimize">{text.agentOptimizeOff}</p>{/if}
      <p class="note" data-testid="html-cache-log">{text.decisionLog}</p>
      {#if view.servingBlocked || view.servingUnavailableTotal > 0}
        <div data-testid={view.servingBlocked ? "html-cache-degraded" : "html-cache-degraded-history"}>
          {#if view.servingBlocked}
            <p class="note"><strong>{text.degraded}.</strong> {text.degradedIntro}</p>
          {:else}
            <p class="note">{formatCount(view.servingUnavailableTotal)} {text.unavailableSince}. {text.degradedHistory}</p>
          {/if}
          <ul>
            {#each text.degradedCauses as cause (cause)}<li>{cause}</li>{/each}
          </ul>
        </div>
      {/if}
      {#if view.warnings.length > 0}
        <div role="alert" data-testid="html-cache-warnings">
          <p class="note"><strong>{text.warnings}</strong></p>
          <ul>
            {#each view.warnings as w (w.name)}<li><code>{w.name}</code>: {formatCount(w.count)}. {w.text}</li>{/each}
          </ul>
        </div>
      {/if}
      {#if view.mode === "serve-variants" || view.stored > 0}
        <ul data-testid="html-cache-engine">
          <li>{text.stored}: {formatCount(view.stored)}. <span class="note">{text.storedNote}</span></li>
          <li>{text.removed}: {formatCount(view.removed)} ({text.removedOnRefusal}: {formatCount(view.removedOnRefusal)})</li>
          <li>{text.purgeRaceRefused}: {formatCount(view.purgeRaceRefused)}</li>
          <li>{text.restoreSkipped}: {formatCount(view.restoreSkipped)}</li>
          <li>{text.notifySent}: {formatCount(view.notifySent)}; {text.notifyLimited}: {formatCount(view.notifyLimited)}</li>
          <li>{text.markdownServed}: {formatCount(view.markdownServed)}; {text.markdownNotModified}: {formatCount(view.markdownNotModified)}</li>
          {#if view.markdownServedBr + view.markdownServedGzip + view.markdownServedIdentity > 0}
            <li>{text.markdownCodings}: {text.markdownBr} {formatCount(view.markdownServedBr)}; {text.markdownGzip} {formatCount(view.markdownServedGzip)}; {text.markdownIdentity} {formatCount(view.markdownServedIdentity)}</li>
          {/if}
          {#if view.encodedCopyMissing + view.encodedCopyUnbound + view.compressDemandSent > 0}
            <li>{text.encodedCopies}: {text.encodedMissing} {formatCount(view.encodedCopyMissing)}; {text.encodedUnbound} {formatCount(view.encodedCopyUnbound)}; {text.compressDemands} {formatCount(view.compressDemandSent)}</li>
          {/if}
          {#if view.codedFallback > 0}
            <li>{text.codedFallback}: {formatCount(view.codedFallback)}</li>
          {/if}
        </ul>
        {#if view.storeRefusals.length > 0}
          <table aria-label={text.storeRefusals}>
            <thead><tr><th scope="col">{text.storeRefusals}</th><th scope="col">{text.count}</th></tr></thead>
            <tbody>
              {#each view.storeRefusals as row (row.name)}
                <tr><td>{row.name}</td><td>{formatCount(row.count)}</td></tr>
              {/each}
            </tbody>
          </table>
        {/if}
        {#if view.fallthroughs.length > 0}
          <table aria-label={text.fallthroughs}>
            <thead><tr><th scope="col">{text.fallthroughs}</th><th scope="col">{text.count}</th></tr></thead>
            <tbody>
              {#each view.fallthroughs as row (row.name)}
                <tr><td>{row.name}</td><td>{formatCount(row.count)}</td></tr>
              {/each}
            </tbody>
          </table>
        {/if}
      {/if}
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
