#!/bin/bash
#
# Builds ngx_pagespeed module, sets up NGINX, and runs Python system tests.
#
# Usage (inside Docker container):
#   ./test/system/run_nginx_tests.sh [options] [pytest-args...]
#
# Options:
#   --build-only    Only build the module, don't run tests
#   --skip-build    Skip building, assume module is already built
#   --keep-running  Keep NGINX running after tests
#   --no-module     Run baseline tests without PageSpeed module
#   --help          Show this help message
#
# Examples:
#   ./test/system/run_nginx_tests.sh                    # Run all tests (automatic/ + system/ + nginx/)
#   ./test/system/run_nginx_tests.sh -k sanity          # Run only sanity tests
#   ./test/system/run_nginx_tests.sh automatic/         # Run only automatic/ tests
#   ./test/system/run_nginx_tests.sh nginx/             # Run only nginx/ tests
#   ./test/system/run_nginx_tests.sh --build-only       # Just build the module
#   ./test/system/run_nginx_tests.sh --no-module        # Run baseline tests without PageSpeed
#
# Test Directories:
#   automatic/ - Golden standard tests shared with Apache (core PageSpeed functionality)
#   system/    - Cross-server tests (IPRO ETag, 404 stats, etc.)
#   nginx/     - NGINX-specific tests for features unique to the NGINX module

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}

show_help() {
    cat << EOF
Usage: $0 [options] [pytest-args...]

Builds ngx_pagespeed module, sets up NGINX with PageSpeed, and runs Python system tests.

Options:
  --build-only    Only build the module, don't run tests
  --skip-build    Skip building, assume module is already built
  --keep-running  Keep NGINX running after tests (for debugging)
  --no-module     Run baseline tests without PageSpeed module
  --jobs=N        Number of parallel build jobs (default: auto)
  --port=N        Port for NGINX to listen on (default: 8080)
  --help          Show this help message

Pytest arguments:
  Any additional arguments are passed to pytest.

Examples:
  $0                                    # Run all tests (automatic/ + system/ + nginx/)
  $0 -k sanity                          # Run only sanity tests
  $0 automatic/                         # Run only automatic/ tests
  $0 nginx/                             # Run only nginx/ tests
  $0 automatic/test_extend_cache.py     # Run specific test file
  $0 -v --tb=long                       # Verbose output with long tracebacks
  $0 --build-only                       # Just build the module
  $0 --no-module -k sanity              # Run baseline sanity tests
  $0 --skip-build -k "not slow"         # Skip build, run non-slow tests

Test Directories:
  automatic/  Golden standard tests shared with Apache. These test core PageSpeed
              functionality (cache extension, image optimization, CSS/JS minification)
              and should pass on all server types.
  system/     Cross-server tests (IPRO ETag, 404 stats, etc.) shared across server
              types, with not_nginx/not_envoy markers for incompatible tests.
  nginx/      NGINX-specific tests for features unique to the ngx_pagespeed module.

Environment Variables:
  PAGESPEED_HOST          Server hostname (default: localhost)
  PAGESPEED_PORT          Server port (default: 8080)
  NGINX_PORT              Alias for PAGESPEED_PORT (default: 8080)
  BAZEL_CONFIG            Bazel config (default: --config=clang-libstdcxx13)
  BAZEL_JOBS              Parallel build jobs (default: auto)
EOF
}

# Parse arguments
BUILD_ONLY=false
SKIP_BUILD=false
KEEP_RUNNING=false
NO_MODULE=false
BAZEL_CONFIG="${BAZEL_CONFIG:---config=clang-libstdcxx13}"
BAZEL_JOBS="${BAZEL_JOBS:-}"
NGINX_PORT="${NGINX_PORT:-8080}"
NGINX_HTTPS_PORT="${NGINX_HTTPS_PORT:-8443}"
PYTEST_ARGS=()

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        --skip-build)
            SKIP_BUILD=true
            shift
            ;;
        --keep-running)
            KEEP_RUNNING=true
            shift
            ;;
        --no-module)
            NO_MODULE=true
            shift
            ;;
        --jobs=*)
            BAZEL_JOBS="${1#*=}"
            shift
            ;;
        --port=*)
            NGINX_PORT="${1#*=}"
            shift
            ;;
        -v|--verbose)
            PYTEST_ARGS+=("-v" "-s")
            shift
            ;;
        -k)
            PYTEST_ARGS+=("-k" "$2")
            shift 2
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            PYTEST_ARGS+=("$1")
            shift
            ;;
    esac
done

# Configuration
NGINX_BINARY="${NGINX_BINARY:-/usr/local/src/nginx/objs/nginx}"
MODULE_PATH="${MODULE_PATH:-$PROJECT_ROOT/bazel-bin/pagespeed/nginx/ngx_pagespeed_module.so}"
NGINX_CONFIG_DIR="${NGINX_CONFIG_DIR:-/tmp/nginx_pagespeed_test}"
NGINX_PID_FILE="$NGINX_CONFIG_DIR/nginx.pid"
NGINX_LOG_DIR="$NGINX_CONFIG_DIR/logs"
NGINX_CACHE_DIR="${PAGESPEED_CACHE_DIR:-/tmp/pagespeed_cache}"
NGINX_CONFIG_FILE="$NGINX_CONFIG_DIR/nginx.conf"
# Pin a fixed (small) worker count by default so the SIGHUP-reload worker-leak
# regression test (test_nginx_reload_no_worker_leak) has a deterministic
# worker_processes value to assert against. Override with NGINX_WORKER_PROCESSES
# (e.g. "auto") if a test run needs the historical autodetect behaviour.
NGINX_WORKER_PROCESSES="${NGINX_WORKER_PROCESSES:-2}"

# TLS configuration
TLS_CERT_DIR="$NGINX_CONFIG_DIR/certs"
TLS_CERT_FILE="$TLS_CERT_DIR/server.crt"
TLS_KEY_FILE="$TLS_CERT_DIR/server.key"

# Build the module
build_module() {
    log_step "Building ngx_pagespeed module..."
    cd "$PROJECT_ROOT"

    # Build jobs argument (empty = let bazel auto-detect)
    JOBS_ARG=""
    if [ -n "$BAZEL_JOBS" ]; then
        JOBS_ARG="--jobs=$BAZEL_JOBS"
        log_info "Using --jobs=$BAZEL_JOBS (adjust with --jobs=N if needed)"
    else
        log_info "Using bazel default parallelism (override with --jobs=N)"
    fi

    if ! bazel build $BAZEL_CONFIG $JOBS_ARG //pagespeed/nginx:ngx_pagespeed_module.so; then
        log_error "Failed to build ngx_pagespeed module"
        exit 1
    fi

    log_info "Module built successfully: $MODULE_PATH"
}

# Check if NGINX binary exists
check_nginx() {
    if [ ! -x "$NGINX_BINARY" ]; then
        log_error "NGINX binary not found at $NGINX_BINARY"
        log_info "Install NGINX with: apt-get install nginx"
        return 1
    fi
    log_info "Found NGINX at $NGINX_BINARY"
    return 0
}

# Check if module exists
check_module() {
    if [ "$NO_MODULE" = true ]; then
        log_info "Running without PageSpeed module (--no-module)"
        return 0
    fi

    if [ ! -f "$MODULE_PATH" ]; then
        log_error "ngx_pagespeed module not found at $MODULE_PATH"
        log_info "Build it first with:"
        log_info "  bazel build $BAZEL_CONFIG //pagespeed/nginx:ngx_pagespeed_module.so"
        return 1
    fi
    log_info "Found ngx_pagespeed module at $MODULE_PATH"
    return 0
}

# Generate self-signed TLS certificates for HTTPS testing
generate_tls_certs() {
    log_info "Generating self-signed TLS certificates..."

    mkdir -p "$TLS_CERT_DIR"

    # Check if certs already exist and are valid
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        if openssl x509 -checkend 86400 -noout -in "$TLS_CERT_FILE" 2>/dev/null; then
            log_info "TLS certificates already exist and are valid"
            return 0
        fi
        log_info "Existing certificates expired, regenerating..."
    fi

    # Generate self-signed certificate valid for 365 days
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$TLS_KEY_FILE" \
        -out "$TLS_CERT_FILE" \
        -days 365 \
        -nodes \
        -subj "/C=US/ST=Test/L=Test/O=PageSpeed/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,DNS:127.0.0.1,IP:127.0.0.1" \
        2>/dev/null

    if [ $? -eq 0 ]; then
        log_info "TLS certificates generated:"
        log_info "  Certificate: $TLS_CERT_FILE"
        log_info "  Private key: $TLS_KEY_FILE"
    else
        log_error "Failed to generate TLS certificates"
        return 1
    fi
}

# Setup directories
setup_directories() {
    log_info "Setting up directories..."
    mkdir -p "$NGINX_CONFIG_DIR"
    mkdir -p "$NGINX_LOG_DIR"
    mkdir -p "$NGINX_CACHE_DIR"
    mkdir -p "$NGINX_CONFIG_DIR/www"
    # NGINX temp directories (needed for proxy, fastcgi, etc.)
    mkdir -p "$NGINX_CONFIG_DIR/client_body_temp"
    mkdir -p "$NGINX_CONFIG_DIR/proxy_temp"
    mkdir -p "$NGINX_CONFIG_DIR/fastcgi_temp"
    # Also create /tmp/nginx directories (nginx default prefix)
    mkdir -p /tmp/nginx/logs
    mkdir -p /tmp/nginx/client_body_temp
    mkdir -p /tmp/nginx/proxy_temp
}

# Setup test content
setup_test_content() {
    log_info "Setting up test content..."

    local DOC_ROOT="$NGINX_CONFIG_DIR/www"

    # Copy test content
    if [ -d "$PROJECT_ROOT/install/mod_pagespeed_example" ]; then
        rm -rf "$DOC_ROOT/mod_pagespeed_example"
        cp -r "$PROJECT_ROOT/install/mod_pagespeed_example" "$DOC_ROOT/"
    else
        log_warn "mod_pagespeed_example not found in install/"
    fi

    if [ -d "$PROJECT_ROOT/install/mod_pagespeed_test" ]; then
        rm -rf "$DOC_ROOT/mod_pagespeed_test"
        cp -r "$PROJECT_ROOT/install/mod_pagespeed_test" "$DOC_ROOT/"
    else
        log_warn "mod_pagespeed_test not found in install/"
    fi

    if [ -d "$PROJECT_ROOT/install/do_not_modify" ]; then
        rm -rf "$DOC_ROOT/do_not_modify"
        cp -r "$PROJECT_ROOT/install/do_not_modify" "$DOC_ROOT/"
    fi

    # Create a simple index.html if nothing exists
    if [ ! -f "$DOC_ROOT/index.html" ]; then
        echo "<html><body><h1>PageSpeed NGINX Test Server</h1></body></html>" > "$DOC_ROOT/index.html"
    fi
}

# Generate NGINX configuration
generate_nginx_config() {
    log_info "Generating NGINX configuration..."

    local DOC_ROOT="$NGINX_CONFIG_DIR/www"
    local LOAD_MODULE_LINE=""

    if [ "$NO_MODULE" = false ]; then
        LOAD_MODULE_LINE="load_module $MODULE_PATH;"
    fi

    cat > "$NGINX_CONFIG_FILE" << EOF
# NGINX configuration for PageSpeed module testing
# Auto-generated by run_nginx_tests.sh
# Based on production ngx_pagespeed configuration

worker_processes $NGINX_WORKER_PROCESSES;
pid $NGINX_PID_FILE;
error_log $NGINX_LOG_DIR/error.log info;

$LOAD_MODULE_LINE

events {
    worker_connections 1024;
}

http {
    # Inline mime types (avoid dependency on /etc/nginx/mime.types)
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

    access_log $NGINX_LOG_DIR/access.log;

    # sendfile must be off for PageSpeed HTML rewriting to work.
    # With sendfile on, static file content bypasses the body filter chain.
    sendfile off;
    keepalive_timeout 65;

    # gzip configuration - PageSpeed will also handle compression
    gzip on;
    gzip_vary on;

    # Turn on gzip for all content types that should benefit from it.
    gzip_types application/ecmascript;
    gzip_types application/javascript;
    gzip_types application/json;
    gzip_types application/pdf;
    gzip_types application/postscript;
    gzip_types application/x-javascript;
    gzip_types image/svg+xml;
    gzip_types text/css;
    gzip_types text/csv;
    gzip_types text/javascript;
    gzip_types text/plain;
    gzip_types text/xml;

    gzip_http_version 1.0;

EOF

    # Add PageSpeed global configuration if module is enabled
    if [ "$NO_MODULE" = false ]; then
        cat >> "$NGINX_CONFIG_FILE" << EOF
    # PageSpeed global settings (http level)
    # Admin paths must be set at http level for global scope
    pagespeed AdminPath /pagespeed_admin;
    pagespeed GlobalAdminPath /pagespeed_global_admin;
    pagespeed StatisticsPath /pagespeed_statistics;
    pagespeed GlobalStatisticsPath /pagespeed_global_statistics;

    # Cache configuration - using disk cache
    pagespeed FileCachePath $NGINX_CACHE_DIR;
    pagespeed CacheFlushPollIntervalSec 1;

    # Enable statistics
    pagespeed Statistics on;
    pagespeed StatisticsLogging on;
    pagespeed LogDir $NGINX_LOG_DIR;

    # Message buffer for testing
    pagespeed MessageBufferSize 100000;

    # Disable Critical Images Beacon so that lazyload_images,
    # inline_preview_images, and other beacon-dependent filters work
    # without requiring real browser beacon data (matches Apache/Envoy).
    pagespeed CriticalImagesBeaconEnabled false;

    # Configure canonicalize_javascript_libraries filter (matches Apache)
    pagespeed Library 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js;

EOF
    fi

    cat >> "$NGINX_CONFIG_FILE" << EOF
    server {
        listen $NGINX_PORT;
        server_name localhost;

        root $DOC_ROOT;
        index index.html;

EOF

    # Add PageSpeed server-level config if module is enabled
    if [ "$NO_MODULE" = false ]; then
        cat >> "$NGINX_CONFIG_FILE" << EOF
        # Enable PageSpeed for this server
        pagespeed on;
        pagespeed RewriteLevel CoreFilters;

        # Ensure requests for pagespeed optimized resources go to the pagespeed
        # handler and no extraneous headers get set.
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
            # Disable nginx gzip for .pagespeed. resources. These resources
            # have known content length set by PageSpeed. Nginx's gzip module
            # would strip Content-Length and use chunked encoding instead.
            gzip off;
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon\$" { }

        # Admin and statistics endpoints
        location ~ "^/pagespeed_admin" { }
        location ~ "^/pagespeed_global_admin" { }
        location /pagespeed_statistics { }
        location /pagespeed_global_statistics { }

        # Serve files in no_cache/ with Cache-Control: no-cache
        # Matches Apache's debug.conf.template configuration
        location ~ /no_cache/ {
            add_header Cache-Control "no-cache" always;
        }

EOF
    fi

    cat >> "$NGINX_CONFIG_FILE" << EOF
        # Serve static files
        location / {
            try_files \$uri \$uri/ =404;
        }
    }

EOF

    # Add HTTPS server block if TLS certs are available
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        cat >> "$NGINX_CONFIG_FILE" << EOF
    server {
        listen $NGINX_HTTPS_PORT ssl;
        server_name localhost;

        ssl_certificate $TLS_CERT_FILE;
        ssl_certificate_key $TLS_KEY_FILE;

        root $DOC_ROOT;
        index index.html;

EOF

        # Add PageSpeed server-level config for HTTPS if module is enabled
        if [ "$NO_MODULE" = false ]; then
            cat >> "$NGINX_CONFIG_FILE" << EOF
        # Enable PageSpeed for this server
        pagespeed on;
        pagespeed RewriteLevel CoreFilters;

        # Allow fetching HTTPS resources with self-signed certificates.
        # Required for CSS combination over HTTPS in test environments.
        pagespeed FetchHttps enable,allow_self_signed;

        # Ensure requests for pagespeed optimized resources go to the pagespeed
        # handler and no extraneous headers get set.
        location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+" {
            add_header "" "";
            gzip off;
        }
        location ~ "^/pagespeed_static/" { }
        location ~ "^/ngx_pagespeed_beacon\$" { }

        # Admin and statistics endpoints
        location ~ "^/pagespeed_admin" { }
        location ~ "^/pagespeed_global_admin" { }
        location /pagespeed_statistics { }
        location /pagespeed_global_statistics { }

        # Serve files in no_cache/ with Cache-Control: no-cache
        location ~ /no_cache/ {
            add_header Cache-Control "no-cache" always;
        }

EOF
        fi

        cat >> "$NGINX_CONFIG_FILE" << EOF
        # Serve static files
        location / {
            try_files \$uri \$uri/ =404;
        }
    }

EOF
    fi

    # Close the http block
    cat >> "$NGINX_CONFIG_FILE" << EOF
}
EOF

    log_info "Configuration written to $NGINX_CONFIG_FILE"
}

# Start NGINX
start_nginx() {
    log_info "Starting NGINX..."

    # Stop if running
    stop_nginx_process

    # Test configuration
    # Use -p to set prefix (for temp dirs) and -e for initial error log
    if ! "$NGINX_BINARY" -t -p "$NGINX_CONFIG_DIR" -e "$NGINX_LOG_DIR/error.log" -c "$NGINX_CONFIG_FILE" 2>&1; then
        log_error "NGINX configuration test failed"
        return 1
    fi

    # Start NGINX
    if ! "$NGINX_BINARY" -p "$NGINX_CONFIG_DIR" -e "$NGINX_LOG_DIR/error.log" -c "$NGINX_CONFIG_FILE" 2>&1; then
        log_error "Failed to start NGINX"
        log_info "Checking error log..."
        tail -30 "$NGINX_LOG_DIR/error.log" 2>/dev/null || true
        return 1
    fi

    # Wait for NGINX to start
    log_info "Waiting for NGINX to be ready..."
    local max_wait=30
    local waited=0
    while [ $waited -lt $max_wait ]; do
        sleep 1
        waited=$((waited + 1))

        # Check if NGINX is responding
        if curl -s --max-time 2 "http://localhost:$NGINX_PORT/" > /dev/null 2>&1; then
            log_info "NGINX is responding after ${waited}s"
            break
        fi
    done

    if [ $waited -ge $max_wait ]; then
        log_warn "NGINX may not be fully ready after ${max_wait}s"
    fi

    # Check for PageSpeed header if module is loaded
    if [ "$NO_MODULE" = false ]; then
        if curl -sI "http://localhost:$NGINX_PORT/mod_pagespeed_example/" 2>/dev/null | grep -qi "X-Page-Speed"; then
            log_info "PageSpeed module is working!"
        else
            log_warn "PageSpeed header not found (module may need warm-up)"
        fi
    fi

    log_info "NGINX is running:"
    log_info "  HTTP:  http://localhost:$NGINX_PORT"
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        log_info "  HTTPS: https://localhost:$NGINX_HTTPS_PORT"
    fi
    if [ "$NO_MODULE" = false ]; then
        log_info "  Admin: http://localhost:$NGINX_PORT/pagespeed_admin"
        log_info "  Stats: http://localhost:$NGINX_PORT/pagespeed_statistics"
    fi
    log_info "  Config: $NGINX_CONFIG_FILE"
    log_info "  Logs: $NGINX_LOG_DIR"
}

# Stop NGINX process
stop_nginx_process() {
    if [ -f "$NGINX_PID_FILE" ]; then
        local pid=$(cat "$NGINX_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            log_info "Stopping NGINX (PID: $pid)..."
            kill -QUIT "$pid" 2>/dev/null || true
            sleep 1
            # Force kill if still running
            if kill -0 "$pid" 2>/dev/null; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        fi
        rm -f "$NGINX_PID_FILE"
    fi

    # Also kill any remaining nginx processes for our config (fallback)
    local remaining_pids
    remaining_pids=$(pgrep -f "nginx.*$NGINX_CONFIG_FILE" 2>/dev/null || true)
    if [ -n "$remaining_pids" ]; then
        log_info "Cleaning up remaining NGINX processes..."
        pkill -f "nginx.*$NGINX_CONFIG_FILE" 2>/dev/null || true
        sleep 1
    fi
}

# Stop NGINX
stop_nginx() {
    stop_nginx_process
    log_info "NGINX stopped"
}

# Setup NGINX
setup_nginx() {
    log_step "Setting up NGINX with PageSpeed..."
    cd "$PROJECT_ROOT"

    check_nginx || return 1
    check_module || return 1
    setup_directories
    setup_test_content
    generate_tls_certs || return 1
    generate_nginx_config
    start_nginx || return 1

    log_info "NGINX is running with PageSpeed"
}

# Run Python tests
run_tests() {
    log_step "Running Python system tests against NGINX..."
    cd "$SCRIPT_DIR"

    # Set environment variables for NGINX testing
    export PAGESPEED_HOST="${PAGESPEED_HOST:-localhost}"
    export PAGESPEED_PORT="${NGINX_PORT:-${PAGESPEED_PORT:-8080}}"
    export PAGESPEED_SERVER_TYPE="nginx"
    export PAGESPEED_STATS_PATH="${PAGESPEED_STATS_PATH:-/pagespeed_statistics}"
    export PAGESPEED_ADMIN_PATH="${PAGESPEED_ADMIN_PATH:-/pagespeed_admin}"
    export PAGESPEED_EXAMPLE_ROOT="${PAGESPEED_EXAMPLE_ROOT:-/mod_pagespeed_example}"
    export PAGESPEED_TEST_ROOT="${PAGESPEED_TEST_ROOT:-/mod_pagespeed_test}"
    export PAGESPEED_CACHE_DIR="${NGINX_CACHE_DIR}"

    # HTTPS configuration for TLS tests
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        export PAGESPEED_HTTPS_HOST="${PAGESPEED_HTTPS_HOST:-localhost}"
        export PAGESPEED_HTTPS_PORT="${NGINX_HTTPS_PORT:-${PAGESPEED_HTTPS_PORT:-8443}}"
    fi

    if [ "$NO_MODULE" = true ]; then
        export PAGESPEED_STATS_ENABLED=0
    else
        export PAGESPEED_STATS_ENABLED=1
    fi

    # Nginx control paths for the SIGHUP-reload worker-leak regression test
    # (test_nginx_reload_no_worker_leak). The test sends SIGHUP to the master
    # (read from $PAGESPEED_NGINX_PID_FILE) and asserts old workers exit.
    export PAGESPEED_NGINX_BINARY="${NGINX_BINARY}"
    export PAGESPEED_NGINX_CONFIG_FILE="${NGINX_CONFIG_FILE}"
    export PAGESPEED_NGINX_PID_FILE="${NGINX_PID_FILE}"
    export PAGESPEED_NGINX_WORKER_PROCESSES="${NGINX_WORKER_PROCESSES}"

    log_info "Test server: http://$PAGESPEED_HOST:$PAGESPEED_PORT (NGINX)"
    if [ -n "${PAGESPEED_HTTPS_HOST:-}" ]; then
        log_info "HTTPS server: https://$PAGESPEED_HTTPS_HOST:$PAGESPEED_HTTPS_PORT"
    fi
    if [ "$NO_MODULE" = true ]; then
        log_info "Mode: Baseline (no PageSpeed module)"
    else
        log_info "Mode: PageSpeed module enabled"
    fi

    # Default to running automatic/, system/, and nginx/ test directories if they exist:
    # - automatic/ contains the golden standard tests shared with Apache. These test
    #   core PageSpeed functionality (cache extension, image optimization, CSS/JS
    #   minification, etc.) and should pass on all server types.
    # - system/ contains cross-server tests (IPRO ETag, 404 stats, etc.) that are
    #   shared across server types (with not_nginx/not_envoy markers as needed).
    # - nginx/ contains NGINX-specific tests for features unique to the NGINX module.
    if [ ${#PYTEST_ARGS[@]} -eq 0 ]; then
        PYTEST_ARGS=("-v")
        if [ -d "$SCRIPT_DIR/automatic" ]; then
            PYTEST_ARGS+=("automatic/")
        fi
        if [ -d "$SCRIPT_DIR/system" ]; then
            PYTEST_ARGS+=("system/")
        fi
        if [ -d "$SCRIPT_DIR/nginx" ]; then
            PYTEST_ARGS+=("nginx/")
        fi
    fi

    # Install pytest if needed
    if ! python3 -c "import pytest" 2>/dev/null; then
        log_info "Installing pytest..."
        pip3 install --user pytest requests
    fi

    # Run pytest
    log_info "Running: python3 -m pytest ${PYTEST_ARGS[*]}"
    python3 -m pytest "${PYTEST_ARGS[@]}"
}

# Cleanup on exit
cleanup() {
    if [ "$KEEP_RUNNING" = false ]; then
        stop_nginx
    else
        log_info "NGINX left running (--keep-running specified)"
        log_info "Stop it manually with: nginx -s stop -c $NGINX_CONFIG_FILE"
    fi
}

# Main
main() {
    cd "$PROJECT_ROOT"

    echo ""
    echo "========================================================================"
    echo " PageSpeed NGINX Integration Tests"
    echo "========================================================================"
    echo ""

    if [ "$NO_MODULE" = true ]; then
        log_info "Mode: Baseline (no PageSpeed module)"
    else
        log_info "Mode: PageSpeed module enabled"
    fi
    echo ""

    # Build module (unless --no-module or --skip-build)
    if [ "$SKIP_BUILD" = false ] && [ "$NO_MODULE" = false ]; then
        build_module
    elif [ "$SKIP_BUILD" = true ]; then
        log_info "Skipping build (--skip-build specified)"
    elif [ "$NO_MODULE" = true ]; then
        log_info "Skipping build (--no-module specified)"
    fi

    # Exit early if build-only
    if [ "$BUILD_ONLY" = true ]; then
        log_info "Build complete (--build-only specified)"
        exit 0
    fi

    # Setup cleanup trap
    trap cleanup EXIT

    # Setup NGINX
    setup_nginx

    # Run tests
    run_tests

    log_info "NGINX system tests completed!"
}

main
