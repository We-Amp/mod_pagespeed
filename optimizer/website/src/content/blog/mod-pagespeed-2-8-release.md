---
title: 'mod_pagespeed 2.8.0: security fixes and nginx.org packages'
description: 'mod_pagespeed 2.8.0 fixes a large number of security issues; update recommended. New nginx module packages for Ubuntu 26.04 and nginx.org''s nginx.'
date: 2026-11-05
tags: ['announcement', 'release', 'security', 'nginx']
product: '1.1'
draft: false
---


mod_pagespeed 2.8.0 fixes a large number of security issues in the nginx, Apache and IIS modules, the optimizer worker and the ASP.NET Core package. We recommend that all sites update. The release also adds native nginx module packages for Ubuntu 26.04 and for nginx.org's stable nginx (as in the official nginx Docker image).

## Security

The fixes fall into six classes. Affected: 2.7.0 and earlier.

- **Restricted content no longer reaches other clients.** Optimized output is now built only from content the site serves to any client, and access rules and origin caching restrictions are respected before anything is served from a cache. Update recommended for all sites, in particular those that restrict parts of a site by address, sign-in or access rules.
- **Cached content stays with its site and URL.** Cached resources are now kept apart per site and per URL. Update recommended for all sites, in particular servers that host several sites.
- **Optimizer requests stay on the site's own hosts.** The optimizer now fetches only from hosts the site serves. Update recommended for sites with agent optimization on (nginx module and ASP.NET Core package).
- **Optimized pages keep the page's structure.** Optimized pages now keep the structure the browser reads in the original. Update recommended for all sites, in particular sites that publish content from others.
- **Requests can no longer exceed the server's configured limits.** Requests now stay within the configured values. Update recommended for all sites.
- **Server workers are protected from crafted content.** The work and memory that one page, stylesheet or image can cause are now bounded, with new directives where operators may need to adjust a bound. Update recommended for all sites.

Several fixes change what is optimized or cached; the [upgrade notes](#upgrade-notes) below list what operators will notice. The Envoy port is experimental: do not expose it to untrusted networks. The [2.8.0 release notes](/docs/release-notes-2-8/#v2-8-0) describe each class, who is affected, what operators will notice, and the credits.

## Upgrade notes

- Update the server modules and the optimizer together: the modules need the optimizer release that ships with them, and in-place optimization stays off with an older one. Reload nginx, Apache or IIS after updating the optimizer; nothing new is optimized until the web server reloads.
- The caches of the nginx, Apache and IIS modules, the optimizer worker and the ASP.NET Core package start cold once, at the update. Expect extra CPU and origin fetches until the caches refill, and some responses go out without optimization until then. The module and optimizer caches need no purge: entries stored by earlier versions are not used. Downstream caches are covered below. On a cluster, update one node at a time.
- Server blocks and virtual hosts now keep separate optimization caches by default, and each IIS site has its own cache. Sites on one server that should share optimized resources, such as a site and its static or CDN host on separate blocks, need the same explicit `CacheFragment`; share one only between sites you operate together.
- Resources that a site serves only to some clients are left as they are; expect lower savings on pages that link to them. To have a resource optimized, serve it to any client.
- Purge downstream caches such as a CDN if your site was affected by the issues in the Security section, because responses they stored before the update remain until they expire. Purge cached HTML in any CDN after upgrading if your site serves pages hydrated by a client-side framework.

## nginx module packages for Ubuntu 26.04 and nginx.org's nginx

2.8.0 adds the nginx module for Ubuntu 26.04 with its stock nginx, and a new package for nginx.org's stable nginx.

The nginx.org module package `nginx-module-pagespeed-nginxorg`, built for nginx.org's stable nginx (as in the official nginx Docker image), is available for Debian 13, Ubuntu 24.04 and Enterprise Linux 9 (AlmaLinux, Rocky Linux, RHEL), amd64 (x86_64) only, one build per nginx version. In Docker, build on the official nginx image pinned to its exact stable version (the Debian-based variant; not alpine, not `nginx:latest`), and move the image tag only once the package for that version exists.

Upgrade nginx and the module together: when both are in the repositories, a plain upgrade moves both. If nginx.org publishes a newer nginx first, `apt upgrade` holds nginx back and the module keeps working (`apt full-upgrade` removes the module); to take an nginx security fix before the module build exists, remove the module, upgrade nginx, and reinstall the module when its package is available.

On Enterprise Linux 9 the package installs a small dnf plugin that holds nginx (and nginx.org's module packages for it) at the version the module is built for until the matching build is available, so a plain `dnf update` takes every other update and `dnf check-update` still lists the pending nginx; to take an nginx security fix first, run `dnf upgrade nginx --allowerasing`, which removes the module package (reinstall it once its build is available).

The packages for each distribution's stock nginx, including the new Ubuntu 26.04 one, ship for:

- amd64 and arm64: Debian 12 and 13, Ubuntu 22.04, 24.04 and 26.04
- x86_64 and aarch64: AlmaLinux, Rocky Linux and RHEL 9
- x86_64 only: AlmaLinux, Rocky Linux and RHEL 10

## In the admin console

The admin console now shows how much of the saving on responses served from the optimizer's cache came from compression applied by mod_pagespeed and how much from its optimization; your web server's own compression on other responses is not counted. The two parts, plus a share not yet attributed, add up to the total saving. The total now counts HTML that the optimizer makes larger, for example by inlining above-the-fold CSS or adding preload hints; earlier versions counted that as zero, so the total can read lower than before without anything optimizing less. After the update the caches start cold, so the split fills in as pages are re-optimized; until then the console shows the total only.

## Also in this release

- Server-rendered React, Next.js, Vue, Nuxt and Svelte pages that mod_pagespeed recognizes as hydrated no longer break hydration because of its HTML rewrites. On pages it recognizes as hydrated, mod_pagespeed leaves stylesheet and script URLs, inline styles, comments, whitespace and script deferral unchanged and still optimizes images, so CSS and script savings may drop on those pages. Recognition can miss a page; the [hydrated pages guide](/docs/hydrated-pages/) lists the filters to disable there.
- On nginx, mod_pagespeed now rewrites a page after nginx's SSI, sub_filter, charset and addition filters, so stylesheets, scripts and images that SSI includes or sub_filter inserts are now optimized. A sub_filter that edited the rewritten page no longer sees mod_pagespeed's output, and rewritten HTML carries a single Cache-Control line.
- A conditional request for an optimized resource now gets a 304 that carries the same caching headers as the full response, so browsers that cached it earlier learn its long lifetime after one revalidation, and those that honour `immutable` (desktop Firefox and Safari) stop revalidating it.
- When the optimizer cannot defer a page's stylesheet, it no longer adds an inline copy of the critical CSS, so the HTML is smaller; first paint still waits for the full stylesheet, as before. Pages whose stylesheet does not block the first paint keep the inline copy. If you set `--no-async-css`, check that your pages look right: inlining without deferral is no longer available.
- Lazy-loaded images now also get `decoding="async"`, unless the page already sets a decoding attribute; the LCP candidate image and the first iframe are left as they are.
- Visitors whose browser did not ask for reduced data always get a normal-quality image.
- Fallback images are now cached only briefly, so each device class gets its own image soon after the update.

---

If you run 2.7.0 or earlier, update now, and read the upgrade notes in the [2.8.0 release notes](/docs/release-notes-2-8/#v2-8-0) first. If you run nginx.org's nginx or Ubuntu 26.04 and have not used the module yet, start with the [nginx install steps](/docs/installation-module/#native-nginx-module).

If you hit a problem after updating, open an issue in [We-Amp/mod_pagespeed](https://github.com/We-Amp/mod_pagespeed/issues) with your version and a reproduction.
