<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import type { Snippet } from 'svelte';
  import { onMount, onDestroy, setContext } from 'svelte';
  import { get } from 'svelte/store';
  import '@pagespeed/ui/common/theme.css';
  import '@pagespeed/ui/common/base.css';
  import { DirectTransport, ConnectionManager } from '@pagespeed/api-client';
  import type { ConnectionState } from '@pagespeed/api-client';
  import { getStoredToken, storeToken, clearToken } from '#lib/auth.ts';
  import Topbar from '#lib/components/Topbar.svelte';
  import Sidebar from '#lib/components/Sidebar.svelte';
  import LoginForm from '#lib/components/LoginForm.svelte';
  import ErrorBanner from '#lib/components/ErrorBanner.svelte';
  import ReconnectOverlay from '#lib/components/ReconnectOverlay.svelte';
  import KeyboardShortcutHelp from '#lib/components/KeyboardShortcutHelp.svelte';
  import LogPanel from '#lib/components/LogPanel.svelte';

  let { children }: { children: Snippet } = $props();

  // -- Transport & connection setup -----------------------------------------

  const token = getStoredToken();

  const transport = new DirectTransport({
    token: token ?? undefined,
  });

  const connectionManager = new ConnectionManager({ transport });

  setContext('api', transport);
  setContext('connection', connectionManager);

  // -- Reactive state from stores ------------------------------------------

  let authRequired = $state(false);
  let connectionState = $state<ConnectionState>('disconnected');
  let connectionError = $state<string | null>(null);
  let errorDismissed = $state(false);
  let loginError = $state('');
  let sidebarOpen = $state(false);
  let reconnectAttempt = $state(0);

  /** True once we have been connected at least once in this session. */
  let wasConnected = $state(false);

  /** User dismissed the reconnect overlay (stale data continues to show). */
  let reconnectDismissed = $state(false);

  /** Whether the keyboard shortcuts help popover is visible. */
  let shortcutHelpVisible = $state(false);

  /** Whether the bottom debug console panel is expanded. */
  let debugPanelOpen = $state(true);

  /** Debug panel body height in pixels (user-resizable). */
  let debugPanelHeight = $state(250);
  const DEBUG_PANEL_MIN_HEIGHT = 80;
  const DEBUG_PANEL_MAX_HEIGHT = 600;

  /** Drag state for resize handle. */
  let resizing = $state(false);
  let resizeStartY = 0;
  let resizeStartHeight = 0;

  function onResizePointerDown(e: PointerEvent) {
    e.preventDefault();
    resizing = true;
    resizeStartY = e.clientY;
    resizeStartHeight = debugPanelHeight;
    (e.target as HTMLElement).setPointerCapture(e.pointerId);
  }

  function onResizePointerMove(e: PointerEvent) {
    if (!resizing) return;
    const delta = resizeStartY - e.clientY;  // drag up = bigger
    debugPanelHeight = Math.max(
      DEBUG_PANEL_MIN_HEIGHT,
      Math.min(DEBUG_PANEL_MAX_HEIGHT, resizeStartHeight + delta),
    );
  }

  function onResizePointerUp() {
    resizing = false;
  }

  /**
   * One-time informational notice about the Apache-2.0 license and support
   * subscriptions. Shown until dismissed; the dismissal persists per browser
   * in localStorage so it is seen once, not on every load.
   */
  const SUPPORT_NOTICE_KEY = 'ps.support-notice.dismissed';
  const SUPPORT_URL = 'https://we-amp.com/licensing/';
  let supportNoticeVisible = $state(false);

  function readSupportNoticeDismissed(): boolean {
    try {
      return localStorage.getItem(SUPPORT_NOTICE_KEY) === '1';
    } catch {
      return false;
    }
  }

  function dismissSupportNotice() {
    supportNoticeVisible = false;
    try {
      localStorage.setItem(SUPPORT_NOTICE_KEY, '1');
    } catch {
      /* storage unavailable: the notice simply shows again next load */
    }
  }

  // Subscribe to the authRequired store for reactive updates.
  const unsubAuth = connectionManager.authRequired.subscribe((val) => {
    authRequired = val;
  });

  const unsubState = connectionManager.state.subscribe((val) => {
    connectionState = val;
    // Reset dismissal when state changes (e.g., reconnecting clears it).
    if (val !== 'error') {
      errorDismissed = false;
    }
    // Track that we were connected at least once.
    if (val === 'connected') {
      wasConnected = true;
      reconnectDismissed = false;
    }
  });

  const unsubReconnectAttempt = connectionManager.reconnectAttempt.subscribe(
    (val) => {
      reconnectAttempt = val;
    },
  );

  const unsubError = connectionManager.error.subscribe((val) => {
    connectionError = val;
    // Reset dismissal when a new error arrives.
    if (val !== null) {
      errorDismissed = false;
    }
  });

  /** Show reconnect overlay only after having been connected before. */
  let showReconnectOverlay = $derived(
    wasConnected &&
      connectionState !== 'connected' &&
      connectionState !== 'disconnected' &&
      !reconnectDismissed,
  );

  /** Translate raw browser errors to friendly messages. */
  let friendlyError = $derived(
    connectionError
      ? connectionError
          .replace(/Failed to fetch/i, 'Cannot reach the PageSpeed worker')
          .replace(/NetworkError/i, 'Cannot reach the PageSpeed worker')
          .replace(/Load failed/i, 'Cannot reach the PageSpeed worker')
      : null,
  );

  let showErrorBanner = $derived(
    connectionState === 'error' &&
      connectionError !== null &&
      !errorDismissed &&
      !showReconnectOverlay,
  );

  /**
   * Compute the current backoff delay in ms for countdown display.
   * Mirrors the exponential backoff formula in ConnectionManager.
   */
  let backoffMs = $derived(
    reconnectAttempt > 0
      ? Math.min(
          1000 * Math.pow(2, reconnectAttempt - 1),
          connectionManager.maxReconnectDelay,
        )
      : 0,
  );

  // -- Keyboard shortcuts ---------------------------------------------------

  function handleGlobalKeydown(e: KeyboardEvent) {
    const target = e.target as HTMLElement | null;
    const isInput =
      target?.tagName === 'INPUT' ||
      target?.tagName === 'TEXTAREA' ||
      target?.tagName === 'SELECT' ||
      target?.isContentEditable;

    // Ctrl+`: Toggle debug console panel.
    if (e.key === '`' && (e.metaKey || e.ctrlKey)) {
      e.preventDefault();
      debugPanelOpen = !debugPanelOpen;
      return;
    }

    // Ctrl+K / Cmd+K: Focus the URL input on the current page.
    if (e.key === 'k' && (e.metaKey || e.ctrlKey)) {
      e.preventDefault();
      const urlInput = document.querySelector<HTMLInputElement>(
        'input[type="url"], input.url-input, #url-input, #diff-url',
      );
      if (urlInput) {
        urlInput.focus();
        urlInput.select();
      }
      return;
    }

    // Escape: Close any open modal/popover, sidebar, or shortcut help.
    if (e.key === 'Escape') {
      if (shortcutHelpVisible) {
        shortcutHelpVisible = false;
        return;
      }
      if (sidebarOpen) {
        sidebarOpen = false;
        return;
      }
      // Let individual components handle Escape for their own modals.
      return;
    }

    // "?" key: Toggle shortcut help (only when not typing in an input).
    if (e.key === '?' && !isInput) {
      shortcutHelpVisible = !shortcutHelpVisible;
      return;
    }
  }

  // -- Lifecycle ------------------------------------------------------------

  onMount(() => {
    connectionManager.connect();
    supportNoticeVisible = !readSupportNoticeDismissed();
    if (typeof window !== 'undefined') {
      window.addEventListener('keydown', handleGlobalKeydown);
    }
  });

  onDestroy(() => {
    unsubAuth();
    unsubState();
    unsubError();
    unsubReconnectAttempt();
    connectionManager.disconnect();
    if (typeof window !== 'undefined') {
      window.removeEventListener('keydown', handleGlobalKeydown);
    }
  });

  // -- Handlers -------------------------------------------------------------

  function handleLoginClick() {
    // authRequired is already true when this is called, login form shows.
  }

  function handleLoginSubmit(tokenValue: string) {
    loginError = '';
    // setToken() re-probes auth and resolves (it never rejects), so the check
    // must run in .then(), not .catch(). Only persist the token after the worker
    // accepts it — otherwise a bad token is written to sessionStorage and
    // replayed on the next reload.
    connectionManager.setToken(tokenValue).then(() => {
      if (get(connectionManager.authRequired)) {
        loginError = 'Invalid token. Please try again.';
        clearToken();
      } else {
        storeToken(tokenValue);
      }
    });
  }

  function handleMenuToggle() {
    sidebarOpen = !sidebarOpen;
  }

  function handleOverlayClick() {
    sidebarOpen = false;
  }

  function handleDismissError() {
    errorDismissed = true;
  }

  function handleReconnectNow() {
    connectionManager.connect();
  }

  function handleDismissReconnect() {
    reconnectDismissed = true;
  }

  function handleCloseShortcutHelp() {
    shortcutHelpVisible = false;
  }

</script>

<div class="shell">
  <Topbar
    {authRequired}
    onLoginClick={handleLoginClick}
    onMenuToggle={handleMenuToggle}
    onShortcutHelp={() => (shortcutHelpVisible = !shortcutHelpVisible)}
  />

  <div class="shell-body">
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
      class="sidebar-wrapper"
      class:sidebar-mobile-open={sidebarOpen}
    >
      <Sidebar />
    </div>

    {#if sidebarOpen}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="mobile-overlay"
        onclick={handleOverlayClick}
        onkeydown={(e) => {
          if (e.key === 'Escape') handleOverlayClick();
        }}
        role="presentation"
      ></div>
    {/if}

    <main class="main-content">
      {#if supportNoticeVisible}
        <div class="support-notice" role="status" data-testid="support-notice">
          <span class="support-notice-text">
            mod_pagespeed is licensed under the Apache License 2.0.
            Support subscriptions:
            <a href={SUPPORT_URL} target="_blank" rel="noopener" class="support-notice-link">{SUPPORT_URL}</a>
          </span>
          <button
            class="support-notice-dismiss"
            onclick={dismissSupportNotice}
            aria-label="Dismiss"
            data-testid="support-notice-dismiss"
          >&times;</button>
        </div>
      {/if}
      {#if showErrorBanner}
        <ErrorBanner
          message={friendlyError ?? ''}
          onDismiss={handleDismissError}
        />
      {/if}
      {@render children()}
      {#if showReconnectOverlay}
        <ReconnectOverlay
          {connectionState}
          error={connectionError}
          attempt={reconnectAttempt}
          {backoffMs}
          onReconnect={handleReconnectNow}
          onDismiss={handleDismissReconnect}
        />
      {/if}
    </main>
  </div>

  <div class="debug-panel" class:debug-panel-open={debugPanelOpen}>
    {#if debugPanelOpen}
      <!-- svelte-ignore a11y_no_static_element_interactions -->
      <div
        class="debug-panel-resize-handle"
        onpointerdown={onResizePointerDown}
        onpointermove={onResizePointerMove}
        onpointerup={onResizePointerUp}
      ></div>
    {/if}
    <!-- svelte-ignore a11y_no_static_element_interactions -->
    <div
      class="debug-panel-header"
      onclick={() => (debugPanelOpen = !debugPanelOpen)}
      onkeydown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); debugPanelOpen = !debugPanelOpen; } }}
      role="button"
      tabindex="0"
      aria-expanded={debugPanelOpen}
      aria-label="Toggle debug console"
    >
      <span class="debug-panel-chevron" class:debug-panel-chevron-open={debugPanelOpen}>&#9656;</span>
      <span class="debug-panel-title">Debug Console</span>
      <span class="debug-panel-shortcut">Ctrl+`</span>
    </div>
    {#if debugPanelOpen}
      <div class="debug-panel-body" style="height:{debugPanelHeight}px">
        <LogPanel />
      </div>
    {/if}
  </div>

  {#if authRequired}
    <LoginForm
      onSubmit={handleLoginSubmit}
      errorMessage={loginError}
    />
  {/if}

  <KeyboardShortcutHelp
    visible={shortcutHelpVisible}
    onClose={handleCloseShortcutHelp}
  />
</div>

<style>
  :global(*),
  :global(*::before),
  :global(*::after) {
    box-sizing: border-box;
  }

  :global(html, body) {
    margin: 0;
    padding: 0;
    height: 100%;
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
    background: var(--ps-bg-primary);
  }

  :global(#app) {
    height: 100%;
  }

  .shell {
    display: flex;
    flex-direction: column;
    height: 100vh;
    overflow: hidden;
  }

  .shell-body {
    display: flex;
    flex: 1;
    min-height: 0;
    overflow: hidden;
  }

  .sidebar-wrapper {
    flex-shrink: 0;
  }

  .main-content {
    flex: 1;
    min-width: 0;
    overflow-y: auto;
    background: var(--ps-bg-primary);
    position: relative;
  }

  /* Informational notice: neutral surface with an info accent, deliberately
     not a warning or error tone. */
  .support-notice {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-xs) var(--ps-space-md);
    background: var(--ps-bg-secondary);
    border-bottom: 1px solid var(--ps-border);
    border-left: 3px solid var(--ps-info, #1a85ff);
    color: var(--ps-fg-secondary);
    font-size: var(--ps-font-size-sm);
  }

  .support-notice-text {
    flex: 1;
  }

  .support-notice-link {
    color: var(--ps-fg-link);
    text-decoration: underline;
  }

  .support-notice-dismiss {
    background: none;
    border: none;
    color: var(--ps-fg-muted);
    font-size: 18px;
    cursor: pointer;
    padding: 0 var(--ps-space-xs);
  }

  .support-notice-dismiss:hover {
    color: var(--ps-fg-primary);
  }

  /* ── Debug panel ──────────────────────────────────────────── */

  .debug-panel {
    flex-shrink: 0;
    border-top: 1px solid var(--ps-border);
    background: var(--ps-bg-primary);
    display: flex;
    flex-direction: column;
  }

  .debug-panel-header {
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 4px 12px;
    background: var(--ps-bg-secondary);
    cursor: pointer;
    user-select: none;
    font-size: 12px;
    font-weight: 500;
    color: var(--ps-fg-primary);
    flex-shrink: 0;
  }

  .debug-panel-header:hover {
    background: var(--ps-bg-hover);
  }

  .debug-panel-header:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: -2px;
  }

  .debug-panel-chevron {
    display: inline-block;
    font-size: 10px;
    transition: transform 0.15s ease;
    color: var(--ps-fg-muted);
  }

  .debug-panel-chevron-open {
    transform: rotate(90deg);
  }

  .debug-panel-title {
    flex: 1;
  }

  .debug-panel-shortcut {
    font-size: 10px;
    color: var(--ps-fg-muted);
    background: var(--ps-bg-primary);
    padding: 0 4px;
    border-radius: 3px;
    border: 1px solid var(--ps-border);
  }

  .debug-panel-resize-handle {
    height: 4px;
    cursor: ns-resize;
    background: transparent;
    flex-shrink: 0;
    transition: background 0.1s ease;
  }

  .debug-panel-resize-handle:hover,
  .debug-panel-resize-handle:active {
    background: var(--ps-accent);
  }

  .debug-panel-body {
    display: flex;
    flex-direction: column;
    overflow: hidden;
  }

  .mobile-overlay {
    display: none;
  }

  /* Mobile: sidebar overlay */
  @media (max-width: 767px) {
    .sidebar-wrapper {
      position: fixed;
      top: 48px;
      left: 0;
      bottom: 0;
      z-index: 100;
      width: 220px;
      transform: translateX(-100%);
      transition: transform 0.2s ease;
    }

    .sidebar-wrapper.sidebar-mobile-open {
      transform: translateX(0);
    }

    .mobile-overlay {
      display: block;
      position: fixed;
      top: 48px;
      left: 0;
      right: 0;
      bottom: 0;
      z-index: 99;
      background: rgba(0, 0, 0, 0.3);
    }
  }
</style>
