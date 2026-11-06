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
    APACHE_ERROR_LOG=/var/log/httpd/error_log
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
    APACHE_ERROR_LOG=/var/log/apache2/error.log
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

# Stage a stale pagespeed.license in the parent of the FileCachePath, where an
# earlier install layout kept it (same fixture as test/system/setup_apache_test.sh):
# test_no_license_apparatus.py asserts the module logs one INFO line about it
# and otherwise ignores it.
STALE_LICENSE_FILE="$(dirname "$CACHE_DIR")/pagespeed.license"
echo "stale-token-from-a-previous-release" > "$STALE_LICENSE_FILE"
chown "$APACHE_USER:$APACHE_GROUP" "$STALE_LICENSE_FILE"

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

    # Resources are optimized in place, under their original URL
    # (test_ipro.py, test_rel_canonical.py wait for that to happen).
    ModPagespeedInPlaceResourceOptimization on

    # The container image's mime table has no webp entry, and the module
    # refuses a typeless input (test_image_resize.py,
    # test_resource_content_type.py).
    AddType image/webp .webp

    # Keeps the module's INFO lines in the server error log;
    # test_no_license_apparatus.py reads its stale-license notice there.
    LogLevel warn pagespeed:info

    # test_zerocopy_serve.py asserts the aliased serve fires on this chain.
    ModPagespeedCycloneZeroCopy on
    ModPagespeedCycloneZeroCopyServe on

    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false
    ModPagespeedMessageBufferSize 100000

    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

    <Location /mod_pagespeed_example>
        Header append 'X-Extra-Header' '1'
    </Location>

    # Fixtures some automatic/ tests need (same as test/system/setup_apache_test.sh).
    # Inputs a rewritten resource must not grow more cacheable than.
    <Directory "/var/www/html/mod_pagespeed_test/ipro/cc200p/">
        Header set Cache-control "private, max-age=200"
    </Directory>
    <Directory "/var/www/html/mod_pagespeed_test/ipro/cc200ns/">
        Header set Cache-control "no-store, max-age=200"
    </Directory>
    # A request can only lower the image inline threshold, so the page gets
    # it from the configuration (test_rewrite_css.py::test_rewrite_css_images).
    <Location /mod_pagespeed_example/rewrite_css_images.html>
        ModPagespeedCssImageInlineMaxBytes 2048
    </Location>
    # The flatten size limit is configuration only (not settable per request).
    <Directory "/var/www/html/mod_pagespeed_test/flatten_limit_tiny">
        ModPagespeedCssFlattenMaxBytes 5
    </Directory>
    <Directory "/var/www/html/mod_pagespeed_test/flatten_limit_medium">
        ModPagespeedCssFlattenMaxBytes 50
    </Directory>

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

    # The module package enables the optimizer-daemon in-place path server-wide
    # (a2enconf pagespeed_daemon). The tests expect the module's classic
    # in-place path on the main server, like the CI harness; the daemon path
    # runs on its own vhost instead (Step 4b, checked in Step 6b). The package
    # file itself stays in conf-available, where Step 4b reads it.
    a2disconf pagespeed_daemon 2>/dev/null || true
    DAEMON_INSTALLED_CONF="$APACHE_CONF_DIR/conf-available/pagespeed_daemon.conf"

    # Pin a fixed prefork pool, as the harness does: test_mpm_thread_resolution.py
    # asserts the module reads exactly this pool shape (16 children) from the MPM.
    a2dismod mpm_event mpm_worker 2>/dev/null || true
    a2enmod mpm_prefork 2>/dev/null || true
    cat > "$APACHE_CONF_DIR/mods-available/mpm_prefork.conf" << 'EOF'
<IfModule mpm_prefork_module>
    StartServers            16
    MinSpareServers         16
    MaxSpareServers         16
    ServerLimit             32
    MaxRequestWorkers       16
    MaxConnectionsPerChild  0
</IfModule>
EOF

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

    # Resources are optimized in place, under their original URL
    # (test_ipro.py, test_rel_canonical.py wait for that to happen).
    ModPagespeedInPlaceResourceOptimization on

    # The container image's mime table has no webp entry, and the module
    # refuses a typeless input (test_image_resize.py,
    # test_resource_content_type.py).
    AddType image/webp .webp

    # Keeps the module's INFO lines in the server error log;
    # test_no_license_apparatus.py reads its stale-license notice there.
    LogLevel warn pagespeed:info

    # test_zerocopy_serve.py asserts the aliased serve fires on this chain.
    ModPagespeedCycloneZeroCopy on
    ModPagespeedCycloneZeroCopyServe on

    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false
    ModPagespeedMessageBufferSize 100000

    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

    <Location /mod_pagespeed_example>
        Header append 'X-Extra-Header' '1'
    </Location>

    # Fixtures some automatic/ tests need (same as test/system/setup_apache_test.sh).
    # Inputs a rewritten resource must not grow more cacheable than.
    <Directory "/var/www/html/mod_pagespeed_test/ipro/cc200p/">
        Header set Cache-control "private, max-age=200"
    </Directory>
    <Directory "/var/www/html/mod_pagespeed_test/ipro/cc200ns/">
        Header set Cache-control "no-store, max-age=200"
    </Directory>
    # A request can only lower the image inline threshold, so the page gets
    # it from the configuration (test_rewrite_css.py::test_rewrite_css_images).
    <Location /mod_pagespeed_example/rewrite_css_images.html>
        ModPagespeedCssImageInlineMaxBytes 2048
    </Location>
    # The flatten size limit is configuration only (not settable per request).
    <Directory "/var/www/html/mod_pagespeed_test/flatten_limit_tiny">
        ModPagespeedCssFlattenMaxBytes 5
    </Directory>
    <Directory "/var/www/html/mod_pagespeed_test/flatten_limit_medium">
        ModPagespeedCssFlattenMaxBytes 50
    </Directory>

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

# CSS must reach the module compressed on this lane; the Debian lane gets
# that from its distro deflate.conf (test_ipro.py: TestIPROVaryParity,
# TestIproWithModDeflate).
AddOutputFilterByType DEFLATE text/css
CONFEOF

    # Same as the Debian branch above: the tests want the classic in-place
    # path on the main server. httpd reads conf.d/*.conf directly, so move the
    # package's daemon drop-in to a name that glob skips; Step 4b reads the
    # directives from the moved file.
    if [ -f "$APACHE_CONF_DIR/conf.d/pagespeed_daemon.conf" ]; then
        mv "$APACHE_CONF_DIR/conf.d/pagespeed_daemon.conf" \
            "$APACHE_CONF_DIR/conf.d/pagespeed_daemon.conf.disabled"
    fi
    DAEMON_INSTALLED_CONF="$APACHE_CONF_DIR/conf.d/pagespeed_daemon.conf.disabled"

    # Ensure required modules are loaded (most are defaults on RHEL)
    # mod_headers and mod_expires may not be auto-loaded
    for mod in headers expires deflate; do
        if [ -f "$APACHE_MODULES_DIR/mod_${mod}.so" ] && \
           ! grep -rq "mod_${mod}" "$APACHE_CONF_DIR/conf.modules.d/" 2>/dev/null; then
            echo "LoadModule ${mod}_module modules/mod_${mod}.so" \
                > "$APACHE_CONF_DIR/conf.modules.d/99-pagespeed-${mod}.conf"
        fi
    done

    # Pin the same fixed prefork pool as the Debian branch above
    # (test_mpm_thread_resolution.py): pick prefork in the MPM module config,
    # then pin the pool shape in a drop-in.
    MPM_MODULE_CONF="$APACHE_CONF_DIR/conf.modules.d/00-mpm.conf"
    if [ -f "$MPM_MODULE_CONF" ]; then
        sed -i \
            -e 's|^#\{0,1\}[[:space:]]*LoadModule mpm_prefork_module|LoadModule mpm_prefork_module|' \
            -e 's|^#\{0,1\}[[:space:]]*LoadModule mpm_worker_module|#LoadModule mpm_worker_module|' \
            -e 's|^#\{0,1\}[[:space:]]*LoadModule mpm_event_module|#LoadModule mpm_event_module|' \
            "$MPM_MODULE_CONF"
    fi
    cat > "$APACHE_CONF_DIR/conf.d/99-pagespeed-mpm.conf" << 'EOF'
<IfModule mpm_prefork_module>
    StartServers            16
    MinSpareServers         16
    MaxSpareServers         16
    ServerLimit             32
    MaxRequestWorkers       16
    MaxConnectionsPerChild  0
</IfModule>
EOF

    echo "ServerName localhost" >> "$APACHE_CONF_DIR/conf/httpd.conf"
fi

# ── Step 4b: vhost for the stock daemon path ───────────────────────────────
# The package's daemon directives are off for the main server now, so one
# vhost keeps the stock customer path under test: the two directives are read
# verbatim from the installed package file (never retyped), on a port of its
# own over the same document root, as the package ships them. Step 6b fetches
# a query-free image copy through this vhost.

DAEMON_VHOST_PORT=8081
DAEMON_DIRECTIVES="$(grep -hE \
    '^[[:space:]]*ModPagespeedDaemon(SocketPath|VolumePath)[[:space:]]' \
    "$DAEMON_INSTALLED_CONF" 2>/dev/null || true)"

if [ "$(printf '%s\n' "$DAEMON_DIRECTIVES" | grep -c .)" -ne 2 ]; then
    log_error "expected the two daemon directives in $DAEMON_INSTALLED_CONF, found:"
    printf '%s\n' "$DAEMON_DIRECTIVES"
    exit 1
fi

if [ "$DISTRO_FAMILY" = "rhel" ]; then
    DAEMON_VHOST_FILE="$APACHE_CONF_DIR/conf.d/pagespeed-daemon-smoke.conf"
else
    DAEMON_VHOST_FILE="$APACHE_CONF_DIR/sites-available/pagespeed-daemon-smoke.conf"
fi

{
    cat <<EOF
Listen $DAEMON_VHOST_PORT
<VirtualHost *:$DAEMON_VHOST_PORT>
    ServerName localhost
    DocumentRoot "$DOC_ROOT"
    ModPagespeedInPlaceResourceOptimization on
EOF
    printf '%s\n' "$DAEMON_DIRECTIVES"
    cat << 'EOF'
</VirtualHost>
EOF
} > "$DAEMON_VHOST_FILE"

if [ "$DISTRO_FAMILY" != "rhel" ]; then
    a2ensite pagespeed-daemon-smoke 2>/dev/null || true
fi

# ── Step 5: Test configuration ──────────────────────────────────────────────
log_info "Testing Apache configuration..."
if ! $APACHE_CTL configtest 2>&1; then
    log_error "Apache configuration test failed"
    exit 1
fi
log_info "Configuration OK"

# ── Step 5b: Start the pagespeed-optimizer worker ───────────────────────────
# In-place optimization is served by the pagespeed-optimizer worker. On a
# host the worker package starts it from its systemd unit; this container has
# no systemd, so start it here the way that unit would: user/group pagespeed,
# /run/pagespeed-optimizer 0750, the cache dir 3770 pagespeed:pagespeed,
# umask 0007, and the unit's ExecStart arguments (--log-format json included;
# the CI shared-cache harness starts the worker without it). The module
# resolves the daemon once, at Apache's start, and serves in-place resources
# unoptimized for that server process when the socket is not there, so the
# worker must come up before Apache.

OPTIMIZER_BIN=/usr/bin/pagespeed-optimizer
OPTIMIZER_RUN_DIR=/run/pagespeed-optimizer
OPTIMIZER_SOCKET="$OPTIMIZER_RUN_DIR/notify.sock"
OPTIMIZER_CACHE_DIR=/var/cache/pagespeed-optimizer/v3

if [ ! -x "$OPTIMIZER_BIN" ]; then
    log_error "pagespeed-optimizer not found at $OPTIMIZER_BIN"
    exit 1
fi

log_info "Starting the pagespeed-optimizer worker..."

# The identity the worker package's sysusers drop-in creates on a host; the
# web-server user joins the group (as the module package's postinst does) so
# Apache can reach the worker's socket and cache volume.
getent group pagespeed >/dev/null || groupadd -r pagespeed
id pagespeed >/dev/null 2>&1 || \
    useradd -r -g pagespeed -d /var/cache/pagespeed-optimizer \
        -s /usr/sbin/nologin pagespeed
usermod -a -G pagespeed "$APACHE_USER"

mkdir -p "$OPTIMIZER_RUN_DIR" "$OPTIMIZER_CACHE_DIR"
chown pagespeed:pagespeed "$OPTIMIZER_RUN_DIR" "$OPTIMIZER_CACHE_DIR"
chmod 0750 "$OPTIMIZER_RUN_DIR"
chmod 3770 "$OPTIMIZER_CACHE_DIR"

# The unit's UMask=0007; the log goes to a file (the unit sends it to the
# journal) so a failed start can be printed below. Uniquely named per run.
WORKER_LOG="/tmp/pagespeed-optimizer-smoke.$$.log"
su -s /bin/sh pagespeed -c "umask 0007; $OPTIMIZER_BIN \
--cache-dir $OPTIMIZER_CACHE_DIR \
--socket $OPTIMIZER_SOCKET \
--cache-size 536870912 \
--log-format json \
--log-level info > $WORKER_LOG 2>&1 &"

# Bounded wait for the notify socket, as the unit's own start hook does
# (300 x 0.1s). Without the socket the smoke would run every in-place test
# against a server that cannot optimize, so fail instead.
WORKER_WAITED=0
while [ ! -S "$OPTIMIZER_SOCKET" ]; do
    if [ "$WORKER_WAITED" -ge 300 ]; then
        log_error "$OPTIMIZER_SOCKET not present after 30s; worker log:"
        cat "$WORKER_LOG" 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
    WORKER_WAITED=$((WORKER_WAITED + 1))
done
log_info "Optimizer worker is up at $OPTIMIZER_SOCKET"

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

# ── Step 6b: smoke the stock daemon path ───────────────────────────────────
# Prove the vhost from Step 4b answers the way the package's own
# pagespeed_daemon.conf arranges: fetch 1 stores the resource, a later fetch
# is served from the shared cache (an Age header, a Vary naming Accept), and
# an image/avif or image/webp variant smaller than the original appears
# within a bounded wait. The daemon path serves query-free URLs only, so the
# test image is copied under a fresh query-free name. On failure the worker
# log is printed and the run stops before pytest.

DAEMON_SMOKE_SRC="$DOC_ROOT/mod_pagespeed_test/ipro/test_image_dont_reuse.png"
if [ ! -f "$DAEMON_SMOKE_SRC" ]; then
    log_error "daemon-path smoke: $DAEMON_SMOKE_SRC not found"
    exit 1
fi

DAEMON_SMOKE_REL="mod_pagespeed_test/daemon-path-smoke-$$.png"
cp "$DAEMON_SMOKE_SRC" "$DOC_ROOT/$DAEMON_SMOKE_REL"
DAEMON_SMOKE_URL="http://localhost:$DAEMON_VHOST_PORT/$DAEMON_SMOKE_REL"
DAEMON_SMOKE_SIZE=$(stat -c %s "$DOC_ROOT/$DAEMON_SMOKE_REL")
DAEMON_SMOKE_BODY="$(mktemp)"
log_info "Daemon-path smoke: $DAEMON_SMOKE_URL ($DAEMON_SMOKE_SIZE bytes); fetch 1 stores it"
curl -s -o /dev/null "$DAEMON_SMOKE_URL" || true

# (a) A later fetch served from the shared cache.
DAEMON_CACHE_TRIES=0
DAEMON_CACHE_OK=0
DAEMON_CACHE_HEADERS=""
while [ "$DAEMON_CACHE_TRIES" -lt 30 ]; do
    DAEMON_CACHE_HEADERS="$(curl -s -o /dev/null -D - "$DAEMON_SMOKE_URL" 2>/dev/null || true)"
    if printf '%s\n' "$DAEMON_CACHE_HEADERS" | tr -d '\r' | grep -qi '^age:' && \
       printf '%s\n' "$DAEMON_CACHE_HEADERS" | tr -d '\r' | \
           grep -qiE '^vary:([^,]*,)*[[:space:]]*accept([,[:space:]]|$)'; then
        DAEMON_CACHE_OK=1
        break
    fi
    sleep 1
    DAEMON_CACHE_TRIES=$((DAEMON_CACHE_TRIES + 1))
done

if [ "$DAEMON_CACHE_OK" -ne 1 ]; then
    log_error "daemon-path smoke: no shared-cache serve (Age header, Vary with Accept) within ${DAEMON_CACHE_TRIES}s; last response headers:"
    printf '%s\n' "$DAEMON_CACHE_HEADERS"
    log_error "optimizer worker log:"
    cat "$WORKER_LOG" 2>/dev/null || true
    rm -f "$DAEMON_SMOKE_BODY"
    exit 1
fi
log_info "Daemon-path smoke: shared-cache serve (fetch $((DAEMON_CACHE_TRIES + 2))):"
printf '%s\n' "$DAEMON_CACHE_HEADERS"

# (b) An avif or webp variant, smaller than the original, within about 60s.
DAEMON_VARIANT_TRIES=0
DAEMON_VARIANT_OK=0
DAEMON_VARIANT_HEADERS=""
while [ "$DAEMON_VARIANT_TRIES" -lt 60 ]; do
    DAEMON_VARIANT_HEADERS="$(curl -s -o "$DAEMON_SMOKE_BODY" -D - \
        -H 'Accept: image/avif,image/webp' "$DAEMON_SMOKE_URL" 2>/dev/null || true)"
    DAEMON_CT="$(printf '%s\n' "$DAEMON_VARIANT_HEADERS" | tr -d '\r' | \
        grep -i '^content-type:' | head -1 | cut -d' ' -f2)"
    DAEMON_LEN=$(stat -c %s "$DAEMON_SMOKE_BODY" 2>/dev/null || echo 0)
    case "${DAEMON_CT,,}" in
        image/avif*|image/webp*)
            if [ "$DAEMON_LEN" -lt "$DAEMON_SMOKE_SIZE" ]; then
                DAEMON_VARIANT_OK=1
                break
            fi
            ;;
    esac
    sleep 1
    DAEMON_VARIANT_TRIES=$((DAEMON_VARIANT_TRIES + 1))
done

if [ "$DAEMON_VARIANT_OK" -ne 1 ]; then
    log_error "daemon-path smoke: no image/avif or image/webp variant smaller than the original ($DAEMON_SMOKE_SIZE bytes) within ${DAEMON_VARIANT_TRIES}s; last response headers:"
    printf '%s\n' "$DAEMON_VARIANT_HEADERS"
    log_error "optimizer worker log:"
    cat "$WORKER_LOG" 2>/dev/null || true
    rm -f "$DAEMON_SMOKE_BODY"
    exit 1
fi
log_info "Daemon-path smoke: variant $DAEMON_CT, $DAEMON_LEN bytes (original $DAEMON_SMOKE_SIZE bytes), fetch $((DAEMON_VARIANT_TRIES + 2)):"
printf '%s\n' "$DAEMON_VARIANT_HEADERS"
rm -f "$DAEMON_SMOKE_BODY"

# ── Step 7: Run tests ──────────────────────────────────────────────────────
log_info "Running system tests..."

# Diagnostic: the worker the in-place tests depend on, and its sockets, so a
# host run can confirm the tests have a worker to talk to. The RHEL image
# installs no ps, so scan /proc when it is missing; /proc/<pid>/comm holds
# only the first 15 characters, so match the command line's executable.
log_info "Optimizer worker process:"
if command -v ps >/dev/null 2>&1; then
    ps -o pid,user,args -C pagespeed-optimizer \
        || log_warn "no pagespeed-optimizer process is running"
else
    WORKER_FOUND=0
    for pid_dir in /proc/[0-9]*; do
        WORKER_ARGS=$(tr '\000' ' ' 2>/dev/null < "$pid_dir/cmdline") || continue
        WORKER_EXE=${WORKER_ARGS%% *}
        if [ "${WORKER_EXE##*/}" = "pagespeed-optimizer" ]; then
            WORKER_PID=${pid_dir#/proc/}
            WORKER_USER=$(stat -c %U "$pid_dir" 2>/dev/null || echo '?')
            echo "$WORKER_PID $WORKER_USER $WORKER_ARGS"
            WORKER_FOUND=1
        fi
    done
    if [ "$WORKER_FOUND" -eq 0 ]; then
        log_warn "no pagespeed-optimizer process is running"
    fi
fi
log_info "Optimizer socket directory $OPTIMIZER_RUN_DIR:"
ls -l "$OPTIMIZER_RUN_DIR"

export PAGESPEED_HOST=localhost
export PAGESPEED_PORT=80
export PAGESPEED_SERVER_TYPE=apache
export PAGESPEED_CACHE_DIR="$CACHE_DIR"
export PAGESPEED_STATS_ENABLED=1
export PAGESPEED_EXAMPLE_ROOT=/mod_pagespeed_example
export PAGESPEED_TEST_ROOT=/mod_pagespeed_test
# Tests read Apache's error log from this path (test_no_license_apparatus.py,
# test_mpm_thread_resolution.py); the per-distro default only matches Debian.
export PAGESPEED_APACHE_ERROR_LOG="$APACHE_ERROR_LOG"
export PYTHONDONTWRITEBYTECODE=1

cd /src/test/system

# Pass through any extra arguments (e.g., -k sanity)
PYTEST_ARGS=("${@}")
if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
    PYTEST_ARGS=("automatic/" "-v")
fi

log_info "Running: python3 -m pytest -p no:cacheprovider ${PYTEST_ARGS[*]}"
exec python3 -m pytest -p no:cacheprovider "${PYTEST_ARGS[@]}"
