#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Script to run mod_pagespeed system tests in Docker.
#
# Prerequisites:
#   - Docker container running: docker compose up -d
#   - Apache module built: docker compose exec dev bazel build //:libmod_pagespeed.so
#
# Usage:
#   docker compose exec dev bash scripts/run_system_tests.sh
#
# Or to run a specific test:
#   docker compose exec dev bash scripts/run_system_tests.sh extend_cache

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

echo_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

echo_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if we're in Docker
if [ ! -f /.dockerenv ] && [ ! -f /run/.containerenv ]; then
    echo_error "This script should be run inside the Docker container."
    echo "Usage: docker compose exec dev bash scripts/run_system_tests.sh"
    exit 1
fi

# Install Apache if needed
install_apache() {
    if ! command -v apache2 &> /dev/null; then
        echo_info "Installing Apache..."
        sudo apt-get update -qq
        sudo apt-get install -y -qq apache2 libapache2-mod-fcgid
    fi
}

# Build the module if needed
build_module() {
    if [ ! -f "$PROJECT_ROOT/bazel-bin/libmod_pagespeed.so" ]; then
        echo_info "Building mod_pagespeed module..."
        cd "$PROJECT_ROOT"
        bazel build //:libmod_pagespeed.so
    fi
}

# Setup Apache with mod_pagespeed
setup_apache() {
    echo_info "Setting up Apache with mod_pagespeed..."

    # Copy module
    sudo cp "$PROJECT_ROOT/bazel-bin/libmod_pagespeed.so" /usr/lib/apache2/modules/mod_pagespeed.so

    # Create directories
    sudo mkdir -p /var/cache/mod_pagespeed /var/log/pagespeed /var/www/html
    sudo chown -R www-data:www-data /var/cache/mod_pagespeed /var/log/pagespeed

    # Copy test content
    sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_example" /var/www/html/
    sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_test" /var/www/html/ 2>/dev/null || true
    sudo cp -r "$PROJECT_ROOT/install/do_not_modify" /var/www/html/ 2>/dev/null || true

    # Create module load file
    sudo tee /etc/apache2/mods-available/pagespeed.load > /dev/null << 'EOF'
LoadModule pagespeed_module /usr/lib/apache2/modules/mod_pagespeed.so
EOF

    # Create module config
    sudo tee /etc/apache2/mods-available/pagespeed.conf > /dev/null << 'EOF'
<IfModule pagespeed_module>
    ModPagespeed on

    # Direct Apache to send all HTML output to the mod_pagespeed output handler
    AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"

    # Enable CoreFilters
    ModPagespeedRewriteLevel CoreFilters

    # Enable statistics
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on

    # Enable HTTPS fetching
    ModPagespeedFetchHttps enable

    <Location /mod_pagespeed_statistics>
        Require local
        SetHandler mod_pagespeed_statistics
    </Location>

    <Location /mod_pagespeed_message>
        Require local
        SetHandler mod_pagespeed_message
    </Location>

    <Location /pagespeed_admin>
        Require local
        SetHandler pagespeed_admin
    </Location>
</IfModule>
EOF

    # Enable required modules
    sudo a2enmod rewrite headers deflate proxy proxy_http expires pagespeed 2>/dev/null || true

    # Set ServerName
    echo "ServerName localhost" | sudo tee /etc/apache2/conf-available/servername.conf > /dev/null
    sudo a2enconf servername 2>/dev/null || true

    # Test config
    if ! sudo apache2ctl configtest 2>&1 | grep -q "Syntax OK"; then
        echo_error "Apache configuration test failed"
        sudo apache2ctl configtest
        exit 1
    fi
}

# Start Apache
start_apache() {
    echo_info "Starting Apache..."
    sudo apache2ctl stop 2>/dev/null || true
    sleep 1
    sudo apache2ctl start
    sleep 2

    # Verify it's running
    if ! curl -s -I http://localhost/ > /dev/null 2>&1; then
        echo_error "Apache failed to start"
        exit 1
    fi

    # Verify mod_pagespeed is working
    if ! curl -sI http://localhost/mod_pagespeed_example/ | grep -q "X-Mod-Pagespeed"; then
        echo_error "mod_pagespeed is not responding"
        exit 1
    fi

    echo_info "Apache with mod_pagespeed is running"
}

# Run system tests
run_tests() {
    echo_info "Running system tests..."

    cd "$PROJECT_ROOT"

    # Set up test environment
    export SERVER_NAME=apache
    export TEMPDIR="/tmp/mod_pagespeed_test.$$"
    mkdir -p "$TEMPDIR"
    export CONTINUE_AFTER_FAILURE=true

    # Some tests are expected to fail in Docker environment
    export PAGESPEED_EXPECTED_FAILURES="~gce_public_cache~"

    # Run specific test or all tests
    if [ -n "$1" ]; then
        export TEST_TO_RUN="$1"
        echo_info "Running test: $1"
    fi

    # Run the tests
    bash pagespeed/automatic/system_test.sh localhost
}

# Main
main() {
    install_apache
    build_module
    setup_apache
    start_apache
    run_tests "$1"
}

main "$@"
