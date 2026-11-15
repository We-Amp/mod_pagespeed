---
title: 'Migrate from ngx_pagespeed'
description: 'Replace an open-source ngx_pagespeed build, compiled in or from a third-party package, with the signed mod_pagespeed nginx module: what carries over.'
order: 13
group: 'Upgrade and migrate'
lastUpdated: 2026-11-15
datePublished: 2026-10-08
faq:
  - q: 'Do my pagespeed directives carry over from ngx_pagespeed?'
    a: 'Yes. The native nginx module keeps the `pagespeed` directive surface and the classic filter names. Every directive loads; a handful of long-inert filters and experimental options are accepted and ignored with a warning, and four directives are new. The exception is seven removed names that stop nginx from starting (listed under Configuration compatibility); remove them.'
  - q: 'Why does nginx refuse to load the new module?'
    a: 'Each module build is pinned to the exact nginx version your distribution ships. An nginx from another repository, including the one a third-party ngx_pagespeed package was built against, has a different version string and nginx refuses the module. Run the stock nginx, or contact us for a matching build.'
---

The current mod_pagespeed continues the nginx port. If you run the open-source
[`ngx_pagespeed`](https://ngxpagespeed.com/install/) today, whether you compiled it into nginx yourself or
installed a third-party package build of it such as the GetPageSpeed packages,
the move is a package swap: remove the old module, install the signed
`nginx-module-pagespeed`, keep your `pagespeed` directives. The package loads
the module itself, so after a restart the same configuration runs on a module
built against current nginx and kept security-patched.

## Before you start

Record what you run, and keep a copy of the old module to roll back to:

```bash
nginx -V 2>&1 | grep -o 'ngx_pagespeed[^ ]*'                   # compiled in, or
ls /usr/lib/nginx/modules/ngx_pagespeed*.so                     # a dynamic module
curl -I http://localhost/ | grep X-Page-Speed                   # present when the module is active
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so{,.bak}   # if dynamic
```

**Mind the nginx version.** Each `nginx-module-pagespeed` build is pinned to
the exact nginx version your distribution ships (see the
[compatibility table](/docs/installation-module/#nginx-compatibility)), and
nginx refuses to load a module built for another version. A third-party
ngx_pagespeed package usually targets, or comes with, an nginx from that same
third-party repository rather than the distribution's. Before you install,
decide whether to move nginx back to the distribution package too; otherwise
[contact us](/contact/) for a matching pinned build.

## Steps

1. **Remove the old module and its repository.** Uninstall the third-party
   package, and disable the repository it came from so the package manager
   cannot pick the old build again; for a compiled-in module, plan to replace
   the nginx binary with the distribution's. Leave your `pagespeed` directives
   in place.
2. **Install the signed module:**

   ```bash
   curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
   sudo apt-get install nginx-module-pagespeed    # Debian / Ubuntu
   sudo dnf install nginx-module-pagespeed        # AlmaLinux / RHEL / Rocky
   ```

3. **Remove the old `load_module` line.** The package loads
   `ngx_pagespeed_module.so` itself; remove any `load_module` line that named
   the old build, so that
   `sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules`
   shows exactly one line, the package's.
4. **Test and restart:** `sudo nginx -t && sudo systemctl restart nginx`.
5. **Verify:** `curl -I http://localhost/ | grep X-Page-Speed`. The header
   is present and reads `on` by default; set the server-wide
   [`ExposeVersionInHeaders`](/docs/configuration/#expose-version-in-headers) option
   if you want it to carry the module version. [Is it working?](/docs/is-it-working/) has
   the other checks.

To add the optimizer worker, install `pagespeed-optimizer` from the same
repository and set two directives; see
[Using the optimizer worker with nginx](/docs/installation-module/#using-the-optimizer-daemon-with-nginx).

## Configuration compatibility

| Directives and filters                                                                                                                                                                                                                             | Status in the current mod_pagespeed                                                                                                   |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| `pagespeed on/off`, `RewriteLevel`, `EnableFilters`, `DisableFilters`, `ForbidFilters`, `Domain`, `MapOriginDomain`, `MapRewriteDomain`, `MapProxyDomain`, `LoadFromFile`, the admin and statistics paths, `FetchHttps`, `UseNativeFetcher` and the rest of the [configuration reference](/docs/configuration/) | Carry over unchanged                                                                                                          |
| `DistributeFetches`, `DistributedRewriteKey`, `DistributedRewriteServers`, `DistributedRewriteTimeoutMs`, `FileCacheCleanIntervalMs`, `FileCacheInodeLimit` and `MaxPrefetchJsElements` | Removed. Unlike Apache, which warns and carries on, nginx does not recognize these names: `nginx -t` fails with `directive "FileCacheInodeLimit" not recognized` and nginx will not start. Delete the lines; none has a replacement. The disk cache stays within `FileCacheSizeKb` |
| `FileCachePath`                                                                                                                                                                                                                                    | Accepted; the module keeps its Cyclone volume under it                                                                        |
| `ShardDomain`                                                                                                                                                                                                                                      | Works, logs a deprecation warning: sharding hurts with HTTP/2 and HTTP/3. Remove it                                           |
| `UseExperimentalJsMinifier`                                                                                                                                                                                                                        | Accepted and ignored; the tokenizer-based minifier is the only one, the legacy minifier was removed                           |
| `ExperimentalCentralControllerPort`, `ExperimentalPopularityContestMaxInFlight`, `ExperimentalPopularityContestMaxQueueSize`                                                                                                                        | Accepted and ignored; the gRPC central controller was removed                                                                 |
| `in_place_optimize_for_browser`, `AllowVaryOn`, `PrivateNotVaryForIE`                                                                                                                                                                              | Retired: accepted with a warning, no effect. Remove them                                                                      |
| `defer_iframe`, `div_structure`, `explicit_close_tags`, `mobilize_precompute`, `split_html`, `split_html_helper`, `flush_subresources`, `make_google_analytics_async`                                                                               | Filter names still parse, log a warning and do nothing. Remove them                                                           |
| `insert_ga`, `AnalyticsID`                                                                                                                                                                                                                         | Deprecated with a warning: Universal Analytics stopped processing hits in 2023                                                |
| `DaemonSocketPath`, `DaemonVolumePath`, `DaemonApiSocketPath`, `DaemonServeStoredEncodings`                                                                                                                                                        | New: hand in-place optimization to the optimizer worker                                                                       |

Apart from the seven removed names above, no directive was renamed. Run `sudo nginx -t` before you restart: it prints every deprecation warning and stops at the first directive it rejects. The `.pagespeed.` resource URLs, the `X-Page-Speed`
header and the beacon paths are unchanged, so existing CDN rules keep working.

## The cache

The module keeps its cache in a Cyclone volume under `FileCachePath`. The
per-resource files an older ngx_pagespeed left in that directory are ignored,
not deleted, and resources are re-optimized on first request, so expect a
brief warm-up. Cache contents persist across restarts, so the warm-up is a
one-time cost. Remove the old per-resource files once the new module has run
for a while; leave the Cyclone volume beside them alone.

## Roll back

Restore the old module file, or the old nginx package, and restart nginx:

```bash
sudo systemctl stop nginx
sudo cp /usr/lib/nginx/modules/ngx_pagespeed_module.so.bak /usr/lib/nginx/modules/ngx_pagespeed_module.so
sudo systemctl start nginx
```

A third-party package rolls back by reinstalling it from its own repository;
remove `nginx-module-pagespeed` first so the two do not both claim the module
path.
