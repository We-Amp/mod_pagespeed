#!/bin/bash
#
# nginx_package_docs.sh — author the docs shipped INSIDE both the deb
# and rpm: pagespeed.conf.sample, pagespeed_admin_restrict.conf.sample, README.md.
#
# The sample-config + admin-restrict snippet are reused verbatim from
# install/nginx/build_nginx_package.sh (the tarball builder). The README is
# rewritten to fix the false "Tested with nginx 1.29.x" string (M0 proved
# exact-version pinning is mandatory), and to state BYOL + the cache-dir
# auto-create note.
#
# Usage: nginx_package_docs.sh <docdir> <version> <deb|rpm> <nginx_upstream_ver>
set -euo pipefail

DOCDIR="$1"
VERSION="$2"
PKGFMT="$3"             # deb | rpm
NGINX_VER="$4"          # exact stock nginx version this module is pinned to

mkdir -p "${DOCDIR}"

# --- sample config (verbatim from build_nginx_package.sh) -------------------
cat > "${DOCDIR}/pagespeed.conf.sample" << 'CONF'
# Load the PageSpeed module (adjust path as needed)
load_module modules/ngx_pagespeed_module.so;

# In the http {} block:
# pagespeed on;
# pagespeed FileCachePath /var/cache/ngx_pagespeed;
# pagespeed AdminPath /pagespeed_admin;
# pagespeed GlobalAdminPath /pagespeed_global_admin;
#
# In a server {} block:
# pagespeed EnableFilters combine_css,combine_javascript;
# pagespeed EnableFilters rewrite_images;
# pagespeed EnableFilters collapse_whitespace;
#
# Admin endpoints are restricted to localhost by default; widen explicitly
# for ops access (e.g. add `allow 10.0.0.0/8;` before `deny all;`).
# URL-path-string ACLs in WAFs/edges are bypassable (`//pagespeed_admin`,
# `/./pagespeed_admin`, `%2e` tricks); rely on nginx's normalized `location`
# match here -- it is the right enforcement layer. Exact-match `location =`
# defeats those bypasses since nginx normalizes the URI before matching.
# Docs: https://modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
# location = /pagespeed_admin        { allow 127.0.0.1; allow ::1; deny all; }
# location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }
CONF

# --- admin-restrict snippet (verbatim from build_nginx_package.sh) ----------
cat > "${DOCDIR}/pagespeed_admin_restrict.conf.sample" << 'CONF'
# Drop-in snippet to restrict the PageSpeed admin endpoints to localhost.
#
# Usage: place inside a `server { ... }` block, e.g.
#   server {
#     listen 80;
#     ...
#     include /etc/nginx/snippets/pagespeed_admin_restrict.conf;
#   }
#
# Admin endpoints are restricted to localhost by default; widen explicitly
# for ops access (e.g. add `allow 10.0.0.0/8;` before `deny all;`).
# URL-path-string ACLs in WAFs/edges are bypassable (`//pagespeed_admin`,
# `/./pagespeed_admin`, `%2e` tricks); rely on nginx's normalized `location`
# match here -- exact-match `location =` defeats those bypasses since nginx
# normalizes the URI before matching.
# Docs: https://modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
location = /pagespeed_admin        { allow 127.0.0.1; allow ::1; deny all; }
location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }
CONF

# --- README -----------------------------------------------------------------
if [ "${PKGFMT}" = "deb" ]; then
  MODPATH="/usr/lib/nginx/modules/"
  LOADNOTE="On stock Ubuntu 24.04 (noble) nginx, this package's \`load_module\`
snippet is auto-enabled via \`/etc/nginx/modules-enabled/\` — no manual edit.
If you run nginx from the nginx.org repo (which does NOT auto-include
\`modules-enabled/\`), add \`load_module modules/ngx_pagespeed_module.so;\` to
the top of \`/etc/nginx/nginx.conf\` yourself."
  INSTALLCMD="sudo apt install nginx-module-pagespeed"
else
  MODPATH="/usr/lib64/nginx/modules/"
  LOADNOTE="On stock AlmaLinux/RHEL/Rocky 9 nginx, this package drops a
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

## Licensing — Bring Your Own License (BYOL)

This package is **free to install**. PageSpeed runs in **pass-through mode** (no
optimization, nginx serves origin content unchanged) until a valid license token
is present; with a valid token, optimization activates. No license material ships
in the package — see https://modpagespeed.com/pricing/ for activation and license tokens.

## Admin endpoints (localhost-only by default)

If you enable \`pagespeed AdminPath\` / \`GlobalAdminPath\`, restrict them at the
web-server layer. Copy \`pagespeed_admin_restrict.conf.sample\` into
\`/etc/nginx/snippets/pagespeed_admin_restrict.conf\` and \`include\` it from each
\`server {}\` that exposes the admin paths. Mirrors the Apache template's
\`Require local\` posture. See
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
package build. If you run a different nginx (e.g. nginx.org stable/mainline),
build from source using the tarball + \`build_nginx_with_pagespeed.sh\`, or
request a pinned build.

## More Information

- https://modpagespeed.com/
- https://packages.modpagespeed.com
EOF
