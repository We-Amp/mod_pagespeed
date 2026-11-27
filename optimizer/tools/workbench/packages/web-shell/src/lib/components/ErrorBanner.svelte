<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  let {
    message,
    onDismiss,
  }: {
    message: string;
    onDismiss: () => void;
  } = $props();
</script>

<div class="error-banner ps-card" role="alert">
  <div class="error-banner-content">
    <span class="error-banner-message">{message}</span>
    <button
      class="ps-btn ps-btn-ghost error-banner-dismiss"
      onclick={onDismiss}
      aria-label="Dismiss error"
    >
      <svg width="14" height="14" viewBox="0 0 14 14" fill="currentColor" aria-hidden="true">
        <path d="M1.707.293a1 1 0 0 0-1.414 1.414L5.586 7 .293 12.293a1 1 0 1 0 1.414 1.414L7 8.414l5.293 5.293a1 1 0 0 0 1.414-1.414L8.414 7l5.293-5.293A1 1 0 0 0 12.293.293L7 5.586 1.707.293z" />
      </svg>
    </button>
  </div>
</div>

<style>
  .error-banner {
    border-left: 3px solid var(--ps-error);
    background: var(--ps-error-bg);
    padding: var(--ps-space-sm) var(--ps-space-lg);
    border-radius: 0;
    box-shadow: none;
    border-top: none;
    border-right: none;
    border-bottom: 1px solid var(--ps-border);
  }

  .error-banner-content {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-md);
  }

  .error-banner-message {
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-error);
    flex: 1;
    min-width: 0;
  }

  .error-banner-dismiss {
    flex-shrink: 0;
    padding: var(--ps-space-xs);
    color: var(--ps-error);
  }
</style>
