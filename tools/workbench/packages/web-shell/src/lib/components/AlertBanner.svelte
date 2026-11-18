<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import type { ActiveAlert } from '#lib/alerts.ts';

  let {
    alerts,
    onDismiss,
  }: {
    alerts: ActiveAlert[];
    onDismiss: (id: string) => void;
  } = $props();
</script>

{#if alerts.length > 0}
  <div class="alert-stack" role="log" aria-label="Active alerts">
    {#each alerts as alert (alert.rule.id)}
      <div
        class="alert-banner"
        class:alert-warning={alert.rule.severity === 'warning'}
        class:alert-error={alert.rule.severity === 'error'}
        role="alert"
        aria-label="{alert.rule.severity === 'error' ? 'Error' : 'Warning'}: {alert.message}"
      >
        <span class="alert-icon" aria-hidden="true">
          {#if alert.rule.severity === 'error'}
            &#x2715;
          {:else}
            &#x26A0;
          {/if}
        </span>
        <span class="alert-message">{alert.message}</span>
        <button
          class="ps-btn ps-btn-ghost alert-dismiss"
          onclick={() => onDismiss(alert.rule.id)}
          aria-label="Dismiss {alert.rule.label} alert"
        >
          <svg
            width="14"
            height="14"
            viewBox="0 0 14 14"
            fill="currentColor"
            aria-hidden="true"
          >
            <path
              d="M1.707.293a1 1 0 0 0-1.414 1.414L5.586 7 .293 12.293a1 1 0 1 0 1.414 1.414L7 8.414l5.293 5.293a1 1 0 0 0 1.414-1.414L8.414 7l5.293-5.293A1 1 0 0 0 12.293.293L7 5.586 1.707.293z"
            />
          </svg>
        </button>
      </div>
    {/each}
  </div>
{/if}

<style>
  .alert-stack {
    display: flex;
    flex-direction: column;
  }

  .alert-banner {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-lg);
    border-bottom: 1px solid var(--ps-border);
  }

  .alert-warning {
    background: var(--ps-warning-bg);
    border-left: 3px solid var(--ps-warning);
  }

  .alert-error {
    background: var(--ps-error-bg);
    border-left: 3px solid var(--ps-error);
  }

  .alert-icon {
    flex-shrink: 0;
    font-size: var(--ps-font-size);
    line-height: 1;
  }

  .alert-warning .alert-icon {
    color: var(--ps-warning);
  }

  .alert-error .alert-icon {
    color: var(--ps-error);
  }

  .alert-message {
    flex: 1;
    min-width: 0;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
  }

  .alert-warning .alert-message {
    color: var(--ps-warning);
  }

  .alert-error .alert-message {
    color: var(--ps-error);
  }

  .alert-dismiss {
    flex-shrink: 0;
    padding: var(--ps-space-xs);
  }

  .alert-warning .alert-dismiss {
    color: var(--ps-warning);
  }

  .alert-error .alert-dismiss {
    color: var(--ps-error);
  }
</style>
