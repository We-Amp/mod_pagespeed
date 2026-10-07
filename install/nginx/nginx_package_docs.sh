#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# nginx_package_docs.sh — author the docs shipped INSIDE both the deb
# and rpm: pagespeed.conf.sample, pagespeed_admin_restrict.conf.sample, README.md,
# plus the repo-root LICENSE and NOTICE copied verbatim.
#
# The sample-config + admin-restrict snippet are reused verbatim from
# install/nginx/build_nginx_package.sh (the tarball builder). The README is
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

# --- sample config (verbatim from build_nginx_package.sh) -------------------
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
# Docs: https://modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
CONF

# --- admin-restrict snippet (verbatim from build_nginx_package.sh) ----------
cat > "${DOCDIR}/pagespeed_admin_restrict.conf.sample" << 'CONF'
# Drop-in snippet to restrict the PageSpeed admin endpoints to localhost.
#
# Usage: place inside EVERY `server { ... }` block, including the default
# server, e.g.
#   server {
#     listen 80;
#     ...
#     include /etc/nginx/snippets/pagespeed_admin_restrict.conf;
#   }
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
# Docs: https://modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
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
else
  MODPATH="/usr/lib64/nginx/modules/"
  LOADNOTE="On stock AlmaLinux/RHEL/Rocky ${DISTRO_MAJOR} nginx, this package drops a
\`load_module\` snippet under \`/usr/share/nginx/modules/\` which stock nginx
auto-includes from the main context — no manual edit. If you run nginx from the
nginx.org repo, ensure your \`nginx.conf\` includes
\`/usr/share/nginx/modules/*.conf\` (or add the \`load_module\` line manually)."
  INSTALLCMD="sudo dnf install nginx-module-pagespeed"
fi

cat > "${DOCDIR}/README.md" << EOF
# nginx-module-pagespeed ${VERSION}

nginx dynamic module for PageSpeed web optimization (We-Amp mod_pagespeed 1.15).

## Installation

If you haven't added the We-Amp package repository yet, add it first:

\`\`\`
curl -fsSL https://packages.modpagespeed.com/install.sh | sudo sh
\`\`\`

Then install the module:

\`\`\`
${INSTALLCMD}
sudo nginx -t && sudo systemctl reload nginx
\`\`\`

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
web-server layer. Copy \`pagespeed_admin_restrict.conf.sample\` into
\`/etc/nginx/snippets/pagespeed_admin_restrict.conf\` and \`include\` it from
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
https://modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
for the threat model (URL-path-string ACLs in WAFs are bypassable; enforce at the
web server's normalized \`location\` match).

## Compatibility — exact nginx version pin

nginx dynamic modules are pinned to the **exact** nginx version they were built
against (down to the patch); nginx refuses to load a module built for any other
version, and \`--with-compat\` does **not** relax this. This package is built
against and **pinned to stock nginx ${NGINX_VER}** for this distribution, and
will only load into that nginx. A routine security update that keeps the same
nginx upstream version is fine; a distro nginx **minor rebase** requires a new
package build. For an nginx version we do not yet package, request a pinned
build (contact us); otherwise use your distro's stock nginx.

## More Information

- https://modpagespeed.com/
- https://packages.modpagespeed.com
EOF
