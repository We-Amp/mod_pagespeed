<!-- SPDX-License-Identifier: Apache-2.0
     Copyright (c) 2024-2026 We-Amp B.V. -->

<script lang="ts">
  import { page } from '$app/state';
  import { resolve } from '$app/paths';
  import { navItems } from '#lib/nav-items.ts';

  // The configured base path ('/console'), without a trailing slash.
  const base = resolve('/').replace(/\/$/, '');

  function isActive(itemHref: string, pathname: string): boolean {
    const fullHref = base + itemHref;
    if (itemHref === '') {
      // Dashboard: exact match on base or base/
      return pathname === base || pathname === base + '/';
    }
    return pathname.startsWith(fullHref);
  }
</script>

<nav class="sidebar" aria-label="Main navigation">
  <ul class="nav-list">
    {#each navItems as item}
      <li class:nav-separator={item.separator}>
        <a
          href="{base}{item.href}"
          class="nav-link"
          class:nav-link-active={isActive(item.href, page.url.pathname)}
          aria-current={isActive(item.href, page.url.pathname) ? 'page' : undefined}
        >
          {item.label}
        </a>
      </li>
    {/each}
  </ul>
</nav>

<style>
  .sidebar {
    width: 220px;
    height: 100%;
    background: var(--ps-bg-secondary);
    border-right: 1px solid var(--ps-border);
    flex-shrink: 0;
    overflow-y: auto;
    padding: var(--ps-space-sm) 0;
  }

  .nav-list {
    list-style: none;
    margin: 0;
    padding: 0;
  }

  .nav-link {
    display: block;
    padding: var(--ps-space-sm) var(--ps-space-lg);
    font-family: var(--ps-font-family);
    font-size: var(--ps-font-size);
    color: var(--ps-fg-primary);
    text-decoration: none;
    border-left: 3px solid transparent;
    transition:
      background-color 0.1s ease,
      border-color 0.1s ease;
  }

  .nav-link:hover {
    background: var(--ps-bg-hover);
  }

  .nav-link-active {
    background: var(--ps-bg-hover);
    border-left-color: var(--ps-accent);
    color: var(--ps-accent);
    font-weight: 500;
  }

  .nav-link:focus-visible {
    outline: 2px solid var(--ps-border-focus);
    outline-offset: -2px;
  }

  .nav-separator {
    margin-top: var(--ps-space-xs);
    border-top: 1px solid var(--ps-border);
    padding-top: var(--ps-space-xs);
  }
</style>
