<script lang="ts">
  import { onMount } from "svelte";
  import { AdminApiClient, ApiError } from "$lib/api/client";
  import { usePolling } from "$lib/api/polling.svelte";
  import {
    formatLicenseDate,
    getLicenseErrorMessage,
  } from "$lib/utils/license-utils";

  // isGlobal is passed by App.svelte to all page components (convention).
  // On per-server admin, mutation endpoints (purchase, apply) are disabled.
  const { basePath = "", isGlobal = false }: { basePath?: string; isGlobal?: boolean } = $props();
  const api = new AdminApiClient(basePath);
  const license = usePolling(() => api.getLicenseStatus(), 30000);

  // ── Manual key entry state ──────────────────────────────────
  let licenseKey = $state("");
  let applyLoading = $state(false);
  let applyMessage = $state<string | null>(null);
  let applyError = $state<string | null>(null);

  // ── Shared terms acceptance ─────────────────────────────────
  let termsAccepted = $state(false);
  let termsError = $state(false);

  // ── Popup checkout state ────────────────────────────────────
  let checkoutNonce = $state<string | null>(null);
  let checkoutPolling = $state(false);
  let checkoutMessage = $state<string | null>(null);
  let checkoutError = $state<string | null>(null);
  let orderRefActivateInFlight = false;
  let purchasePollTimer: ReturnType<typeof setInterval> | null = null;

  // ── Guards ─────────────────────────────────────────────────
  let destroyed = false;
  let activationComplete = false;
  let cancelPolling = $state(false);

  let autoClearTimers: ReturnType<typeof setTimeout>[] = [];

  function autoClear(setter: () => void) {
    const id = setTimeout(() => { if (!destroyed) setter(); }, 8000);
    autoClearTimers.push(id);
  }

  // ── Derived status ──────────────────────────────────────────
  let statusColor = $derived.by(() => {
    if (!license.data) return "neutral";
    if (license.data.expired) return "error";
    if (license.data.licensed) {
      if (license.data.expires) {
        const daysLeft = (license.data.expires * 1000 - Date.now()) / (1000 * 60 * 60 * 24);
        if (daysLeft <= 0) return "error";
        if (daysLeft < 7) return "warning";
      }
      return "success";
    }
    // the design record soft enforcement: unlicensed keeps the optimization core running,
    // so it is an amber warning (matching the topbar pill in App.svelte), not a
    // red error. Red stays reserved for the genuinely-expired case.
    return "warning";
  });

  let statusLabel = $derived.by(() => {
    if (!license.data) return "Unknown";
    if (license.data.expired) return "Expired";
    if (!license.data.licensed) return "Unlicensed";
    if (daysRemaining !== null && daysRemaining <= 0) return "Expired";
    if (daysRemaining !== null && daysRemaining <= 30) return `${daysRemaining} days remaining`;
    return license.data.license_type
      ? license.data.license_type.charAt(0).toUpperCase() + license.data.license_type.slice(1)
      : "Licensed";
  });

  let daysRemaining = $derived.by(() => {
    if (!license.data?.expires) return null;
    const days = Math.ceil((license.data.expires * 1000 - Date.now()) / (1000 * 60 * 60 * 24));
    return days;
  });

  // Only the global admin endpoint can manage licenses. The isGlobal prop is
  // set by the router based on which admin endpoint is being served; the
  // server's is_global field is not authoritative for UI gating.
  let canManageLicense = $derived(isGlobal);

  // ── Manual key apply ────────────────────────────────────────
  async function applyLicense() {
    if (!licenseKey.trim()) return;
    applyLoading = true;
    applyMessage = null;
    applyError = null;
    try {
      const result = await api.applyLicense(licenseKey.trim());
      if (result.success) {
        applyMessage = result.message ?? "License applied successfully.";
        autoClear(() => { applyMessage = null; });
        licenseKey = "";
        checkoutError = null;
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

  // ── Popup checkout flow (aligned with 2.0 reference) ────────
  function openCheckout(period: "monthly" | "annual") {
    if (!termsAccepted) {
      termsError = true;
      return;
    }
    termsError = false;
    checkoutMessage = null;
    checkoutError = null;
    cancelPolling = false;
    activationComplete = false;

    const nonce = typeof crypto.randomUUID === 'function'
      ? crypto.randomUUID()
      : Array.from(crypto.getRandomValues(new Uint8Array(16)),
          b => b.toString(16).padStart(2, '0')).join('').replace(
          /(.{8})(.{4})(.{4})(.{4})(.{12})/, '$1-$2-$3-$4-$5');
    checkoutNonce = nonce;
    checkoutPolling = true;

    const product = period === "annual" ? "business-site-annual" : "business-site-monthly";
    const buyUrl = `https://modpagespeed.com/buy/?nonce=${nonce}&product=${product}&origin=${encodeURIComponent(window.location.origin)}`;
    const win = window.open(buyUrl, "mps-checkout", "width=520,height=720,scrollbars=yes");
    if (!win || win.closed) {
      // Popup blocked — fall back to new tab (postMessage won't work, but polling will)
      window.open(buyUrl, "_blank");
    }

    // Start nonce-based polling immediately (doesn't wait for postMessage)
    startPurchasePolling(nonce);
  }

  function startPurchasePolling(nonce: string) {
    stopPurchasePolling();
    let pollCount = 0;
    const maxPolls = 60; // 5 minutes at 5s intervals
    purchasePollTimer = setInterval(async () => {
      if (destroyed || cancelPolling) { stopPurchasePolling(); return; }
      pollCount++;
      if (pollCount > maxPolls) {
        stopPurchasePolling();
        checkoutPolling = false;
        checkoutNonce = null;
        checkoutError = "Could not retrieve license. You can paste your key manually below.";
        return;
      }
      try {
        const result = await api.activateLicense(nonce);
        if (result.found && result.token) {
          stopPurchasePolling();
          await activateAndRecordConsent(result.token);
        }
      } catch (err) {
        // Rate limit or server error — keep polling
        if (err instanceof ApiError && err.status >= 400 && err.status < 500 && err.status !== 429) {
          stopPurchasePolling();
          checkoutPolling = false;
          checkoutNonce = null;
          checkoutError = err.message;
        }
      }
    }, 5000);
  }

  function stopPurchasePolling() {
    if (purchasePollTimer) {
      clearInterval(purchasePollTimer);
      purchasePollTimer = null;
    }
  }

  // OrderRef fast-path: triggered by postMessage from checkout page.
  // Retries 4x with backoff (FastSpring may not have settled the order yet).
  function handlePostMessage(event: MessageEvent) {
    if (event.origin !== "https://modpagespeed.com") return;
    if (!event.data || typeof event.data !== "object") return;
    if (event.data.type !== "mps-purchase-complete") return;
    if (!checkoutNonce || event.data.nonce !== checkoutNonce) return;

    const orderRef = event.data.orderRef;
    if (typeof orderRef !== 'string' || !orderRef) return;
    if (orderRefActivateInFlight) return;

    orderRefActivateInFlight = true;
    const savedNonce = checkoutNonce;
    stopPurchasePolling(); // Avoid rate-limit collisions

    (async () => {
      const delays = [2000, 5000, 10000, 15000];
      for (let i = 0; i < delays.length; i++) {
        if (destroyed) return;
        await new Promise(r => setTimeout(r, delays[i]));
        try {
          const result = await api.activateLicense(savedNonce, orderRef);
          if (!destroyed && result.found && result.token) {
            await activateAndRecordConsent(result.token);
            return;
          }
        } catch { /* retry */ }
      }
      // All retries failed — restart nonce polling as fallback
      if (!destroyed && checkoutPolling) {
        startPurchasePolling(savedNonce);
      }
    })().finally(() => { orderRefActivateInFlight = false; });
  }

  async function activateAndRecordConsent(token: string) {
    if (destroyed || activationComplete) return;
    activationComplete = true;
    checkoutMessage = "License activated successfully!";
    autoClear(() => { checkoutMessage = null; });
    checkoutPolling = false;
    checkoutNonce = null;
    license.refresh();

    // Record consent best-effort (after activation, like 2.0)
    api.recordConsent(true).catch(() => { /* best-effort */ });
  }

  onMount(() => {
    window.addEventListener("message", handlePostMessage);
    return () => {
      destroyed = true;
      window.removeEventListener("message", handlePostMessage);
      stopPurchasePolling();
      autoClearTimers.forEach(clearTimeout);
    };
  });

  function handleCancelPolling() {
    cancelPolling = true;
    stopPurchasePolling();
    checkoutPolling = false;
    checkoutNonce = null;
  }
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
        <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
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
    <!-- A. Status Card -->
    <div class="status-card">
      <div class="status-row">
        <span class="status-badge badge-{statusColor}" role="status">{statusLabel}</span>
        {#if daysRemaining !== null}
          <span class="days-remaining" class:expiring-soon={daysRemaining !== null && daysRemaining > 0 && daysRemaining < 7}>
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
              <!-- "domain" in the status JSON is the subscriber email (legacy
                   key name); the design record site domain is "site_domain". -->
              <span class="detail-label">Account</span>
              <span class="detail-value">{license.data.domain}</span>
            </div>
          {/if}
          {#if license.data.scope}
            <div class="detail">
              <span class="detail-label">Scope</span>
              <span class="detail-value">{license.data.scope}</span>
            </div>
          {/if}
          {#if license.data.site_domain}
            <div class="detail">
              <span class="detail-label">Site</span>
              <span class="detail-value">{license.data.site_domain}</span>
            </div>
          {/if}
          {#if license.data.expires}
            <div class="detail">
              <span class="detail-label">Expires</span>
              <span class="detail-value">{formatLicenseDate(license.data.expires)}</span>
            </div>
          {/if}
        </div>
      {/if}

      {#if license.data.error}
        <div class="license-error" role="alert">
          {getLicenseErrorMessage(license.data.error)}
        </div>
      {/if}
    </div>

    <!-- B. Purchase (not licensed or expired, global admin only) -->
    {#if license.data.expired && canManageLicense}
      <div class="section">
        <div class="info-card">
          <p>Your license has expired. Please renew or purchase a new license.</p>
        </div>
      </div>
    {/if}
    {#if (!license.data.licensed || license.data.expired) && canManageLicense}
      <!-- Shared terms acceptance -->
      <div class="section terms-section">
        <label class="checkbox-label">
          <input
            type="checkbox"
            bind:checked={termsAccepted}
            onchange={() => { termsError = false; }}
          />
          <span>I agree to the <a href="https://modpagespeed.com/terms/" target="_blank" rel="noopener noreferrer">Terms of Service</a> and <a href="https://modpagespeed.com/privacy/" target="_blank" rel="noopener noreferrer">Privacy Policy</a></span>
        </label>
        {#if termsError}
          <div class="feedback feedback-error" role="alert">Please accept the Terms of Service to continue.</div>
        {/if}
      </div>

      <div class="section">
        <h2>Purchase License</h2>
        <div class="action-card">
          <p>The optimizer runs unlicensed (with a warning); a commercial license is required for production use. Buying here starts a Business subscription: the configured price/year or the configured price/month, licensed per site, unlimited servers for that site. Billed immediately; cancel anytime.</p>
          <div class="buy-buttons">
            <button
              class="btn btn-primary"
              onclick={() => openCheckout("monthly")}
              disabled={checkoutPolling}
            >
              Buy Monthly
            </button>
            <button
              class="btn btn-primary"
              onclick={() => openCheckout("annual")}
              disabled={checkoutPolling}
            >
              Buy Annual
            </button>
            <a
              href="https://modpagespeed.com/pricing/"
              target="_blank"
              rel="noopener noreferrer"
              class="btn btn-secondary"
            >
              Compare plans and pricing
            </a>
          </div>

          {#if checkoutPolling}
            <div class="checkout-progress">
              <span class="spinner" aria-hidden="true"></span>
              <span>Waiting for purchase confirmation...</span>
              <button class="btn btn-secondary btn-sm" onclick={handleCancelPolling}>Cancel</button>
            </div>
          {/if}

          {#if checkoutMessage}
            <div class="feedback feedback-success" role="alert">{checkoutMessage}</div>
          {/if}
          {#if checkoutError}
            <div class="feedback feedback-error" role="alert">{checkoutError}</div>
          {/if}
        </div>
      </div>
    {:else if (!license.data.licensed || license.data.expired) && !canManageLicense}
      <div class="section">
        <div class="info-card">
          <p>License management is available on the <strong>global admin</strong> endpoint.
            Use <code>/pagespeed_global_admin/</code> to purchase, activate, or apply a license.</p>
        </div>
      </div>
    {/if}

    <!-- D. Manual Key Entry (global admin only) -->
    {#if canManageLicense}
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
        <div class="feedback feedback-success" role="alert">{applyMessage}</div>
      {/if}
      {#if applyError}
        <div class="feedback feedback-error" role="alert">{applyError}</div>
      {/if}
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

  .terms-section {
    margin-bottom: var(--ps-space-lg);
  }

  .key-form {
    display: flex;
    flex-direction: column;
    gap: var(--ps-space-sm);
    max-width: 500px;
  }

  .buy-buttons {
    display: flex;
    flex-wrap: wrap;
    gap: var(--ps-space-sm);
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

  .checkbox-label {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text);
    cursor: pointer;
  }

  .checkbox-label a {
    color: var(--ps-primary);
    text-decoration: underline;
  }

  .checkout-progress {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    margin-top: var(--ps-space-md);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
  }

  .spinner {
    display: inline-block;
    width: 16px;
    height: 16px;
    border: 2px solid var(--ps-border);
    border-top-color: var(--ps-primary);
    border-radius: 50%;
    animation: spin 0.8s linear infinite;
  }

  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
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

  .info-card {
    padding: var(--ps-space-lg);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius-lg);
    background: var(--ps-bg-secondary);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text-secondary);
    line-height: 1.6;
  }

  .info-card code {
    background: var(--ps-bg-tertiary);
    padding: 0.1em 0.4em;
    border-radius: var(--ps-border-radius);
    font-family: var(--ps-font-mono);
    font-size: 0.9em;
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

  .btn {
    padding: var(--ps-space-sm) var(--ps-space-md);
    border: 1px solid var(--ps-border);
    border-radius: var(--ps-border-radius);
    font-size: var(--ps-font-size-sm);
    cursor: pointer;
    white-space: nowrap;
  }

  a.btn {
    text-decoration: none;
    display: inline-block;
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

  .btn-sm {
    padding: var(--ps-space-xs) var(--ps-space-sm);
    font-size: var(--ps-font-size-xs);
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
