# cPanel / EasyApache 4: install and verify mod_pagespeed (EA4 RPM)

Source: https://modpagespeed.com/docs/cpanel/. Header: `X-Mod-Pagespeed`.
Scope: the signed `ea-apache24-mod_pagespeed` RPM on a cPanel / WHM host running
EasyApache 4 (EA4) Apache. Hosts without cPanel use the Apache recipe:
https://modpagespeed.com/recipes/apache.md

## 1. Prerequisites

AlmaLinux, Rocky, RHEL or CloudLinux 8, 9 or 10 on x86_64, with cPanel and EA4
Apache (`ea-apache24`). arm64 and Ubuntu EA4 are not built. LiteSpeed Web Server
is not supported.

```bash
. /etc/os-release && echo "$ID $VERSION_ID $(uname -m)"
test -d /usr/local/cpanel && echo 'cPanel host' || echo 'not a cPanel host: use https://modpagespeed.com/recipes/apache.md'
httpd -v
rpm -q ea-apache24-mod_pagespeed
```

Pass: the OS line shows major version 8, 9 or 10 and `x86_64`, the second line
prints `cPanel host`, `httpd -v` prints an Apache 2.4 version, and the last
command prints `package ea-apache24-mod_pagespeed is not installed`.

Stop and ask if the package is already installed, if the host is not x86_64, or
if the web server is not EA4 Apache.

## 2. Install

Ticking the module in WHM (below) rebuilds Apache and turns the filter on for
every virtual host served by `ea-apache24-httpd`. On a host that serves
production traffic, get the operator's OK first.

The RPMs and the repository metadata are signed with the repository signing
key (fingerprint `DF00 E296 BE91 41CE A541  1346 F50D 6054 F107 12A0`); dnf checks both. The repo stays disabled for routine updates and is enabled per
command.

```bash
sudo tee /etc/yum.repos.d/modpagespeed-ea4.repo >/dev/null <<'REPO'
[ea4]
name=mod_pagespeed for EasyApache 4
baseurl=https://packages.modpagespeed.com/yum/ea4/el$releasever/x86_64/
enabled=0
repo_gpgcheck=1
gpgcheck=1
gpgkey=https://packages.modpagespeed.com/pubkey.gpg
REPO
sudo dnf install -y --enablerepo=ea4 ea-apache24-mod_pagespeed
```

Pass: dnf finishes without error and `rpm -q ea-apache24-mod_pagespeed` prints a
package version. The version may show a leading `2:`; that is the RPM epoch, not
a major version.

## 3. Minimal configuration

The RPM installs the module file and its configuration but does not turn the
module on. The operator turns it on in WHM; an agent cannot do this step.

1. WHM, _Software_, _EasyApache 4_, _Customize_ on the current profile.
2. _Apache Modules_ step: tick `ea-apache24-mod_pagespeed`.
3. _Review_, then _Provision_. EA4 rebuilds Apache.

The configuration is `/etc/apache2/conf.d/pagespeed.conf`; it is marked
`%config(noreplace)`, so edits survive upgrades. Confirm the module is loaded
after the provision finishes:

```bash
httpd -M 2>/dev/null | grep pagespeed
```

Pass: one `pagespeed_module` line. No output means the box was not ticked or the
provision did not finish; re-check the _Apache Modules_ step. Filters are tuned
in that file: https://modpagespeed.com/docs/configuration/

## 4. Verify

```bash
curl -s -o /dev/null -D - '<site-url>/?mps-verify=agent' | grep -i '^x-mod-pagespeed:'
```

Pass: one `X-Mod-Pagespeed: <version>` line; the header is on every HTML
response the module handles. No header:
https://modpagespeed.com/docs/troubleshooting/#no-x-mod-pagespeed-or-x-page-speed-header

Which Apache build the module targets:

```bash
rpm -q --requires ea-apache24-mod_pagespeed | grep ea-apache24-mmn
```

Pass: an `ea-apache24-mmn` requirement. A routine `ea-apache24` update keeps the
same Apache module ABI and does not unload the module.

## 5. Rollback

```bash
# Remove the module (the package's uninstall script reloads EA4 Apache; no provision needed)
sudo dnf remove -y ea-apache24-mod_pagespeed

# Stop the repo from being offered
sudo rm -f /etc/yum.repos.d/modpagespeed-ea4.repo

# Confirm it is gone: no output
httpd -M 2>/dev/null | grep pagespeed
```

`/etc/apache2/conf.d/pagespeed.conf` stays on disk so a later reinstall picks it
up unchanged. To remove it as well, delete it and `pagespeed_libraries.conf` in
the same directory after the package is gone.

Before a `cpanel/elevate` OS upgrade the module must be removed first (any
third-party EA4 module blocks elevate); reinstall it afterwards with the install
commands above. Details: https://modpagespeed.com/docs/cpanel/#before-an-os-upgrade-cpanelelevate
