<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { onDestroy, onMount, tick } from 'svelte';
  import type { ConnectionState } from '@pagespeed/api-client';

  let {
    connectionState,
    error,
    attempt,
    backoffMs,
    onReconnect,
    onDismiss,
  }: {
    connectionState: ConnectionState;
    error: string | null;
    attempt: number;
    backoffMs: number;
    onReconnect: () => void;
    onDismiss: () => void;
  } = $props();

  /** Countdown seconds remaining until next automatic reconnect. */
  let countdown = $state(0);
  let countdownTimer: ReturnType<typeof setInterval> | null = null;

  // Whenever the attempt or backoffMs changes, restart the countdown.
  $effect(() => {
    // Access reactive dependencies.
    const _attempt = attempt;
    const _backoffMs = backoffMs;
    const _connState = connectionState;

    // Only run countdown when in error state (waiting for next attempt).
    if (_connState === 'error' && _attempt > 0 && _backoffMs > 0) {
      countdown = Math.ceil(_backoffMs / 1000);
      startCountdown(_backoffMs);
    } else {
      stopCountdown();
      countdown = 0;
    }
  });

  function startCountdown(totalMs: number): void {
    stopCountdown();
    const start = Date.now();
    countdownTimer = setInterval(() => {
      const elapsed = Date.now() - start;
      const remaining = Math.max(0, Math.ceil((totalMs - elapsed) / 1000));
      countdown = remaining;
      if (remaining <= 0) {
        stopCountdown();
      }
    }, 250);
  }

  function stopCountdown(): void {
    if (countdownTimer !== null) {
      clearInterval(countdownTimer);
      countdownTimer = null;
    }
  }

  onDestroy(() => {
    stopCountdown();
  });

  let dismissBtn: HTMLButtonElement | undefined = $state();

  function handleReconnectClick(): void {
    stopCountdown();
    onReconnect();
  }

  /** Focus the Dismiss button on mount so keyboard users have a clear target. */
  onMount(async () => {
    await tick();
    dismissBtn?.focus();
  });

  /** Trap focus within the dialog: Tab/Shift+Tab cycles between Reconnect and Dismiss. */
  function handleKeydown(event: KeyboardEvent): void {
    if (event.key === 'Escape') {
      onDismiss();
      return;
    }
    if (event.key !== 'Tab') return;
    const overlay = event.currentTarget as HTMLElement;
    const focusable = overlay.querySelectorAll<HTMLElement>(
      'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
    );
    if (focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (event.shiftKey) {
      if (document.activeElement === first) {
        event.preventDefault();
        last.focus();
      }
    } else {
      if (document.activeElement === last) {
        event.preventDefault();
        first.focus();
      }
    }
  }
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<div
  class="reconnect-overlay"
  role="dialog"
  aria-modal="true"
  aria-labelledby="reconnect-dialog-title"
  tabindex="-1"
  onkeydown={handleKeydown}
>
  <div class="reconnect-card ps-card">
    <span class="ps-spinner reconnect-spinner" aria-hidden="true"></span>
    <h2 class="reconnect-title" id="reconnect-dialog-title">Reconnecting...</h2>

    <p class="reconnect-attempt">
      Attempt {attempt} of &infin;
    </p>

    {#if connectionState === 'error' && countdown > 0}
      <p class="reconnect-countdown">
        Retrying in {countdown}s...
      </p>
    {:else if connectionState === 'connecting'}
      <p class="reconnect-countdown">
        Connecting...
      </p>
    {/if}

    {#if error}
      <p class="reconnect-error">{error}</p>
    {/if}

    <div class="reconnect-actions">
      <button
        class="ps-btn ps-btn-primary"
        onclick={handleReconnectClick}
      >
        Reconnect Now
      </button>
      <button
        class="ps-btn ps-btn-ghost reconnect-dismiss"
        onclick={onDismiss}
        bind:this={dismissBtn}
      >
        Dismiss
      </button>
    </div>
  </div>
</div>

<style>
  .reconnect-overlay {
    position: absolute;
    inset: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    background: rgba(0, 0, 0, 0.4);
    z-index: 50;
  }

  .reconnect-card {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-md);
    max-width: 340px;
    width: 90%;
    padding: var(--ps-space-xl);
    text-align: center;
  }

  .reconnect-spinner {
    width: 28px;
    height: 28px;
  }

  .reconnect-title {
    margin: 0;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
  }

  .reconnect-attempt {
    margin: 0;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-secondary);
  }

  .reconnect-countdown {
    margin: 0;
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-muted);
  }

  .reconnect-error {
    margin: 0;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-error);
    word-break: break-word;
  }

  .reconnect-actions {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: var(--ps-space-sm);
    width: 100%;
    margin-top: var(--ps-space-sm);
  }

  .reconnect-dismiss {
    font-size: var(--ps-font-size-sm);
  }
</style>
