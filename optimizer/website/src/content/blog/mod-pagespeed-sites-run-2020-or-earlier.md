---
title: 'Most mod_pagespeed sites run a release from 2020 or earlier'
description: 'HTTP Archive, September 2026: 99.9% of PageSpeed origins that report a version run a release from 2020 or earlier. The queries are public.'
date: 2026-10-17
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['data', 'mod_pagespeed', 'ngx_pagespeed', 'security']
draft: false
product: '1.1'
---

In HTTP Archive's September 2026 crawl, 20,625 origins sent a mod_pagespeed or ngx_pagespeed header carrying a version. All but 22 named a release from 2020 or earlier: 20,603 origins, 99.9% ([installed-base census](/blog/pagespeed-installed-base-2026/#what-no-longer-receives-fixes)). The last upstream release, 1.14.36.1, pins libwebp 0.6.1 from November 2017, and the archived trunk pins libwebp 1.1.0 from December 2019. Both are inside the range NVD lists for [CVE-2023-4863](https://nvd.nist.gov/vuln/detail/CVE-2023-4863), the 2023 libwebp heap overflow that was exploited in the wild: libwebp before 1.3.2.

An origin is the root page of a site, and the version is the value of the module's `X-Mod-Pagespeed` (Apache) or `X-Page-Speed` (nginx) header. Another 18,939 origins send the header with the version hidden and are left out of the shares.

## The builds still answering

The five largest groups, with counts from the census ([`distribution.csv`](https://github.com/We-Amp/pagespeed-census/blob/main/data/2026-09-01/distribution.csv)) and release dates from the [upstream release notes](https://github.com/apache/incubator-pagespeed-mod/blob/master/html/doc/release_notes.html):

| Reported version | Released                                 | Origins |
| ---------------- | ---------------------------------------- | ------- |
| 1.13.35.2        | 5 February 2018, last Google-era release | 12,725  |
| 1.14.36.1        | July 2020, last upstream release         | 4,182   |
| 1.4.26.5         | October 2013 security update             | 2,073   |
| 1.11.33.4        | 3 October 2016                           | 1,011   |
| 1.9.32.14        | 28 March 2016                            | 343     |

By era, 16,421 origins (79.6%) report a Google-era build and 4,182 (20.3%) the 1.14 incubator line. The other 22 report a value the census counts as the current line (three numbers, 1.15.0 or later): a rule on the header, not a check of the installed release.

The maintained module was renumbered from 1.1.0 to 1.15.0 in June 2026 to sort after 1.14.36.1. In the September crawl no header carries a 2.x product version, and the census would count any 2.x value as hidden.

## What the last upstream releases pin

The Apache Incubator repositories for [mod_pagespeed](https://github.com/apache/incubator-pagespeed-mod) and [ngx_pagespeed](https://github.com/apache/incubator-pagespeed-ngx) are archived on GitHub. At the last two release tags, `third_party/libwebp` in the mod_pagespeed repository is a git submodule:

- At [`v1.13.35.2`](https://github.com/apache/incubator-pagespeed-mod/tree/v1.13.35.2/third_party) it points to [`3d97bb75`](https://github.com/webmproject/libwebp/commit/3d97bb75144147e47db278ec76e5e70c6b2243db), the commit tagged libwebp v0.5.1 (July 2016).
- At [`v1.14.36.1`](https://github.com/apache/incubator-pagespeed-mod/tree/v1.14.36.1/third_party) it points to [`6b7a95fd`](https://github.com/webmproject/libwebp/commit/6b7a95fd8385baa6d18c96d5c0f2bea5632faf55), the commit tagged libwebp v0.6.1 (November 2017).
- On the archived `master` branch, the Bazel build fetches [libwebp 1.1.0](https://github.com/apache/incubator-pagespeed-mod/blob/master/bazel/repositories.bzl) (December 2019) rather than a submodule.

The fix for CVE-2023-4863 shipped in [libwebp 1.3.2](https://github.com/webmproject/libwebp/blob/v1.3.2/NEWS) on 13 September 2023; all three pins are in NVD's range. The maintained module's 1.1.0 general-availability release (May 2026) moved libwebp to 1.5.0, which includes the fix ([1.1 module release notes](/docs/release-notes-1-1/#v1-1-0)).

Our [security table](/docs/release-notes/#security-updates) lists CVE-2023-4863 against the upstream releases, 1.14.36.1 and earlier. Whether a given build links the bundled copy depends on how it was built; a header cannot show that.

## What the data cannot tell you

- **A version string shows lineage, not patches.** A packager can keep the version string while linking a newer system library or backporting fixes, and the census cannot see either. Ask your packager which fixes yours carries.
- **These are source pins.** This post makes no claim about any published package.
- **Hidden versions are not counted as out of date.** Across all 39,564 detected origins, the true share is unknown in either direction ([methodology](https://github.com/We-Amp/pagespeed-census/blob/main/METHODOLOGY.md)). The title's "most" rests on 20,603 being 52% of those 39,564.
- **The counts are floors.** Detection misses roughly one in five pages that send the header; those origins are in none of these numbers.
- **The share is small.** PageSpeed-serving origins are 0.245% of root pages, down from 0.935% in December 2020, and none are detected in the top 1k.

## Check your own servers

```sh
curl -s -D - -o /dev/null https://example.com/ \
  | grep -i -E '^x-(mod-pagespeed|page-speed):'
```

Compare the value with the [release notes](/docs/release-notes/#by-line) rather than counting its numbers. The old lines report four numbers, such as `1.13.35.2` or `1.14.36.1`, or `1.15.0.0-<revision>` from an unreleased trunk build. A `1.1.23.x` value is a 2012 Google-era build, not the maintained line. The maintained module reported `1.1.0` before the renumber, then `1.15.0` through `1.19.0` (from 2.1.0 the package version: 2.4.0 reports `1.19.0`), and later releases report the product version. If the header carries no version, check the installed package (`dpkg -l | grep -i pagespeed` or `rpm -qa | grep -i pagespeed`). ngx_pagespeed is usually compiled into nginx from source, so those list nothing: `nginx -V` shows whether it was built in, and the header is the only version signal.

If you are on an old line, update or remove the module. For whether the current line is safe, see [is mod_pagespeed safe?](/blog/is-mod-pagespeed-safe/). Packages and per-server steps are on the [download page](/download/).

## Rerun it

The queries, tables and method are public in [pagespeed-census](https://github.com/We-Amp/pagespeed-census): code under Apache-2.0, data and charts under CC BY 4.0. A run needs BigQuery; `uv run census dry-run --date 2026-09-01` reports what each query would scan and spends nothing.

We make the maintained mod_pagespeed line, so we have an interest in these numbers. That is why the queries are public. Rerun them, and tell us where our reading is wrong.

mod_pagespeed 2.1.0 and later is developed by We-Amp B.V. and is not affiliated with or endorsed by Google.
