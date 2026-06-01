<script lang="ts">
  // About page: build/version info, a compact license summary, and the legal
  // links (Privacy Policy + Terms of Service). Mirrors the ModPageSpeed 2.0
  // admin console About page — in particular its "Legal" section.
  import { onMount } from "svelte";
  import { AdminApiClient } from "$lib/api/client";
  import type { LicenseStatusResponse } from "$lib/api/types";

  // basePath is passed by App.svelte to every page component (convention).
  // isGlobal is also passed but unused here.
  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);

  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";

  // ── License summary ─────────────────────────────────────────
  // Single fetch on mount; the full license surface lives on #/license.
  let license = $state<LicenseStatusResponse | null>(null);
  let licenseLoaded = $state(false);

  onMount(async () => {
    try {
      license = await api.getLicenseStatus();
    } catch {
      license = null;
    } finally {
      licenseLoaded = true;
    }
  });

  // Days until expiry (null when no expiry is set).
  let daysRemaining = $derived.by(() => {
    if (!license?.expires) return null;
    return Math.ceil((license.expires * 1000 - Date.now()) / (1000 * 60 * 60 * 24));
  });

  // Status badge colour + label, derived like License.svelte — except the
  // unlicensed state uses the design record soft-enforcement amber tone (matching
  // App.svelte's topbar pill and the 2.0 About page): the optimization core
  // keeps running while unlicensed, so it is a warning, not a red error.
  // Red is reserved for the genuinely-expired case.
  type StatusColor = "success" | "warning" | "error" | "neutral";

  let statusColor = $derived.by((): StatusColor => {
    if (!licenseLoaded || !license) return "neutral";
    if (license.expired) return "error";
    if (license.licensed) {
      if (daysRemaining !== null && daysRemaining <= 0) return "error";
      if (daysRemaining !== null && daysRemaining < 7) return "warning";
      return "success";
    }
    return "warning";
  });

  let statusLabel = $derived.by(() => {
    if (!licenseLoaded) return "Checking…";
    if (!license) return "Unavailable";
    if (license.expired) return "Expired";
    if (!license.licensed) return "Unlicensed";
    if (daysRemaining !== null && daysRemaining <= 0) return "Expired";
    if (daysRemaining !== null && daysRemaining <= 30) {
      return `${daysRemaining} day${daysRemaining === 1 ? "" : "s"} remaining`;
    }
    return license.license_type
      ? license.license_type.charAt(0).toUpperCase() + license.license_type.slice(1)
      : "Licensed";
  });
</script>

<div class="page">
  <div class="header">
    <h1>About</h1>
  </div>

  <!-- Section A: Version & system info -->
  <section class="about-section">
    <h2>mod_pagespeed 1.15</h2>
    <div class="info-grid">
      <div class="info-item">
        <span class="info-label">Version</span>
        <span class="info-value">{appVersion}</span>
      </div>
    </div>
  </section>

  <!-- Section B: License summary -->
  <section class="about-section">
    <h2>License</h2>
    <a
      href="#/license"
      class="license-summary-link"
      aria-label="License status: {statusLabel}. Open the License page to manage."
    >
      <div class="status-card">
        <div class="status-row">
          <span class="status-badge badge-{statusColor}" role="status">{statusLabel}</span>
          <span class="manage-link">Manage &rarr;</span>
        </div>
      </div>
    </a>
  </section>

  <!-- Section C: Legal -->
  <section class="about-section">
    <h2>Legal</h2>
    <div class="legal-links">
      <a
        href="https://modpagespeed.com/privacy/"
        target="_blank"
        rel="noopener noreferrer"
        class="legal-link"
      >
        Privacy Policy
      </a>
      <a
        href="https://modpagespeed.com/terms/"
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
  .page {
    max-width: 680px;
  }

  .header {
    margin-bottom: var(--ps-space-lg);
  }

  h1 {
    margin: 0;
  }

  .about-section {
    margin-bottom: var(--ps-space-xl);
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
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
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-text-tertiary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .info-value {
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    color: var(--ps-text);
  }

  .license-summary-link {
    display: block;
    text-decoration: none;
  }

  .license-summary-link:hover .status-card {
    border-color: var(--ps-primary);
  }

  .status-card {
    padding: var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
    transition: border-color 0.15s ease;
  }

  .status-row {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-sm);
  }

  .status-badge {
    display: inline-block;
    padding: var(--ps-space-xs) var(--ps-space-md);
    border-radius: var(--ps-border-radius);
    font-weight: 700;
    font-size: var(--ps-font-size-sm);
  }

  .badge-success {
    background: var(--ps-success);
    color: #ffffff;
  }

  .badge-warning {
    background: var(--ps-warning);
    color: #000000;
  }

  .badge-error {
    background: var(--ps-error);
    color: #ffffff;
  }

  .badge-neutral {
    background: var(--ps-bg-tertiary);
    color: var(--ps-text-secondary);
  }

  .manage-link {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-primary);
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
