#!/bin/bash
#
# Sets up Apache with mod_pagespeed for running Python system tests.
#
# Usage (inside Docker container):
#   ./test/system/setup_apache_test.sh [start|stop|status]
#
# After starting, run Python tests with:
#   python3 -m pytest test/system/automatic/ -v \
#       --test-env PAGESPEED_HOST=localhost --test-env PAGESPEED_PORT=80
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Configuration
APACHE_MODULES_DIR=/usr/lib/apache2/modules
APACHE_CONF_DIR=/etc/apache2
MODULE_PATH="${MODULE_PATH:-$PROJECT_ROOT/bazel-bin/libmod_pagespeed.so}"
APACHE_HTTPS_PORT="${APACHE_HTTPS_PORT:-8443}"
APACHE_SECONDARY_PORT="${APACHE_SECONDARY_PORT:-8081}"

# TLS configuration
TLS_CERT_DIR="/tmp/apache_pagespeed_test/certs"
TLS_CERT_FILE="$TLS_CERT_DIR/server.crt"
TLS_KEY_FILE="$TLS_CERT_DIR/server.key"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

check_module() {
    if [ ! -f "$MODULE_PATH" ]; then
        log_error "libmod_pagespeed.so not found at $MODULE_PATH"
        log_info "Build it first with: bazel build //:libmod_pagespeed.so"
        return 1
    fi
    return 0
}

setup_directories() {
    log_info "Setting up directories (clearing old caches)..."
    sudo rm -rf /var/cache/mod_pagespeed /var/cache/mod_pagespeed_secondary
    sudo mkdir -p /var/cache/mod_pagespeed /var/cache/mod_pagespeed_secondary /var/log/pagespeed
    sudo chown -R www-data:www-data /var/cache/mod_pagespeed /var/cache/mod_pagespeed_secondary /var/log/pagespeed
    sudo chmod 755 /var/cache/mod_pagespeed /var/cache/mod_pagespeed_secondary /var/log/pagespeed

    # Pre-seed test license file if LICENSE_TOKEN is set (from --testkey build).
    # Path: /var/cache/pagespeed.license (parent of FileCachePath).
    if [ -n "${LICENSE_TOKEN:-}" ]; then
        log_info "Writing test license to /var/cache/pagespeed.license"
        echo "$LICENSE_TOKEN" | sudo tee /var/cache/pagespeed.license > /dev/null
        sudo chown www-data:www-data /var/cache/pagespeed.license
    fi
}

install_module() {
    log_info "Installing mod_pagespeed module..."
    sudo cp "$MODULE_PATH" "$APACHE_MODULES_DIR/mod_pagespeed.so"
    sudo chmod 644 "$APACHE_MODULES_DIR/mod_pagespeed.so"
}

setup_test_content() {
    log_info "Setting up test content..."

    # Create document root if needed
    sudo mkdir -p /var/www/html

    # Copy test content
    if [ -d "$PROJECT_ROOT/install/mod_pagespeed_example" ]; then
        sudo rm -rf /var/www/html/mod_pagespeed_example
        sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_example" /var/www/html/
    fi

    if [ -d "$PROJECT_ROOT/install/mod_pagespeed_test" ]; then
        sudo rm -rf /var/www/html/mod_pagespeed_test
        sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_test" /var/www/html/
    fi

    if [ -d "$PROJECT_ROOT/install/do_not_modify" ]; then
        sudo rm -rf /var/www/html/do_not_modify
        sudo cp -r "$PROJECT_ROOT/install/do_not_modify" /var/www/html/
    fi

    # Set permissions
    sudo chown -R www-data:www-data /var/www/html
}

configure_apache() {
    log_info "Configuring Apache..."

    # Create pagespeed.load
    sudo tee "$APACHE_CONF_DIR/mods-available/pagespeed.load" > /dev/null << EOF
LoadModule pagespeed_module $APACHE_MODULES_DIR/mod_pagespeed.so
EOF

    # Create pagespeed.conf - matching bash test debug.conf.template
    sudo tee "$APACHE_CONF_DIR/mods-available/pagespeed.conf" > /dev/null << 'EOF'
<IfModule pagespeed_module>
    ModPagespeed on
    AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

    ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
    ModPagespeedLogDir "/var/log/pagespeed"
    ModPagespeedRewriteLevel CoreFilters
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on
    ModPagespeedInPlaceResourceOptimization on
    ModPagespeedFetchHttps enable,allow_self_signed

    # Match bash test configuration from debug.conf.template
    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false

    # Configure canonicalize_javascript_libraries filter
    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

    # Add X-Extra-Header for header propagation tests
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

    <Location /pagespeed_global_admin>
        Require all granted
        SetHandler pagespeed_global_admin
    </Location>
</IfModule>

# Allow .htaccess files in test and example directories (matches debug.conf.template)
<Directory "/var/www/html/mod_pagespeed_test/">
    AllowOverride All
    Options +FollowSymLinks
</Directory>
<Directory "/var/www/html/mod_pagespeed_example/">
    AllowOverride All
</Directory>

# Configure no_cache directory to serve Cache-Control: no-cache headers
<Directory "/var/www/html/mod_pagespeed_test/no_cache/">
    Header set Cache-control "no-cache"
</Directory>
EOF

    # Set ServerName to avoid warning
    echo "ServerName localhost" | sudo tee "$APACHE_CONF_DIR/conf-available/servername.conf" > /dev/null

    # Pin a SMALL prefork MPM so the Cyclone background-thread leak regression
    # test (test_graceful_restart_no_thread_leak) surfaces quickly: with a tight
    # ServerLimit/MaxRequestWorkers, a child that cannot exit on graceful (because
    # a joinable Cyclone HitTracker thread keeps it alive) exhausts the scoreboard
    # within a handful of graceful cycles and Apache logs AH03490. prefork gives a
    # 1:1 process-to-slot mapping so the leak is directly observable as lingering
    # apache2 child PIDs. See pagespeed/system StopCacheBackgroundThreads.
    sudo a2dismod mpm_event mpm_worker 2>/dev/null || true
    sudo a2enmod mpm_prefork 2>/dev/null || true
    sudo tee "$APACHE_CONF_DIR/mods-available/mpm_prefork.conf" > /dev/null << 'EOF'
<IfModule mpm_prefork_module>
    StartServers            2
    MinSpareServers         1
    MaxSpareServers         3
    ServerLimit             6
    MaxRequestWorkers       6
    MaxConnectionsPerChild  0
</IfModule>
EOF

    # Configure HTTPS VirtualHost if TLS certs are available
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        log_info "Configuring HTTPS VirtualHost on port $APACHE_HTTPS_PORT..."
        sudo tee "$APACHE_CONF_DIR/sites-available/pagespeed-https.conf" > /dev/null << EOF
Listen $APACHE_HTTPS_PORT

<VirtualHost *:$APACHE_HTTPS_PORT>
    ServerName localhost

    SSLEngine on
    SSLCertificateFile $TLS_CERT_FILE
    SSLCertificateKeyFile $TLS_KEY_FILE

    DocumentRoot /var/www/html

    <IfModule pagespeed_module>
        ModPagespeed on
        AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

        ModPagespeedFileCachePath "/var/cache/mod_pagespeed/"
        ModPagespeedLogDir "/var/log/pagespeed"
        ModPagespeedRewriteLevel CoreFilters
        ModPagespeedStatistics on
        ModPagespeedStatisticsLogging on
        ModPagespeedFetchHttps enable,allow_self_signed

        ModPagespeedBlockingRewriteKey psatest
        ModPagespeedCriticalImagesBeaconEnabled false

        # Map HTTPS origin to HTTP so resource fetches avoid HTTPS loopback.
        # Without this, the CSS combiner times out because curl cannot complete
        # the HTTPS fetch back to ourselves with a self-signed certificate.
        ModPagespeedMapOriginDomain http://localhost https://localhost:$APACHE_HTTPS_PORT

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
</VirtualHost>
EOF
        sudo a2ensite pagespeed-https 2>/dev/null || true
    fi

    # Configure secondary VirtualHost for IPRO caching tests
    log_info "Configuring secondary VirtualHost on port $APACHE_SECONDARY_PORT..."
    sudo tee "$APACHE_CONF_DIR/sites-available/pagespeed-secondary.conf" > /dev/null << EOF
Listen $APACHE_SECONDARY_PORT

<VirtualHost *:$APACHE_SECONDARY_PORT>
    ServerName ipro.example.com

    DocumentRoot /var/www/html

    # Allow .htaccess files for IPRO test subdirectories (nocache, no-cache-control-header)
    <Directory "/var/www/html/mod_pagespeed_test">
        AllowOverride All
        Options +FollowSymLinks
    </Directory>

    <IfModule pagespeed_module>
        ModPagespeed on
        AddOutputFilterByType MOD_PAGESPEED_OUTPUT_FILTER text/html

        ModPagespeedFileCachePath "/var/cache/mod_pagespeed_secondary/"
        ModPagespeedLogDir "/var/log/pagespeed"
        ModPagespeedRewriteLevel CoreFilters
        ModPagespeedStatistics on
        ModPagespeedStatisticsLogging on
        ModPagespeedInPlaceResourceOptimization on
        ModPagespeedFetchHttps enable,allow_self_signed

        ModPagespeedBlockingRewriteKey psatest
        ModPagespeedCriticalImagesBeaconEnabled false

        <Location /mod_pagespeed_statistics>
            Require all granted
            SetHandler mod_pagespeed_statistics
        </Location>

        <Location /pagespeed_admin>
            Require all granted
            SetHandler pagespeed_admin
        </Location>
    </IfModule>
</VirtualHost>
EOF
    sudo a2ensite pagespeed-secondary 2>/dev/null || true

    # Enable required modules (including ssl for HTTPS)
    sudo a2enmod rewrite headers deflate proxy proxy_http expires ssl 2>/dev/null || true
    sudo a2enmod pagespeed 2>/dev/null || true
    sudo a2enconf servername 2>/dev/null || true

    # Verify configuration
    if ! sudo apache2ctl configtest 2>&1 | grep -q "Syntax OK"; then
        log_error "Apache configuration test failed"
        sudo apache2ctl configtest
        return 1
    fi

    log_info "Apache configuration OK"
}

start_apache() {
    log_info "Starting Apache..."

    # Stop if running
    sudo apache2ctl stop 2>/dev/null || true
    sleep 1

    # Start Apache
    sudo apache2ctl start
    sleep 2

    # Verify Apache is running
    if ! curl -s -I http://localhost/ > /dev/null 2>&1; then
        log_error "Apache failed to start"
        log_info "Checking error log..."
        sudo tail -20 /var/log/apache2/error.log 2>/dev/null || true
        return 1
    fi

    # Verify mod_pagespeed is working
    if curl -sI http://localhost/mod_pagespeed_example/ 2>/dev/null | grep -q "X-Mod-Pagespeed\|X-Page-Speed"; then
        log_info "mod_pagespeed is working!"
    else
        log_warn "mod_pagespeed header not found (may need a few seconds to warm up)"
    fi

    log_info "Apache with mod_pagespeed is running on port 80"
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        log_info "HTTPS is available on port $APACHE_HTTPS_PORT"
    fi
    log_info ""
    log_info "Run Python tests with:"
    log_info "  python3 -m pytest test/system/automatic/test_sanity.py -v"
    log_info ""
    log_info "Or run all tests:"
    log_info "  python3 -m pytest test/system/automatic/ -v"
}

stop_apache() {
    log_info "Stopping Apache..."
    sudo apache2ctl stop 2>/dev/null || true
    log_info "Apache stopped"
}

show_status() {
    if pgrep -x apache2 > /dev/null 2>&1; then
        log_info "Apache is running"
        if curl -sI http://localhost/mod_pagespeed_example/ 2>/dev/null | grep -q "X-Mod-Pagespeed\|X-Page-Speed"; then
            log_info "mod_pagespeed is active"
        else
            log_warn "mod_pagespeed not detected in response"
        fi
    else
        log_info "Apache is not running"
    fi
}

# Main
case "${1:-start}" in
    start)
        check_module || exit 1
        setup_directories
        install_module
        setup_test_content
        generate_tls_certs || exit 1
        configure_apache
        start_apache
        ;;
    stop)
        stop_apache
        ;;
    status)
        show_status
        ;;
    restart)
        stop_apache
        sleep 1
        check_module || exit 1
        start_apache
        ;;
    *)
        echo "Usage: $0 [start|stop|status|restart]"
        exit 1
        ;;
esac
