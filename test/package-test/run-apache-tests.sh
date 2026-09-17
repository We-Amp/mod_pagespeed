#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Entrypoint script for Apache package test containers.
# Detects Debian vs RHEL layout, installs test content, configures Apache,
# starts the server, and runs the system test suite.
#
# Expected mounts:
#   /src  - Source tree (read-only) for test content and test framework
#
# Expected environment:
#   DISTRO_FAMILY - "debian" or "rhel" (set by Dockerfile)
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

DISTRO_FAMILY="${DISTRO_FAMILY:-debian}"

# Distro-specific paths
if [ "$DISTRO_FAMILY" = "rhel" ]; then
    APACHE_USER=apache
    APACHE_GROUP=apache
    APACHE_SERVICE=httpd
    APACHE_MODULES_DIR=/usr/lib64/httpd/modules
    APACHE_CONF_DIR=/etc/httpd
    APACHE_CTL="apachectl"
    DOC_ROOT=/var/www/html
    MODULE_SO="$APACHE_MODULES_DIR/mod_pagespeed.so"
    CONF_FILE="$APACHE_CONF_DIR/conf.d/pagespeed.conf"
else
    APACHE_USER=www-data
    APACHE_GROUP=www-data
    APACHE_SERVICE=apache2
    APACHE_MODULES_DIR=/usr/lib/apache2/modules
    APACHE_CONF_DIR=/etc/apache2
    APACHE_CTL="apache2ctl"
    DOC_ROOT=/var/www/html
    MODULE_SO="$APACHE_MODULES_DIR/mod_pagespeed.so"
    CONF_FILE="$APACHE_CONF_DIR/mods-available/pagespeed.conf"
fi

CACHE_DIR=/var/cache/mod_pagespeed
LOG_DIR=/var/log/pagespeed

# ── Step 1: Verify module is installed ──────────────────────────────────────
log_info "Verifying mod_pagespeed module installation..."

if [ ! -f "$MODULE_SO" ]; then
    log_error "mod_pagespeed.so not found at $MODULE_SO"
    log_info "Contents of modules directory:"
    ls -la "$APACHE_MODULES_DIR"/ | grep -i pagespeed || echo "(none)"
    exit 1
fi

log_info "Module found: $MODULE_SO ($(du -h "$MODULE_SO" | cut -f1))"

# ── Step 2: Set up directories ──────────────────────────────────────────────
log_info "Setting up directories..."
mkdir -p "$CACHE_DIR" "$LOG_DIR"
chown "$APACHE_USER:$APACHE_GROUP" "$CACHE_DIR" "$LOG_DIR"
chmod 755 "$CACHE_DIR" "$LOG_DIR"

# ── Step 3: Copy test content from source tree ──────────────────────────────
log_info "Copying test content from /src/install/ ..."

for dir in mod_pagespeed_example mod_pagespeed_test do_not_modify; do
    if [ -d "/src/install/$dir" ]; then
        rm -rf "$DOC_ROOT/$dir"
        cp -r "/src/install/$dir" "$DOC_ROOT/"
        log_info "  Copied $dir"
    else
        log_warn "  /src/install/$dir not found"
    fi
done

chown -R "$APACHE_USER:$APACHE_GROUP" "$DOC_ROOT"

# ── Step 4: Configure Apache for testing ────────────────────────────────────
log_info "Configuring Apache for testing..."

if [ "$DISTRO_FAMILY" = "debian" ]; then
    # Debian: Create load file and conf, then enable
    cat > "$APACHE_CONF_DIR/mods-available/pagespeed.load" << EOF
LoadModule pagespeed_module $APACHE_MODULES_DIR/mod_pagespeed.so
EOF

    cat > "$CONF_FILE" << 'CONFEOF'
<IfModule pagespeed_module>
    ModPagespeed on
    AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"
    ModPagespeedRewriteLevel CoreFilters
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on
    ModPagespeedFetchHttps enable

    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false
    ModPagespeedMessageBufferSize 100000

    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

    <Location /mod_pagespeed_example>
        Header append 'X-Extra-Header' '1'
    </Location>

    <Location /mod_pagespeed_statistics>
        Require all granted
        SetHandler mod_pagespeed_statistics
    </Location>

    <Location /mod_pagespeed_message>
        Require all granted
        SetHandler mod_pagespeed_message
    </Location>

    <Location /pagespeed_admin>
        Require all granted
        SetHandler pagespeed_admin
    </Location>
</IfModule>

<Directory "/var/www/html/mod_pagespeed_test/no_cache/">
    Header set Cache-control "no-cache"
</Directory>
CONFEOF

    echo "ServerName localhost" > "$APACHE_CONF_DIR/conf-available/servername.conf"

    a2enmod rewrite headers deflate proxy proxy_http expires 2>/dev/null || true
    a2enmod pagespeed 2>/dev/null || true
    a2enconf servername 2>/dev/null || true

else
    # RHEL: The RPM postinst already placed config in /etc/httpd/conf.d/.
    # Overwrite with test-specific configuration.
    cat > "$CONF_FILE" << CONFEOF
LoadModule pagespeed_module $APACHE_MODULES_DIR/mod_pagespeed.so

<IfModule pagespeed_module>
    ModPagespeed on
    AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"
    ModPagespeedRewriteLevel CoreFilters
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on
    ModPagespeedFetchHttps enable

    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false
    ModPagespeedMessageBufferSize 100000

    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

    <Location /mod_pagespeed_example>
        Header append 'X-Extra-Header' '1'
    </Location>

    <Location /mod_pagespeed_statistics>
        Require all granted
        SetHandler mod_pagespeed_statistics
    </Location>

    <Location /mod_pagespeed_message>
        Require all granted
        SetHandler mod_pagespeed_message
    </Location>

    <Location /pagespeed_admin>
        Require all granted
        SetHandler pagespeed_admin
    </Location>
</IfModule>

<Directory "/var/www/html/mod_pagespeed_test/no_cache/">
    Header set Cache-control "no-cache"
</Directory>
CONFEOF

    # Ensure required modules are loaded (most are defaults on RHEL)
    # mod_headers and mod_expires may not be auto-loaded
    for mod in headers expires deflate; do
        if [ -f "$APACHE_MODULES_DIR/mod_${mod}.so" ] && \
           ! grep -rq "mod_${mod}" "$APACHE_CONF_DIR/conf.modules.d/" 2>/dev/null; then
            echo "LoadModule ${mod}_module modules/mod_${mod}.so" \
                > "$APACHE_CONF_DIR/conf.modules.d/99-pagespeed-${mod}.conf"
        fi
    done

    echo "ServerName localhost" >> "$APACHE_CONF_DIR/conf/httpd.conf"
fi

# ── Step 5: Test configuration ──────────────────────────────────────────────
log_info "Testing Apache configuration..."
if ! $APACHE_CTL configtest 2>&1; then
    log_error "Apache configuration test failed"
    exit 1
fi
log_info "Configuration OK"

# ── Step 6: Start Apache ────────────────────────────────────────────────────
log_info "Starting Apache..."
if [ "$DISTRO_FAMILY" = "rhel" ]; then
    # On RHEL, apachectl tries systemd which doesn't work in containers
    httpd -k start 2>/dev/null || httpd
else
    $APACHE_CTL start || $APACHE_CTL -k start
fi

# Wait for Apache to respond
log_info "Waiting for Apache to be ready..."
max_wait=30
waited=0
while [ $waited -lt $max_wait ]; do
    if curl -s --max-time 2 http://localhost/ > /dev/null 2>&1; then
        log_info "Apache is responding after ${waited}s"
        break
    fi
    sleep 1
    waited=$((waited + 1))
done

if [ $waited -ge $max_wait ]; then
    log_error "Apache failed to start within ${max_wait}s"
    if [ "$DISTRO_FAMILY" = "rhel" ]; then
        tail -20 /var/log/httpd/error_log 2>/dev/null || true
    else
        tail -20 /var/log/apache2/error.log 2>/dev/null || true
    fi
    exit 1
fi

# Verify mod_pagespeed header
if curl -sI http://localhost/mod_pagespeed_example/ 2>/dev/null | grep -qi "X-Mod-Pagespeed\|X-Page-Speed"; then
    log_info "mod_pagespeed is active!"
else
    log_warn "mod_pagespeed header not found (may need warm-up)"
fi

# ── Step 7: Run tests ──────────────────────────────────────────────────────
log_info "Running system tests..."

export PAGESPEED_HOST=localhost
export PAGESPEED_PORT=80
export PAGESPEED_SERVER_TYPE=apache
export PAGESPEED_CACHE_DIR="$CACHE_DIR"
export PAGESPEED_STATS_ENABLED=1
export PAGESPEED_EXAMPLE_ROOT=/mod_pagespeed_example
export PAGESPEED_TEST_ROOT=/mod_pagespeed_test
export PYTHONDONTWRITEBYTECODE=1

cd /src/test/system

# Pass through any extra arguments (e.g., -k sanity)
PYTEST_ARGS=("${@}")
if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
    PYTEST_ARGS=("automatic/" "-v")
fi

log_info "Running: python3 -m pytest -p no:cacheprovider ${PYTEST_ARGS[*]}"
exec python3 -m pytest -p no:cacheprovider "${PYTEST_ARGS[@]}"
