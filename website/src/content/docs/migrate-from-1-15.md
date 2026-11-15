---
title: 'Migrate from mod_pagespeed 1.15'
description: 'Move a 1.15 install to the current mod_pagespeed in place: one package upgrade adds the optimizer worker, configuration carries over, the old cache stays.'
order: 12
group: 'Upgrade and migrate'
lastUpdated: 2026-11-15
datePublished: 2026-10-08
faq:
  - q: 'Does my 1.15 configuration carry over?'
    a: 'Yes. The current mod_pagespeed keeps the same directives and the same filter names; 1.14 and 1.15 configurations load unchanged. The Apache packages add one file, `pagespeed_daemon.conf`, which points the module at the optimizer worker.'
  - q: 'Is the cache purged?'
    a: 'No. The optimizer worker opens its own volume under `/var/cache/pagespeed-optimizer/v3` and never touches the Cyclone volume a 1.15 install kept under its file-cache path. The first requests are cold; the old file stays where it is and makes a rollback warm.'
---

The move from a 1.15 install is a package upgrade in place. The current mod_pagespeed
continues the 1.15 module lineage on top of a separate optimizer worker, keeps
the same directives and the same filter names, and installs from the same
signed repository your 1.15 packages came from. Your configuration carries
over; what you gain is the worker.

## What changes

| In 1.15                                           | In the current mod_pagespeed                                                                                                                                                         |
| ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| The module optimizes in process                   | The module still serves and rewrites in process; the `pagespeed-optimizer` worker builds optimized variants beside it and the module serves them from the shared cache |
| One package per web server                        | The Apache module package depends on `pagespeed-optimizer` at the same version; one upgrade command moves both                                                   |
| A Cyclone volume under the module's file-cache path | The module's own cache stays where it was; the worker keeps its volume under `/var/cache/pagespeed-optimizer/v3`                                             |
| `X-Mod-Pagespeed` / `X-Page-Speed` headers        | Unchanged                                                                                                                                                      |
| Admin console at `/pagespeed_admin`               | Unchanged, plus an Overview page and panels for the worker                                                                                                     |

## Upgrade

```bash
# Debian / Ubuntu
sudo apt-get update
sudo apt-get install --only-upgrade mod-pagespeed            # Apache
sudo apt-get install --only-upgrade nginx-module-pagespeed   # nginx
# AlmaLinux / RHEL / Rocky
sudo dnf upgrade mod-pagespeed             # Apache
sudo dnf upgrade nginx-module-pagespeed    # nginx
```

The `mod-pagespeed` package depends on `pagespeed-optimizer` at the exact same
version, so the Apache upgrade pulls the worker in. The nginx package does
not: `sudo apt-get install pagespeed-optimizer` (or `dnf install`), as its own
command, adds it, and two directives hand nginx's in-place optimization to it;
see [Using the optimizer worker with nginx](/docs/installation-module/#using-the-optimizer-daemon-with-nginx).
A worker installed after the module leaves the nginx user outside its group:
run `sudo usermod -a -G pagespeed www-data` (`nginx` on Enterprise Linux) and
restart nginx.

Then start things in order:

```bash
sudo systemctl restart pagespeed-optimizer   # the package starts it; make sure it is up
sudo systemctl restart apache2               # httpd on Enterprise Linux, or nginx
```

At boot the packaged unit orders the optimizer ahead of the web server and
counts it as started only once its notification socket exists. During the
upgrade itself, when you start things by hand, keep that order: optimizer
first, web server second.

On Apache the package also installs `pagespeed_daemon.conf`
(`/etc/apache2/conf-available/` on Debian and Ubuntu, `/etc/httpd/conf.d/` on
the Red Hat family), which sets `ModPagespeedDaemonSocketPath` and
`ModPagespeedDaemonVolumePath` to the worker's defaults, so the module reaches
the worker without editing any configuration. A file you created by hand
earlier at that path stops an unattended Debian upgrade with a dpkg prompt;
remove it before upgrading, or install with
`-o Dpkg::Options::=--force-confold` to keep it.

## The cache

Nothing is purged. A 1.15 install stores its cache in a Cyclone volume under
the module's file-cache path; the optimizer worker opens its own volume under
`/var/cache/pagespeed-optimizer/v3` and never touches the old file. The first
requests after the upgrade are misses and the cache refills as traffic
arrives, so pick a quiet hour if your origin is sensitive to that. Both files
exist during the upgrade, so the disk needs room for the second volume. The
old file is what makes a rollback warm; delete it yourself once you are
confident you will not roll back.

## Platform notes

- **IIS / Windows Server.** The IIS package ships as a signed installer on the
  mod_pagespeed GitHub release: run the current MSI over the existing install
  and `iisreset`. Running the MSI takes every IIS site on the server down for
  the duration; plan a maintenance window. See
  [Install on IIS](/docs/install-iis/#install-the-module).
- **Debian 11 (bullseye).** The repository's bullseye suite carries the 1.15.0
  module packages; the current packages need Debian 12 or newer.
- **cPanel EasyApache 4 (EL8).** The module runs without the optimizer worker.

## Verify

```bash
curl -I http://localhost/ | grep -i -E 'X-Mod-Pagespeed|X-Page-Speed'
```

The header is unchanged. Open the admin console's Overview page: the optimizer
card reads **Running** once the module has reached the worker, and the
Statistics page's `ipro_daemon_served` rises as in-place requests are answered
from the worker's cache. [Is it working?](/docs/is-it-working/) has the rest.

## Roll back

Roll the module and `pagespeed-optimizer` back together to the 1.15 packages
you ran; the old Cyclone volume reopens still warm unless you deleted it.
[Uninstall and rollback](/docs/uninstall/#roll-back-to-the-previous-release)
has the commands.
