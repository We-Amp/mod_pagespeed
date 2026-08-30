<script lang="ts">
  import { router, routes } from "$lib/router.svelte";
  import { detectBasePath } from "$lib/utils/base-path";
  import {
    PRODUCT_DISPLAY_NAME,
    PRODUCT_NAME,
    VENDOR,
    VENDOR_HOST,
    VENDOR_URL,
    WEBSITE,
    WEBSITE_HOST,
  } from "$lib/data/product-facts-console";
  import type { Component } from "svelte";

  const { basePath, isGlobal } = detectBasePath();
  const consoleLabel = isGlobal ? "Global Admin" : "Admin";
  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";

  let sidebarOpen = $state(false);
  let loadedComponent = $state<Component | null>(null);
  let loadError = $state<string | null>(null);

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

<div class="layout">
  <!-- Topbar -->
  <header class="topbar">
    <button class="menu-toggle" onclick={toggleSidebar} aria-label="Toggle menu">
      <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor">
        <path d="M3 18h18v-2H3v2zm0-5h18v-2H3v2zm0-7v2h18V6H3z" />
      </svg>
    </button>
    <span class="topbar-title">
      <a href={WEBSITE} target="_blank" rel="noopener noreferrer" class="topbar-logo-link" aria-label="{PRODUCT_DISPLAY_NAME} – visit {WEBSITE_HOST}">
        <svg class="topbar-logo" viewBox="0 0 322 36" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
          <!-- Prompt chevron -->
          <text class="logo-accent" x="0" y="30" font-family="'JetBrains Mono', 'SF Mono', 'Fira Code', monospace" font-weight="700" font-size="26">&#x276F;</text>
          <!-- Name -->
          <text x="24" y="30" font-family="Inter, system-ui, sans-serif" font-weight="700" font-size="32" fill="currentColor" letter-spacing="-1.2">{PRODUCT_NAME}</text>
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
          href={VENDOR_URL}
          target="_blank"
          rel="noopener noreferrer"
          class="topbar-subtitle-link"
          aria-label="{VENDOR} – visit {VENDOR_HOST}"
        >{VENDOR_HOST}</a>
      </span>
      <span class="topbar-badge">{consoleLabel}</span>
    </span>
  </header>

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
    min-width: 0;
  }

  .topbar-logo-link {
    display: flex;
    align-items: center;
    text-decoration: none;
    color: inherit;
    border-radius: var(--ps-border-radius);
    transition: opacity 0.15s ease;
    flex-shrink: 0;
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
    flex-shrink: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
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

  /* On the narrowest phones the logo and scope badge can no longer coexist;
     drop the badge rather than clipping it. */
  @media (max-width: 430px) {
    .topbar-badge {
      display: none;
    }
  }
</style>
