<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Support: the console's single, gentle pointer to support subscriptions,
  // the documentation, and a diagnostics bundle to paste into a request.
  // No license state, no activation flow, no nag — the panel is dismissible
  // and its dismissal is remembered (support-panel.ts); everything below it
  // is permanent. The diagnostics bundle reads this console's own pages once
  // when the page opens (and again on request) and is shown as plain text to
  // copy or download; nothing is sent anywhere (diagnostics.ts).
  import { useConsole } from "$lib/api/context";
  import PageHeader from "$lib/PageHeader.svelte";
  import {
    DOCS,
    DOCS_URL,
    PRODUCT_NAME,
    SUPPORT_PAGE_URL,
    SUPPORT_TERMS_URL,
    SUPPORT_URL,
    VENDOR,
  } from "$lib/data/product-facts-console";
  import {
    loadSupportDismissed,
    saveSupportDismissed,
    supportSentence,
  } from "$lib/utils/support-panel";
  import {
    collectDiagnostics,
    copyText,
    diagnosticsFileName,
    diagnosticsText,
    downloadText,
    type DiagnosticsData,
  } from "$lib/utils/diagnostics";
  import { detectedDialect } from "$lib/utils/dialect";

  const { api, scope } = useConsole();
  const build = import.meta.env.VITE_APP_VERSION ?? "dev";

  let dismissed = $state(loadSupportDismissed());
  let data = $state<DiagnosticsData | null>(null);
  let reading = $state(false);
  let keepQueryStrings = $state(false);
  let includeConfig = $state(false);
  let status = $state<"" | "copied" | "manual" | "downloaded">("");
  // The scope settles when the shell's configuration read answers, which
  // may be after this page's reading: the text names it as it is now.
  let where = $derived(scope.isGlobal ? "whole server" : `this host (${scope.host || "unknown host"})`);
  let text = $derived(
    data === null
      ? ""
      : diagnosticsText(data, where, { queryStrings: keepQueryStrings, configText: includeConfig }),
  );

  function setDismissed(value: boolean) {
    dismissed = value;
    saveSupportDismissed(value);
  }

  async function readDiagnostics() {
    reading = true;
    status = "";
    try {
      data = await collectDiagnostics(api, {
        build,
        userAgent: globalThis.navigator?.userAgent ?? "",
        server: detectedDialect(),
      });
    } finally {
      reading = false;
    }
  }

  async function copyDiagnostics() {
    status = (await copyText(text)) ? "copied" : "manual";
  }

  function downloadDiagnostics() {
    if (data === null) return;
    status = downloadText(text, diagnosticsFileName(data.generatedAt)) ? "downloaded" : "manual";
  }

  // One reading when the page opens; "Read again" takes another.
  void readDiagnostics();
</script>

<div class="page">
  <PageHeader title="Support" />

  {#if !dismissed}
    <div class="card support-panel" data-testid="support-panel">
      <button
        class="dismiss"
        onclick={() => setDismissed(true)}
        aria-label="Dismiss support panel"
      >&times;</button>
      <p class="support-text">
        {supportSentence(PRODUCT_NAME, VENDOR)}
      </p>
      <a
        href={SUPPORT_URL}
        target="_blank"
        rel="noopener noreferrer"
        class="support-link"
      >View support subscriptions &rarr;</a>
    </div>
  {:else}
    <p class="note">
      The support panel is hidden.
      <button class="show-again" onclick={() => setDismissed(false)}>
        Show again
      </button>
    </p>
  {/if}

  <section class="docs-section" aria-labelledby="support-docs-heading">
    <h2 class="section-title" id="support-docs-heading">Documentation and help</h2>
    <ul class="docs-list">
      <li><a href={DOCS_URL} target="_blank" rel="noopener noreferrer">Admin console documentation</a></li>
      <li><a href={SUPPORT_URL} target="_blank" rel="noopener noreferrer">Support subscriptions</a></li>
      <li><a href={SUPPORT_TERMS_URL} target="_blank" rel="noopener noreferrer">Support terms</a></li>
    </ul>
  </section>

  <section class="diagnostics-section" aria-labelledby="support-diagnostics-heading">
    <h2 class="section-title" id="support-diagnostics-heading">Diagnostics</h2>
    <p class="note">
      Read once when this page opens; nothing is sent anywhere. Treat it as public and review it before you
      share it.
    </p>
    <div class="control-row diagnostics-options">
      <label class="field-check">
        <input type="checkbox" bind:checked={keepQueryStrings} data-testid="diagnostics-query-strings" />
        Keep query strings
      </label>
      <label class="field-check">
        <input type="checkbox" bind:checked={includeConfig} data-testid="diagnostics-config" />
        Include the configuration
      </label>
    </div>
    <div class="control-row diagnostics-actions">
      <div class="control-group">
        <button
          type="button"
          class="btn btn-primary"
          data-testid="copy-diagnostics"
          disabled={data === null}
          onclick={copyDiagnostics}
        >Copy diagnostics</button>
        <button
          type="button"
          class="btn btn-secondary"
          data-testid="download-diagnostics"
          disabled={data === null}
          onclick={downloadDiagnostics}
        >Download .txt</button>
        <button
          type="button"
          class="btn btn-secondary"
          data-testid="reread-diagnostics"
          disabled={reading}
          onclick={readDiagnostics}
        >Read again</button>
      </div>
    </div>
    <p class="note diagnostics-where" data-testid="diagnostics-where">
      Where to send it: with a
      <a href={SUPPORT_PAGE_URL} target="_blank" rel="noopener noreferrer">support request</a>,
      or paste it into an issue report. The
      <a href={DOCS.diagnose} target="_blank" rel="noopener noreferrer">troubleshooting guide</a>
      says what else helps.
    </p>
    <p class="diagnostics-status" role="status" data-testid="diagnostics-status">
      {#if reading}
        Reading…
      {:else if status === "copied"}
        Copied to the clipboard.
      {:else if status === "manual"}
        This browser did not allow it; select the text below and copy it.
      {:else if status === "downloaded"}
        Downloaded as a text file.
      {/if}
    </p>
    <label class="diagnostics-label" for="diagnostics-text">Diagnostics text</label>
    <textarea
      id="diagnostics-text"
      class="diagnostics-text"
      data-testid="diagnostics-text"
      readonly
      rows="16"
      value={text}
    ></textarea>
  </section>
</div>

<style>
  /* A card: as wide as its text, not the window. */
  .support-panel {
    max-width: var(--ps-card-max);
    position: relative;
  }

  .dismiss {
    position: absolute;
    top: var(--ps-space-sm);
    right: var(--ps-space-sm);
    background: none;
    border: none;
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-lg);
    line-height: 1;
    cursor: pointer;
    padding: var(--ps-space-xs);
    border-radius: var(--ps-border-radius);
    /* At least 24 px square: a pointer target (WCAG 2.5.8). */
    min-width: 24px;
    min-height: 24px;
  }

  .dismiss:hover {
    color: var(--ps-text);
    background: var(--ps-surface-hover);
  }

  .support-text {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    line-height: 1.5;
    margin-bottom: var(--ps-space-md);
    padding-right: var(--ps-space-lg);
  }

  .support-link {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
    text-decoration: none;
    font-weight: 500;
  }

  .support-link:hover {
    text-decoration: underline;
  }

  .docs-section,
  .diagnostics-section {
    margin-top: var(--ps-space-lg);
  }

  .docs-list {
    margin: var(--ps-space-sm) 0 0 var(--ps-space-md);
    display: grid;
    gap: var(--ps-space-xs);
  }

  .show-again {
    background: none;
    border: none;
    padding: 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
    cursor: pointer;
    text-decoration: underline;
  }

  .diagnostics-options,
  .diagnostics-actions {
    margin-bottom: var(--ps-space-sm);
  }

  .diagnostics-status {
    min-height: 1.4em;
    font-size: var(--ps-font-size-sm);
  }

  .diagnostics-label {
    display: block;
    margin-bottom: var(--ps-space-xs);
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
  }

  .diagnostics-text {
    width: 100%;
    box-sizing: border-box;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    background: var(--ps-bg-secondary);
    color: var(--ps-text);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    padding: var(--ps-space-sm);
  }
</style>
