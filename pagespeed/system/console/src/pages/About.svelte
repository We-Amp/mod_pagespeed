<script lang="ts">
  // About page: build/version info and the legal links (Privacy Policy +
  // Terms of Service). Product names and URLs come from the product-facts
  // single source (lib/data/product-facts-console.ts), never from literals.
  import {
    PRIVACY_URL,
    PRODUCT_NAME,
    TERMS_URL,
  } from "$lib/data/product-facts-console";

  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";
</script>

<div class="page">
  <div class="header">
    <h1>About</h1>
  </div>

  <!-- Section A: Version & system info -->
  <section class="about-section">
    <h2>{PRODUCT_NAME} {appVersion}</h2>
    <div class="info-grid">
      <div class="info-item">
        <span class="info-label">Version</span>
        <span class="info-value">{appVersion}</span>
      </div>
    </div>
  </section>

  <!-- Section B: Legal -->
  <section class="about-section">
    <h2>Legal</h2>
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
