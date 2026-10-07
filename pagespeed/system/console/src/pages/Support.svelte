<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // Support: the console's single, gentle pointer to support subscriptions,
  // the documentation, and a diagnostics bundle to paste into a request.
  // No license state, no activation flow, no nag — the panel is dismissible
  // and its dismissal is remembered (support-panel.ts); everything below it
  // is permanent. "Copy diagnostics" reads this console's own pages once and
  // copies plain text; nothing is sent anywhere (diagnostics.ts).
  import { useConsole } from "$lib/api/context";
  import PageHeader from "$lib/PageHeader.svelte";
  import {
    DOCS_URL,
    PRODUCT_NAME,
    SUPPORT_TERMS_URL,
    SUPPORT_URL,
    VENDOR,
  } from "$lib/data/product-facts-console";
  import {
    loadSupportDismissed,
    saveSupportDismissed,
    supportSentence,
  } from "$lib/utils/support-panel";
  import { collectDiagnostics, copyText } from "$lib/utils/diagnostics";

  const { api, scope } = useConsole();
  const build = import.meta.env.VITE_APP_VERSION ?? "dev";

  let dismissed = $state(loadSupportDismissed());
  let diagnostics = $state<"idle" | "working" | "copied" | "manual">("idle");
  let diagnosticsText = $state("");

  function setDismissed(value: boolean) {
    dismissed = value;
    saveSupportDismissed(value);
  }

  async function copyDiagnostics() {
    diagnostics = "working";
    const where = scope.isGlobal ? "whole server" : `this host (${scope.host || "unknown host"})`;
    diagnosticsText = await collectDiagnostics(api, where, build);
    diagnostics = (await copyText(diagnosticsText)) ? "copied" : "manual";
  }
</script>

<div class="page">
  <PageHeader title="Support" />

  {#if !dismissed}
    <div class="support-panel" data-testid="support-panel">
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
    <p class="dismissed-note">
      The support panel is hidden.
      <button class="show-again" onclick={() => setDismissed(false)}>
        Show again
      </button>
    </p>
  {/if}

  <section class="docs-section" aria-labelledby="support-docs-heading">
    <h2 id="support-docs-heading">Documentation and help</h2>
    <ul class="docs-list">
      <li><a href={DOCS_URL} target="_blank" rel="noopener noreferrer">Admin console documentation</a></li>
      <li><a href={SUPPORT_URL} target="_blank" rel="noopener noreferrer">Support subscriptions</a></li>
      <li><a href={SUPPORT_TERMS_URL} target="_blank" rel="noopener noreferrer">Support terms</a></li>
    </ul>
  </section>

  <section class="diagnostics-section" aria-labelledby="support-diagnostics-heading">
    <h2 id="support-diagnostics-heading">Diagnostics</h2>
    <p class="diagnostics-intro">
      Copies the module and console builds, the optimizer's version, a fingerprint of the
      configuration and the last 50 warnings and errors as plain text, for a support request.
      Nothing is sent anywhere. The messages can contain addresses your visitors requested,
      query strings included: review the text before you share it.
    </p>
    <button
      type="button"
      class="btn btn-secondary"
      data-testid="copy-diagnostics"
      disabled={diagnostics === "working"}
      onclick={copyDiagnostics}
    >Copy diagnostics</button>
    <p class="diagnostics-status" role="status" data-testid="diagnostics-status">
      {#if diagnostics === "working"}
        Collecting…
      {:else if diagnostics === "copied"}
        Copied to the clipboard.
      {:else if diagnostics === "manual"}
        This browser did not allow copying; select the text below and copy it.
      {/if}
    </p>
    {#if diagnostics === "manual"}
      <label class="diagnostics-label" for="diagnostics-text">Diagnostics text</label>
      <textarea
        id="diagnostics-text"
        class="diagnostics-text"
        data-testid="diagnostics-text"
        readonly
        rows="12"
        value={diagnosticsText}
      ></textarea>
    {/if}
  </section>
</div>

<style>
  /* A card: as wide as its text, not the window. */
  .support-panel {
    max-width: var(--ps-card-max);
    position: relative;
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
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

  .dismissed-note {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
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

  .diagnostics-intro {
    margin: var(--ps-space-sm) 0;
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
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
