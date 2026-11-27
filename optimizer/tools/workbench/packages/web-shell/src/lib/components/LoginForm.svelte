<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    onSubmit,
    errorMessage = '',
  }: {
    onSubmit: (token: string) => void;
    errorMessage?: string;
  } = $props();

  let token = $state('');
  let submitting = $state(false);

  async function handleSubmit(e: SubmitEvent) {
    e.preventDefault();
    if (!token.trim() || submitting) return;
    submitting = true;
    try {
      onSubmit(token.trim());
    } finally {
      submitting = false;
    }
  }
</script>

<div class="login-overlay" role="dialog" aria-label="Authentication required">
  <div class="login-card ps-card">
    <h2 class="login-title">Authentication Required</h2>
    <p class="login-description">
      Enter your API token to access the PageSpeed Console.
    </p>
    <form onsubmit={handleSubmit}>
      <div class="form-group">
        <label for="api-token" class="form-label">API Token</label>
        <input
          id="api-token"
          type="password"
          class="ps-input"
          placeholder="Enter API token"
          bind:value={token}
          autocomplete="off"
        />
      </div>
      {#if errorMessage}
        <p class="error-message">{errorMessage}</p>
      {/if}
      <button
        type="submit"
        class="ps-btn ps-btn-primary login-submit"
        disabled={!token.trim() || submitting}
      >
        {#if submitting}
          <span class="ps-spinner" aria-label="Connecting"></span>
        {/if}
        Connect
      </button>
    </form>
  </div>
</div>

<style>
  .login-overlay {
    position: fixed;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(0, 0, 0, 0.5);
    z-index: 200;
    padding: var(--ps-space-lg);
  }

  .login-card {
    width: 100%;
    max-width: 400px;
  }

  .login-title {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    margin: 0 0 var(--ps-space-sm);
  }

  .login-description {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
    margin: 0 0 var(--ps-space-lg);
  }

  .form-group {
    margin-bottom: var(--ps-space-md);
  }

  .form-label {
    display: block;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    font-weight: 500;
    color: var(--ps-fg-secondary);
    margin-bottom: var(--ps-space-xs);
  }

  .error-message {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-error);
    margin: 0 0 var(--ps-space-md);
  }

  .login-submit {
    width: 100%;
  }
</style>
