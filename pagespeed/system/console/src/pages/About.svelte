<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  // About: the module build, the optimizer's version and commit, this
  // console's build, where the documentation is, and the legal links. Names
  // and URLs come from the product-facts single source. The optimizer is
  // asked once: this page does not poll.
  import PageHeader from "$lib/PageHeader.svelte";
  import { useConsole } from "$lib/api/context";
  import {
    DOCS_URL,
    PRIVACY_URL,
    PRODUCT_NAME,
    TERMS_URL,
    VENDOR,
    VENDOR_HOST,
    VENDOR_URL,
  } from "$lib/data/product-facts-console";
  import {
    isDirtyBuild,
    optimizerVersionsStatus,
    optimizerVersionText,
    type OptimizerVersionsStatus,
  } from "$lib/utils/versions";

  // Where the pages of the earlier console went: its name, and the page that holds it now.
  const OLD_CONSOLE_PAGES: ReadonlyArray<{ old: string; now: string; href: string }> = [
    { old: "Console (graphs)", now: "Statistics, Graphs view", href: "#/graphs" },
    { old: "Message History", now: "Logs, Module source", href: "#/logs?source=module" },
  ];

  const { api } = useConsole();
  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";
  const dirty = isDirtyBuild(appVersion);

  let optimizer = $state<OptimizerVersionsStatus | null>(null);
  optimizerVersionsStatus(() => api.daemonHealth()).then((status) => {
    optimizer = status;
  });
</script>

<div class="page">
  <PageHeader title="About" />

  <section class="about-section" aria-labelledby="about-versions">
    <h2 class="section-title" id="about-versions">Versions</h2>
    <!-- The module and the console ship from one build, so their rows show
         the same stamp: two labels, one source. -->
    <dl class="info-grid">
      <div class="info-item">
        <dt class="metric-label">Module build</dt>
        <dd
          class="info-value metric-value mono-value num"
          class:dirty-value={dirty}
          data-testid="about-module-version"
          title={appVersion}
        >
          {appVersion}
          {#if dirty}
            <span class="dirty-note">built with uncommitted changes</span>
          {/if}
        </dd>
      </div>
      <div class="info-item">
        <dt class="metric-label">Optimizer</dt>
        <dd class="info-value metric-value mono-value num" data-testid="about-optimizer-version">
          {optimizerVersionText(optimizer)}
        </dd>
      </div>
      <div class="info-item">
        <dt class="metric-label">{PRODUCT_NAME} console build</dt>
        <dd class="info-value metric-value mono-value num" data-testid="about-console-version">{appVersion}</dd>
      </div>
    </dl>
  </section>

  <section class="about-section" aria-labelledby="about-docs">
    <h2 class="section-title" id="about-docs">Documentation</h2>
    <p><a href={DOCS_URL} target="_blank" rel="noopener noreferrer" class="legal-link">Admin console documentation</a></p>
  </section>

  <section class="about-section" aria-labelledby="about-old-console" data-testid="about-old-console">
    <h2 class="section-title" id="about-old-console">Coming from the old console?</h2>
    <p class="note">Two pages of the earlier admin console have a new place. The others kept their names.</p>
    <dl class="old-console">
      {#each OLD_CONSOLE_PAGES as row (row.old)}
        <div class="old-console-row">
          <dt>{row.old}</dt>
          <dd><a href={row.href} class="legal-link old-console-link">{row.now}</a></dd>
        </div>
      {/each}
    </dl>
  </section>

  <section class="about-section" aria-labelledby="about-legal">
    <h2 class="section-title" id="about-legal">Legal</h2>
    <p class="note" data-testid="about-vendor">
      {PRODUCT_NAME} is published by {VENDOR}
      (<a href={VENDOR_URL} target="_blank" rel="noopener noreferrer" class="legal-link">{VENDOR_HOST}</a>).
    </p>
    <div class="legal-links">
      <a
        href={PRIVACY_URL}
        target="_blank"
        rel="noopener noreferrer"
        class="legal-link"
      >
        Privacy Policy
      </a>
      <a
        href={TERMS_URL}
        target="_blank"
        rel="noopener noreferrer"
        class="legal-link"
      >
        Terms of Service
      </a>
    </div>
  </section>
</div>

<style>
  .about-section {
    margin-bottom: var(--ps-space-xl);
  }

  .dirty-value {
    color: var(--ps-warning-text);
  }

  .dirty-note {
    display: block;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-xs);
    font-weight: 400;
  }

  dd {
    margin: 0;
  }

  .old-console {
    margin: 0;
  }

  .old-console-row {
    display: flex;
    flex-wrap: wrap;
    align-items: baseline;
    gap: var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
  }

  .old-console-row dt {
    min-width: 10rem;
  }

  /* Two links one above the other: each at least 24 px tall (WCAG 2.5.8). */
  .old-console-link {
    display: inline-flex;
    align-items: center;
    min-height: 24px;
  }

  .legal-links {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
  }

  .legal-link {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    text-decoration: underline;
  }

  .legal-link:hover {
    color: var(--ps-primary);
  }
</style>
