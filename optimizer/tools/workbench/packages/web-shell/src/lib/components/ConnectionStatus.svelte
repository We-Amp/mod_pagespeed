<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { getContext, onDestroy } from 'svelte';
  import type { ConnectionManager, ConnectionState } from '@pagespeed/api-client';
  import type { Readable } from 'svelte/store';

  const connectionManager = getContext<ConnectionManager>('connection');

  const stateStore: Readable<ConnectionState> = connectionManager.state;
  const errorStore: Readable<string | null> = connectionManager.error;

  let currentState = $state<ConnectionState>('disconnected');
  let errorMessage = $state<string | null>(null);

  // Subscribe to stores for reactive updates.
  const unsubState = stateStore.subscribe((val) => {
    currentState = val;
  });

  const unsubError = errorStore.subscribe((val) => {
    errorMessage = val;
  });

  let dotClass = $derived<string>(
    currentState === 'connected'
      ? 'ps-status-dot-green'
      : currentState === 'connecting'
        ? 'ps-status-dot-yellow'
        : currentState === 'error'
          ? 'ps-status-dot-red'
          : '',
  );

  let label = $derived<string>(
    currentState === 'connected'
      ? 'Connected'
      : currentState === 'connecting'
        ? 'Connecting...'
        : currentState === 'error' && errorMessage
          ? `Connection Error: ${errorMessage}`
          : currentState === 'error'
            ? 'Connection Error'
            : 'Disconnected',
  );

  let spinning = $derived(currentState === 'connecting');

  onDestroy(() => {
    unsubState();
    unsubError();
  });
</script>

<div class="connection-status" role="status" aria-live="polite">
  <span
    class="ps-status-dot {dotClass}"
    class:status-dot-spinning={spinning}
    aria-hidden="true"
  ></span>
  <span class="ps-badge {currentState === 'error' ? 'ps-badge-error' : currentState === 'connected' ? 'ps-badge-success' : ''}">
    {label}
  </span>
</div>

<style>
  .connection-status {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-xs);
  }

  .status-dot-spinning {
    animation: ps-pulse 1.2s ease-in-out infinite;
  }

  @keyframes ps-pulse {
    0%,
    100% {
      opacity: 1;
    }
    50% {
      opacity: 0.3;
    }
  }

  @media (prefers-reduced-motion: reduce) {
    .status-dot-spinning {
      animation: none;
      opacity: 0.7;
    }
  }
</style>
