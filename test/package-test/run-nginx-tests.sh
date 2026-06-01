#!/bin/bash
#
# Entrypoint script for nginx package test containers.
# Configures nginx with the packaged PageSpeed module, starts the server,
# and runs the system test suite.
#
# Expected mounts:
#   /src  - Source tree (read-only) for test content and test framework
#
# Expected environment:
#   NGINX_BINARY    - Path to nginx binary (default: /usr/local/nginx/sbin/nginx)
#   MODULE_PATH     - Path to ngx_pagespeed_module.so (set by Dockerfile)
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

NGINX_BINARY="${NGINX_BINARY:-/usr/local/nginx/sbin/nginx}"
MODULE_PATH="${MODULE_PATH:-/usr/local/nginx/modules/ngx_pagespeed_module.so}"
NGINX_PORT=8080
WORK_DIR=/tmp/nginx_pagespeed_test
DOC_ROOT="$WORK_DIR/www"
LOG_DIR="$WORK_DIR/logs"
CACHE_DIR="$WORK_DIR/cache"
CONFIG_FILE="$WORK_DIR/nginx.conf"
PID_FILE="$WORK_DIR/nginx.pid"

# ── Step 1: Verify module and nginx exist ───────────────────────────────────
log_info "Verifying nginx installation..."

if [ ! -x "$NGINX_BINARY" ]; then
    log_error "nginx binary not found at $NGINX_BINARY"
    exit 1
fi
log_info "nginx: $($NGINX_BINARY -v 2>&1)"

if [ ! -f "$MODULE_PATH" ]; then
    log_error "ngx_pagespeed module not found at $MODULE_PATH"
    exit 1
fi
log_info "Module: $MODULE_PATH ($(du -h "$MODULE_PATH" | cut -f1))"

# ── Step 2: Set up directories ──────────────────────────────────────────────
log_info "Setting up directories..."
mkdir -p "$WORK_DIR" "$DOC_ROOT" "$LOG_DIR" "$CACHE_DIR"
mkdir -p "$WORK_DIR/client_body_temp" "$WORK_DIR/proxy_temp" "$WORK_DIR/fastcgi_temp"

# ── Step 3: Copy test content ───────────────────────────────────────────────
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

# Create index.html
echo "<html><body><h1>PageSpeed nginx Package Test</h1></body></html>" > "$DOC_ROOT/index.html"

# ── Step 4: Generate nginx configuration ────────────────────────────────────
log_info "Generating nginx configuration..."

cat > "$CONFIG_FILE" << EOF
worker_processes auto;
pid $PID_FILE;
error_log $LOG_DIR/error.log info;

load_module $MODULE_PATH;

events {
    worker_connections 1024;
}

http {
    types {
        text/html                             html htm shtml;
        text/css                              css;
        text/xml                              xml;
        application/javascript                js;
        application/json                      json;
        image/gif                             gif;
        image/jpeg                            jpeg jpg;
        image/png                             png;
        image/webp                            webp;
        image/svg+xml                         svg svgz;
        image/x-icon                          ico;
        application/pdf                       pdf;
        font/woff                             woff;
        font/woff2                            woff2;
    }
    default_type application/octet-stream;

    access_log $LOG_DIR/access.log;

    sendfile off;
    keepalive_timeout 65;

    gzip on;
    gzip_vary on;
    gzip_types application/ecmascript application/javascript application/json
               application/pdf application/postscript application/x-javascript
               image/svg+xml text/css text/csv text/javascript text/plain text/xml;
    gzip_http_version 1.0;

    # PageSpeed global settings
    pagespeed AdminPath /pagespeed_admin;
    pagespeed GlobalAdminPath /pagespeed_global_admin;
    pagespeed StatisticsPath /pagespeed_statistics;
    pagespeed GlobalStatisticsPath /pagespeed_global_statistics;
    pagespeed FileCachePath $CACHE_DIR;
    pagespeed CacheFlushPollIntervalSec 1;
    pagespeed Statistics on;
    pagespeed StatisticsLogging on;
    pagespeed LogDir $LOG_DIR;
    pagespeed MessageBufferSize 100000;

    # Disable Critical Images Beacon so that lazyload_images,
    # inline_preview_images, and other beacon-dependent filters work
    # without requiring real browser beacon data (matches CI config).
    pagespeed CriticalImagesBeaconEnabled false;

    # Configure canonicalize_javascript_libraries filter (matches CI config)
    pagespeed Library 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js;

    server {
        listen $NGINX_PORT;
        server_name localhost;
        root $DOC_ROOT;
        index index.html;

        pagespeed on;
        pagespeed RewriteLevel CoreFilters;

        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
            # Disable nginx gzip for .pagespeed. resources (matches CI config)
            gzip off;
        }

        # Serve files in no_cache/ with Cache-Control: no-cache (matches CI config)
        location ~ /no_cache/ {
            add_header Cache-Control "no-cache" always;
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon\$" { }
        location ~ "^/pagespeed_admin" { }
        location ~ "^/pagespeed_global_admin" { }
        location /pagespeed_statistics { }
        location /pagespeed_global_statistics { }

        location / {
            try_files \$uri \$uri/ =404;
        }
    }
}
EOF

# ── Step 5: Test configuration ──────────────────────────────────────────────
log_info "Testing nginx configuration..."
if ! "$NGINX_BINARY" -t -p "$WORK_DIR" -e "$LOG_DIR/error.log" -c "$CONFIG_FILE" 2>&1; then
    log_error "nginx configuration test failed"
    exit 1
fi
log_info "Configuration OK"

# ── Step 6: Start nginx ────────────────────────────────────────────────────
log_info "Starting nginx..."
"$NGINX_BINARY" -p "$WORK_DIR" -e "$LOG_DIR/error.log" -c "$CONFIG_FILE"
sleep 2

# Wait for nginx to respond
max_wait=30
waited=0
while [ $waited -lt $max_wait ]; do
    if curl -s --max-time 2 "http://localhost:$NGINX_PORT/" > /dev/null 2>&1; then
        log_info "nginx is responding after ${waited}s"
        break
    fi
    sleep 1
    waited=$((waited + 1))
done

if [ $waited -ge $max_wait ]; then
    log_error "nginx failed to start within ${max_wait}s"
    cat "$LOG_DIR/error.log" 2>/dev/null | tail -30
    exit 1
fi

# Verify PageSpeed header
if curl -sI "http://localhost:$NGINX_PORT/mod_pagespeed_example/" 2>/dev/null | grep -qi "X-Page-Speed"; then
    log_info "PageSpeed module is active!"
else
    log_warn "PageSpeed header not found (may need warm-up)"
fi

# ── Step 7: Run tests ──────────────────────────────────────────────────────
log_info "Running system tests..."

export PAGESPEED_HOST=localhost
export PAGESPEED_PORT="$NGINX_PORT"
export PAGESPEED_SERVER_TYPE=nginx
export PAGESPEED_CACHE_DIR="$CACHE_DIR"
export PAGESPEED_STATS_ENABLED=1
export PAGESPEED_STATS_PATH=/pagespeed_statistics
export PAGESPEED_ADMIN_PATH=/pagespeed_admin
export PAGESPEED_EXAMPLE_ROOT=/mod_pagespeed_example
export PAGESPEED_TEST_ROOT=/mod_pagespeed_test
export PYTHONDONTWRITEBYTECODE=1

cd /src/test/system

# Pass through any extra arguments
PYTEST_ARGS=("${@}")
if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
    PYTEST_ARGS=("automatic/" "-v")
fi

log_info "Running: python3 -m pytest -p no:cacheprovider ${PYTEST_ARGS[*]}"
exec python3 -m pytest -p no:cacheprovider "${PYTEST_ARGS[@]}"
