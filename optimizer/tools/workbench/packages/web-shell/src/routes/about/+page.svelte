<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy, onMount } from 'svelte';
  import type { ConnectionManager } from '@pagespeed/api-client';
  import { formatUptime } from '@pagespeed/api-client';
  import type { HealthResponse } from '@pagespeed/api-client';
  import type { Readable } from 'svelte/store';

  const connectionManager = getContext<ConnectionManager>('connection');

  const SUPPORT_URL = 'https://we-amp.com/licensing/';

  // -- Health data subscription -----------------------------------------------

  let version = $state<string | undefined>(undefined);
  let uptimeSeconds = $state(0);

  let unsubHealth: (() => void) | undefined;

  function handleHealthUpdate(h: HealthResponse | null) {
    if (h) {
      version = h.version;
      uptimeSeconds = h.uptime_seconds;
    }
  }

  onMount(() => {
    unsubHealth = (connectionManager.healthData as Readable<HealthResponse | null>).subscribe(handleHealthUpdate);
  });

  onDestroy(() => {
    unsubHealth?.();
  });

  // -- Uptime formatting ------------------------------------------------------

  let uptimeFormatted = $derived(formatUptime(uptimeSeconds));
</script>

<div class="about-page">
  <!-- Section A: Version & System Info -->
  <section class="about-section">
    <h2 class="section-title">mod_pagespeed</h2>
    <div class="info-grid">
      <div class="info-item">
        <span class="info-label">Version</span>
        <span class="info-value">{version ?? 'unknown'}</span>
      </div>
      <div class="info-item">
        <span class="info-label">Uptime</span>
        <span class="info-value">{uptimeFormatted}</span>
      </div>
    </div>
  </section>

  <!-- Section B: Support -->
  <section class="about-section">
    <h2 class="section-title">Support</h2>
    <p class="support-text">
      mod_pagespeed is licensed under the Apache License 2.0.
      Support subscriptions:
      <a href={SUPPORT_URL} target="_blank" rel="noopener" class="support-link">{SUPPORT_URL}</a>
    </p>
  </section>

  <!-- Section C: Legal -->
  <section class="about-section">
    <h2 class="section-title">Legal</h2>
    <div class="legal-links">
      <a href="https://www.we-amp.com/privacy/" target="_blank" rel="noopener" class="legal-link">
        Privacy Policy
      </a>
      <a href="https://modpagespeed.com/terms/" target="_blank" rel="noopener" class="legal-link">
        Terms of Service
      </a>
    </div>
  </section>
</div>

<style>
  .about-page {
    padding: var(--ps-space-lg);
    max-width: 680px;
  }

  .about-section {
    margin-bottom: var(--ps-space-xl);
  }

  .section-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-md) 0;
    padding-bottom: var(--ps-space-xs);
    border-bottom: 1px solid var(--ps-border);
  }

  .info-grid {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-lg);
  }

  .info-item {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .info-label {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted, #666);
    text-transform: uppercase;
    letter-spacing: 0.03em;
  }

  .info-value {
    font-size: var(--ps-font-size);
    font-family: var(--ps-font-mono);
    color: var(--ps-fg-primary);
  }

  .support-text {
    margin: 0;
    font-size: var(--ps-font-size);
    line-height: 1.5;
    color: var(--ps-fg-primary);
  }

  .support-link {
    color: var(--ps-fg-link);
    text-decoration: underline;
  }

  .support-link:hover {
    color: var(--ps-accent);
  }

  .legal-links {
    display: flex;
    gap: var(--ps-space-lg);
  }

  .legal-link {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-fg-muted, #666);
    text-decoration: underline;
  }

  .legal-link:hover {
    color: var(--ps-accent);
  }
</style>
