---
title: 'Is mod_pagespeed deprecated? (2026)'
description: 'Yes, mod_pagespeed 1.13.35.2 is deprecated: the binaries run but have had no security updates in years. What to use on Apache, nginx, IIS and ASP.NET Core.'
date: 2026-05-20
author: 'Otto van der Schaaf'
tags: ['deprecation', 'migration', 'security']
draft: false
lastUpdated: 2026-10-24
---

_Disclosure: I founded We-Amp, which develops mod_pagespeed._

Yes. `mod_pagespeed` is deprecated. The 1.13.35.2 binaries still install, but the project receives no maintenance and the dependency stack has not been patched in years. The maintained continuation is [mod_pagespeed 2](/), licensed under Apache-2.0, developed by We-Amp B.V.

## Practical paths forward

| You currently run                         | Best path                                                                                                                                   |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `mod_pagespeed` 1.13.35.2 on Apache       | [mod_pagespeed 2](/) — drop-in replacement                                                                                                |
| `ngx_pagespeed` on nginx                  | [mod_pagespeed 2](/) — drop-in for nginx                                                                                                  |
| IISpeed on Windows / IIS                  | [mod_pagespeed for IIS](/alternatives/iispeed/) — the IIS module ships as a signed MSI on the mod_pagespeed GitHub release                                |
| Envoy filter chain                        | No packaged build                                                                                                                          |
| Reverse proxy in front of any HTTP origin | [mod_pagespeed](/) — see the [migration guide](/docs/migrating-to-2-1/)                                                                  |
| ASP.NET Core application                  | [WeAmp.PageSpeed NuGet middleware](/blog/aspnet-core-middleware/)                                                                           |

Through 2026, We-Amp shipped two continuations — mod_pagespeed 1.15 and ModPageSpeed 2.0 — and they have since converged: mod_pagespeed 2 is a native in-process module for Apache and nginx, using the same `ModPagespeed*` directives and the built-in `/pagespeed_admin/` console, with a separate optimizer worker doing the heavy optimization work outside the web server. It is licensed under Apache-2.0. mod_pagespeed 2 is a drop-in upgrade for mod_pagespeed 1.15 — see the [upgrade guide](/docs/migrating-to-2-1/#upgrading-from-1-15). The deeper comparison lives in the [mod_pagespeed alternatives](/blog/mod-pagespeed-alternatives/) post.

## Install snippets

```bash
# mod_pagespeed — ASP.NET Core middleware
dotnet add package WeAmp.PageSpeed.AspNetCore
```

```bash
# mod_pagespeed (Apache) — Debian/Ubuntu
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install mod-pagespeed
```

```bash
# mod_pagespeed (Apache) — RHEL/Fedora
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf install mod-pagespeed
```

```bash
# mod_pagespeed (nginx) — Debian/Ubuntu / RHEL/Fedora
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install nginx-module-pagespeed   # or: sudo dnf install nginx-module-pagespeed
```

```nginx
# nginx (mod_pagespeed 2, drop-in for ngx_pagespeed; the package loads the module itself)
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
```

## What happened — the short history

`mod_pagespeed` started as a Google open-source project in 2010. It was an Apache module that rewrote HTML, CSS, JavaScript, and images on the fly to reduce page weight. The nginx port (`ngx_pagespeed`) followed in 2013, and the IIS port (IISpeed, by We-Amp) shortly after.

The module shipped at industrial scale. Hosting providers bundled it in their cPanel and Plesk stacks. CDN vendors used the libraries inside their own optimization layers. It served billions of pages.

Google handed `mod_pagespeed` to the Apache Software Foundation as part of incubation, and active Google-side development effectively ended at that point. Maintenance contributions came in from a small group of external maintainers, including the team at We-Amp, who prepared the 1.13.35.2 release as external contributors.

The Apache Incubator podling retired in 2023, and the repositories were marked read-only on GitHub in 2025:

- [`apache/incubator-pagespeed-mod`](https://github.com/apache/incubator-pagespeed-mod)
- [`apache/incubator-pagespeed-ngx`](https://github.com/apache/incubator-pagespeed-ngx)

That's the state today. The code is public, the binaries are downloadable, the documentation is up. Nobody is patching it.

## What still works

The 1.13.35.2 binaries still install. They still build (against an increasingly narrow toolchain). And they still serve pages: image optimization, CSS/JS minification, critical CSS, all running.

If you have a working install and your threat model accepts unpatched dependencies, you can keep running it. Some operators have. The accumulating cost shows up in several places. The bundled `libpng`, `libwebp`, `libjpeg`, and ICU versions have all racked up CVEs since the 1.13.35.2 release — `libwebp` shipped a cluster of buffer-overflow CVEs disclosed 2021–2024, including CVE-2023-4863, a heap overflow that hit every browser using the bundled decoder; `libpng` saw read-out-of-bounds issues in chunk parsing; ICU saw memory-safety issues in normalization — and `mod_pagespeed` decodes attacker-controlled image bytes on every rewrite, which is exactly the threat surface those CVEs hit. Builds against current Apache trunk fail, so distributions that ship newer Apache eventually stop being viable. WebP support landed before the freeze; AVIF never did. LCP, INP, and CLS signals are missing from the filter set. And the released versions still build with a 2012-era gyp toolchain, Python 2, and old glibc constraints, so setting it up from scratch on a 2026 machine is a weekend project.

The binaries work; the ecosystem around them is decaying.

## mod_pagespeed 2 — the maintained continuation

We-Amp B.V. — the team that helped build ngx_pagespeed, maintained mod_pagespeed, and drove the project's Apache incubation — develops the converged continuation, `mod_pagespeed 2`: the native in-process module, joined by a separate optimizer worker, licensed under Apache-2.0.

The worker architecture moves optimization work out of the request path: a separate worker process reads originals from a shared cache, generates the optimized variants, and writes them back, so the request path no longer waits on encoding.

The optimization libraries (the parts that decide how to recompress a JPEG, how to fold a stylesheet, how to extract critical CSS) are the same ones mod_pagespeed proved at scale. The architecture around them is new.

`mod_pagespeed 2` continues the original codebase with security patches and a current toolchain on Apache and nginx. The IIS module ships as a signed MSI on the mod_pagespeed GitHub release — see the [upgrade guide](/docs/migrating-to-2-1/#upgrading-from-1-15) for details.

## What about PageSpeed Insights?

If you've been searching for "PageSpeed" and ended up here, a quick disambiguation: `pagespeed.web.dev` (PageSpeed Insights, PSI) is a different Google product. It's a diagnostic tool that scores a URL on Core Web Vitals, and it's still actively maintained. PSI tells you whether your page passes; a server-side module like mod_pagespeed implements the fixes PSI recommends.

See [Google PageSpeed Module alternative](/alternatives/google-pagespeed-module/) for the full disambiguation.

## Where to read next

- [Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) — the maintenance status in one page
- [mod_pagespeed alternative](/alternatives/mod-pagespeed/) — full comparison and decision matrix
- [ngx_pagespeed alternative](/alternatives/ngx-pagespeed/) — for the nginx audience specifically
- [IISpeed alternative](/alternatives/iispeed/) — for Windows / IIS / ASP.NET
- [Google PageSpeed Module alternative](/alternatives/google-pagespeed-module/) — disambiguation against PageSpeed Insights
- [mod_pagespeed alternatives — the deep comparison](/blog/mod-pagespeed-alternatives/)
- [Why I rebuilt mod_pagespeed from scratch](/blog/why-i-rebuilt-mod-pagespeed/) — the origin story behind ModPageSpeed 2.0
- [Upgrading to mod_pagespeed](/docs/migrating-to-2-1/#upgrading-from-1-15) — the drop-in upgrade
- [Migrating from ModPageSpeed 2.0](/docs/migrating-to-2-1/) — for the Docker/Helm line
- [Getting started](/docs/getting-started/) — concrete install steps
- [Production deployment](/docs/production-deployment/) — hardening and rollout for a live server

Two URLs and you're done. Use `packages.modpagespeed.com` for the signed packages, `modpagespeed.com/download` for everything else. The old binaries aren't moving from [`dl-ssl.google.com`](/blog/google-dl-ssl-mod-pagespeed-download/); you should.
