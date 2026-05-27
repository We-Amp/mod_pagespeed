#!/bin/bash
#
# Builds nginx from source with the PageSpeed module compiled in.
#
# Usage:
#   ./scripts/build_nginx_with_pagespeed.sh [options]
#
# Options:
#   --nginx-version=VERSION  NGINX version to download (default: 1.30.1)
#   --nginx-src=PATH         Use existing nginx source at PATH
#   --prefix=PATH            Install prefix (default: /tmp/nginx-pagespeed)
#   --build-dir=PATH         Build directory (default: /tmp/nginx-build)
#   --skip-bazel-build       Skip building the PageSpeed archive
#   --skip-download          Skip downloading nginx (use existing source)
#   --clean                  Clean build directory before starting
#   --install                Run 'make install' after building
#   --jobs=N                 Number of parallel jobs (default: auto)
#   --help                   Show this help message
#
# This script:
# 1. Downloads nginx source (or uses existing source)
# 2. Builds the PageSpeed static archive with Bazel
# 3. Configures nginx with --add-module pointing to our module
# 4. Builds nginx with PageSpeed compiled in
#
# The resulting nginx binary will have PageSpeed built-in and can be used
# for testing without dynamic module loading.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_step() { echo -e "${BLUE}[STEP]${NC} $1"; }

show_help() {
    head -30 "$0" | tail -27 | sed 's/^#//'
}

# Default values
NGINX_VERSION="1.30.1"
NGINX_SRC=""
PREFIX="/tmp/nginx-pagespeed"
BUILD_DIR="/tmp/nginx-build"
SKIP_BAZEL_BUILD=false
SKIP_DOWNLOAD=false
CLEAN=false
DO_INSTALL=false
JOBS=""
BAZEL_CONFIG="${BAZEL_CONFIG:---config=clang-libstdcxx13}"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --nginx-version=*)
            NGINX_VERSION="${1#*=}"
            shift
            ;;
        --nginx-src=*)
            NGINX_SRC="${1#*=}"
            SKIP_DOWNLOAD=true
            shift
            ;;
        --prefix=*)
            PREFIX="${1#*=}"
            shift
            ;;
        --build-dir=*)
            BUILD_DIR="${1#*=}"
            shift
            ;;
        --skip-bazel-build)
            SKIP_BAZEL_BUILD=true
            shift
            ;;
        --skip-download)
            SKIP_DOWNLOAD=true
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        --install)
            DO_INSTALL=true
            shift
            ;;
        --jobs=*)
            JOBS="${1#*=}"
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Resolve nginx source path
if [ -z "$NGINX_SRC" ]; then
    NGINX_SRC="$BUILD_DIR/nginx-$NGINX_VERSION"
fi

# Setup build directory
setup_build_dir() {
    log_step "Setting up build directory..."

    if [ "$CLEAN" = true ] && [ -d "$BUILD_DIR" ]; then
        log_info "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    mkdir -p "$BUILD_DIR"
    log_info "Build directory: $BUILD_DIR"
}

# Download nginx source
download_nginx() {
    if [ "$SKIP_DOWNLOAD" = true ]; then
        if [ -d "$NGINX_SRC/src" ]; then
            log_info "Using existing nginx source at $NGINX_SRC"
            return 0
        else
            log_error "nginx source not found at $NGINX_SRC"
            exit 1
        fi
    fi

    log_step "Downloading nginx $NGINX_VERSION..."

    if [ -d "$NGINX_SRC/src" ]; then
        log_info "nginx source already exists at $NGINX_SRC"
        return 0
    fi

    local NGINX_URL="https://nginx.org/download/nginx-$NGINX_VERSION.tar.gz"
    local NGINX_TAR="$BUILD_DIR/nginx-$NGINX_VERSION.tar.gz"

    log_info "Downloading from $NGINX_URL"
    curl -fsSL "$NGINX_URL" -o "$NGINX_TAR"

    log_info "Extracting..."
    tar -xzf "$NGINX_TAR" -C "$BUILD_DIR"
    rm -f "$NGINX_TAR"

    log_info "nginx source extracted to $NGINX_SRC"
}

# Build PageSpeed archive with Bazel
build_pagespeed_archive() {
    if [ "$SKIP_BAZEL_BUILD" = true ]; then
        log_info "Skipping Bazel build (--skip-bazel-build)"
        return 0
    fi

    log_step "Building PageSpeed archive with Bazel..."
    cd "$PROJECT_ROOT"

    local JOBS_ARG=""
    if [ -n "$JOBS" ]; then
        JOBS_ARG="--jobs=$JOBS"
    fi

    # Build the shared library (not the static archive, as the shared library
    # approach is simpler for dynamic loading)
    if ! bazel build $BAZEL_CONFIG $JOBS_ARG //pagespeed/nginx:ngx_pagespeed_module.so; then
        log_error "Failed to build PageSpeed module"
        exit 1
    fi

    log_info "PageSpeed module built successfully"
}

# Configure nginx
configure_nginx() {
    log_step "Configuring nginx..."
    cd "$NGINX_SRC"

    # Set MOD_PAGESPEED_DIR for the config file
    export MOD_PAGESPEED_DIR="$PROJECT_ROOT"

    # Basic configure options
    local CONFIGURE_OPTS=(
        "--prefix=$PREFIX"
        "--with-http_ssl_module"
        "--with-http_v2_module"
        "--with-http_realip_module"
        "--with-http_stub_status_module"
        "--with-threads"
        "--with-file-aio"
        "--with-cc-opt=-O2"
        "--with-ld-opt=-Wl,-rpath,$PREFIX/lib"
    )

    # Check if PCRE2 is available (nginx 1.21.5+)
    if pkg-config --exists libpcre2-8 2>/dev/null; then
        log_info "Using PCRE2"
        # nginx auto-detects PCRE2
    elif pkg-config --exists libpcre 2>/dev/null; then
        log_info "Using PCRE"
    else
        log_warn "PCRE not found - nginx may fail to build"
    fi

    # Add the PageSpeed module
    CONFIGURE_OPTS+=("--add-module=$PROJECT_ROOT/pagespeed/nginx")

    log_info "Configure options: ${CONFIGURE_OPTS[*]}"

    ./configure "${CONFIGURE_OPTS[@]}"
}

# Build nginx
build_nginx() {
    log_step "Building nginx..."
    cd "$NGINX_SRC"

    local JOBS_ARG=""
    if [ -n "$JOBS" ]; then
        JOBS_ARG="-j$JOBS"
    fi

    make $JOBS_ARG

    log_info "nginx built successfully"
    log_info "Binary: $NGINX_SRC/objs/nginx"
}

# Install nginx
install_nginx() {
    if [ "$DO_INSTALL" != true ]; then
        log_info "Skipping install (use --install to install)"
        return 0
    fi

    log_step "Installing nginx to $PREFIX..."
    cd "$NGINX_SRC"

    make install

    log_info "nginx installed to $PREFIX"
    log_info "Binary: $PREFIX/sbin/nginx"
}

# Print summary
print_summary() {
    echo ""
    echo "========================================================================"
    echo " Build Complete"
    echo "========================================================================"
    echo ""
    echo "nginx binary: $NGINX_SRC/objs/nginx"
    if [ "$DO_INSTALL" = true ]; then
        echo "Installed to: $PREFIX"
        echo ""
        echo "To test:"
        echo "  $PREFIX/sbin/nginx -V"
        echo "  $PREFIX/sbin/nginx -t -c /path/to/nginx.conf"
    else
        echo ""
        echo "To install, run:"
        echo "  cd $NGINX_SRC && make install"
        echo ""
        echo "Or run this script with --install"
    fi
    echo ""
    echo "To run tests with this nginx:"
    echo "  NGINX_BINARY=$NGINX_SRC/objs/nginx ./test/system/run_nginx_tests.sh"
    echo ""
}

# Main
main() {
    echo ""
    echo "========================================================================"
    echo " Building nginx with PageSpeed"
    echo "========================================================================"
    echo ""
    echo "nginx version: $NGINX_VERSION"
    echo "Project root:  $PROJECT_ROOT"
    echo "Build dir:     $BUILD_DIR"
    echo "Install prefix: $PREFIX"
    echo ""

    setup_build_dir
    download_nginx
    build_pagespeed_archive
    configure_nginx
    build_nginx
    install_nginx
    print_summary
}

main
