#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# nginx_package_docs.sh — author the docs shipped INSIDE both the deb
# and rpm: pagespeed.conf.sample, pagespeed_admin_restrict.conf.sample, README.md,
# plus the repo-root LICENSE and NOTICE copied verbatim.
#
# The sample config and the admin-restrict sample started as copies of the
# ones in install/nginx/build_nginx_package.sh (the tarball builder), which
# keeps its own copies; they have since diverged (package-specific paths and
# links), so edit each where it lives. The conffile
# /etc/nginx/snippets/pagespeed_admin_localhost.conf is NOT written here: the
# deb/rpm build scripts write it from their own short, pinned heredoc. The README is
# rewritten to fix the false "Tested with nginx 1.29.x" string (M0 proved
# exact-version pinning is mandatory), and to state the license + the cache-dir
# auto-create note.
#
# Usage: nginx_package_docs.sh <docdir> <version> <deb|rpm> <nginx_upstream_ver> [<distro_major>]
set -euo pipefail

DOCDIR="$1"
VERSION="$2"
PKGFMT="$3"             # deb | rpm
NGINX_VER="$4"          # exact stock nginx version this module is pinned to
DISTRO_MAJOR="${5:-9}"  # RHEL-family major (9|10) for the rpm README wording; deb ignores it
# The package the README is for: the distribution package, or the package for
# nginx.org's nginx (NGINX_PKG_NAME, set by the nginx.org legs).
PKG="${NGINX_PKG_NAME:-nginx-module-pagespeed}"
NGINXORG=0
[ "${PKG}" = "nginx-module-pagespeed-nginxorg" ] && NGINXORG=1
UPDATENOTE=""

mkdir -p "${DOCDIR}"

# --- license text + attribution notices --------------------------------------
# The Apache-2.0 terms want both next to the binaries; the statically linked
# BSD/MIT/Zlib/IJG-licensed components ask the same of their notices, so
# THIRD-PARTY-NOTICES ships alongside. The repo-root files are the single
# source; copied verbatim, never re-authored here. The rpm spec lists them as
# %license and %doc; the deb ships the staged tree as is.
SRCDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
install -m 644 "${SRCDIR}/LICENSE" "${SRCDIR}/NOTICE" \
  "${SRCDIR}/THIRD-PARTY-NOTICES" "${DOCDIR}/"

# --- sample config (started as the tarball builder's copy) ------------------
cat > "${DOCDIR}/pagespeed.conf.sample" << 'CONF'
# Load the PageSpeed module (adjust path as needed)
load_module modules/ngx_pagespeed_module.so;

# In the http {} block:
# pagespeed on;
# pagespeed FileCachePath /var/cache/ngx_pagespeed;
#
# In a server {} block:
# pagespeed EnableFilters combine_css,combine_javascript;
# pagespeed EnableFilters rewrite_images;
# pagespeed EnableFilters collapse_whitespace;
#
# Admin pages: set AdminPath inside the one server {} block that should
# serve them, and restrict it in that same block. The blocks below allow
# localhost only; widen explicitly for ops access (e.g. add
# `allow 10.0.0.0/8;` before `deny all;`).
# Each admin path takes two blocks: `=` covers the page itself and `^~`
# every page below it, ahead of any regular-expression location.
# pagespeed AdminPath /pagespeed_admin;
# location =  /pagespeed_admin  { allow 127.0.0.1; allow ::1; deny all; }
# location ^~ /pagespeed_admin/ { allow 127.0.0.1; allow ::1; deny all; }
#
# A handler path set in the http {} block is served by EVERY server {}
# block: its location blocks then belong in each of them, including the
# default server. GlobalAdminPath and GlobalStatisticsPath can only be set
# in the http {} block, so if you set them, put their blocks in every
# server {} block (pagespeed_admin_restrict.conf.sample is made for that):
# pagespeed GlobalAdminPath /pagespeed_global_admin;       # http {}
# location =  /pagespeed_global_admin  { allow 127.0.0.1; allow ::1; deny all; }
# location ^~ /pagespeed_global_admin/ { allow 127.0.0.1; allow ::1; deny all; }
#
# An admin path and every page below it are one unit for access control:
# give both blocks the same rule and add no location blocks for single pages
# below an admin path. To expose statistics on their own, use StatisticsPath.
# StatisticsPath, GlobalStatisticsPath, ConsolePath and MessagesPath have no
# pages below them: restrict each one you set with a single
# `location = /that_path { allow 127.0.0.1; allow ::1; deny all; }` block.
# Set handler paths without a trailing slash: a path set as `/x/` serves
# `/x/` and the pages below it, not `/x`, and takes one `location ^~ /x/`.
# URL-path-string ACLs in WAFs/edges are brittle; enforce the restriction
# here, in nginx's own `location` match -- it is the right enforcement layer.
# Docs: https://modpagespeed.com/docs/admin-console/#url-path-acls-are-brittle
CONF

# --- admin-restrict sample (started as the tarball builder's copy) ----------
cat > "${DOCDIR}/pagespeed_admin_restrict.conf.sample" << 'CONF'
# Drop-in snippet to restrict the PageSpeed admin endpoints to localhost.
#
# Usage: place inside EVERY `server { ... }` block, including the default
# server, e.g.
#   server {
#     listen 80;
#     ...
#     include /etc/nginx/snippets/pagespeed_admin_localhost.conf;
#   }
#
# The nginx-module-pagespeed package installs the four admin location lines
# below, with a short header, as the config file
# /etc/nginx/snippets/pagespeed_admin_localhost.conf; include that one.
#
# A handler path set in the http {} block is served by every server {}
# block, and GlobalAdminPath and GlobalStatisticsPath can only be set there.
# A server {} block without these blocks serves those pages unrestricted.
# Only an AdminPath set inside a single server {} block can be restricted in
# that block alone.
#
# The blocks below allow localhost only; widen explicitly for ops access
# (e.g. add `allow 10.0.0.0/8;` before `deny all;`).
# Each admin path takes two blocks: `=` covers the page itself and `^~`
# every page below it, ahead of any regular-expression location.
# An admin path and every page below it are one unit for access control:
# give both blocks the same rule and add no location blocks for single pages
# below an admin path. To expose statistics on their own, use StatisticsPath.
# URL-path-string ACLs in WAFs/edges are brittle; enforce the restriction
# here, in nginx's own `location` match -- it is the right enforcement layer.
# Docs: https://modpagespeed.com/docs/admin-console/#url-path-acls-are-brittle
#
# The paths must be the ones set with `pagespeed AdminPath` and
# `pagespeed GlobalAdminPath`, spelled exactly the same. Set them without a
# trailing slash: a path set as `/x/` serves `/x/` and the pages below it,
# not `/x`, and takes one `location ^~ /x/` block.
location =  /pagespeed_admin         { allow 127.0.0.1; allow ::1; deny all; }
location ^~ /pagespeed_admin/        { allow 127.0.0.1; allow ::1; deny all; }
location =  /pagespeed_global_admin  { allow 127.0.0.1; allow ::1; deny all; }
location ^~ /pagespeed_global_admin/ { allow 127.0.0.1; allow ::1; deny all; }

# StatisticsPath, GlobalStatisticsPath, ConsolePath and MessagesPath have no
# pages below them: restrict each one you set with a single block, e.g.
# location = /ngx_pagespeed_statistics { allow 127.0.0.1; allow ::1; deny all; }
CONF

# --- README -----------------------------------------------------------------
if [ "${PKGFMT}" = "deb" ]; then
  MODPATH="/usr/lib/nginx/modules/"
  LOADNOTE="On stock Debian/Ubuntu nginx, this package's \`load_module\`
snippet is auto-enabled via \`/etc/nginx/modules-enabled/\` — no manual edit.
If you run nginx from the nginx.org repo (which does NOT auto-include
\`modules-enabled/\`), add \`load_module modules/ngx_pagespeed_module.so;\` to
the top of \`/etc/nginx/nginx.conf\` yourself."
  INSTALLCMD="sudo apt install nginx-module-pagespeed"
  WORKERCMD="sudo apt install pagespeed-optimizer"
  WEBUSER="www-data"
  if [ "${NGINXORG}" = 1 ]; then
    LOADNOTE="nginx.org's \`nginx.conf\` does not read \`/etc/nginx/modules-enabled/\`: add
\`include /etc/nginx/modules-enabled/*.conf;\` at the top of \`/etc/nginx/nginx.conf\`.
This package ships its \`load_module\` line there and takes it away when it is
removed, so removing the package leaves nginx able to start."
    INSTALLCMD="sudo apt install ${PKG}"
    WEBUSER="nginx"
  fi
else
  MODPATH="/usr/lib64/nginx/modules/"
  LOADNOTE="On stock AlmaLinux/RHEL/Rocky ${DISTRO_MAJOR} nginx, this package drops a
\`load_module\` snippet under \`/usr/share/nginx/modules/\` which stock nginx
auto-includes from the main context — no manual edit. If you run nginx from the
nginx.org repo, ensure your \`nginx.conf\` includes
\`/usr/share/nginx/modules/*.conf\` (or add the \`load_module\` line manually)."
  INSTALLCMD="sudo dnf install ${PKG}"
  WORKERCMD="sudo dnf install pagespeed-optimizer"
  WEBUSER="nginx"
  if [ "${NGINXORG}" = 1 ]; then
    LOADNOTE="nginx.org's \`nginx.conf\` does not read \`/usr/share/nginx/modules/\`: add
\`include /usr/share/nginx/modules/*.conf;\` at the top of \`/etc/nginx/nginx.conf\`.
Updates: see \"Updates and the nginx hold\" below."
    UPDATENOTE="
## Updates and the nginx hold

This package is built for one nginx.org nginx version and works only with that
version. When nginx.org publishes a newer nginx before the matching build of this
package is available, a plain \`dnf update\` would otherwise fail as a whole and
install nothing. To prevent that, this package installs a dnf plugin,
\`pagespeed_nginx_pin\`
(\`/usr/lib/python3.9/site-packages/dnf-plugins/pagespeed_nginx_pin.py\`, settings in
\`/etc/dnf/plugins/pagespeed_nginx_pin.conf\`).

While this package is installed, the plugin holds nginx, and nginx.org module
packages that need the same nginx (such as \`nginx-module-njs\`), at a version this
package has a build for:

- \`sudo dnf update\` and dnf-automatic install every other update and keep nginx.
  Once the build for the newer nginx is in the repository, the same \`dnf update\`
  moves nginx, this package and nginx.org's module packages together.
- \`dnf check-update\` and \`dnf list updates\` still show the newer nginx, so
  monitoring sees a pending nginx update.
- A command that names nginx is left to dnf. To move nginx and this package
  together by hand:
  \`sudo dnf install nginx-<version> ${PKG}-<matching build>\`.
  \`sudo dnf upgrade nginx\` on its own fails with dnf's explanation while no
  matching build exists.
- To take an nginx update before the matching build exists, for example a security
  fix: \`sudo dnf upgrade nginx --allowerasing\`. This removes this package (nginx
  keeps starting: the package's load line goes with it) and installs the newer
  nginx. Install this package again once its build for that nginx is available.
- The plugin acts only on this host's nginx.org nginx and only while this package
  is installed. It is removed together with the package
  (\`sudo dnf remove ${PKG}\`), after which dnf behaves as before.

Not tested: updates through PackageKit or Cockpit, which are likely to bypass dnf
plugins, so a plain update there may still fail while nginx.org is ahead of the
matching build (use \`dnf update\` instead); dnf5 (Enterprise Linux 10, for which
this package is not built); and Rocky Linux 9 and RHEL 9, which are expected to
behave as AlmaLinux 9, where all of the above was tested.
"
  fi
fi

# The compatibility paragraph names the nginx the package is pinned to.
if [ "${NGINXORG}" = 1 ]; then
  PINNOTE="This package is built
against and **pinned to nginx.org's stable nginx ${NGINX_VER}**, and will only load
into that nginx. It cannot be installed alongside \`nginx-module-pagespeed\`, the
package for the distribution's own nginx. Each nginx.org stable release gets a
matching build, which the package version names (\`+ngx<nginx version>\`); upgrade
nginx and this package together. Until the build for a newer nginx is available,
a plain upgrade holds nginx back, and a full upgrade removes this package."
  if [ "${PKGFMT}" = rpm ]; then
    PINNOTE="This package is built
against and **pinned to nginx.org's stable nginx ${NGINX_VER}**, and will only load
into that nginx. It cannot be installed alongside \`nginx-module-pagespeed\`, the
package for the distribution's own nginx. Each nginx.org stable release gets a
matching build, which the package release names (\`.ngx<nginx version>\`); a plain
\`dnf update\` moves nginx and this package together once that build is available,
and holds nginx until then (see \"Updates and the nginx hold\")."
  fi
else
  PINNOTE="This package is built
against and **pinned to stock nginx ${NGINX_VER}** for this distribution, and
will only load into that nginx. A routine security update that keeps the same
nginx upstream version is fine; a distro nginx **minor rebase** requires a new
package build. For an nginx version we do not yet package, request a pinned
build (contact us); otherwise use your distro's stock nginx."
fi

cat > "${DOCDIR}/README.md" << EOF
# ${PKG} ${VERSION}

nginx dynamic module for PageSpeed web optimization (We-Amp mod_pagespeed ${VERSION}).

## Installation

If you haven't added the We-Amp package repository yet, add it first:

\`\`\`
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
\`\`\`

Then install the optimizer worker and the module, as two separate commands,
worker first:

\`\`\`
${WORKERCMD}
${INSTALLCMD}
sudo nginx -t && sudo systemctl restart nginx
\`\`\`

The module package adds the nginx user to the worker's \`pagespeed\` group
only if that group already exists when its install script runs. Installed in
one transaction, the package manager may run the module's script first (dnf
does), and the nginx user is then left out. To fix an existing install:
\`sudo usermod -a -G pagespeed ${WEBUSER}\`, then restart nginx. Point the
module at the worker with \`pagespeed DaemonSocketPath\` and
\`pagespeed DaemonVolumePath\`; see
https://modpagespeed.com/docs/installation-module/#native-nginx-module

The module \`.so\` installs to \`${MODPATH}\`.

${LOADNOTE}

Then enable PageSpeed in your config:

\`\`\`
pagespeed on;
pagespeed FileCachePath /var/cache/ngx_pagespeed;
\`\`\`

The \`FileCachePath\` directory is created automatically at config-parse time
and chowned to the worker user — **no manual \`mkdir\`/\`chown\` needed**.

## License

This package is licensed under the Apache License 2.0. The license text
(\`LICENSE\`) and the attribution notices (\`NOTICE\`) are installed next to
this file.

## Admin endpoints (restrict them to localhost)

If you enable \`pagespeed AdminPath\` / \`GlobalAdminPath\`, restrict them at the
web-server layer. This package installs the restriction as
\`/etc/nginx/snippets/pagespeed_admin_localhost.conf\` (a config file: your
edits survive upgrades; \`pagespeed_admin_restrict.conf.sample\` next to this
README has the same \`location\` blocks with the explanation). If you copied
the sample to \`/etc/nginx/snippets/pagespeed_admin_restrict.conf\` earlier,
that copy stays as it is; include one of the two, not both. Nothing includes
the snippet on its own: \`include\` it from
**every** \`server {}\` block, including the default server: a handler path
set in the \`http {}\` block is served by every \`server {}\` block, and
\`GlobalAdminPath\` / \`GlobalStatisticsPath\` can only be set there. Only an
\`AdminPath\` set inside a single \`server {}\` block can be restricted in that
block alone.

The snippet restricts each admin path with two \`location\` blocks: \`=\`
covers the page itself and \`^~\` every page below it, ahead of any
regular-expression location. An admin path and every page below it are one
unit for access control: give both blocks the same rule and add no
\`location\` blocks for single pages below an admin path (to expose statistics
on their own, use \`StatisticsPath\`). Set handler paths without a trailing
slash: a path set as \`/x/\` serves \`/x/\` and the pages below it, not
\`/x\`. Mirrors the Apache template's \`Require local\` posture. See
https://modpagespeed.com/docs/admin-console/#url-path-acls-are-brittle
for the threat model (URL-path-string ACLs in WAFs are bypassable; enforce at the
web server's normalized \`location\` match).

Upgrades: the shipped text of \`pagespeed_admin_localhost.conf\` is never
changed in place; a changed snippet would ship under a new file name, named in
the CHANGELOG. Should a release ever change it anyway and you have edited your
copy, Debian/Ubuntu ask which version to keep (a noninteractive upgrade then
needs \`-o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold\`
to keep yours), and AlmaLinux/RHEL/Rocky keep yours and write the new one next
to it as \`.rpmnew\`.

Removal: before \`apt-get purge ${PKG}\` (Debian/Ubuntu) or
\`dnf remove ${PKG}\` (AlmaLinux/RHEL/Rocky), delete the
\`include /etc/nginx/snippets/pagespeed_admin_localhost.conf;\` lines from your
\`server {}\` blocks: purge and erase remove the file (an edited copy is kept on
EL as \`.rpmsave\`), and an \`include\` of a missing file fails \`nginx -t\`.

## Compatibility — exact nginx version pin

nginx dynamic modules are pinned to the **exact** nginx version they were built
against (down to the patch); nginx refuses to load a module built for any other
version, and \`--with-compat\` does **not** relax this. ${PINNOTE}
${UPDATENOTE}
## More Information

- https://modpagespeed.com/
- https://packages.modpagespeed.com
EOF
