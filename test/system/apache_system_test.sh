#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

#
# Bazel wrapper script for running Apache mod_pagespeed system tests.
#
# This script is designed to be run as a Bazel sh_test target.
# It requires sudo access and network capabilities.
#
# Usage (via Bazel):
#   bazel test //test/system:apache_system_test --test_output=streamed
#
# Or run directly:
#   ./test/system/apache_system_test.sh

set -e

# Find the project root (where WORKSPACE is)
find_workspace_root() {
    local dir="$1"
    while [ "$dir" != "/" ]; do
        if [ -f "$dir/WORKSPACE" ] || [ -f "$dir/WORKSPACE.bazel" ]; then
            echo "$dir"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    return 1
}

# Bazel sets TEST_SRCDIR and TEST_WORKSPACE for tests
if [ -n "$TEST_SRCDIR" ] && [ -n "$TEST_WORKSPACE" ]; then
    RUNFILES_DIR="$TEST_SRCDIR/$TEST_WORKSPACE"
    PROJECT_ROOT="$RUNFILES_DIR"
else
    # Running directly, not via Bazel
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    PROJECT_ROOT="$(find_workspace_root "$SCRIPT_DIR")"
fi

if [ -z "$PROJECT_ROOT" ] || [ ! -d "$PROJECT_ROOT" ]; then
    echo "ERROR: Could not find project root"
    exit 1
fi

# Module path - check both bazel-bin locations
MODULE_PATH=""
for path in \
    "$PROJECT_ROOT/bazel-bin/libmod_pagespeed.so" \
    "$PROJECT_ROOT/../../../bazel-bin/libmod_pagespeed.so" \
    "/src/bazel-bin/libmod_pagespeed.so"; do
    if [ -f "$path" ]; then
        MODULE_PATH="$path"
        break
    fi
done

if [ -z "$MODULE_PATH" ]; then
    echo "ERROR: libmod_pagespeed.so not found. Build it first with:"
    echo "  bazel build //:libmod_pagespeed.so"
    exit 1
fi

echo "Using module: $MODULE_PATH"
echo "Project root: $PROJECT_ROOT"

# Check if Apache is installed
if ! command -v apache2 &> /dev/null && ! command -v httpd &> /dev/null; then
    echo "ERROR: Apache is not installed"
    echo "Install with: sudo apt-get install apache2"
    exit 1
fi

# Determine Apache paths based on distro
if [ -d /etc/apache2 ]; then
    # Debian/Ubuntu
    APACHE_CONF_DIR=/etc/apache2
    APACHE_MODULES_DIR=/usr/lib/apache2/modules
    APACHE_CTL="apache2ctl"
    MODS_AVAILABLE="$APACHE_CONF_DIR/mods-available"
    MODS_ENABLED="$APACHE_CONF_DIR/mods-enabled"
elif [ -d /etc/httpd ]; then
    # RHEL/CentOS
    APACHE_CONF_DIR=/etc/httpd
    APACHE_MODULES_DIR=/usr/lib64/httpd/modules
    APACHE_CTL="apachectl"
    MODS_AVAILABLE="$APACHE_CONF_DIR/conf.modules.d"
    MODS_ENABLED="$APACHE_CONF_DIR/conf.modules.d"
else
    echo "ERROR: Unsupported Apache configuration"
    exit 1
fi

# Create temp directory for test
TEST_TMPDIR="${TMPDIR:-/tmp}/pagespeed_system_test.$$"
mkdir -p "$TEST_TMPDIR"

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    sudo $APACHE_CTL stop 2>/dev/null || true
    rm -rf "$TEST_TMPDIR"
}
trap cleanup EXIT

# Setup Apache with mod_pagespeed
setup_apache() {
    echo "Setting up Apache with mod_pagespeed..."

    # Create directories
    sudo mkdir -p /var/cache/mod_pagespeed /var/log/pagespeed
    sudo chown -R www-data:www-data /var/cache/mod_pagespeed /var/log/pagespeed 2>/dev/null || \
    sudo chown -R apache:apache /var/cache/mod_pagespeed /var/log/pagespeed 2>/dev/null || true

    # Copy module
    sudo cp "$MODULE_PATH" "$APACHE_MODULES_DIR/mod_pagespeed.so"

    # Setup test content
    sudo mkdir -p /var/www/html
    sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_example" /var/www/html/
    sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_test" /var/www/html/ 2>/dev/null || true
    sudo cp -r "$PROJECT_ROOT/install/do_not_modify" /var/www/html/ 2>/dev/null || true

    # Create module configuration
    if [ -d "$MODS_AVAILABLE" ]; then
        sudo tee "$MODS_AVAILABLE/pagespeed.load" > /dev/null << EOF
LoadModule pagespeed_module $APACHE_MODULES_DIR/mod_pagespeed.so
EOF

        sudo tee "$MODS_AVAILABLE/pagespeed.conf" > /dev/null << 'EOF'
<IfModule pagespeed_module>
    ModPagespeed on
    AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"
    ModPagespeedRewriteLevel CoreFilters
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on
    ModPagespeedFetchHttps enable

    # Zero-copy aliased serving under the STOCK filter chain:
    # mod_reqtimeout stays enabled and the AddOutputFilterByType harness
    # above stays in place -- the zerocopy_serve system test asserts the
    # aliased path fires anyway.
    ModPagespeedCycloneZeroCopy on
    ModPagespeedCycloneZeroCopyServe on

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

        # Enable modules (Debian/Ubuntu style).  reqtimeout is explicit so
        # the zerocopy_serve test always exercises the stock Debian chain
        # even on images where the default-enabled set was trimmed.
        sudo a2enmod rewrite headers deflate proxy proxy_http expires reqtimeout 2>/dev/null || true
        sudo a2enmod pagespeed 2>/dev/null || true
    fi

    # Set ServerName
    echo "ServerName localhost" | sudo tee "$APACHE_CONF_DIR/conf-available/servername.conf" > /dev/null 2>/dev/null || true
    sudo a2enconf servername 2>/dev/null || true

    # Verify configuration
    if ! sudo $APACHE_CTL configtest 2>&1 | grep -q "Syntax OK"; then
        echo "ERROR: Apache configuration test failed"
        sudo $APACHE_CTL configtest
        return 1
    fi
}

# Start Apache
start_apache() {
    echo "Starting Apache..."
    sudo $APACHE_CTL stop 2>/dev/null || true
    sleep 1
    sudo $APACHE_CTL start
    sleep 2

    # Verify Apache is running
    if ! curl -s -I http://localhost/ > /dev/null 2>&1; then
        echo "ERROR: Apache failed to start"
        sudo tail -20 /var/log/apache2/error.log 2>/dev/null || \
        sudo tail -20 /var/log/httpd/error_log 2>/dev/null || true
        return 1
    fi

    # Verify mod_pagespeed is working
    if ! curl -sI http://localhost/mod_pagespeed_example/ | grep -q "X-Mod-Pagespeed"; then
        echo "ERROR: mod_pagespeed is not responding correctly"
        return 1
    fi

    echo "Apache with mod_pagespeed is running"
}

# Run system tests
run_tests() {
    echo "Running system tests..."

    cd "$PROJECT_ROOT"

    # Set up test environment variables required by pagespeed/apache/system_test.sh
    export SERVER_NAME=apache
    export TEMPDIR="$TEST_TMPDIR"
    export CONTINUE_AFTER_FAILURE=true

    # Apache-specific environment variables
    if [ -d /etc/apache2 ]; then
        # Debian/Ubuntu
        export APACHE_DEBUG_PAGESPEED_CONF="$MODS_AVAILABLE/pagespeed.conf"
        export APACHE_LOG=/var/log/apache2/error.log
        export APACHE_DOC_ROOT=/var/www/html
    else
        # RHEL/CentOS
        export APACHE_DEBUG_PAGESPEED_CONF=/etc/httpd/conf.d/pagespeed.conf
        export APACHE_LOG=/var/log/httpd/error_log
        export APACHE_DOC_ROOT=/var/www/html
    fi

    # SECONDARY_HOSTNAME is set by apache/system_test.sh but only when CACHE_FLUSH_TEST=on.
    # For regular tests, ensure it's defined (as empty) to avoid 'set -u' errors.
    export SECONDARY_HOSTNAME="${SECONDARY_HOSTNAME:-}"

    # Some tests are expected to fail in certain environments
    export PAGESPEED_EXPECTED_FAILURES="~gce_public_cache~"

    # Run specific test if specified
    if [ -n "${1:-}" ]; then
        export TEST_TO_RUN="$1"
        echo "Running test: $1"
    fi

    # Run the Apache system tests (which calls automatic/system_test.sh internally)
    bash "$PROJECT_ROOT/pagespeed/apache/system_test.sh" localhost
}

# Main
main() {
    setup_apache
    start_apache
    run_tests "$@"
}

main "$@"
