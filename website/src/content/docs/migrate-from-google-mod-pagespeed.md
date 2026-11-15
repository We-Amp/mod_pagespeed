---
title: 'Migrate from the archived Google module'
description: 'Move from the archived mod_pagespeed or ngx_pagespeed binaries to the current mod_pagespeed: package swap, what maps to what, cache, rollback.'
order: 15
group: 'Upgrade and migrate'
lastUpdated: 2026-11-15
datePublished: 2026-10-08
faq:
  - q: 'Is the current mod_pagespeed a drop-in replacement for the archived module?'
    a: 'Yes. It continues the same codebase, keeps the `pagespeed` and `ModPagespeed` directive surface and the classic filter names, and loads in process on Apache and nginx. Your configuration, `.htaccess` directives included, carries over, with seven removed option names to delete on nginx (see the next answer).'
  - q: 'What do I lose?'
    a: 'No working feature. A handful of long-inert filters and experimental options are accepted and ignored with a warning, and the old file cache is left in place while the new Cyclone cache warms up. One exception: on nginx, seven removed option names (`FileCacheInodeLimit`, `FileCacheCleanIntervalMs`, `MaxPrefetchJsElements`, `DistributeFetches` and the three `DistributedRewrite*` names) stop nginx from starting until you delete them; Apache only warns.'
---

mod_pagespeed is an open-source project originally developed at Google. It was
donated to the Apache Software Foundation in 2017, the Incubator podling
retired in 2023, and the repositories became read-only in 2025; the 1.13.35.2
binaries still install but have had no security fixes since. The current mod_pagespeed,
developed by We-Amp B.V. and not affiliated with or endorsed by Google,
continues that codebase: same
directives, same filter names, built against current Apache and nginx, with
ongoing security fixes. The move is a package swap and a restart.

## If apt or rpm already reports an error {#package-errors}

These messages come from the archived Google repository, which is no longer maintained; its signatures no longer verify on current systems. Its packages end at 1.13.35.2, which has had no security fixes since. On Debian and Ubuntu, apt prints "is no longer signed" when the source was trusted before, and "is not signed" otherwise:

```text
E: The repository 'http://dl.google.com/linux/mod-pagespeed/deb stable Release' is no longer signed.
N: Updating from such a repository can't be done securely, and is therefore disabled by default.
```

```text
W: The repository 'http://dl.google.com/linux/mod-pagespeed/deb stable Release' is not signed.
```

```text
W: GPG error: http://dl.google.com/linux/mod-pagespeed/deb stable Release: The following signatures were invalid: EXPKEYSIG 78BD65473CB3BD13 Google Inc. (Linux Packages Signing Authority) <linux-packages-keymaster@google.com>
```

On Enterprise Linux, installing the archived rpm directly prints:

```text
warning: mod-pagespeed-stable_current_x86_64.rpm: Header V4 DSA/SHA1 Signature, key ID 7fac5991: NOKEY
```

Do not re-import Google's key and do not mark the repository as trusted: that keeps the archived package installable. Move to the maintained package instead. On Debian and Ubuntu, step 1 of [the block below](#apache-on-debian-and-ubuntu) disables the source file the archived package created, `/etc/apt/sources.list.d/mod-pagespeed.list`, and the `apt-get update` error stops once that file is out of the way. The signed repository that the `install.sh` script from packages.modpagespeed.com adds replaces it, for both Debian-family and Enterprise Linux systems.

**Is it still maintained?** The Google and Apache releases are archived. mod_pagespeed continues the codebase; the [release notes](/docs/release-notes/) list each release and its security fixes.

## Before you start

Record the version you have, and keep a copy of your configuration and of the
archived package for a rollback. If you run the archived beta, write
`mod-pagespeed-beta` wherever this page says `mod-pagespeed-stable`, in the
file names below too. Run the block for your setup.

```bash
# Apache on Debian or Ubuntu
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
sudo cp -a /etc/apache2/mods-available/pagespeed.conf ~/pagespeed.conf.google
curl -fsSL -o ~/mod-pagespeed-stable_current_amd64.deb https://dl.google.com/linux/direct/mod-pagespeed-stable_current_amd64.deb
```

```bash
# Apache on Enterprise Linux
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
sudo cp -a /etc/httpd/conf.d/pagespeed.conf ~/pagespeed.conf.google
curl -fsSL -o ~/mod-pagespeed-stable_current_x86_64.rpm https://dl.google.com/linux/direct/mod-pagespeed-stable_current_x86_64.rpm
```

```bash
# nginx: the module file (Debian and Ubuntu, or Enterprise Linux path) and the
# load_module line that loads it
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
for d in /usr/lib/nginx/modules /usr/lib64/nginx/modules; do
  if [ -f "$d/ngx_pagespeed_module.so" ]; then
    sudo cp "$d/ngx_pagespeed_module.so" "$d/ngx_pagespeed_module.so.bak"
  fi
done
sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx | tee ~/ngx_pagespeed-load_module.txt
```

## Apache on Debian and Ubuntu

```bash
# 1. Disable the archived package's apt source
if [ -f /etc/apt/sources.list.d/mod-pagespeed.list ]; then
  sudo mv /etc/apt/sources.list.d/mod-pagespeed.list /etc/apt/sources.list.d/mod-pagespeed.list.disabled
fi &&
sudo truncate -s 0 /etc/default/mod-pagespeed &&
# 2. Add the signed repository
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh &&
# 3. Swap the packages in one run
sudo apt-get install mod-pagespeed mod-pagespeed-stable- &&
# 4. Check the configuration, then restart
sudo apache2ctl configtest && sudo systemctl restart apache2 &&
# 5. Remove what the archived package left behind
sudo apt-get purge mod-pagespeed-stable
```

Each step runs only if the one before it succeeded, so the block can be pasted
whole. If a step stops, fix what it reports and run the remaining steps.

- **Step 1, the archived apt source.** The archived package added its own
  source, `/etc/apt/sources.list.d/mod-pagespeed.list`. Its signing key no
  longer verifies, so while it is enabled every `apt-get update` fails with
  `NO_PUBKEY 78BD65473CB3BD13` and "is not signed", and the install script in
  step 2 stops there. The archived package's daily cron job puts the source
  back while `/etc/default/mod-pagespeed` asks it to; an empty file turns that
  off. Newer versions of the install script do both for you.
- **Step 3, the swap.** The trailing `-` on `mod-pagespeed-stable-` removes
  the archived package explicitly, in the same run that installs the new one,
  so Apache never has its module configuration removed without a replacement.
  Do not remove the archived package on its own first. Apache keeps serving
  on the module it has loaded until the restart in step 4.
- **If you edited `pagespeed.conf`,** dpkg stops in step 3 and asks:
  `*** pagespeed.conf (Y/I/N/O/D/Z) [default=N] ?`. Answer `N` to keep your
  file. Its directives carry over unchanged; kept directives the new module
  no longer needs, such as `ModPagespeedFileCacheInodeLimit`, log a
  deprecation warning and still work. The new defaults are saved beside it as
  `/etc/apache2/mods-available/pagespeed.conf.dpkg-dist`; compare them with
  `diff /etc/apache2/mods-available/pagespeed.conf /etc/apache2/mods-available/pagespeed.conf.dpkg-dist`.
- **Unattended runs.** Without a terminal dpkg cannot answer the prompt, so
  keep your file with
  `sudo apt-get install -y -o Dpkg::Options::=--force-confold mod-pagespeed mod-pagespeed-stable-`.
  A run without a terminal and without the option stops with "end of file on
  stdin at conffile prompt" and exits 100, leaving `mod-pagespeed`
  unconfigured (`iU` in `dpkg -l`) and no PageSpeed module enabled in Apache.
  Apache keeps serving the archived module only until its next reload (the
  nightly log rotation does one), then serves without PageSpeed. To recover,
  run the unattended command above again, or
  `sudo dpkg --configure -a --force-confold`, then restart Apache.
- **Step 5, purge rather than remove,** and only after the install in step 3
  succeeded: while the archived package is still installed, the purge would
  remove it with nothing in its place. After the swap the archived package is
  left in the "config files" state, with its daily cron job. The purge deletes
  the cron job and leaves `pagespeed.conf` alone, which now belongs to the new
  package. The empty `/etc/default/mod-pagespeed` from step 1 stays, which is
  harmless.

## Apache on Enterprise Linux

Do the swap right after the repository is configured:

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh &&
sudo dnf swap mod-pagespeed-stable mod-pagespeed &&
sudo sed -i 's/mod_pagespeed_ap24\.so/mod_pagespeed.so/' /etc/httpd/conf.d/pagespeed.conf &&
sudo apachectl configtest && sudo systemctl restart httpd
```

`dnf swap` removes the archived package and installs the new one in one
transaction. If you edited `/etc/httpd/conf.d/pagespeed.conf`, rpm keeps your
file and writes the new one beside it as `pagespeed.conf.rpmnew`. On
Enterprise Linux the `LoadModule` line lives in that file, and yours names
`mod_pagespeed_ap24.so`, which packages before 2.5.0 do not ship: there the
`sed` line points it at `mod_pagespeed.so`, without which `configtest` fails.
From 2.5.0 the package ships `mod_pagespeed_ap24.so` as a link to the module,
so on current packages the `sed` line is unnecessary and stays harmless. The `sed` runs only after the
swap succeeded: on the archived package's own configuration it would point
Apache at that package's `mod_pagespeed.so`, its Apache 2.2 build, which
Apache 2.4 cannot load. Compare your file with the new defaults:

```bash
diff /etc/httpd/conf.d/pagespeed.conf /etc/httpd/conf.d/pagespeed.conf.rpmnew
```

then remove the `.rpmnew` file:

```bash
sudo rm -f /etc/httpd/conf.d/pagespeed.conf.rpmnew
```

Kept directives the new module no longer needs log a deprecation warning and
still work. Because `configtest` runs first, Apache keeps serving on the old
module until the configuration is valid.

## nginx

```bash
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh &&
if command -v apt-get >/dev/null; then
  sudo apt-get install nginx-module-pagespeed
else
  sudo dnf install nginx-module-pagespeed
fi &&
# remove your own load_module line from nginx.conf; the package loads the module
sudo sed -i '/^[[:space:]]*load_module[[:space:]].*ngx_pagespeed_module\.so;/d' /etc/nginx/nginx.conf &&
sudo grep -Rns '^[[:space:]]*load_module.*pagespeed' /etc/nginx /usr/share/nginx/modules &&
sudo nginx -t && sudo systemctl restart nginx
```

If `nginx -t` stops with `directive "..." not recognized` for one of `DistributeFetches`, `DistributedRewriteKey`, `DistributedRewriteServers`, `DistributedRewriteTimeoutMs`, `FileCacheCleanIntervalMs`, `FileCacheInodeLimit` and `MaxPrefetchJsElements`, delete that line: Apache warns about these names and carries on, but nginx refuses to start. None has a replacement.

The package loads `ngx_pagespeed_module.so` itself, so the `sed` line removes
the `load_module` line you added to `/etc/nginx/nginx.conf` for the old
module. The `grep` must then show exactly one line, the package's; if it
shows a second one in another file, delete that by hand and run the last line
again. The package installs its module at the path a hand-installed
`ngx_pagespeed_module.so` usually has, and overwrites that file, which is why
[Before you start](#before-you-start) keeps a `.bak` copy. If you compiled
ngx_pagespeed into nginx, you are also moving to the distribution's nginx
package; see
[Migrate from ngx_pagespeed](/docs/migrate-from-ngx-pagespeed/) for the
version pinning that matters there.

## What maps to what

| You have                                                                | You get                                                                                                                                                              |
| ----------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `mod-pagespeed-stable` or `mod-pagespeed-beta` (Apache)                 | `mod-pagespeed` from packages.modpagespeed.com; it depends on the `pagespeed-optimizer` worker and installs the configuration that points the module at it          |
| `ngx_pagespeed` compiled into nginx, or a third-party package           | `nginx-module-pagespeed`, a dynamic module pinned to your distribution's stock nginx                                                                                  |
| `ModPagespeed*` directives, `pagespeed` directives, `.htaccess`          | Unchanged                                                                                                                                                            |
| The filter names                                                        | Unchanged; the long-inert and experimental ones are accepted and ignored with a warning (see the [compatibility table](/docs/migrate-from-ngx-pagespeed/#configuration-compatibility)) |
| `ModPagespeedFileCachePath` / `pagespeed FileCachePath`                  | Accepted; the module keeps a Cyclone volume under it, and the old per-resource cache files in that directory are ignored                                             |
| Memcached or Redis as external cache                                    | Still supported; see [cache modes](/docs/cache-modes/#external-cache-backends-memcached-and-redis)                                                                   |
| `/pagespeed_admin`, `/pagespeed_global_admin`, statistics and messages  | Same paths, with a reworked console; restrict them as before                                                                                                         |
| `X-Mod-Pagespeed` (Apache) / `X-Page-Speed` (nginx)                     | Unchanged                                                                                                                                                            |
| `.pagespeed.` resource URLs, the beacon, `?PageSpeed=off`                | Unchanged                                                                                                                                                            |
| Nothing                                                                 | The optimizer worker: a separate process that builds optimized variants beside the module. The Apache packages wire it up; on nginx two directives do                |

## The cache

The module uses Cyclone Cache, a different cache backend. On first start after
the move, the old per-resource file cache is ignored, not deleted; the Cyclone
volume starts empty and resources are re-optimized on first request, so expect
a brief warm-up. The warm-up is a one-time cost: cache contents persist across
restarts. Once the move has run for a while, remove the old per-resource files
from the file-cache directory and leave the Cyclone volume beside them alone.
For cache sizing and storage options, see
[cache sizing](/docs/cache-modes/#cache-sizing).

## Verify

```bash
curl -sI http://localhost/ | grep -E 'X-Mod-Pagespeed|X-Page-Speed'
```

The header now reads `on` (releases before 2.7.0 print their version), no
longer `1.13.35.2-0`: `X-Mod-Pagespeed` on Apache, `X-Page-Speed` on nginx. Open
`/pagespeed_global_admin` on the server to see the configuration and
statistics; [Is it working?](/docs/is-it-working/) has the rest.

## Roll back

Roll back after the restart if the site misbehaves on the new module. A
`configtest` failure does not need a rollback: the old module keeps serving
until the restart, so fix the configuration instead. Rolling back reinstalls
the archived package you saved under [Before you start](#before-you-start) and
puts your configuration back; if you no longer have the package, the same URL
still serves 1.13.35.2.

### Apache on Debian and Ubuntu

```bash
sudo apt-get purge mod-pagespeed pagespeed-optimizer &&
sudo truncate -s 0 /etc/default/mod-pagespeed &&
sudo apt-get install ~/mod-pagespeed-stable_current_amd64.deb &&
sudo cp ~/pagespeed.conf.google /etc/apache2/mods-available/pagespeed.conf &&
sudo apache2ctl configtest && sudo systemctl restart apache2
```

The purge removes the new module, the worker and their configuration, and
deletes the worker's cache directory; your own configuration comes back from
the copy. The empty `/etc/default/mod-pagespeed` stops the archived package
from adding its apt source again, which would break `apt-get update` once
more. Apache serves on the module it has loaded until the restart.

### Apache on Enterprise Linux

```bash
sudo truncate -s 0 /etc/default/mod-pagespeed &&
sudo dnf swap mod-pagespeed ~/mod-pagespeed-stable_current_x86_64.rpm &&
sudo cp ~/pagespeed.conf.google /etc/httpd/conf.d/pagespeed.conf &&
sudo rm -f /etc/httpd/conf.d/pagespeed.conf.rpmnew &&
sudo apachectl configtest && sudo systemctl restart httpd
```

`dnf swap` also removes the worker, which nothing needs any more, and rpm
saves the archived package's default configuration as `pagespeed.conf.rpmnew`,
which your restored copy replaces. Empty `/etc/default/mod-pagespeed` before
the swap: when the file is missing, the archived package's install script
creates it asking for its yum repository and adds that repository at once.
The repository can stay configured on either system: neither apt nor dnf
installs `mod-pagespeed` again unless you ask for it.

### nginx

```bash
if command -v apt-get >/dev/null; then
  sudo apt-get purge nginx-module-pagespeed
else
  sudo dnf remove nginx-module-pagespeed
fi &&
restored=yes &&
for d in /usr/lib/nginx/modules /usr/lib64/nginx/modules; do
  if [ -f "$d/ngx_pagespeed_module.so.bak" ]; then
    sudo cp "$d/ngx_pagespeed_module.so.bak" "$d/ngx_pagespeed_module.so" || restored=no
  fi
done && [ "$restored" = yes ] &&
# put back the load_module line the migration removed from nginx.conf
line="$(sed -n 's|^/etc/nginx/nginx\.conf:[0-9]*:[[:space:]]*||p' ~/ngx_pagespeed-load_module.txt | head -n 1)" &&
if [ -n "$line" ] && ! sudo grep -qxF "$line" /etc/nginx/nginx.conf; then
  sudo sed -i "1i $line" /etc/nginx/nginx.conf
fi &&
sudo nginx -t && sudo systemctl restart nginx
```

Remove the package before you restore the file: the package owns that path,
and removing it afterwards would delete the module you restored, so the
restore runs only after the removal succeeded. The purge also deletes the
package's own `load_module` snippet, which holds nothing of yours unless you
edited it; the module's cache and log directories are kept. The block then
puts the `load_module` line saved under [Before you start](#before-you-start)
back at the top of `/etc/nginx/nginx.conf`, the file the migration removed
it from. If the saved line came from another file, put it back there by hand
before `nginx -t`; `cat ~/ngx_pagespeed-load_module.txt` shows where it was.

After a rollback Apache answers `X-Mod-Pagespeed: 1.13.35.2-0` again. The
archived 1.0 documentation stays available at [/1.0/](/1.0/), and
[Is mod_pagespeed still maintained?](/mod-pagespeed-still-maintained/) covers
what changed and what did not.
