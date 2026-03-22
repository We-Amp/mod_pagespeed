<script lang="ts">
  import { router, routes } from "$lib/router.svelte";
  import { detectBasePath } from "$lib/utils/base-path";
  import { AdminApiClient } from "$lib/api/client";
  import { onMount } from "svelte";
  import type { Component } from "svelte";

  const { basePath, isGlobal } = detectBasePath();
  const consoleLabel = isGlobal ? "Global Admin" : "Admin";

  let sidebarOpen = $state(false);
  let loadedComponent = $state<Component | null>(null);
  let loadError = $state<string | null>(null);
  let showLicenseBanner = $state(false);
  let bannerDismissed = $state(false);

  const api = new AdminApiClient(basePath);

  onMount(async () => {
    try {
      const status = await api.getLicenseStatus();
      showLicenseBanner = !status.licensed;
    } catch {
      showLicenseBanner = true;
    }
  });

  // Reactively load the component when the route changes.
  $effect(() => {
    const route = router.currentRoute;
    loadedComponent = null;
    loadError = null;
    if (route) {
      route.component().then(
        (mod) => { loadedComponent = mod.default; },
        (err) => { loadError = err.message ?? String(err); }
      );
    }
  });

  function toggleSidebar() {
    sidebarOpen = !sidebarOpen;
  }

  function navigate(path: string) {
    router.navigate(path);
    sidebarOpen = false;
  }

  function dismissBanner() {
    bannerDismissed = true;
  }
</script>

<div class="layout" class:has-banner={showLicenseBanner && !bannerDismissed}>
  <!-- Topbar -->
  <header class="topbar">
    <button class="menu-toggle" onclick={toggleSidebar} aria-label="Toggle menu">
      <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
        <path d="M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z" />
      </svg>
    </button>
    <span class="topbar-title">
      <a href="https://modpagespeed.com" target="_blank" rel="noopener noreferrer" class="topbar-logo-link" aria-label="ModPageSpeed – visit modpagespeed.com">
        <svg class="topbar-logo" viewBox="0 0 370 58" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
          <!-- Prompt chevron -->
          <text class="logo-accent" x="0" y="30" font-family="'JetBrains Mono', 'SF Mono', 'Fira Code', monospace" font-weight="700" font-size="26">&#x276F;</text>
          <!-- Name -->
          <text x="24" y="30" font-family="Inter, system-ui, sans-serif" font-weight="700" font-size="32" fill="currentColor" letter-spacing="-1.2">mod_pagespeed</text>
          <!-- Blinking cursor -->
          <rect class="logo-accent logo-cursor" x="312" y="6" width="2.5" height="28" rx="1"/>
          <!-- Green status dot -->
          <circle cx="5" cy="49" r="3" fill="#059669"/>
          <!-- Output line -->
          <text class="logo-accent" x="14" y="53" font-family="'JetBrains Mono', 'SF Mono', monospace" font-weight="700" font-size="14">v1.1</text>
          <text class="logo-muted" x="56" y="53" font-family="'JetBrains Mono', 'SF Mono', monospace" font-weight="600" font-size="14"> · running · we-amp.com</text>
        </svg>
      </a>
      <span class="topbar-badge">{consoleLabel}</span>
    </span>
  </header>

  <!-- License warning banner -->
  {#if showLicenseBanner && !bannerDismissed}
    <div class="license-banner">
      <span class="license-banner-text">
        No active license. <a href="#/license" class="license-banner-link">Manage license &rarr;</a>
      </span>
      <button class="license-banner-dismiss" onclick={dismissBanner} aria-label="Dismiss">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor">
          <path d="M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z" />
        </svg>
      </button>
    </div>
  {/if}

  <!-- Sidebar -->
  <nav class="sidebar" class:open={sidebarOpen}>
    <ul class="nav-list">
      {#each routes as route}
        <li>
          <button
            class="nav-item"
            class:active={router.hash === route.path}
            onclick={() => navigate(route.path)}
          >
            {route.label}
          </button>
        </li>
      {/each}
    </ul>
  </nav>

  <!-- Backdrop for mobile sidebar -->
  {#if sidebarOpen}
    <div class="backdrop" onclick={() => (sidebarOpen = false)} role="presentation"></div>
  {/if}

  <!-- Main content -->
  <main class="content">
    {#if loadError}
      <p class="error">Failed to load page: {loadError}</p>
    {:else if loadedComponent}
      <svelte:component this={loadedComponent} {basePath} {isGlobal} />
    {:else}
      <p class="loading">Loading...</p>
    {/if}
  </main>
</div>

<style>
  .layout {
    display: grid;
    grid-template-areas:
      "topbar topbar"
      "sidebar content";
    grid-template-columns: var(--ps-sidebar-width) 1fr;
    grid-template-rows: var(--ps-topbar-height) 1fr;
    height: 100vh;
    overflow: hidden;
  }

  .layout.has-banner {
    grid-template-areas:
      "topbar topbar"
      "banner banner"
      "sidebar content";
    grid-template-rows: var(--ps-topbar-height) auto 1fr;
    overflow: hidden;
  }

  .topbar {
    grid-area: topbar;
    display: flex;
    align-items: center;
    gap: var(--ps-space-sm);
    padding: 0 var(--ps-space-md);
    background: var(--ps-primary);
    color: var(--ps-text-inverse);
    z-index: 10;
  }

  .topbar-title {
    display: flex;
    align-items: center;
    gap: var(--ps-space-xs, 4px);
    font-size: var(--ps-font-size-lg);
    font-weight: 500;
  }

  .topbar-logo-link {
    display: flex;
    align-items: center;
    text-decoration: none;
    color: inherit;
    border-radius: var(--ps-border-radius);
    transition: opacity 0.15s ease;
  }

  .topbar-logo-link:hover {
    opacity: 0.85;
  }

  .topbar-logo {
    height: 20px;
    width: auto;
  }

  .logo-accent {
    fill: #60a5fa;
  }

  .logo-muted {
    fill: rgba(255, 255, 255, 0.5);
  }

  @media (prefers-color-scheme: dark) {
    .logo-accent {
      fill: #1e3a5f;
    }

    .logo-muted {
      fill: rgba(0, 0, 0, 0.4);
    }
  }

  .logo-cursor {
    animation: blink 1.2s ease-in-out infinite;
  }

  @keyframes blink {
    0%, 100% { opacity: 0.9; }
    50% { opacity: 0; }
  }

  @media (prefers-reduced-motion: reduce) {
    .logo-cursor {
      animation: none;
      opacity: 0.9;
    }
  }

  .topbar-badge {
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    padding: 2px 8px;
    border-radius: var(--ps-border-radius);
    background: rgba(255, 255, 255, 0.15);
    letter-spacing: 0.02em;
    white-space: nowrap;
  }

  .menu-toggle {
    display: none;
    background: none;
    border: none;
    color: inherit;
    cursor: pointer;
    padding: var(--ps-space-xs);
    border-radius: var(--ps-border-radius);
  }

  .menu-toggle:hover {
    background: rgba(255, 255, 255, 0.1);
  }

  .sidebar {
    grid-area: sidebar;
    background: var(--ps-bg-secondary);
    border-right: 1px solid var(--ps-border-light);
    overflow-y: auto;
    padding: var(--ps-space-sm) 0;
  }

  .nav-list {
    list-style: none;
  }

  .nav-item {
    display: block;
    width: 100%;
    padding: var(--ps-space-sm) var(--ps-space-lg);
    background: none;
    border: none;
    color: var(--ps-text-secondary);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size-sm);
    text-align: left;
    cursor: pointer;
    transition: background 0.15s, color 0.15s;
  }

  .nav-item:hover {
    background: var(--ps-surface-hover);
    color: var(--ps-text);
  }

  .nav-item.active {
    color: var(--ps-primary);
    background: var(--ps-primary-light);
    font-weight: 500;
  }

  .content {
    grid-area: content;
    padding: var(--ps-space-lg);
    overflow-y: auto;
  }

  .license-banner {
    grid-area: banner;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: var(--ps-space-sm);
    padding: var(--ps-space-sm) var(--ps-space-md);
    background: color-mix(in srgb, var(--ps-warning) 15%, var(--ps-bg));
    border-bottom: 2px solid var(--ps-warning);
    font-size: var(--ps-font-size-sm);
    color: var(--ps-text);
  }

  .license-banner-text {
    text-align: center;
  }

  .license-banner-link {
    color: var(--ps-primary);
    text-decoration: none;
    font-weight: 500;
  }

  .license-banner-link:hover {
    text-decoration: underline;
  }

  .license-banner-dismiss {
    background: none;
    border: none;
    color: var(--ps-text-secondary);
    cursor: pointer;
    padding: var(--ps-space-xs);
    border-radius: var(--ps-border-radius);
    display: flex;
    align-items: center;
    flex-shrink: 0;
  }

  .license-banner-dismiss:hover {
    background: color-mix(in srgb, var(--ps-warning) 25%, var(--ps-bg));
    color: var(--ps-text);
  }

  .backdrop {
    display: none;
  }

  .error {
    color: var(--ps-error);
    padding: var(--ps-space-md);
  }

  .loading {
    color: var(--ps-text-tertiary);
    padding: var(--ps-space-md);
  }

  @media (max-width: 768px) {
    .layout {
      grid-template-columns: 1fr;
      grid-template-areas:
        "topbar"
        "content";
    }

    .layout.has-banner {
      grid-template-areas:
        "topbar"
        "banner"
        "content";
    }

    .menu-toggle {
      display: block;
    }

    .sidebar {
      position: fixed;
      top: var(--ps-topbar-height);
      left: 0;
      bottom: 0;
      width: var(--ps-sidebar-width);
      z-index: 20;
      transform: translateX(-100%);
      transition: transform 0.2s ease;
    }

    .sidebar.open {
      transform: translateX(0);
    }

    .backdrop {
      display: block;
      position: fixed;
      inset: 0;
      top: var(--ps-topbar-height);
      background: rgba(0, 0, 0, 0.3);
      z-index: 15;
    }
  }
</style>
