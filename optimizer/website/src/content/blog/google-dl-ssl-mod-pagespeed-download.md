---
title: 'mod_pagespeed download: dl-ssl.google.com is frozen'
description: 'The dl-ssl.google.com mod_pagespeed .deb is the last stable build, 1.13.35.2 (2018), with years of unpatched CVEs. Get the maintained download instead.'
date: 2026-05-20
author: 'Otto van der Schaaf'
tags: ['deprecation', 'history', 'downloads']
draft: false
lastUpdated: 2026-10-24
---

If you landed here, you probably ran something like this and got a
working `.deb`:

```bash
wget https://dl-ssl.google.com/dl/linux/direct/mod-pagespeed-stable_current_amd64.deb
sudo dpkg -i mod-pagespeed-stable_current_amd64.deb
```

That URL has historically served `mod_pagespeed` 1.13.35.2, the last
stable `.deb` Google shipped (2018) before handing the project to the
Apache Software Foundation. If the URL still resolves for you in 2026,
the file behind it is that same frozen binary. It has had no security
patches in years.

## Install the maintained build instead

If you came here looking for an install command, this is the one to
run on Debian or Ubuntu. It installs the signed apt repository at
`packages.modpagespeed.com` and the maintained mod_pagespeed
package:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install mod-pagespeed
```

The package is now `mod-pagespeed`; it replaces `mod-pagespeed-stable`
in one step and keeps your configuration (see the
[migration guide](/docs/migrate-from-google-mod-pagespeed/)). Same filter
syntax, same admin console at `/pagespeed_admin/`. Full instructions
below, or jump straight to the [download page](/download/) for every
platform.

This post explains what happened to the original download, why the
file might still be there, what's broken about installing it today,
and where to get a maintained build instead.

## Short timeline

| Year      | Event                                                                                                                       |
| --------- | --------------------------------------------------------------------------------------------------------------------------- |
| 2010      | Google releases `mod_pagespeed` for Apache.                                                                                 |
| 2013      | nginx port released as `ngx_pagespeed`.                                                                                     |
| 2017      | Google donates the project to the Apache Software Foundation; active Google development ends.                               |
| 2018      | `mod_pagespeed` 1.13.35.2 ships — Google's last stable release.                                                             |
| 2020      | `mod_pagespeed` 1.14.36.1 ships — the final open-source release, under the Apache incubator.                                |
| 2023      | Apache Incubator podling retires.                                                                                           |
| 2025      | GitHub repositories marked read-only.                                                                                       |
| 2018–2026 | The last Google `.deb` (1.13.35.2) stays accessible at the `dl-ssl.google.com` URL with no patches and no successor banner. |
| 2026      | You're reading this.                                                                                                        |

The full history is at [mod_pagespeed is deprecated](/blog/mod-pagespeed-deprecated-2026/) if you want the context.

## Why the download URL still works

Google didn't take the file down. They left it accessible as a courtesy
to operators who had install scripts and CI pipelines referencing it.
There is no redirect to a successor, no banner on the file, and no
notice in the package metadata. The `.deb` itself is unchanged from its
2018 build.

This is the part that traps people. The URL is the same URL it has been
for 15 years. Hosting providers' documentation still references it.
Stack Overflow answers from 2018 still recommend it. The install
completes successfully. There is no in-band signal that this is a
frozen artifact.

## What's broken about installing it in 2026

Concrete list, not editorial.

### Unpatched CVEs in image-decoding dependencies

`mod_pagespeed` bundles its own `libpng`, `libwebp`, `libjpeg-turbo`,
and ICU versions, statically linked. The bundled versions are pinned to
their 2018-era releases. Every CVE in those libraries since then is unpatched
in the module's address space, including the ones that affect parsing
attacker-controlled image bytes (e.g. CVE-2023-4863 in libwebp, a heap
overflow that hit every browser using the bundled decoder), which is
exactly what `mod_pagespeed` does on every request that hits its
rewrite path.

A sampling of unpatched CVE territory in the bundled libraries:

- `libwebp` (multiple buffer-overflow CVEs disclosed 2021–2024, including CVE-2023-4863)
- `libpng` (read-out-of-bounds CVEs in chunk parsing)
- ICU (multiple memory-safety issues in normalization)

If your threat model accepts "module decodes attacker-controlled image
bytes with five-year-old image libraries", the 2018 build is technically
functional. For most operators it is not.

### No build against newer Apache releases

Apache 2.4 has been the production line for over a decade. Builds
against current Apache trunk fail because internal APIs have shifted,
and `mod_pagespeed` has no upstream maintainer to follow those
changes. Distributions that backport API changes or move toward newer
Apache releases eventually break the module. There is no upstream
fix because the upstream is archived.

### No AVIF in Google's build

AVIF never landed in Google's open-source releases. Google's last build
only emits WebP and the legacy JPEG/PNG paths. In 2026 that's a meaningful weight
penalty for above-the-fold hero images: AVIF is typically 20–30%
smaller than WebP at equivalent quality (see [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/)).
AVIF encoding landed in mod_pagespeed: the module and the optimizer worker both emit it.

### No Core Web Vitals signals

LCP, INP, and CLS-aware filters were on the roadmap. None of them
shipped. The module's optimization passes are tuned for 2017-era
page-weight metrics, not the 2024+ Core Web Vitals.

The actual filters (image variants, CSS inlining, JS minification)
still produce useful output. But Google's own ranking metric, the one
the module was originally designed to improve, has moved on, and the
2018 build doesn't know about the new shape of the problem.

### Toolchain rot

The build system wants old Bazel, Python 2 in some helper scripts, and
glibc constraints that match 2020 LTS distributions. Building from
source on a 2026 machine is a weekend project even if you don't change
a single line of code. This matters if you ever need to patch a CVE
yourself, audit the build for supply-chain assurance, or rebuild for a
custom Apache version.

## Where to get a maintained build instead

You have two actively-maintained paths. The right choice depends on
your deployment shape: a native in-process module that drops into an
existing 1.13.35.2 install, or a reverse-proxy rewrite for nginx.

### Path 1: `mod_pagespeed`, the native in-process module {#path-1-mod_pagespeed-115-the-native-in-process-module}

Same configuration syntax as 1.13.35.2. Same filter names. Same admin
console. The native in-process module for Apache 2.4+, nginx, and IIS.
CVE patches, a current toolchain, and the Cyclone Cache.

If you have a working 1.13.35.2 install and want a security update
that doesn't require config changes, this is the path. The
[mod_pagespeed download docs](/download/) list every platform package;
the [mod_pagespeed site](/) has the full reference.

**Debian / Ubuntu (Apache):**

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install mod-pagespeed
```

**Debian / Ubuntu (nginx):**

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install nginx-module-pagespeed
```

**RHEL / Rocky / Fedora (Apache):**

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf install mod-pagespeed
```

**RHEL / Rocky / Fedora (nginx):**

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf install nginx-module-pagespeed
```

The setup script installs a signed apt or yum repository at
`packages.modpagespeed.com`, signed with the We-Amp public GPG key. CVE
patches ship through the same channel. The nginx package targets each
distro's stock nginx (Ubuntu 24.04 ships nginx 1.24.0, AlmaLinux 9 ships
1.20.1); for nginx.org stable or mainline builds, compile the module from
the source tarball.

Configuration in `/etc/httpd/conf.d/pagespeed.conf` or
`/etc/nginx/conf.d/pagespeed.conf` is identical to the Google build. No
migration needed beyond verifying the new install loaded:

```bash
# Apache
$ httpd -M 2>&1 | grep pagespeed
 pagespeed_module (shared)

$ curl -sI http://localhost/ | grep -i pagespeed
X-Mod-Pagespeed: <version>
```

The header carries the version of the package you installed.

```bash
# nginx
$ nginx -T 2>&1 | grep -i pagespeed
load_module modules/ngx_pagespeed_module.so;
pagespeed on;
...

$ curl -sI http://localhost/ | grep -iE 'page-?speed'
X-Page-Speed: <version>
```

The header carries the version of the package you installed.

### Path 2: the optimizer worker, from the 2.0 re-architecture {#path-2-modpagespeed-20-the-rewrite}

Built from scratch by the 2.0 re-architecture for nginx and
container-native deployment. Separate worker process, shared Cyclone
cache via mmap, runtime content negotiation against `Accept`. A
different architecture from the original mod_pagespeed, with its own
independent C++23 optimization core underneath.

If you want a reverse-proxy plus async-worker model on nginx in front of
any HTTP origin, this is the path.

```bash
# mod_pagespeed — ASP.NET Core middleware
dotnet add package WeAmp.PageSpeed.AspNetCore
```

It optimizes out of the box, and it is licensed under Apache-2.0 and free
to run in development and in production.

Full setup walkthrough at [Run mod_pagespeed with Docker Compose in 5
minutes](/blog/run-with-docker-compose/).

### IIS and ASP.NET Core {#iis-aspnet-core-envoy}

If you were installing `mod_pagespeed` because it had a port for your
server, the modern equivalents:

- **IIS:** the [download page](/download/) ships an MSI installer. The IIS module ships as a signed MSI on the mod_pagespeed GitHub release.
- **ASP.NET Core:** the [WeAmp.PageSpeed.AspNetCore NuGet
  middleware](/go/nuget/?from=blog-google-dl-ssl)
  registers the mod_pagespeed optimization core as ASP.NET Core middleware.

## What to do with the existing install

If you already installed the 2018 build, here's the migration:

```bash
# Add the signed repository
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh

# Swap the packages in one run (keeps your pagespeed.conf)
sudo apt-get install mod-pagespeed mod-pagespeed-stable-

# Check, restart, verify
sudo apache2ctl configtest && sudo systemctl restart apache2
curl -sI http://localhost/ | grep -iE 'page-?speed'
```

On RHEL, Rocky and AlmaLinux the swap is
`sudo dnf swap mod-pagespeed-stable mod-pagespeed`; the
[migration guide](/docs/migrate-from-google-mod-pagespeed/) has every step,
including disabling the old repository and rolling back.

Your existing `pagespeed.conf` continues to work. Same filter names,
same admin console at `/pagespeed_admin/` (and `/pagespeed_global_admin`
in the module for the process-wide view). Cache directory under
`/var/cache/mod_pagespeed` is preserved across the migration and will be
re-populated as new requests hit.

If you used the `dl-ssl.google.com` URL in a Dockerfile, CI script, or
Ansible role, swap the install line for
`curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh` and
install `mod-pagespeed` instead of `mod-pagespeed-stable`.

## Why this page exists

The deprecation isn't loud. Google didn't take the file down, didn't
add a banner, didn't redirect to anything. If you found this page by
searching for `mod_pagespeed download`, you probably found the
`dl-ssl.google.com` URL first and were about to install a 2020 binary
with years of unpatched CVEs.

The maintained continuation lives on [modpagespeed.com](/): mod_pagespeed. The optimization work that `mod_pagespeed` did still matters. The
2020 binary isn't where to get it in 2026.

## Get the maintained build

```bash
# Debian / Ubuntu
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo apt install mod-pagespeed

# RHEL / Rocky / Fedora
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
sudo dnf install mod-pagespeed
```

mod_pagespeed 2 is the drop-in continuation. It optimizes out of the
box, and it is licensed under Apache-2.0 and free to run in development and
in production — see the [license](/license/).
Starting fresh on nginx? The
[mod_pagespeed Docker Compose quickstart](/blog/run-with-docker-compose/)
covers the rewrite.

## Related

- [mod_pagespeed is deprecated](/blog/mod-pagespeed-deprecated-2026/) — full deprecation story
- [mod_pagespeed alternative in 2026](/alternatives/mod-pagespeed/) — decision matrix
- [Google PageSpeed Module alternative](/alternatives/google-pagespeed-module/) — disambiguation
- [All alternatives](/alternatives/)
- [mod_pagespeed docs](/) — the maintained continuation
- [Run ModPageSpeed 2.0 with Docker Compose](/blog/run-with-docker-compose/) — modern install path
