<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import ConnectionStatus from './ConnectionStatus.svelte';

  let {
    authRequired = false,
    onLoginClick,
    onMenuToggle,
    onShortcutHelp,
  }: {
    authRequired?: boolean;
    onLoginClick?: () => void;
    onMenuToggle?: () => void;
    onShortcutHelp?: () => void;
  } = $props();

</script>

<header class="topbar">
  <div class="topbar-left">
    <button
      class="ps-btn ps-btn-ghost menu-toggle"
      onclick={onMenuToggle}
      aria-label="Toggle navigation"
    >
      <svg width="18" height="18" viewBox="0 0 18 18" fill="currentColor">
        <rect y="2" width="18" height="2" rx="1" />
        <rect y="8" width="18" height="2" rx="1" />
        <rect y="14" width="18" height="2" rx="1" />
      </svg>
    </button>
    <span class="topbar-title">
      <svg class="topbar-logo" viewBox="0 0 370 58" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
        <text x="0" y="30" font-family="'JetBrains Mono', 'SF Mono', monospace" font-weight="700" font-size="26" fill="#1d4ed8">&#x276F;</text>
        <text x="24" y="30" font-family="Inter, system-ui, sans-serif" font-weight="700" font-size="32" fill="currentColor" letter-spacing="-1.2">mod_pagespeed</text>
        <rect x="312" y="6" width="2.5" height="28" rx="1" fill="#1d4ed8">
          <animate attributeName="opacity" values="0.9;0;0.9" dur="1.2s" repeatCount="indefinite"/>
        </rect>
        <circle cx="5" cy="49" r="3" fill="#059669"/>
        <text x="14" y="53" font-family="'JetBrains Mono', 'SF Mono', monospace" font-weight="600" font-size="14" fill="#78716c">running · we-amp.com</text>
      </svg>
      Console
    </span>
  </div>

  <div class="topbar-spacer"></div>
  <div class="topbar-right">
    <button
      class="ps-btn ps-btn-ghost help-btn"
      onclick={onShortcutHelp}
      title="Keyboard shortcuts (?)"
      aria-label="Keyboard shortcuts"
    >
      ?
    </button>
    {#if authRequired}
      <button class="ps-btn ps-btn-primary" onclick={onLoginClick}>
        Login
      </button>
    {:else}
      <ConnectionStatus />
    {/if}
  </div>
</header>

<style>
  .topbar {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    padding: var(--ps-space-sm) var(--ps-space-lg);
    background: var(--ps-bg-secondary);
    border-bottom: 1px solid var(--ps-border);
    min-height: 48px;
    flex-shrink: 0;
  }

  .topbar-left {
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    flex-shrink: 0;
  }

  .topbar-title {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-lg);
    font-weight: 600;
    color: var(--ps-fg-primary);
    white-space: nowrap;
  }

  .topbar-logo {
    height: 20px;
    width: auto;
  }

  .topbar-spacer {
    flex: 1;
  }

  .topbar-right {
    display: flex;
    align-items: center;
    flex-shrink: 0;
  }

  .help-btn {
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size);
    font-weight: 600;
    width: 28px;
    height: 28px;
    padding: 0;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    border-radius: var(--ps-radius-sm);
  }

  .menu-toggle {
    display: none;
    padding: var(--ps-space-xs);
  }

  @media (max-width: 767px) {
    .menu-toggle {
      display: inline-flex;
    }

  }
</style>
