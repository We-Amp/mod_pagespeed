<script lang="ts">
  import { router, routes } from "$lib/router.svelte";
  import { detectBasePath } from "$lib/utils/base-path";
  import { AdminApiClient } from "$lib/api/client";
  import { onMount } from "svelte";
  import type { Component } from "svelte";

  const { basePath, isGlobal } = detectBasePath();
  const consoleLabel = isGlobal ? "Global Admin" : "Admin";
  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";

  let sidebarOpen = $state(false);
  let loadedComponent = $state<Component | null>(null);
  let loadError = $state<string | null>(null);
  let showLicenseBanner = $state(false);

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
</script>

<div class="layout" class:has-banner={showLicenseBanner}>
  <!-- Topbar -->
  <header class="topbar">
    <button class="menu-toggle" onclick={toggleSidebar} aria-label="Toggle menu">
      <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
        <path d="M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z" />
      </svg>
    </button>
    <span class="topbar-title">
      <a href="https://modpagespeed.com" target="_blank" rel="noopener noreferrer" class="topbar-logo-link" aria-label="ModPageSpeed – visit modpagespeed.com">
        <svg class="topbar-logo" viewBox="0 0 322 36" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
          <!-- Prompt chevron -->
          <text class="logo-accent" x="0" y="30" font-family="'JetBrains Mono', 'SF Mono', 'Fira Code', monospace" font-weight="700" font-size="26">&#x276F;</text>
          <!-- Name -->
          <text x="24" y="30" font-family="Inter, system-ui, sans-serif" font-weight="700" font-size="32" fill="currentColor" letter-spacing="-1.2">mod_pagespeed</text>
          <!-- Blinking cursor -->
          <rect class="logo-accent logo-cursor" x="312" y="6" width="2.5" height="28" rx="1"/>
        </svg>
      </a>
      <span class="topbar-subtitle">
        <span class="topbar-version" title="Build version">{appVersion}</span>
        <span class="topbar-subtitle-sep" aria-hidden="true">·</span>
        <span class="topbar-running">running</span>
        <span class="topbar-subtitle-sep" aria-hidden="true">·</span>
        <a
          href="https://we-amp.com"
          target="_blank"
          rel="noopener noreferrer"
          class="topbar-subtitle-link"
          aria-label="We-Amp – visit we-amp.com"
        >we-amp.com</a>
      </span>
      <span class="topbar-badge">{consoleLabel}</span>
      {#if showLicenseBanner}
        <!-- the design record: soft enforcement — amber warning, not a red error. -->
        <span class="topbar-pill-unlicensed" aria-label="License status: unlicensed (optimization running)">Unlicensed</span>
      {/if}
    </span>
  </header>

  <!-- License warning banner -->
  <!-- the design record: soft enforcement — amber warning, not a red error. Optimization
       keeps running while unlicensed; this only nudges toward activating. -->
  {#if showLicenseBanner}
    <div class="license-banner" role="status">
      <svg class="license-banner-icon" width="18" height="18" viewBox="0 0 24 24"
           fill="none" stroke="currentColor" stroke-width="2"
           stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
        <path d="M10.29 3.86 1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/>
        <line x1="12" y1="9" x2="12" y2="13"/>
        <line x1="12" y1="17" x2="12.01" y2="17"/>
      </svg>
      <span class="license-banner-text">
        <strong>Unlicensed — optimization is running; activate a license to remove the warning.</strong>
        <a href="#/license" class="license-banner-link"
          >Purchase a license or apply a key in the License section&nbsp;&rarr;</a>
      </span>
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

  @media (prefers-color-scheme: dark) {
    .logo-accent {
      fill: #1e3a5f;
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

  .topbar-subtitle {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    margin-left: 8px;
    font-family: 'JetBrains Mono', 'SF Mono', 'Fira Code', monospace;
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: rgba(255, 255, 255, 0.55);
    white-space: nowrap;
  }

  .topbar-version {
    color: #60a5fa;
    font-weight: 700;
  }

  .topbar-subtitle-sep {
    opacity: 0.6;
  }

  .topbar-subtitle-link {
    color: inherit;
    text-decoration: none;
    border-bottom: 1px solid transparent;
    transition: border-color 0.15s, color 0.15s;
  }

  .topbar-subtitle-link:hover,
  .topbar-subtitle-link:focus-visible {
    border-bottom-color: rgba(255, 255, 255, 0.6);
    color: rgba(255, 255, 255, 0.9);
    outline: none;
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

  .topbar-pill-unlicensed {
    font-size: var(--ps-font-size-xs);
    font-weight: 700;
    padding: 2px 10px;
    border-radius: var(--ps-border-radius);
    background: var(--ps-warning, #f59e0b);
    color: #1f1300;
    letter-spacing: 0.04em;
    text-transform: uppercase;
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

  .license-banner-icon {
    color: var(--ps-warning);
    flex-shrink: 0;
  }

  .license-banner-text strong {
    font-weight: 600;
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

    .topbar-subtitle {
      display: none;
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
