// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Install links for the step-by-step fix posts. A published post is frozen
// content, so the link to the install path that matches the post's server
// lives here and the blog post template renders it below the post. Each
// platform points at its card on /download/ (the card ids are set in
// src/pages/download.astro). A post that is not listed gets no extra link.
// `kind` names what the platform installs (a server module or the ASP.NET
// Core middleware) and words the lead-in above the links.

export type InstallPlatform = 'apache' | 'nginx' | 'aspnet-core';

export const INSTALL_PLATFORM_LINKS: Record<
  InstallPlatform,
  { label: string; href: string; event: string; kind: 'module' | 'middleware' }
> = {
  apache: {
    label: 'Install on Apache',
    href: '/download/#apache',
    event: 'cta_blog_bottom_platform_apache',
    kind: 'module',
  },
  nginx: {
    label: 'Install on nginx',
    href: '/download/#nginx',
    event: 'cta_blog_bottom_platform_nginx',
    kind: 'module',
  },
  'aspnet-core': {
    label: 'Install on ASP.NET Core',
    href: '/download/#aspnet-core',
    event: 'cta_blog_bottom_platform_aspnet',
    kind: 'middleware',
  },
};

// Post slug -> the servers whose config the post shows, in display order.
// A post that gives both an Apache and an nginx config lists both; a post
// that gives only an nginx config (the nginx posts, the Magento LCP post)
// lists nginx. The ASP.NET Core posts configure the in-process middleware
// in Program.cs only, so they list the ASP.NET Core card.
export const BLOG_INSTALL_PLATFORMS: Record<string, InstallPlatform[]> = {
  'fix-lcp-wordpress-2026': ['apache', 'nginx'],
  'fix-cls-wordpress-2026': ['apache', 'nginx'],
  'fix-inp-wordpress-2026': ['apache', 'nginx'],
  'fix-lcp-woocommerce-2026': ['apache', 'nginx'],
  'fix-cls-woocommerce-2026': ['apache', 'nginx'],
  'fix-inp-woocommerce-2026': ['apache', 'nginx'],
  'fix-lcp-magento-2026': ['nginx'],
  'fix-cls-magento-2026': ['apache', 'nginx'],
  'fix-inp-magento-2026': ['apache', 'nginx'],
  'fix-lcp-nginx-2026': ['nginx'],
  'fix-cls-nginx-2026': ['nginx'],
  'fix-inp-nginx-2026': ['nginx'],
  'fix-lcp-aspnet-core-2026': ['aspnet-core'],
  'fix-cls-aspnet-core-2026': ['aspnet-core'],
  'fix-inp-aspnet-core-2026': ['aspnet-core'],
};
