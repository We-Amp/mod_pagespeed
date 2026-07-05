#!/bin/bash
#
# Build a distributable nginx PageSpeed package.
#
# Usage:
#   ./install/nginx/build_nginx_package.sh [-o output_dir]
#
# Prerequisites:
#   - Bazel build of //pagespeed/nginx:ngx_pagespeed_module.so completed
#   - Source tree with net/instaweb/public/VERSION

set -euo pipefail

SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="$(cd "${SCRIPTDIR}/../.." && pwd)"
OUTPUTDIR="${PWD}"

while getopts ":o:h" opt; do
  case $opt in
    o) OUTPUTDIR="$OPTARG"; mkdir -p "${OUTPUTDIR}" ;;
    h) echo "Usage: $(basename "$0") [-o output_dir]"; exit 0 ;;
    *) echo "Invalid option: -$OPTARG"; exit 1 ;;
  esac
done

# Read version
source "${SRCDIR}/net/instaweb/public/VERSION"
if [ -n "${PRERELEASE:-}" ]; then
  VERSION="${MAJOR}.${MINOR}.${BUILD}-${PRERELEASE}"
else
  VERSION="${MAJOR}.${MINOR}.${BUILD}"
fi

# Detect architecture
ARCH="$(uname -m)"
case "${ARCH}" in
  x86_64|amd64)  ARCH_LABEL="x86_64" ;;
  aarch64|arm64) ARCH_LABEL="aarch64" ;;
  *)             ARCH_LABEL="${ARCH}" ;;
esac

PKGNAME="ngx_pagespeed-${VERSION}"
PKGDIR="${OUTPUTDIR}/${PKGNAME}"
SO_PATH="${SRCDIR}/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so"

if [ ! -f "${SO_PATH}" ]; then
  echo "ERROR: nginx module not found at ${SO_PATH}"
  echo "Build it first: bazel build --config=clang-libstdcxx13 //pagespeed/nginx:ngx_pagespeed_module.so"
  exit 1
fi

echo "Creating nginx PageSpeed package: ${PKGNAME}"

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}"

# Copy module binary
cp "${SO_PATH}" "${PKGDIR}/ngx_pagespeed_module.so"

# Copy build-from-source script if available
if [ -f "${SRCDIR}/scripts/build_nginx_with_pagespeed.sh" ]; then
  cp "${SRCDIR}/scripts/build_nginx_with_pagespeed.sh" "${PKGDIR}/"
fi

# Create sample config
cat > "${PKGDIR}/pagespeed.conf.sample" << 'CONF'
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
# Docs: https://www.modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
# location = /pagespeed_admin        { allow 127.0.0.1; allow ::1; deny all; }
# location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }
CONF

# Create standalone admin-restriction snippet that operators can `include` from
# any server {} that exposes pagespeed admin paths. Default-deny matches the
# Apache template posture (Require local) at install/common/pagespeed.conf.template.
cat > "${PKGDIR}/pagespeed_admin_restrict.conf.sample" << 'CONF'
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
# Docs: https://www.modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle
location = /pagespeed_admin        { allow 127.0.0.1; allow ::1; deny all; }
location = /pagespeed_global_admin { allow 127.0.0.1; allow ::1; deny all; }
CONF

# Create README
cat > "${PKGDIR}/README.md" << EOF
# ngx_pagespeed ${VERSION}

nginx dynamic module for PageSpeed web optimization.

## Installation

### Dynamic module (recommended)

1. Copy \`ngx_pagespeed_module.so\` to your nginx modules directory:
   \`\`\`
   cp ngx_pagespeed_module.so /usr/lib/nginx/modules/
   \`\`\`

2. Add to your nginx.conf (before the \`http\` block):
   \`\`\`
   load_module modules/ngx_pagespeed_module.so;
   \`\`\`

3. Enable PageSpeed in your server block:
   \`\`\`
   pagespeed on;
   pagespeed FileCachePath /var/cache/ngx_pagespeed;
   \`\`\`

4. Restart nginx:
   \`\`\`
   nginx -t && systemctl restart nginx
   \`\`\`

### Admin endpoints (localhost-only by default)

If you enable \`pagespeed AdminPath\` / \`GlobalAdminPath\`, restrict them at
the web-server layer. Copy \`pagespeed_admin_restrict.conf.sample\` into
\`/etc/nginx/snippets/pagespeed_admin_restrict.conf\` and \`include\` it from
each \`server {}\` that exposes the admin paths. Mirrors the Apache template's
\`Require local\` posture.

See https://www.modpagespeed.com/1.1/docs/admin-console/#url-path-acls-are-brittle for the
threat model (URL-path-string ACLs in WAFs are bypassable; enforce at the
web server's normalized \`location\` match).

### Build from source

Use the included \`build_nginx_with_pagespeed.sh\` script to build
nginx with PageSpeed compiled in.

## Compatibility

This tarball ships the module .so built against the dev nginx (currently
1.30.3). nginx dynamic modules are pinned to the EXACT nginx version they were
built against -- nginx refuses to load a module built for any other version, and
--with-compat does NOT relax this (confirmed empirically, the design record M0: a module
built against 1.30.2 will not even load into 1.30.3). To use this .so you must
run the exact nginx version it was built against, or rebuild from source against
your nginx with the included build_nginx_with_pagespeed.sh. For one-step
\`apt install\`/\`dnf install\` against your distro's stock nginx, use the signed
packages at https://packages.modpagespeed.com instead (built and version-pinned
per distro: Ubuntu 24.04 nginx 1.24.0, AlmaLinux 9 nginx 1.20.1).

- Requires Linux ${ARCH_LABEL}

## More Information

- https://github.com/we-amp/mod_pagespeed
EOF

# Create tarball
cd "${OUTPUTDIR}"
tar czf "${PKGNAME}-linux-${ARCH_LABEL}.tar.gz" "${PKGNAME}/"
rm -rf "${PKGDIR}"

echo "Package created: ${OUTPUTDIR}/${PKGNAME}-linux-${ARCH_LABEL}.tar.gz"
