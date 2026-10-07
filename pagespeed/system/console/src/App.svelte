<!--
  SPDX-License-Identifier: Apache-2.0
  Copyright (c) 2024-2026 We-Amp B.V.
-->

<script lang="ts">
  import { onMount, tick, untrack, type Component } from "svelte";
  import { AdminApiClient } from "$lib/api/client";
  import { connection, type ConnectionView } from "$lib/api/connection";
  import { provideConsole } from "$lib/api/context";
  import { pollers } from "$lib/api/poller";
  import ConnectionBanner from "$lib/ConnectionBanner.svelte";
  import { HostLens, activeLensHost } from "$lib/host-lens.svelte";
  import HostLensSelect from "$lib/HostLensSelect.svelte";
  import { ConsoleScope } from "$lib/console-scope.svelte";
  import LoadError from "$lib/LoadError.svelte";
  import { NAV_GROUPS, router, routes } from "$lib/router.svelte";
  import ShortcutHelp from "$lib/ShortcutHelp.svelte";
  import { ShortcutReader } from "$lib/shortcuts";
  import { isNavActive, navKeyHint } from "$lib/utils/nav";
  import { detectBasePath } from "$lib/utils/base-path";
  import { DEFAULT_PATH, LatestGate, pageTitle } from "$lib/utils/hash-route";
  import { LensFollower, lensAddressRewrite, lensFromParams, lensStorageKey, loadLens, withLens } from "$lib/utils/host-lens";
  import { markProgrammaticFocus } from "$lib/utils/programmatic-focus";
  import { moduleTag } from "$lib/utils/versions";
  import {
    CONSOLE_TITLE,
    PRODUCT_DISPLAY_NAME,
    PRODUCT_NAME,
    VENDOR,
    VENDOR_HOST,
    VENDOR_URL,
    WEBSITE,
    WEBSITE_HOST,
  } from "$lib/data/product-facts-console";

  const { basePath, isGlobal: pathSaysGlobal } = detectBasePath();
  const api = new AdminApiClient(basePath);
  const scope = new ConsoleScope(pathSaysGlobal, window.location.host);
  // The host lens: a shared link's lens= wins over the remembered one. Each
  // console remembers its own (lensStorageKey), and only the whole-server
  // console ever selects or stores one: a per-host console's link with
  // lens= changes nothing.
  const lensKey = lensStorageKey(api.basePath);
  const lens = new HostLens(scope.isGlobal ? loadLens(undefined, lensKey) : null, lensKey);
  const linkedLens = lensFromParams(router.params);
  if (scope.isGlobal && linkedLens !== null) lens.select(linkedLens);
  provideConsole({ api, scope, lens });
  let lensHost = $derived(activeLensHost(scope, lens));

  // A link followed later can carry lens= too, and a renamed whole-server
  // admin path is known as such only once the configuration answers: then
  // the remembered lens is restored (LensFollower). A navigation and the
  // scope are the only dependencies; the address is read live, untracked,
  // as the control rewrites it in place. Whenever a lens is active, the
  // address carries it: an address typed or followed without lens= is
  // rewritten in place (replaceState -- no history entry, no reload, no
  // hashchange, so this cannot loop). Every navigation counts, also one to
  // the address as it was before such a rewrite (router.navigations; the
  // router's hash is not written, which would reload the page). A per-host
  // console never does.
  const lensFollower = new LensFollower(scope.isGlobal, () => loadLens(undefined, lensKey));
  $effect(() => {
    void router.navigations;
    const isGlobal = scope.isGlobal;
    untrack(() => {
      const next = lensFollower.next(isGlobal, window.location.hash, lens.host);
      if (next !== undefined) lens.select(next);
      const address = lensAddressRewrite(isGlobal, window.location.hash, lens.host);
      if (address !== null) history.replaceState(history.state, "", address);
    });
  });

  // The control: select, then rewrite the current address in place (no
  // reload) so it can be shared.
  function selectLens(value: string): void {
    lens.select(value === "" ? null : value);
    history.replaceState(history.state, "", withLens(window.location.hash || DEFAULT_PATH, lens.host));
  }
  // The console's scope comes from the configuration, read once for every
  // page; the admin path stands in until then, or if the read fails. A
  // read that fails at start is retried once the connection to the server
  // is next seen restored (scope.config stays null until a read succeeds).
  function loadConfig(): void {
    api.getConfig().then(
      (r) => scope.apply({ scope: r.scope, host: r.host }),
      () => {},
    );
  }
  loadConfig();
  const appVersion = import.meta.env.VITE_APP_VERSION ?? "dev";

  let sidebarOpen = $state(false);
  let loadedComponent = $state<Component | null>(null);
  let loadError = $state<string | null>(null);

  // What the top bar says about the server: answered or not (not a health check).
  let connectionView = $state<ConnectionView>(connection.view);
  onMount(() =>
    connection.subscribe((v) => {
      connectionView = v;
      if (scope.config === null && v.status === "connected" && v.lastAnsweredAt !== null) {
        loadConfig();
      }
    }),
  );

  // Load the page for the current hash (a new query on the same page loads
  // it afresh). Only the most recent navigation may show its page -- a
  // slower load must not replace a later one; the tab title names the page,
  // and after the first load focus moves to the new page's heading.
  const loads = new LatestGate();
  let firstLoad = true;
  $effect(() => {
    const route = router.currentRoute;
    const ticket = loads.begin();
    // Every navigation closes the phone's drawer, however it was made
    // (link tap, keyboard shortcut, back/forward).
    sidebarOpen = false;
    loadedComponent = null;
    loadError = null;
    document.title = pageTitle(route.title ?? route.label, CONSOLE_TITLE);
    route.component().then(
      async (mod) => {
        if (!loads.isCurrent(ticket)) return;
        loadedComponent = mod.default;
        if (firstLoad) {
          firstLoad = false;
          return;
        }
        await tick();
        focusPageHeadingAfterNavigation();
      },
      (err) => {
        if (loads.isCurrent(ticket)) loadError = err?.message ?? String(err);
      },
    );
  });

  function pageHeading(): HTMLElement | null {
    // A page may name a heading further down (a section= link): it takes the
    // focus, and the browser brings it into view.
    const heading =
      document.querySelector<HTMLElement>("main [data-page-focus]") ??
      document.querySelector<HTMLElement>("main h1") ??
      document.getElementById("main");
    if (heading !== null && !heading.hasAttribute("tabindex")) heading.setAttribute("tabindex", "-1");
    return heading;
  }

  /** The skip link: the user asked for it, so the usual focus ring rules apply. */
  function focusPageHeading(): void {
    pageHeading()?.focus();
  }

  /** After a navigation: the console moves focus, so no focus box (programmatic-focus.ts). */
  function focusPageHeadingAfterNavigation(): void {
    const heading = pageHeading();
    if (heading === null) return;
    markProgrammaticFocus(heading);
    heading.focus();
  }

  function toggleSidebar() {
    sidebarOpen = !sidebarOpen;
  }

  const shortcuts = new ShortcutReader();
  let helpOpen = $state(false);

  function isEditable(target: EventTarget | null): boolean {
    return (
      target instanceof HTMLElement &&
      (target.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName))
    );
  }

  function onKeydown(e: KeyboardEvent): void {
    if (helpOpen) return; // the dialog handles its own keys
    const action = shortcuts.read(
      { key: e.key, ctrlKey: e.ctrlKey, metaKey: e.metaKey, altKey: e.altKey, editable: isEditable(e.target) },
      Date.now(),
    );
    if (action === null) return;
    e.preventDefault();
    switch (action.kind) {
      case "help":
        helpOpen = true;
        break;
      case "refresh":
        void pollers.refreshAll();
        break;
      case "search":
        document.querySelector<HTMLElement>("main input[type='text'], main input[type='search']")?.focus();
        break;
      case "go":
        router.navigate(action.path);
        break;
    }
  }
</script>

<svelte:window onkeydown={onKeydown} />

<div class="layout">
  <!-- Topbar -->
  <header class="topbar">
    <button type="button" class="skip-link" onclick={focusPageHeading}>Skip to content</button>
    <button
      type="button"
      class="menu-toggle"
      onclick={toggleSidebar}
      aria-label="Toggle menu"
      aria-expanded={sidebarOpen}
      aria-controls="console-nav"
    >
      <svg width="24" height="24" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
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
        <span class="topbar-version" title={appVersion}>{moduleTag(appVersion)}</span>
        <span class="topbar-subtitle-sep" aria-hidden="true">·</span>
        <span class="topbar-connection" data-testid="connection-state">{connectionView.status === "connected" ? "connected" : "reconnecting"}</span>
        <span class="topbar-subtitle-sep topbar-vendor" aria-hidden="true">·</span>
        <a
          href={VENDOR_URL}
          target="_blank"
          rel="noopener noreferrer"
          class="topbar-subtitle-link topbar-vendor"
          aria-label="{VENDOR} – visit {VENDOR_HOST}"
        >{VENDOR_HOST}</a>
      </span>
      <span class="topbar-tools">
        <!-- Tells the two consoles apart; a per-host console needs no label. -->
        {#if scope.isGlobal}<span class="topbar-badge">Global Admin</span>{/if}
        <button type="button" class="shortcuts-button" onclick={() => (helpOpen = true)} aria-label="Keyboard shortcuts" title="Keyboard shortcuts (?)">?</button>
      </span>
    </span>
    {#if scope.isGlobal}
      <span class="topbar-lens">
        <HostLensSelect id="host-lens-top" testid="host-lens" host={lens.host} options={lens.options} onselect={selectLens} />
      </span>
    {/if}
  </header>

  <!-- Sidebar -->
  <nav class="sidebar" class:open={sidebarOpen} id="console-nav" aria-label="Console pages">
    <div class="drawer-scope">
      <span class="drawer-chip">{scope.isGlobal ? "Whole server" : scope.host || "This host"}</span>
      {#if scope.isGlobal}
        <HostLensSelect id="host-lens-drawer" testid="host-lens-drawer" host={lens.host} options={lens.options} onselect={selectLens} />
      {/if}
    </div>
    {#each NAV_GROUPS as group (group.id)}
      {#if group.label}
        <p class="nav-group-label" class:nav-group-label--muted={group.muted === true} id="nav-group-{group.id}">{group.label}</p>
      {/if}
      <ul class="nav-list" aria-labelledby={group.label ? `nav-group-${group.id}` : undefined}>
        {#each routes.filter((r) => r.group === group.id && r.hidden !== true && (r.globalOnly !== true || scope.isGlobal)) as route (route.path)}
          {@const hint = navKeyHint(route.path)}
          {@const active = isNavActive(route, router.path, routes)}
          <li class="nav-row">
            <a
              class="nav-item"
              class:active
              href={withLens(route.path, lensHost)}
              aria-current={active ? "page" : undefined}
              aria-keyshortcuts={hint ?? undefined}
              onclick={() => (sidebarOpen = false)}
            >{route.label}</a>
            <!-- The hint sits beside the link, not in it: the link's name stays the page's name. -->
            {#if hint}<span class="nav-key" aria-hidden="true">{hint}</span>{/if}
          </li>
        {/each}
      </ul>
    {/each}
  </nav>

  <!-- Backdrop for mobile sidebar -->
  {#if sidebarOpen}
    <div class="backdrop" onclick={() => (sidebarOpen = false)} role="presentation"></div>
  {/if}

  <!-- Main content -->
  <main class="content" id="main" tabindex="-1">
    <ConnectionBanner />
    {#if lensHost !== null && router.currentRoute.hostAware !== true}
      <p class="lens-note" data-testid="lens-note">
        Host lens {lensHost}: this page has no per-host figures, so it shows the whole server.
      </p>
    {/if}
    {#if loadError}
      <LoadError message={`Failed to load page: ${loadError}`} />
    {:else if loadedComponent}
      {@const Page = loadedComponent}
      <Page />
    {:else}
      <p class="loading">Loading...</p>
    {/if}
  </main>
  <ShortcutHelp open={helpOpen} onclose={() => (helpOpen = false)} />
</div>

<style>
  .layout {
    display: grid;
    grid-template-areas:
      "topbar topbar"
      "sidebar content";
    grid-template-columns: var(--ps-sidebar-width) 1fr;
    grid-template-rows: var(--ps-topbar-height) 1fr;
    min-height: 100vh;
    min-height: 100dvh;
  }

  /* One rhythm across the top bar: groups (the logo, the version line, the
     console label with the shortcuts button, the host lens) sit
     --ps-space-md apart, items within a group --ps-space-sm, and the bar's
     side padding is the content's, so their edges line up. */
  .topbar {
    grid-area: topbar;
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
    padding: 0 var(--ps-gutter);
    background: var(--ps-topbar-bg);
    color: var(--ps-topbar-fg);
    position: sticky;
    top: 0;
    z-index: 10;
  }

  .topbar-title {
    display: flex;
    align-items: center;
    gap: var(--ps-space-md);
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
    fill: var(--ps-topbar-accent);
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
    gap: var(--ps-space-sm);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    color: var(--ps-topbar-fg-muted);
    white-space: nowrap;
  }

  .topbar-version {
    font-weight: 700;
  }

  .topbar-subtitle-link {
    color: inherit;
    text-decoration: none;
    border-bottom: 1px solid transparent;
    transition: border-color 0.15s;
  }

  .topbar-subtitle-link:hover,
  .topbar-subtitle-link:focus-visible {
    border-bottom-color: currentColor;
  }

  .topbar-tools {
    display: inline-flex;
    align-items: center;
    gap: var(--ps-space-sm);
    min-width: 0;
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

  .shortcuts-button {
    flex-shrink: 0;
    width: 1.75rem;
    height: 1.75rem;
    border: 1px solid rgba(255, 255, 255, 0.4);
    border-radius: 50%;
    background: none;
    color: inherit;
    font-weight: 600;
    cursor: pointer;
  }

  .topbar-lens {
    margin-left: auto;
    flex-shrink: 1;
    min-width: 0;
  }

  .lens-note {
    margin: 0 0 var(--ps-space-md);
    padding: var(--ps-space-xs) var(--ps-space-sm);
    border-left: 3px solid var(--ps-primary);
    background: var(--ps-bg-secondary);
    color: var(--ps-text-secondary);
    font-size: var(--ps-font-size-sm);
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

  /* The sidebar's band always runs from the top bar to the bottom of the
     window, on a short page, anywhere on a long one and while a page is
     loading: it is exactly one window tall (the dynamic viewport where the
     browser knows it) and stays put while the content scrolls; links that
     do not fit scroll inside it. */
  .sidebar {
    grid-area: sidebar;
    background: var(--ps-bg-secondary);
    border-right: 1px solid var(--ps-border-light);
    overflow-y: auto;
    padding: var(--ps-space-sm) 0;
    position: sticky;
    top: var(--ps-topbar-height);
    height: calc(100vh - var(--ps-topbar-height));
    height: calc(100dvh - var(--ps-topbar-height));
    align-self: start;
  }

  .drawer-scope {
    display: none;
  }

  .nav-list {
    list-style: none;
  }

  /* The link spans the whole row, so its highlight (current page, hover)
     covers the shortcut hint too; the hint sits over the link's right
     padding and lets pointer events through to it. */
  .nav-row {
    position: relative;
  }

  /* Room for the widest hint ("g o") at the hint's own size. */
  .nav-row .nav-item {
    padding-right: calc(var(--ps-space-md) + 3em);
  }

  .nav-key {
    position: absolute;
    top: 50%;
    right: var(--ps-space-md);
    transform: translateY(-50%);
    font-family: var(--ps-font-mono);
    font-size: var(--ps-font-size-xs);
    color: var(--ps-text-secondary);
    white-space: nowrap;
    pointer-events: none;
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
    text-decoration: none;
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

  .skip-link {
    position: absolute;
    left: -9999px;
    top: 0;
  }

  .skip-link:focus {
    left: var(--ps-space-sm);
    top: var(--ps-space-sm);
    z-index: 30;
    padding: var(--ps-space-xs) var(--ps-space-md);
    border: 2px solid var(--ps-primary);
    border-radius: var(--ps-border-radius);
    background: var(--ps-bg);
    color: var(--ps-text);
  }

  .nav-group-label {
    margin: var(--ps-space-md) 0 var(--ps-space-xs);
    padding: 0 var(--ps-space-lg);
    font-size: var(--ps-font-size-xs);
    font-weight: 600;
    letter-spacing: 0.05em;
    text-transform: uppercase;
    color: var(--ps-text-secondary);
  }

  .nav-group-label--muted {
    font-weight: 500;
    letter-spacing: normal;
    text-transform: none;
  }

  .content {
    grid-area: content;
    padding: var(--ps-gutter);
  }

  /* The top bar's band is dark in both schemes: its focus ring is the
     top-bar foreground. */
  .topbar :focus-visible {
    outline-color: var(--ps-topbar-fg);
  }

  .backdrop {
    display: none;
  }

  .loading {
    color: var(--ps-text-secondary);
    padding: var(--ps-space-md);
  }

  /* On a narrow desktop window the vendor link gives way, so the console
     label and the host lens keep their full width. */
  @media (max-width: 960px) {
    .topbar-vendor {
      display: none;
    }
  }

  @media (max-width: 768px) {
    .nav-key {
      display: none;
    }

    /* The phone's top bar keeps its compact spacing. */
    .topbar {
      gap: var(--ps-space-sm);
      padding: 0 var(--ps-space-md);
    }

    .topbar-title,
    .topbar-tools {
      gap: var(--ps-space-xs);
    }

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

    /* The drawer: everything below the top bar, from top to bottom. */
    .sidebar {
      position: fixed;
      top: var(--ps-topbar-height);
      left: 0;
      bottom: 0;
      height: auto;
      align-self: stretch;
      width: var(--ps-sidebar-width);
      z-index: 20;
      transform: translateX(-100%);
      transition: transform 0.2s ease;
    }

    .topbar-lens {
      display: none;
    }

    .drawer-scope {
      display: flex;
      align-items: center;
      flex-wrap: wrap;
      gap: var(--ps-space-sm);
      padding: var(--ps-space-sm) var(--ps-space-lg);
      border-bottom: 1px solid var(--ps-border-light);
    }

    .drawer-chip {
      font-size: var(--ps-font-size-xs);
      font-weight: 600;
      padding: 2px var(--ps-space-sm);
      border-radius: 999px;
      background: var(--ps-primary-light);
      color: var(--ps-primary);
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
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
