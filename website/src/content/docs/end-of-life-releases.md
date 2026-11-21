---
title: 'End-of-life releases: 1.14.36.1 and earlier'
seoTitle: 'mod_pagespeed 1.14.36.1 and earlier: end-of-life advisory'
description: 'Advisory for the archived mod_pagespeed and ngx_pagespeed releases, 1.14.36.1 and earlier: what is known, how to check a server, what to do.'
order: 17
group: 'Upgrade and migrate'
lastUpdated: 2026-11-17
datePublished: 2026-11-17
---

If a report says a server reports mod_pagespeed or ngx_pagespeed 1.14.36.1 or earlier, it
concerns releases that receive no security fixes from the original project.
[Check the server's header](#check), then
[remove the module or move to a build that still receives security fixes](#what-to-do).

These releases build their own copies of several image libraries into the module, so
operating-system updates do not replace them. Their libwebp is in the range affected by
[CVE-2023-4863](https://www.cve.org/CVERecord?id=CVE-2023-4863). The original project's
last release was 1.14.36.1, in 2020. The project was donated to the Apache Software
Foundation in 2017, the Incubator podling retired in 2023, and the repositories became
read-only in 2025.

mod_pagespeed is an open-source project originally developed at Google. The current
mod_pagespeed is developed by We-Amp B.V. and is not affiliated with or endorsed by
Google.

## Which releases this covers {#scope}

- Every release of the original project. The last two are 1.13.35.2 (February 2018)
  and 1.14.36.1 (2020); all earlier releases are covered too.
- Builds made from the original project's unreleased source after 1.14.36.1. They
  identify themselves as `1.15.0.0-` followed by a number.
- IISpeed, We-Amp's deprecated product for IIS, which was built on these releases. It
  reports five numbers, such as `1.14.36.1.4`, and is end of life too.

It does not cover releases of the current mod_pagespeed. For those, see the
[security updates](/docs/release-notes/#security-updates) in the release notes and the
supported versions on the [security page](/security/). If we emailed you about one of
these releases, see [how to confirm the notice](/security/#end-of-life-notices).

## How to check a server {#check}

The module adds a response header to the pages it serves: `X-Mod-Pagespeed` on
Apache and `X-Page-Speed` on nginx and on IIS.

```bash
curl -sL -o /dev/null -D - https://www.example.com/ | grep -i -E 'x-mod-pagespeed|x-page-speed'
```

| Header value                                                           | What it means                                                                                                                     |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| Four numbers up to `1.14.36.1`, such as `1.13.35.2-0` or `1.14.36.1-0` | Identifies as a release of the original project. End of life. |
| `1.15.0.0-` and a number                                               | Identifies as a build of the original project's unreleased source. End of life.                                                   |
| Five numbers, such as `1.14.36.1.4` | Usually IISpeed, We-Amp's deprecated product for IIS. End of life. |
| Three numbers, such as `1.19.0` or `2.8.2`                             | A release of the current mod_pagespeed. Not covered here; check it against the [security updates](/docs/release-notes/#security-updates). |
| Any other suffix, such as `1.13.35.2-5w` | Not matched here. Ask whoever built it what it contains. |
| `on`, `1` or other text | The version is hidden. The current mod_pagespeed shows `on` by default since 2.7.0, but any build can be set to show it. Check the installed package, as shown below. |

No header does not mean no module: the header can be changed, or removed by other
configuration or a proxy. To see what is
installed, run `dpkg -l | grep -i pagespeed` on Debian and Ubuntu or
`rpm -qa | grep -i pagespeed` on Enterprise Linux, and `nginx -V` for a module compiled
into nginx.

The header shows which release a build identifies as. It does not show what is inside.
A third party can rebuild an old release with newer libraries and keep the version
number. If your module came from a packager other than the original project, ask the
packager which library versions it contains and whether it carries fixes for the
module's own code.

## What is known {#known}

- **The bundled image libraries are the versions pinned at release.** 1.13.35.2 builds
  in libwebp 0.5.1 and 1.14.36.1 builds in libwebp 0.6.1. Builds of the unreleased
  source carry libwebp 0.6.1 or 1.1.0.
- **CVE-2023-4863 applies to those libwebp versions.** It is a memory-safety flaw in
  WebP decoding, rated high (CVSS 8.8) by NVD. The CVE record lists every libwebp
  version before 1.3.2 as affected.
- **A system update does not change the module.** The original project's build compiles
  libwebp from its own source tree into the module and has no switch for a system
  libwebp.
- **Default settings are in scope.** Image optimization, WebP included, is on in the
  default configuration.
- **Other bundled libraries are of the same age.** The copies of libpng, libjpeg-turbo,
  giflib and zlib date from 2016 and 2017. This page does not map CVEs to them.
- **Defects in the module's own code have been fixed since.** The current
  mod_pagespeed has fixed further defects inherited from these releases: memory safety
  in image processing, denial of service from untrusted input, and cross-site scripting
  in generated output. They are listed by impact and affected version under
  [security updates](/docs/release-notes/#security-updates).

## What is not known {#not-known}

- **Whether CVE-2023-4863 can be exploited through the module.** As of 17 November
  2026, we had not attempted it and knew of no public report of exploitation through
  the module. Public reports of exploitation of this CVE concerned client software.
- **Whether a given site is exposed.** That depends on whether images from sources the
  operator does not control reach the module's image optimization: uploads to the site
  itself, a proxied backend, or other domains the configuration authorizes. Only the
  operator can judge it.
- **What third-party builds contain.** We looked at public packaging information and
  found no packager that states it updated these libraries under an original version
  number. We have not inspected third-party binaries.
- **Which library versions IISpeed carries.** The IISpeed installer on our download
  page was built in September 2022, before the fixed libwebp existed. Treat IISpeed like
  the releases above.

## What to do {#what-to-do}

Back up the module's configuration and test each step in staging first.

1. **Remove the module if nothing depends on it.** On Apache, run
   `sudo apt-get purge mod-pagespeed-stable` (Debian, Ubuntu) or
   `sudo dnf remove --noautoremove mod-pagespeed-stable` (Enterprise Linux; `yum remove`
   on older systems), using `-beta` if you installed that package. `--noautoremove`
   keeps the web server installed. Delete `/etc/apt/sources.list.d/mod-pagespeed.list`
   if it is still there, then restart Apache. On nginx, delete the `pagespeed`
   directives and any `load_module` line for `ngx_pagespeed_module.so`, then run
   `sudo nginx -t` and restart. If `nginx -V` lists ngx_pagespeed, install an nginx
   build without it. On IIS, uninstall IISpeed under _Apps & features_, then run
   `iisreset`. An expired license does not switch IISpeed fully off, so uninstall it in
   that case too.
2. **Or move to a build that still receives security fixes.** The current mod_pagespeed
   keeps the directives and the filter names, with a few exceptions listed in the
   guides: [Migrate from the archived Google module](/docs/migrate-from-google-mod-pagespeed/)
   and [Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/). From IISpeed, see
   [Migrate from IISpeed](/docs/migrate-from-iispeed/). Other maintained
   builds and implementations exist; we have not assessed them.

### If you cannot do either yet {#interim}

Switch the module fully off as a stopgap. It stays installed, so this is not a fix.

- **Apache:** `ModPagespeed unplugged`.
- **nginx, 1.12.34.2 and later:** `pagespeed unplugged;`.
- **nginx, 1.12.34.1 and earlier:** use `pagespeed off;`. On those releases `unplugged`
  crashes the server.
- **IISpeed:** no stopgap has been verified, and an expired license is not one.
  Uninstall it.

Set it in the main server configuration, and remove any `ModPagespeed on` or
`pagespeed on;` from virtual hosts and server blocks, which can turn it back on. Then
restart. Apart from the old nginx releases above, `off` is not enough: the module still
answers requests for `.pagespeed.` resources and still honors PageSpeed query
parameters. See [Setting the module fully off](/1.0/doc/configuration.html#unplugged)
in the original documentation. We have not verified any narrower measure, such as
disabling single image filters, and do not recommend relying on one.

## Detection rule for scanners {#rule}

This section is for scanning organizations. Apply the rule to the trimmed value of `X-Mod-Pagespeed` or `X-Page-Speed`. The header
appears on responses the module processed, so redirects, error pages and unrelated
virtual hosts may not show it. A match means the build identifies as an end-of-life
release; it is not a confirmed vulnerability. Leave any value that does not match
untagged. The rule may change; check the date of this page.

In words: four numbers at or below `1.14.36.1`, without leading zeros, optionally
followed by a hyphen with or without digits, `-stable` or `-beta`; or `1.15.0.0-` and
digits.

As an expression, to be applied as a full match, case-sensitive, on a single line:

```text
^(?:(?:0\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)|1\.(?:[0-9]|1[0-3])\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)|1\.14\.(?:[0-9]|[12][0-9]|3[0-5])\.(?:0|[1-9][0-9]*)|1\.14\.36\.[01])(?:-(?:[0-9]*|stable|beta))?|1\.15\.0\.0-[0-9]+)$
```

| Value                                                                   | Result              |
| ----------------------------------------------------------------------- | ------------------- |
| `1.13.35.2-0`, `1.14.36.1-0`, `1.14.36.1`, `1.9.32.3-4448`, `1.7.30.4-` | Match               |
| `1.12.34.2-stable`, `0.8.1.5`, `1.15.0.0-8917`                          | Match               |
| `on`, `1`, `enabled`, `Powered By mod_pagespeed`                        | No match            |
| `1.1.0`, `1.15.0`, `1.19.0`, `2.8.2`                                    | No match            |
| `1.15.0.0`, `1.14.36.2`, `1.14.37.0-0`, `1.13.35.2-5w`                  | No match            |
| `1.14.36.1.4`                                                           | No match; see below |

IISpeed reports five numbers, often followed by text. It is end of life as well and has
its own expression:

```text
^1\.1[34]\.[0-9]+\.[0-9]+\.[0-9]+(?: .*)?$
```

## Sources {#sources}

- The CVE record for [CVE-2023-4863](https://www.cve.org/CVERecord?id=CVE-2023-4863)
  and its [NVD entry](https://nvd.nist.gov/vuln/detail/CVE-2023-4863).
- The libraries pinned by the original project's release tags,
  [1.13.35.2](https://github.com/apache/incubator-pagespeed-mod/tree/v1.13.35.2/third_party)
  and
  [1.14.36.1](https://github.com/apache/incubator-pagespeed-mod/tree/v1.14.36.1/third_party).
  The `libwebp` entries point at libwebp
  [0.5.1](https://github.com/webmproject/libwebp/tree/v0.5.1) and
  [0.6.1](https://github.com/webmproject/libwebp/tree/v0.6.1).
- The original documentation on
  [switching the module off](/1.0/doc/configuration.html#unplugged).
- The [Apache Incubator page](https://incubator.apache.org/projects/pagespeed.html)
  for the project's status.
- Counts of deployed versions: the
  [installed-base census](https://github.com/We-Amp/pagespeed-census).

## About this advisory {#about}

First published 17 November 2026; the date at the top of the page shows the latest
change. No severity is assigned here: the library CVE has its own score, and whether it
can be reached through the module depends on the site.

We-Amp develops the current mod_pagespeed and earns its income from support and consulting, so
we are not a neutral party. Removing the module is a complete fix, and any build that
still receives security fixes will do; the migration guides above cover ours.

This is general information as of the date at the top of this page, not advice for a specific system. If
something here is wrong or out of date, for example because your packages ship newer
libraries under an original version number, tell us through the
[contact page](/contact/). We will review it, correct the page where needed and note
the change. To report a vulnerability in the current mod_pagespeed, see the
[security policy](/security/).
