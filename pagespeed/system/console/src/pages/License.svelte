<script lang="ts">
  import { AdminApiClient } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import {
    formatLicenseDate,
    getLicenseErrorMessage,
  } from "$lib/utils/license-utils";

  const { basePath = "" }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const license = usePolling(() => api.getLicenseStatus(), 30000);

  let licenseKey = $state("");
  let applyLoading = $state(false);
  let applyMessage = $state<string | null>(null);
  let applyError = $state<string | null>(null);

  let trialLoading = $state(false);
  let trialMessage = $state<string | null>(null);
  let trialError = $state<string | null>(null);

  async function applyLicense() {
    if (!licenseKey.trim()) return;
    applyLoading = true;
    applyMessage = null;
    applyError = null;
    try {
      const result = await api.applyLicense(licenseKey.trim());
      if (result.success) {
        applyMessage = result.message ?? "License applied successfully.";
        licenseKey = "";
        license.refresh();
      } else {
        applyError = result.error
          ? getLicenseErrorMessage(result.error)
          : (result.message ?? "Failed to apply license.");
      }
    } catch (err) {
      applyError = err instanceof Error ? err.message : String(err);
    } finally {
      applyLoading = false;
    }
  }

  async function startTrial() {
    trialLoading = true;
    trialMessage = null;
    trialError = null;
    try {
      const result = await api.startTrial();
      if (result.success) {
        trialMessage = "Trial started successfully.";
        license.refresh();
      } else {
        trialError = result.error
          ? getLicenseErrorMessage(result.error)
          : "Failed to start trial.";
      }
    } catch (err) {
      trialError = err instanceof Error ? err.message : String(err);
    } finally {
      trialLoading = false;
    }
  }

  let statusColor = $derived.by(() => {
    if (!license.data) return "neutral";
    if (license.data.licensed) {
      if (license.data.expires) {
        const daysLeft = (license.data.expires * 1000 - Date.now()) / (1000 * 60 * 60 * 24);
        if (daysLeft < 7) return "warning";
      }
      return "success";
    }
    return "error";
  });

  let statusLabel = $derived.by(() => {
    if (!license.data) return "Unknown";
    if (license.data.licensed) {
      return license.data.license_type
        ? license.data.license_type.charAt(0).toUpperCase() + license.data.license_type.slice(1)
        : "Licensed";
    }
    return "Not Licensed";
  });

  let daysRemaining = $derived.by(() => {
    if (!license.data?.expires) return null;
    const days = Math.ceil((license.data.expires * 1000 - Date.now()) / (1000 * 60 * 60 * 24));
    return days;
  });
</script>

<div class="page">
  <div class="header">
    <h1>License</h1>
    <button class="btn btn-secondary" onclick={() => license.refresh()}>
      Refresh Status
    </button>
  </div>

  {#if license.loading}
    <p class="loading">Loading license status...</p>
  {:else if license.error}
    <div class="license-unavailable" data-testid="license-unavailable">
      <div class="unavailable-icon">
        <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
          <circle cx="12" cy="12" r="10"/>
          <line x1="12" y1="8" x2="12" y2="12"/>
          <line x1="12" y1="16" x2="12.01" y2="16"/>
        </svg>
      </div>
      <h2 class="unavailable-title">License Service Unavailable</h2>
      <p class="unavailable-message">
        The license status endpoint could not be reached. This means
        the license service is not configured on this server.
      </p>
      <p class="unavailable-detail">{license.error.message}</p>
      <div class="unavailable-actions">
        <button class="btn btn-primary" onclick={() => license.refresh()}>
          Retry
        </button>
        <a
          href="https://modpagespeed.com/buy/"
          target="_blank"
          rel="noopener noreferrer"
          class="btn btn-secondary"
        >
          Purchase License
        </a>
      </div>
    </div>
  {:else if license.data}
    <!-- Status Card -->
    <div class="status-card">
      <div class="status-row">
        <span class="status-badge badge-{statusColor}">{statusLabel}</span>
        {#if daysRemaining !== null}
          <span class="days-remaining" class:expiring-soon={daysRemaining !== null && daysRemaining < 7}>
            {#if daysRemaining > 0}
              {daysRemaining} day{daysRemaining === 1 ? "" : "s"} remaining
            {:else}
              Expired
            {/if}
          </span>
        {/if}
      </div>

      {#if license.data.licensed}
        <div class="details-grid">
          {#if license.data.license_type}
            <div class="detail">
              <span class="detail-label">Plan</span>
              <span class="detail-value">{license.data.license_type}</span>
            </div>
          {/if}
          {#if license.data.domain}
            <div class="detail">
              <span class="detail-label">Domain</span>
              <span class="detail-value">{license.data.domain}</span>
            </div>
          {/if}
          {#if license.data.expires}
            <div class="detail">
              <span class="detail-label">Expires</span>
              <span class="detail-value">{formatLicenseDate(license.data.expires)}</span>
            </div>
          {/if}
          {#if license.data.features && license.data.features.length > 0}
            <div class="detail">
              <span class="detail-label">Features</span>
              <span class="detail-value">{license.data.features.join(", ")}</span>
            </div>
          {/if}
        </div>
      {/if}

      {#if license.data.error}
        <div class="license-error">
          {getLicenseErrorMessage(license.data.error)}
        </div>
      {/if}
    </div>

    <!-- Apply License Key -->
    <div class="section">
      <h2>Apply License Key</h2>
      <form class="key-form" onsubmit={(e) => { e.preventDefault(); applyLicense(); }}>
        <label class="form-label" for="license-key">License Key</label>
        <input
          id="license-key"
          type="text"
          class="form-input"
          placeholder="Enter your license key..."
          bind:value={licenseKey}
        />
        <button
          class="btn btn-primary"
          type="submit"
          disabled={applyLoading || !licenseKey.trim()}
        >
          {applyLoading ? "Applying..." : "Apply License"}
        </button>
      </form>

      {#if applyMessage}
        <div class="feedback feedback-success">{applyMessage}</div>
      {/if}
      {#if applyError}
        <div class="feedback feedback-error">{applyError}</div>
      {/if}
    </div>

    <!-- Trial & Purchase -->
    {#if !license.data.licensed}
      <div class="section">
        <h2>Get Started</h2>
        <div class="actions-row">
          {#if license.data.trial_available}
            <div class="action-card">
              <h3>Free Trial</h3>
              <p>Try ModPageSpeed with full features for 14 days.</p>
              <button
                class="btn btn-primary"
                onclick={startTrial}
                disabled={trialLoading}
              >
                {trialLoading ? "Starting..." : "Start Trial"}
              </button>
              {#if trialMessage}
                <div class="feedback feedback-success">{trialMessage}</div>
              {/if}
              {#if trialError}
                <div class="feedback feedback-error">{trialError}</div>
              {/if}
            </div>
          {/if}

          <div class="action-card">
            <h3>Purchase License</h3>
            <p>Get a commercial license for your domain.</p>
            <a
              href="https://modpagespeed.com/buy/"
              target="_blank"
              rel="noopener noreferrer"
              class="btn btn-primary buy-link"
            >
              Buy Now
            </a>
          </div>
        </div>
      </div>
    {/if}
  {/if}
</div>

<style>
  .page {
    max-width: 960px;
  }

  .header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    margin-bottom: var(--ps-space-lg);
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
  }

  h1 {
    margin: 0;
  }

  h2 {
    font-size: var(--ps-font-size-lg);
    margin-bottom: var(--ps-space-md);
  }

  h3 {
    font-size: var(--ps-font-size-base);
    margin-bottom: var(--ps-space-sm);
  }

  .status-card {
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
    margin-bottom: var(--ps-space-xl);
  }

  .status-row {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    margin-bottom: var(--ps-space-md);
    flex-wrap: wrap;
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

  .days-remaining {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .expiring-soon {
    color: var(--ps-warning);
    font-weight: 600;
  }

  .details-grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(200px, 1fr));
    gap: var(--ps-space-md);
  }

  .detail {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-xs);
  }

  .detail-label {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-text-tertiary);
    text-transform: uppercase;
    letter-spacing: 0.05em;
  }

  .detail-value {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text);
  }

  .license-error {
    margin-top: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-error);
    border-radius: var(--ps-border-radius);
    color: var(--ps-error);
    font-size: var(--ps-font-size-sm);
    background: color-mix(in srgb, var(--ps-error) 5%, var(--ps-bg));
  }

  .section {
    margin-bottom: var(--ps-space-xl);
  }

  .key-form {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
    max-width: 500px;
  }

  .form-label {
    font-size: var(--ps-font-size-sm);
    font-weight: 600;
    color: var(--ps-text);
  }

  .form-input {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    font-family: var(--ps-font-mono);
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .form-input:focus {
    outline: none;
    border-color: var(--ps-primary);
    box-shadow: 0 0 0 2px var(--ps-primary-light);
  }

  .feedback {
    margin-top: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
  }

  .feedback-success {
    color: var(--ps-success);
    background: color-mix(in srgb, var(--ps-success) 8%, var(--ps-bg));
    border: 1px solid var(--ps-success);
  }

  .feedback-error {
    color: var(--ps-error);
    background: color-mix(in srgb, var(--ps-error) 5%, var(--ps-bg));
    border: 1px solid var(--ps-error);
  }

  .actions-row {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(250px, 1fr));
    gap: var(--ps-space-lg);
  }

  .action-card {
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg);
  }

  .action-card p {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-md);
  }

  .buy-link {
    display: inline-block;
    text-decoration: none;
    text-align: center;
  }

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
  }

  .btn:disabled {
    opacity: 0.6;
    cursor: not-allowed;
  }

  .btn-primary {
    background: var(--ps-primary);
    color: var(--ps-text-inverse);
    border-color: var(--ps-primary);
  }

  .btn-primary:hover:not(:disabled) {
    background: var(--ps-primary-hover);
  }

  .btn-secondary {
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .btn-secondary:hover {
    background: var(--ps-surface-hover);
  }

  .loading {
    color: var(--ps-text-secondary);
  }

  .error {
    color: var(--ps-error);
  }

  .license-unavailable {
    text-align: center;
    padding: var(--ps-space-xl) var(--ps-space-lg);
    border: 1px dashed var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
    max-width: 500px;
  }

  .unavailable-icon {
    color: var(--ps-text-tertiary);
    margin-bottom: var(--ps-space-md);
  }

  .unavailable-title {
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-text-secondary);
    margin-bottom: var(--ps-space-sm);
  }

  .unavailable-message {
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    line-height: 1.5;
    margin-bottom: var(--ps-space-sm);
  }

  .unavailable-detail {
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-tertiary);
    font-family: var(--ps-font-mono);
    margin-bottom: var(--ps-space-lg);
  }

  .unavailable-actions {
    display: flex;
    gap: var(--ps-space-sm);
    justify-content: center;
    flex-wrap: wrap;
  }

  @media (max-width: 600px) {
    .header {
      flex-direction: column;
      align-items: flex-start;
    }

    .details-grid {
      grid-template-columns: 1fr;
    }

    .actions-row {
      grid-template-columns: 1fr;
    }
  }
</style>
