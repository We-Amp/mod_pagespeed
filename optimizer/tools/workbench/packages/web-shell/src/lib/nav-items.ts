// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/**
 * Left-nav configuration for the workbench console SPA.
 *
 * Items are rendered in order. A `separator: true` flag draws a divider
 * above the item. Routes themselves live under `src/routes/` and remain
 * reachable by URL even when their nav entry is hidden.
 */
export interface NavItem {
  label: string;
  href: string;
  description: string;
  /** Show a divider above this item. */
  separator?: boolean;
}

export const navItems: NavItem[] = [
  { label: 'Dashboard', href: '', description: 'Overview and stats' },
  {
    label: 'Configuration',
    href: '/config',
    description: 'Worker settings',
  },
  { label: 'URLs', href: '/urls', description: 'Cached URL inspector' },
  { label: 'Savings', href: '/savings', description: 'Bandwidth savings' },
  {
    label: 'Metrics',
    href: '/metrics',
    description: 'All counters and stats',
  },
  // Logs, Waterfall, and Visual Diff are hidden from the nav until their
  // UX is finalized. The routes (`/logs`, `/waterfall`, `/diff`) remain
  // reachable by URL. The guard in `__tests__/nav-items.test.ts` prevents
  // accidental re-introduction — update it if you re-enable any entry.
  // { label: 'Logs', href: '/logs', description: 'Real-time log stream' },
  // {
  //   label: 'Waterfall',
  //   href: '/waterfall',
  //   description: 'Network waterfall capture',
  //   separator: true,
  // },
  // {
  //   label: 'Visual Diff',
  //   href: '/diff',
  //   description: 'Screenshot comparison',
  // },
  {
    label: 'About',
    href: '/about',
    description: 'Version & system info',
    separator: true,
  },
];
