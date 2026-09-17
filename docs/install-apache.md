# Installing mod_pagespeed for Apache

## Requirements

- Apache 2.4+
- Linux x86_64 (Ubuntu 22.04+, Debian 12+, RHEL 9+, Rocky 9+)

## Install

Add the We-Amp package repository (once), then install. Direct `.deb` / `.rpm` /
`.so` downloads (amd64 + arm64) are also on the
[downloads page](https://modpagespeed.com/1.1/docs/downloads/).

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh

sudo apt install mod-pagespeed     # Debian / Ubuntu
sudo dnf install mod-pagespeed     # RHEL / AlmaLinux / Rocky 9
```

The package installs `mod_pagespeed.so` (`/usr/lib/apache2/modules/` on
Debian/Ubuntu, `/usr/lib64/httpd/modules/` on EL9), its config, and the cache
directory at `/var/cache/mod_pagespeed/`. Enable and restart:

```bash
sudo a2enmod pagespeed && sudo systemctl restart apache2   # Debian/Ubuntu
sudo systemctl restart httpd                               # EL9
```

> Building from source uses the public mod_pagespeed source tree (see
> `DEVELOPER.md`); the published packages are the supported path for most users.

## SELinux (RHEL / AlmaLinux / Rocky 9)

Two things apply on EL9-family hosts running SELinux in Enforcing mode; both
are the standard posture for third-party Apache modules, not extra hardening.

**The rpm ships the SELinux policy for the optimizer daemon.** On a stock
enforcing host the distribution policy gives Apache no access to the
daemon's cache volume and notify socket, so in-place optimization could
never turn on. The rpm therefore installs the `pagespeed-optimizer` policy
module in its post-install step. It does this whenever SELinux is on —
Enforcing or Permissive — and skips it only where SELinux is disabled.
This needs no operator action.

**The module's fetcher needs the `httpd_can_network_connect` boolean.** The
module fetches subresources over HTTP — including from the server itself
over loopback — and the stock EL9 policy denies Apache outbound network
connections by default. Set the boolean persistently:

```bash
sudo setsebool -P httpd_can_network_connect 1
```

This is the same requirement other Apache modules that fetch over HTTP
document; the boolean permits Apache to open outbound TCP connections, both
loopback and external. Without it the module logs fetch failures and serves
resources unoptimized.

Debian and Ubuntu use AppArmor rather than SELinux; neither item applies
there, and the deb ships no SELinux policy.

## Configuration

Basic configuration in Apache config:
```apache
LoadModule pagespeed_module /usr/lib/apache2/modules/mod_pagespeed.so

<IfModule pagespeed_module>
    ModPagespeed on
    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"

    # Required only for outbound HTTPS fetching of subresources from other
    # origins; omit if all resources are served over HTTP.
    ModPagespeedSslCertDirectory /etc/ssl/certs

    # Add filters beyond the default CoreFilters set. collapse_whitespace
    # (HTML minification) is NOT part of CoreFilters and must be enabled here.
    # combine_css, combine_javascript, and image rewriting are already active
    # via the default CoreFilters level — no EnableFilters directive needed.
    ModPagespeedEnableFilters collapse_whitespace

    # Admin and statistics endpoints (restrict access in production).
    ModPagespeedAdminDomains localhost
    ModPagespeedStatisticsDomains localhost
</IfModule>
```

`ModPagespeed on` enables the CoreFilters rewrite level by default, which
includes 29 filters covering CSS/JavaScript minification and
combining, image optimization, and caching. The module is not a no-op after
this single directive; CoreFilters is already active. Use
`ModPagespeedEnableFilters` to add filters beyond the default set (for example,
`collapse_whitespace`, which CoreFilters does not include), and
`ModPagespeedDisableFilters` to turn off individual default filters. To change
the whole set at once, set `ModPagespeedRewriteLevel` to `CoreFilters` (the
default), `OptimizeForBandwidth`, `PassThrough`, or `AllFilters`.

The `ModPagespeedAdminDomains` and `ModPagespeedStatisticsDomains` directives
gate the `/pagespeed_admin` and `/pagespeed_statistics` endpoints. They are
shown here scoped to `localhost`; in production, restrict access with a firewall
or ACL rather than relying on these directives alone.

## Verification

```bash
curl -I http://localhost/
# Should show an X-PageSpeed: <version> response header
```

## System Tests

```bash
./test/system/run_system_tests.sh
./test/system/run_system_tests.sh -k sanity  # Quick sanity check
```

Expected: 195 pass, 14 skip, 0 fail.

## Next steps: configuration & operations

Full configuration and operations documentation lives on modpagespeed.com:

- [Filter selection](https://modpagespeed.com/1.1/docs/filter-selection/) — rewrite levels and per-filter enable/disable
- [Filter reference](https://modpagespeed.com/1.1/docs/filter-reference/) — what each filter does and which level enables it
- [Admin console](https://modpagespeed.com/1.1/docs/admin-console/) — statistics, cache inspection, and cache purging
