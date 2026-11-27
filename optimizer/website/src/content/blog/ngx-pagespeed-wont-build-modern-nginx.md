---
title: "ngx_pagespeed won't build on modern nginx: cause and fix"
description: "Why upstream ngx_pagespeed won't compile on nginx 1.25+ (sys_siglist, glibc, OpenSSL 3, toolchain drift) — and the prebuilt module to install instead."
date: 2026-06-06
lastUpdated: 2026-10-22
tags: ['ngx_pagespeed', 'nginx', 'migration', 'troubleshooting']
product: '1.1'
howTo:
  name: 'Install the prebuilt nginx-module-pagespeed instead of compiling ngx_pagespeed'
  description: 'Replace a failing from-source ngx_pagespeed build with the signed, prebuilt nginx-module-pagespeed (1.15) from packages.modpagespeed.com.'
  steps:
    - name: 'Add the signed repository'
      text: 'Run the install script, which detects your distro, installs the GPG signing key, and writes the apt or yum source file.'
    - name: 'Install the module'
      text: 'Install nginx-module-pagespeed with apt. It drops a modules-enabled auto-include snippet, so there is no .so path to wire up by hand.'
    - name: 'Reload and verify'
      text: 'Reload nginx and confirm the module is live by checking for the X-Page-Speed response header.'
---

If you have tried to build ngx_pagespeed against nginx 1.25 or later, you have probably seen one of these:

```
undefined reference to `sys_siglist'
```

```
psol/include/.../ ... : error: 'EVP_MD_CTX_create' was not declared in this scope
```

```
make: *** [psol] Error 2
```

The same `./configure --add-module=...` recipe that produced a clean nginx binary on Ubuntu 18.04 now dies partway through linking, or the resulting binary segfaults on first request. Your config didn't change. The toolchain underneath it did.

## What the errors mean

ngx_pagespeed is a thin nginx module. The optimization work lives in [PSOL, the PageSpeed Optimization Libraries](/psol/): a large precompiled C++ artifact the build downloads and links against. PSOL was compiled years ago against the toolchain and glibc of its day. The errors above are what you get when you link those old binaries against a current toolchain.

The messages aren't self-explanatory, so here is what each one points at:

- **`sys_siglist` removed from glibc.** glibc 2.32 (2020) dropped the long-deprecated `sys_siglist` symbol in favor of `strsignal()`. PSOL and the surrounding build reference it. On any distro shipping glibc 2.32 or newer — Ubuntu 21.10 and up, Debian 12, current Fedora and RHEL derivatives — the link step fails with `undefined reference to 'sys_siglist'`. This is the single most common report.
- **C++ ABI and OpenSSL 3 drift.** The prebuilt PSOL expects an older libstdc++ ABI and the OpenSSL 1.1 API. Modern distros ship OpenSSL 3, GCC 12/13/14, and a newer libstdc++. Symbols get renamed, removed, or change signatures, and the mismatch shows up as undefined references or, worse, a binary that links but misbehaves at runtime.
- **nginx internals moved.** The module API and the internal structs ngx_pagespeed reaches into are not frozen. Around nginx 1.23 the from-source build started failing for a chunk of users, and the gap has widened through the 1.25 and 1.27 lines and into the current stable and mainline series.

You can check the version gap yourself. As of mid-2026, nginx publishes 1.30 as the stable branch and 1.31 as mainline. The last PSOL build predates that by a wide margin.

## Why upstream stopped tracking nginx

ngx_pagespeed was solid, open-source infrastructure, built and contributed to throughout the Google era. The project moved to the Apache Incubator in 2017 and the podling retired in 2023, and the upstream `apache/incubator-pagespeed-ngx` repository is no longer actively developed. PSOL has not been rebuilt for new glibc, new compilers, or OpenSSL 3. nginx kept moving; the precompiled libraries did not.

So when you `git clone` upstream and point `--add-module` at it on a 2026 box, you are linking a years-old binary into a current toolchain. It fails, and `./configure` flags cannot fix a binary that references a symbol the C library no longer exports.

## Two paths from here

**Path one: keep patching the from-source build.** You can keep it alive for a while. People do. The recipe is roughly: `sed` the `sys_siglist` references over to `strsignal()`, pin an older GCC, find OpenSSL 1.1 compat headers, and rebuild PSOL from source against your toolchain when the precompiled artifact refuses to link. It can be made to work on a given box on a given day. The catch is that it is a moving target. Every nginx upgrade, glibc bump, and base-image change can break it again, on production servers you own. You end up maintaining a fork of a build system upstream no longer updates, which is recurring work for a module that should sit quietly and optimize.

**Path two: stop compiling.** The optimization work mod_pagespeed does still works: image recompression, WebP, CSS and JS minification, cache extension with [content-hashed URLs](/blog/content-hash-urls/), critical CSS. The part that broke is the from-source build. We maintain the module and ship it prebuilt and signed from a package repository. No PSOL download, no `--add-module`, no recompiling nginx.

## Installing the prebuilt module

mod_pagespeed 2 is the maintained continuation of the open-source module — the same native, in-process nginx module, built for current distros and pinned to each distro's stock nginx. It serves from a signed apt and yum repository at `packages.modpagespeed.com`, amd64 and arm64.

One line adds the repo. It detects the distribution, installs the GPG signing key, and writes the source file:

```sh
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
```

Then install the nginx module:

```sh
# Debian 12/13, Ubuntu 22.04/24.04
sudo apt-get install nginx-module-pagespeed
```

That drops a modules-enabled auto-include snippet, so there is no `.so` path to wire into your config by hand. Enable it in your `http {}` block the same way you always did:

```nginx
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
```

Reload, then verify the module is actually live by checking the response header it stamps on optimized responses:

```sh
sudo nginx -t && sudo systemctl reload nginx
curl -sI https://your-site.example/ | grep -i x-page-speed
# X-Page-Speed: <version>
```

The header carries the version of the package you installed.

If that header is present, the module loaded and is rewriting responses on the request path, with no compile step on your end.

One constraint to know up front: each prebuilt `.so` is pinned to its distro's stock nginx version, because nginx's `--with-compat` does not relax the version check for a native module. The repository currently covers Debian 12 (nginx 1.22.1), Debian 13 (1.26.3), Ubuntu 22.04 (1.18.0), and Ubuntu 24.04 (1.24.0), on amd64 and arm64. If you run a hand-rolled nginx newer than your distro's stock build, you still need a module compiled against that exact version. The difference is that it becomes our build to maintain, not yours. The Apache module ships the same way (`mod-pagespeed` via apt, plus el9 and el10 via yum), so a mixed Apache and nginx fleet stays on one repository.

## When to move to mod_pagespeed {#when-to-reach-for-20-instead}

The prebuilt [module](/) is the closest match to what you already run, and mod_pagespeed ships it. If you are reworking this part of the stack anyway, the optimizer worker — the second part, out of the C++23 2.0 re-architecture — runs behind an nginx reverse proxy or as ASP.NET Core middleware, generating viewport-aware variants into the variant-aware cache both parts map. Installing the module alone is the smallest change; adding the worker is the larger one.

## The short version

The from-source ngx_pagespeed build fails on modern nginx because the precompiled PSOL libraries reference symbols that current glibc, GCC, and OpenSSL no longer provide (`sys_siglist` most often), against an nginx whose internals have moved on. The upstream repository is no longer actively developed, so PSOL will not be rebuilt there. You can keep patching the from-source build, or install the maintained, signed, prebuilt module and skip the PSOL compile entirely.

The [ngx_pagespeed alternatives](/alternatives/ngx-pagespeed/) page lays out the options side by side. Install steps are at [apt + yum setup](/download/apt-yum/).
