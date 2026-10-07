#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

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

# Lane-fixture bookkeeping (test/system/conftest.py KNOWN_LANE_FIXTURES): every
# fixture this script provisions is recorded in $LANE_FIXTURES_FILE, and
# run_system_tests.sh exports the list as PAGESPEED_LANE_FIXTURES.
LANE_STATE_DIR=/tmp/apache_pagespeed_test
LANE_FIXTURES_FILE="$LANE_STATE_DIR/lane_fixtures"

FLUSH_ORIGIN_PORT="${FLUSH_ORIGIN_PORT:-8093}"
FLUSH_ORIGIN_PID="$LANE_STATE_DIR/flush_origin.pid"

# Lane fixture "remote_config": pagespeed/system/pathological_server.py,
# serving the remote-configuration files the #REMOTE_CONFIG vhosts fetch.
RCPORT="${RCPORT:-8090}"
RCPORT_PID="$LANE_STATE_DIR/pathological_server.pid"

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

reset_lane_fixtures() {
    mkdir -p "$LANE_STATE_DIR"
    rm -f "$LANE_FIXTURES_FILE"
}

provide_fixture() {
    mkdir -p "$LANE_STATE_DIR"
    echo "$1" >> "$LANE_FIXTURES_FILE"
}

generate_tls_certs() {
    log_info "Generating self-signed TLS certificates..."

    mkdir -p "$TLS_CERT_DIR"

    # Check if certs already exist and are valid
    if [ -f "$TLS_CERT_FILE" ] && [ -f "$TLS_KEY_FILE" ]; then
        # Also require the www.modpagespeed.com SAN (system/test_inline_unauth.py's
        # stand-in origin): the module's fetcher keeps host-name verification on
        # even with allow_self_signed (pagespeed/system/curl_fetch.cc sets
        # CURLOPT_SSL_VERIFYHOST 2 unconditionally -- allow_self_signed only
        # relaxes the CA check), so it really checks the certificate's name.
        # An unexpired cert from before the stand-in existed would otherwise
        # pass -checkend but lack this SAN, and must be regenerated too.
        if openssl x509 -checkend 86400 -noout -in "$TLS_CERT_FILE" 2>/dev/null \
            && openssl x509 -noout -text -in "$TLS_CERT_FILE" 2>/dev/null \
                | grep -q "DNS:www.modpagespeed.com"; then
            log_info "TLS certificates already exist and are valid"
            return 0
        fi
        log_info "Existing certificates expired or missing the stand-in host name, regenerating..."
    fi

    # Generate self-signed certificate valid for 365 days
    openssl req -x509 -newkey rsa:2048 \
        -keyout "$TLS_KEY_FILE" \
        -out "$TLS_CERT_FILE" \
        -days 365 \
        -nodes \
        -subj "/C=US/ST=Test/L=Test/O=PageSpeed/CN=localhost" \
        -addext "subjectAltName=DNS:localhost,DNS:127.0.0.1,IP:127.0.0.1,DNS:www.modpagespeed.com" \
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

    # Lane fixture "cache_flush": the test runner touches cache.flush in these
    # roots (conftest.py flush_cache; cache_flushing.sh touched the primary and
    # _secondary roots). Only the top-level directory is opened up; PageSpeed
    # still owns everything it creates beneath it.
    sudo chmod 0777 /var/cache/mod_pagespeed /var/cache/mod_pagespeed_secondary
    provide_fixture cache_flush

    # Lane fixture "stats_log": start every run with no statistics log, so
    # statistics_logging.sh's "every logged timestamp is from this run" holds.
    sudo rm -f /var/log/pagespeed/stats_log_*
    provide_fixture stats_log

    # FileCachePath roots of the fixture vhosts (configure_fixture_vhosts,
    # configure_external_origin); top level runner-writable like the primary
    # (cache_flushing.sh touches cache.flush in several of them).
    local suffix
    for suffix in _optionsbycookieson _optionsbycookiesoff _ipro_for_browser \
                  _mpsoff _purge _mpsoff_dir_on _mpsoff_htaccess_on _test \
                  _external_origin _with_shm _ipro_proxy; do
        sudo rm -rf "/var/cache/mod_pagespeed$suffix"
        sudo mkdir -p "/var/cache/mod_pagespeed$suffix"
        sudo chown www-data:www-data "/var/cache/mod_pagespeed$suffix"
        sudo chmod 0777 "/var/cache/mod_pagespeed$suffix"
    done

    # Drop-in compatibility fixture: releases before 2.1 kept a license token
    # at <parent of FileCachePath>/pagespeed.license. Stage a stale one so the
    # whole suite runs against an upgraded-install layout; the module must
    # ignore it (one INFO line, no behaviour change) — asserted by
    # automatic/test_no_license_apparatus.py.
    log_info "Staging a stale pagespeed.license fixture at /var/cache/pagespeed.license"
    echo "stale-token-from-a-previous-release" | sudo tee /var/cache/pagespeed.license > /dev/null
    sudo chown www-data:www-data /var/cache/pagespeed.license
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

    # debug.conf.template serves the purge/ and cache_flush/ trees from the
    # document root (the devel harness copied them there).
    local tree
    for tree in purge cache_flush; do
        if [ -d "$PROJECT_ROOT/install/mod_pagespeed_test/$tree" ]; then
            sudo rm -rf "/var/www/html/$tree"
            sudo cp -r "$PROJECT_ROOT/install/mod_pagespeed_test/$tree" "/var/www/html/$tree"
        fi
    done

    # Set permissions
    sudo chown -R www-data:www-data /var/www/html

    # Lane fixture "doc_root_scratch": the runner owns purge/ and cache_flush/
    # so cache_purge / cache_flushing ports can write scratch files (the bash
    # suite used $SUDO cp); Apache only needs to read them.
    sudo chown -R "$(id -u):$(id -g)" /var/www/html/purge /var/www/html/cache_flush
    provide_fixture doc_root_scratch

    # Stand-in for https://www.modpagespeed.com (see pin_external_origin).
    if [ -d "$SCRIPT_DIR/external_origin/www.modpagespeed.com" ]; then
        sudo rm -rf /var/www/external_origin
        sudo mkdir -p /var/www/external_origin
        sudo cp -r "$SCRIPT_DIR/external_origin/www.modpagespeed.com" /var/www/external_origin/
        sudo chown -R www-data:www-data /var/www/external_origin
    else
        log_warn "external_origin/www.modpagespeed.com not found: the https stand-in will not be served"
    fi
}

# Lane fixtures "debug_conf_dirs", "admin_handlers", "compressed_metadata_cache":
# server-scope test configuration of the bash suite. Blocks marked
# debug.conf.template are copied from install/debug.conf.template with
# @@APACHE_DOC_ROOT@@ -> /var/www/html and @@APACHE_DOMAIN@@ -> localhost.
configure_fixture_server_scope() {
    log_info "Configuring server-scope lane fixtures..."
    sudo tee "$APACHE_CONF_DIR/conf-available/pagespeed-test-fixtures.conf" > /dev/null << 'EOF'
# ---- debug_conf_dirs -------------------------------------------------------

# debug.conf.template
<Location ~ "\.pagespeed\.([a-z]\.)?[a-z]{2}\.[^.]{10}\.[^.]+">
  # For testing that AddResourceHeader gets passed through
  ModPagespeedAddResourceHeader "X-Foo" "Bar"
</Location>

# debug.conf.template
<Directory "/var/www/html/mod_pagespeed_test/compressed/" >
    # Files in this directory are already compressed so always add
    # the right header.
    Header set Cache-control "max-age=600"
    Header append 'Content-Encoding' 'gzip'
    AddType text/javascript .custom_ext
</Directory>

# debug.conf.template: the following directories are used by
# automatic/system_tests/smaxage.sh to test that we properly add s-maxage to
# cache-control headers for unoptimized ipro responses.
#
# Directory naming is based on:
#   cc:   cache-control
#   sma:  s-max-age
#   p:    private
#   nc:   no-cache
#   ns:   no-store
#   nt:   no-transform
#   nsp:  no spaces
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200/" >
    Header set Cache-control "max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/nocc/" >
    header unset Cache-Control
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200p/" >
    Header set Cache-control "private, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200nc/" >
    Header set Cache-control "no-cache, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200ns/" >
    Header set Cache-control "no-store, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200nt/" >
    Header set Cache-control "no-transform, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma5/" >
    Header set Cache-control "s-maxage=5, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50/" >
    Header set Cache-control "s-maxage=50, max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50nsp/" >
    Header set Cache-control "s-maxage=50,max-age=200"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc9/" >
    Header set Cache-control "max-age=9"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200cc9/" >
    Header set Cache-control "max-age=200, max-age=9"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50sma5/" >
    Header set Cache-control "max-age=200, s-maxage=50, s-maxage=5"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50cc9/" >
    Header set Cache-control "max-age=200, s-maxage=50, max-age=9"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50cc9nsp/" >
    Header set Cache-control "max-age=200,s-maxage=50,max-age=9"
</Directory>
<Directory "/var/www/html/mod_pagespeed_test/ipro/cc200sma50sma51/" >
    Header set Cache-control "max-age=200, s-maxage=50, s-maxage=51"
</Directory>

# debug.conf.template
<Directory "/var/www/html/mod_pagespeed_test/ipro/wait/" >
    # TODO(jmarantz): ModPagespeedInPlaceWaitForOptimized should be superfluous,
    # or made equivalent to ModPagespeedInPlaceRewriteDeadlineMs -1, which waits
    # forever.  Otherwise ModPagespeedInPlaceRewriteDeadlineMs should just have
    # the specified deadline.
    # # See https://github.com/apache/incubator-pagespeed-mod/issues/1171 for more
    # detailed discussion.
    ModPagespeedInPlaceWaitForOptimized on
</Directory>

<Directory "/var/www/html/mod_pagespeed_test/ipro/wait/long/" >
    # Make the deadline long here for valgrind tests.  We could
    # conditionalize this.
    ModPagespeedInPlaceRewriteDeadlineMs 10000
</Directory>

<Directory "/var/www/html/mod_pagespeed_test/ipro/wait/short/" >
    # Make the deadline short here as we expect to always miss it
    # in tests.
    ModPagespeedInPlaceRewriteDeadlineMs 1
</Directory>

# debug.conf.template
<Directory "/var/www/html/mod_pagespeed_test/disable_no_transform" >
  ModPagespeedDisableRewriteOnNoTransform off
  <IfModule headers_module>
     <FilesMatch "\.(js|css)$">
        Header append 'Cache-Control' 'no-transform'
     </FilesMatch>
  </IfModule>
</Directory>

<Directory "/var/www/html/mod_pagespeed_test/no_transform" >
  Header append 'Cache-Control' 'no-transform'
</Directory>

# debug.conf.template: test to make sure that user-authenticated resources do
# not get cached and optimized.
<Directory "/var/www/html/mod_pagespeed_test/auth" >
  AllowOverride AuthConfig
  AuthType Basic
  AuthName "Restricted Files"
  AuthBasicProvider file
  AuthUserFile /var/www/html/mod_pagespeed_test/auth/passwd.conf
  Require user user1
</Directory>

# debug.conf.template: a configuration hierarchy where at the root we have
# turned on a few filters, several of which do not preserve URLs, and in a
# subdirectory we have turned on preserve URLs.
<Directory "/var/www/html/mod_pagespeed_test/preserveurls" >
  ModPagespeedRewriteLevel CoreFilters
</Directory>

<Directory "/var/www/html/mod_pagespeed_test/preserveurls/on" >
ModPagespeedJsPreserveURLs On
ModPagespeedImagePreserveURLs On
ModPagespeedCssPreserveURLs On

# TODO(jmarantz): PreserveURLs should override the explicit setting at
# the level above that turns on inline_preview_images, resize_mobile_images,
# and lazyload_images
ModPagespeedDisableFilters inline_preview_images,resize_mobile_images
ModPagespeedDisableFilters lazyload_images
</Directory>

# debug.conf.template
<Directory "/var/www/html/mod_pagespeed_test/forbid_all_disabled/disabled" >
    # Prevent the enabling of these filters for files in this directory
    # -and- all subdirectories (they can't override it, deliberately).
    ModPagespeedForbidAllDisabledFilters true
    ModPagespeedDisableFilters remove_quotes,remove_comments
    ModPagespeedDisableFilters collapse_whitespace
    # Enable this, which was disabled in ../.htaccess, to test that we can
    # enable something already disabled at the same time as we forbid all.
    ModPagespeedEnableFilters inline_css
</Directory>

# debug.conf.template: directory-scope coverage for the retained no-op option.
<Directory "/var/www/html/mod_pagespeed_test/experimental_js_minifier" >
  ModPagespeedUseExperimentalJsMinifier on
</Directory>

# debug.conf.template: make a non-empty subdirectory config to make sure that
# cache.flush updates get transmitted to nested configurations.
<Directory "/var/www/html/cache_flush/" >
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters inline_css
  ModPagespeedDisableFilters add_instrumentation
</Directory>

# debug.conf.template
<Directory "/var/www/html/mod_pagespeed_test/public" >
  Header set Cache-control "public,max-age=600"
  ModPagespeedPreserveUrlRelativity off
</Directory>

# adapted from debug.conf.template:422-424 (server-wide there; scoped to
# /mod_pagespeed_example here)
<Location /mod_pagespeed_example>
    Header append 'X-Extra-Header' '1'
</Location>

# json_content_type.sh: the module treats JSON as heuristically non-cacheable
# (ContentType::IsLikelyStaticResource), so the JSON the bash saw minified in
# place gets an explicit lifetime here.
<Directory "/var/www/html/mod_pagespeed_test">
  <Files "example.json">
    Header set Cache-Control "max-age=600"
  </Files>
</Directory>

# debug.conf.template:66-67
ModPagespeedPermitIdsForCssCombining "allowed-to-combine-1-*"
ModPagespeedPermitIdsForCssCombining "allowed-to-combine-2-*"

# debug.conf.template:478 sets AvoidRenamingIntrospectiveJavascript off for the
# whole server; the lane keeps the default and applies the template's value to
# the one page aris.sh and combine_javascript.sh use for the "off" contrast.
<Location /mod_pagespeed_test/avoid_renaming_introspective_javascript__off.html>
  ModPagespeedAvoidRenamingIntrospectiveJavascript off
</Location>

# debug.conf.template:250-251
ModPagespeedLoadFromFile "http://localhost/mod_pagespeed_test/ipro/instant/" \
                         "/var/www/html/mod_pagespeed_test/ipro/instant/"

# debug.conf.template:1141-1147
<Directory "/var/www/html/mod_pagespeed_test/ipro/instant/wait/" >
  ModPagespeedInPlaceWaitForOptimized on
  ModPagespeedInPlaceRewriteDeadlineMs 5000
</Directory>

# debug.conf.template:1171-1173
<Directory "/var/www/html/mod_pagespeed_test/ipro/instant/deadline/" >
    ModPagespeedInPlaceRewriteDeadlineMs -1
</Directory>

# debug.conf.template:253-269
<Directory "/var/www/html/mod_pagespeed_test/ipro/cookie/" >
  # Add Vary:Cookie.  This should prevent us from optimizing the
  # vary_cookie.css even though ModPagespeedRespectVary is off.
  Header append Vary Cookie
  ModPagespeedRespectVary off
  ModPagespeedInPlaceWaitForOptimized on
</Directory>

<Directory "/var/www/html/mod_pagespeed_test/ipro/cookie2/" >
  # Add Vary:Cookie2.  This should prevent us from optimizing the
  # vary_cookie2.css even though ModPagespeedRespectVary is off.
  Header append Vary Cookie2
  ModPagespeedRespectVary off
  ModPagespeedInPlaceWaitForOptimized on
</Directory>

# debug.conf.template:275-278
<Directory "/var/www/html/mod_pagespeed_test/vary/no_respect/" >
  ModPagespeedDisableFilters add_instrumentation,inline_css
  ModPagespeedRespectVary off
</Directory>

# debug.conf.template:424 formerly appended X-Extra-Header to every response; the lane
# appends it to /mod_pagespeed_example only (pagespeed.conf), so the test tree
# gets its own append instead of a second server-wide one.
<Location /mod_pagespeed_test>
  Header append 'X-Extra-Header' '1'
</Location>

# debug.conf.template:467-472
ModPagespeedLoadFromFile "http://localhost/mod_pagespeed_test/load_from_file/web_dir/" "/var/www/html/mod_pagespeed_test/load_from_file/file_dir/"
ModPagespeedLoadFromFileMatch "^http://localhost/mod_pagespeed_test/load_from_file_match/web_([^/]*)/" "/var/www/html/mod_pagespeed_test/load_from_file/file_\1/"
ModPagespeedLoadFromFileRule Disallow "/var/www/html/mod_pagespeed_test/load_from_file/file_dir/httponly/"
ModPagespeedLoadFromFileRuleMatch Disallow \.ssp.css$
ModPagespeedLoadFromFileRuleMatch Allow exception\.ssp\.css$

# debug.conf.template:426-428
<Directory "/var/www/html/mod_pagespeed_test/nostore" >
  Header append 'Cache-Control' 'no-store'
</Directory>

# debug.conf.template:149-156
<Directory "/var/www/html/mod_pagespeed_test/htaccess" >
    AllowOverride All
    ModPagespeedInPlaceWaitForOptimized on
    ModPagespeedInPlaceRewriteDeadlineMs 10000
</Directory>
ModPagespeedLoadFromFile \
    "http://localhost/mod_pagespeed_test/htaccess/" \
    "/var/www/html/mod_pagespeed_test/htaccess/"

# debug.conf.template:52-54
<Directory "/var/www/html/mod_pagespeed_test/max_html_parse_size" >
  ModPagespeedMaxHtmlParseBytes 5000
</Directory>

# debug.conf.template:131-135
<Directory "/var/www/html/mod_pagespeed_test/shard" >
  ModPagespeedShardDomain "localhost" shard1,shard2
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters extend_cache
</Directory>

# debug.conf.template:491-496
# These will be sent to the origin domain when fetching subresources.
ModPagespeedCustomFetchHeader header value
ModPagespeedCustomFetchHeader x-other False
<Location /mod_pagespeed_log_request_headers.js>
  SetHandler mod_pagespeed_log_request_headers
</Location>

# debug.conf.template:500
ModPagespeedRespectXForwardedProto on

# debug.conf.template:502-504
<Location ~ "/mod_pagespeed_test/response_headers.html*">
  SetHandler mod_pagespeed_response_options_handler
  # debug.conf.template:137-138 (server-wide there; scoped to this page here)
  ModPagespeedEnableFilters add_instrumentation
</Location>

# debug.conf.template:396-402 (mod_include itself is enabled with a2enmod)
AddType text/html .shtml
AddOutputFilter INCLUDES .shtml

# debug.conf.template:486-489
<ModPagespeedIf !spdy>
  ModPagespeedShardDomain nonspdy.example.com s1.example.com,s2.example.com
</ModPagespeedIf>

# ---- admin_handlers --------------------------------------------------------

# debug.conf.template (handler_access_messages.sh reads these at server scope;
# "Allow localhost" keeps the lane's own vhosts allowed).
ModPagespeedMessagesDomains Allow messages-allowed.example.com
ModPagespeedMessagesDomains Allow cleared-inherited.example.com
ModPagespeedMessagesDomains Allow cleared-inherited-reallowed.example.com
ModPagespeedMessagesDomains Allow more-messages-allowed.example.com
ModPagespeedMessagesDomains Allow anything-*-wildcard.example.com
ModPagespeedMessagesDomains Allow localhost

# debug.conf.template handler Locations, with the lane's access style.
<Location /mod_pagespeed_global_statistics>
    Require all granted
    SetHandler mod_pagespeed_global_statistics
</Location>
<Location /pagespeed_console>
    Require all granted
    SetHandler pagespeed_console
</Location>
# The handler for "pagespeed_admin" is fixed in name, but you can put
# it on any URL path, and everything should work.
<Location /alt/admin/path>
    Require all granted
    SetHandler pagespeed_admin
</Location>

# ---- compressed_metadata_cache ---------------------------------------------

# debug.conf.template
ModPagespeedCompressMetadataCache true
EOF
    sudo a2enconf pagespeed-test-fixtures 2>/dev/null || true
    provide_fixture debug_conf_dirs
    provide_fixture admin_handlers
    provide_fixture compressed_metadata_cache
}

# Lane fixture "secondary_vhosts": the name-based VirtualHosts
# install/debug.conf.template defines for the bash suite, on the secondary
# port. Each block is copied from the template with
#   @@APACHE_DOC_ROOT@@     -> /var/www/html
#   @@MOD_PAGESPEED_CACHE@@ -> /var/cache/mod_pagespeed
#   @@PAGESPEED-TEST-HOST@@ -> selfsigned.modpagespeed.com
#   <VirtualHost localhost:PORT> -> <VirtualHost *:PORT>  (the lane's existing
#       secondary vhost is *:PORT; an address-specific block would outrank it)
# plus one added line per vhost, "Header always set X-Test-Vhost", which
# system/test_lane_fixtures.py uses to prove routing. The template's
# commented-out #MEMCACHED/#REDIS/#REWRITE lines are not copied. The file
# sorts after pagespeed-secondary.conf, so ipro.example.com stays the default
# vhost of the secondary port.
configure_fixture_vhosts() {
    log_info "Configuring fixture vhosts on port $APACHE_SECONDARY_PORT..."
    local conf="$APACHE_CONF_DIR/sites-available/pagespeed-test-vhosts.conf"
    sudo tee "$conf" > /dev/null << 'EOF'
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName secondary.example.com
  Header always set X-Test-Vhost "secondary.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeed on
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed_secondary"
  ModPagespeedCompressMetadataCache false
  ModPagespeedMapProxyDomain secondary.example.com/gstatic_images \
                             http://www.gstatic.com/psa/static

  ModPagespeedCacheFlushFilename cache.flush
  # If you uncomment this, the test will fail, proving we can disable
  # cache-flush polling.
  # ModPagespeedCacheFlushPollIntervalSec 0

  # Helps testing that the deprecated ReportUnloadTime option still parses
  # and stays a no-op (the beacon always sends on page-hide).
  ModPagespeedReportUnloadTime on

  # Make a non-empty subdirectory config to make sure that
  # cache.flush updates get transmitted to nested configurations.
  <Directory "/var/www/html/cache_flush/" >
    ModPagespeedRewriteLevel PassThrough
    ModPagespeedEnableFilters inline_css
    ModPagespeedDisableFilters add_instrumentation
  </Directory>
</VirtualHost>

# Sets up a virtual host where we can specify forbidden filters without
# affecting any other hosts.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName forbidden.example.com
  Header always set X-Test-Vhost "forbidden.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedBlockingRewriteKey psatest
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  # Start with all core filters enabled ...
  ModPagespeedRewriteLevel CoreFilters
  # ... then forbid these filters ...
  ModPagespeedForbidFilters remove_quotes,remove_comments,collapse_whitespace
  ModPagespeedForbidFilters rewrite_css,resize_images
  # ... and disable but not forbid this one (to ensure we retain its URL).
  ModPagespeedDisableFilters inline_css
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName unauthorizedresources.example.com
  Header always set X-Test-Vhost "unauthorizedresources.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedInlineResourcesWithoutExplicitAuthorization Script,Stylesheet
  ModPagespeedCssInlineMaxBytes 1000000
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName pagespeed-off.example.com
  Header always set X-Test-Vhost "pagespeed-off.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed off
  ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName pagespeed-on.example.com
  Header always set X-Test-Vhost "pagespeed-on.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed on
  ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName pagespeed-unplugged.example.com
  Header always set X-Test-Vhost "pagespeed-unplugged.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed unplugged
  ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName pagespeed-standby.example.com
  Header always set X-Test-Vhost "pagespeed-standby.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed standby
  ModPagespeedEnableFilters collapse_whitespace
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName url-attribute.example.com
  Header always set X-Test-Vhost "url-attribute.example.com"

  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  # Don't actually try to rewrite any resources; the ones in
  # rewrite_domains.html don't actually exist.
  ModPagespeedRewriteLevel PassThrough

  # This is used for testing dynamically defined url-valued
  # attributes
  ModPagespeedUrlValuedAttribute span src Hyperlink
  ModPagespeedUrlValuedAttribute hr imgsrc Image
  ModPagespeedDomainRewriteHyperlinks on
  ModPagespeedMapRewriteDomain http://dst.example.com http://src.example.com
  ModPagespeedEnableFilters rewrite_domains
  ModPagespeedUrlValuedAttribute custom a Image
  ModPagespeedUrlValuedAttribute custom b otherResource
  ModPagespeedUrlValuedAttribute custom c hyperlink
  ModPagespeedUrlValuedAttribute img alt-src Image
  ModPagespeedUrlValuedAttribute video alt-a Image
  ModPagespeedUrlValuedAttribute video alt-b Image
  ModPagespeedUrlValuedAttribute video alt-b Image

  ModPagespeedUrlValuedAttribute link data-stylesheet Stylesheet
  ModPagespeedUrlValuedAttribute span data-stylesheet-a Stylesheet
  ModPagespeedUrlValuedAttribute span data-stylesheet-b Stylesheet
  ModPagespeedUrlValuedAttribute span data-stylesheet-c Stylesheet

  # Also test that we can redefine spec-defined attributes.
  ModPagespeedUrlValuedAttribute blockquote cite Image
</VirtualHost>

# Proxy selfsigned.modpagespeed.com for testing Issue 582.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName selfsigned.modpagespeed.com
  Header always set X-Test-Vhost "selfsigned.modpagespeed.com"

  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeed off
  ServerAlias selfsigned.modpagespeed.com

  ProxyPass / http://selfsigned.modpagespeed.com/
  ProxyPassReverse / http://selfsigned.modpagespeed.com/
</VirtualHost>

# The following three VHosts are created for Issue 599.
# Create three sites for a MapProxyDomain experiment. The sites are:
# cdn: forwards requests to proxy.
# proxy: runs MPS, and optimizes data from origin.
# origin: a normal website that is potentially external to proxy.

# The CDN in our example which simply forwards requests to the proxy.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName cdn.pm.example.com
  Header always set X-Test-Vhost "cdn.pm.example.com"

  # Point the docroot somewhere useless so that we know we're not fetching
  # from the CDN's filesystem. Note that in particular we are not attempting to
  # run CGI scripts.
  DocumentRoot "/var/www/html/mod_pagespeed_example/cgi"

  # Tell mod_proxy that we need to use a proxy to reach our VirtualHost servers.
  ProxyRemote * http://localhost:@@APACHE_SECONDARY_PORT@@
  ProxyPass /external/ http://proxy.pm.example.com/external/
  ProxyPassReverse /external/ http://proxy.pm.example.com/external/

  # Unplugged so that it passes the .pagespeed. requests through to the proxy.
  ModPagespeed unplugged
</VirtualHost>

# The proxy that runs MPS and can proxy data from origin. When the CDN
# requests proxied data from the proxy the proxy knows to fetch it from the
# origin server.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName proxy.pm.example.com
  Header always set X-Test-Vhost "proxy.pm.example.com"
  DocumentRoot "/var/www/html/mod_pagespeed_test/"
  # The usual cache location so that it gets cleared for on_cache_flush tests.
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  # We have to fetch through our localhost proxy to get to our other vhosts.
  ModPagespeedFetchProxy "localhost:@@APACHE_SECONDARY_PORT@@"

  # Origin resources should be optimized and hosted on proxy/external but
  # rewritten to cdn/external.
  ModPagespeedDomain proxy.pm.example.com
  ModPagespeedMapProxyDomain proxy.pm.example.com/external \
                             origin.pm.example.com \
                             cdn.pm.example.com/external
  ModPagespeedRewriteLevel CoreFilters
  ModPagespeedRewriteDeadlinePerFlushMs -1
</VirtualHost>

# The origin that serves the images to be proxied (Puzzle.jpg)
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName origin.pm.example.com
  Header always set X-Test-Vhost "origin.pm.example.com"
  DocumentRoot "/var/www/html/mod_pagespeed_example/images"
  ModPagespeed unplugged
</VirtualHost>

# For testing setting options by cookies.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName options-by-cookies-enabled.example.com
  Header always set X-Test-Vhost "options-by-cookies-enabled.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_optionsbycookieson"
  ModPagespeedAllowOptionsToBeSetByCookies true
  ModPagespeedStickyQueryParameters sticky_secret
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters collapse_whitespace
  ModPagespeedDisableFilters remove_comments,add_instrumentation
</VirtualHost>
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName options-by-cookies-disabled.example.com
  Header always set X-Test-Vhost "options-by-cookies-disabled.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_optionsbycookiesoff"
  ModPagespeedAllowOptionsToBeSetByCookies false
  ModPagespeedDisableFilters add_instrumentation
</VirtualHost>

# For testing request option overriding.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName request-option-override.example.com
  Header always set X-Test-Vhost "request-option-override.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRequestOptionOverride abc
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters collapse_whitespace
  ModPagespeedDisableFilters remove_comments,add_instrumentation
</VirtualHost>

# For testing signed urls.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName signed-urls.example.com
  Header always set X-Test-Vhost "signed-urls.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedUrlSigningKey helloworld
  ModPagespeedRewriteLevel PassThrough
</VirtualHost>

# For testing signed urls, ignoring signature validity.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName signed-urls-transition.example.com
  Header always set X-Test-Vhost "signed-urls-transition.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedUrlSigningKey helloworld
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedAcceptInvalidSignatures true
</VirtualHost>
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName unsigned-urls-transition.example.com
  Header always set X-Test-Vhost "unsigned-urls-transition.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  # This server will not sign URLs, but AcceptInvalidSignatures is on.
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedAcceptInvalidSignatures true
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName compressedcache.example.com
  Header always set X-Test-Vhost "compressedcache.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed on
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css
  ModPagespeedHttpCacheCompressionLevel 9
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName uncompressedcache.example.com
  Header always set X-Test-Vhost "uncompressedcache.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeed on
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css
  ModPagespeedHttpCacheCompressionLevel 0
  AddOutputFilterByType DEFLATE text/css
  DeflateCompressionLevel 1
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName ipro-for-browser.example.com
  Header always set X-Test-Vhost "ipro-for-browser.example.com"
  ModPagespeedEnableFilters rewrite_images,rewrite_css
  ModPagespeedEnableFilters convert_to_webp_lossless
  ModPagespeedJpegRecompressionQuality 75
  ModPagespeedWebpRecompressionQuality 70
  ModPagespeedInPlaceResourceOptimization on
  DocumentRoot "/var/www/html/mod_pagespeed_example"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_ipro_for_browser"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName ipro-for-browser-vary-on-auto.example.com
  Header always set X-Test-Vhost "ipro-for-browser-vary-on-auto.example.com"
  ModPagespeedEnableFilters rewrite_images,rewrite_css
  ModPagespeedEnableFilters convert_to_webp_lossless
  ModPagespeedEnableFilters convert_to_webp_animated
  ModPagespeedInPlaceResourceOptimization on
  ModPagespeedImageRecompressionQuality 90
  ModPagespeedJpegRecompressionQuality 75
  ModPagespeedJpegRecompressionQualityForSmallScreens 55
  ModPagespeedJpegQualityForSaveData 35
  ModPagespeedWebpRecompressionQuality 70
  ModPagespeedWebpRecompressionQualityForSmallScreens 50
  ModPagespeedWebpQualityForSaveData 30
  ModPagespeedWebpAnimatedRecompressionQuality 60
  DocumentRoot "/var/www/html/mod_pagespeed_example"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_ipro_for_browser"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName ipro-for-browser-vary-on-none.example.com
  Header always set X-Test-Vhost "ipro-for-browser-vary-on-none.example.com"
  ModPagespeedEnableFilters rewrite_images
  ModPagespeedInPlaceResourceOptimization on
  ModPagespeedImageRecompressionQuality 75
  DocumentRoot "/var/www/html/mod_pagespeed_example"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_ipro_for_browser"
</VirtualHost>

# For testing Modpagespeed unplugged.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName mpsunplugged.example.com
  Header always set X-Test-Vhost "mpsunplugged.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeed unplugged
</VirtualHost>

# For testing Modpagespeed off.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName mpsoff.example.com
  Header always set X-Test-Vhost "mpsoff.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_mpsoff"
  ModPagespeed off
</VirtualHost>

# Set up a reverse proxy (rproxy.) and origin (origin.) as vhosts for
# showing that we can configure PageSpeed via response headers.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName rproxy.rmcomments.example.com
  Header always set X-Test-Vhost "rproxy.rmcomments.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedDisableFilters add_instrumentation,remove_comments
  # Note that we don't enable remove_comments here; that setting comes from
  # the response headers from origin.rmcomments.example.com
  ProxyRemote * http://localhost:@@APACHE_SECONDARY_PORT@@
  ProxyPass / http://origin.rmcomments.example.com/
  ProxyPassReverse / http://origin.rmcomments.example.com/
</VirtualHost>
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName origin.rmcomments.example.com
  Header always set X-Test-Vhost "origin.rmcomments.example.com"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  DocumentRoot "/var/www/html"
  ModPagespeed unplugged
  Header add PageSpeedFilters remove_comments
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName purge.example.com
  Header always set X-Test-Vhost "purge.example.com"

  # Test purging individual URLs without flushing the entire metadata cache.
  ModPagespeedEnableCachePurge on

  ModPagespeedPurgeMethod PURGE
  DocumentRoot "/var/www/html/purge"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_purge"
  ModPagespeedDisableFilters add_instrumentation
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css
</VirtualHost>

# For testing ModPagespeed off, but with a directory-scope turning it
# back on.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName psoff-dir-on.example.com
  Header always set X-Test-Vhost "psoff-dir-on.example.com"
  DocumentRoot "/var/www/html/purge"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_mpsoff_dir_on"
  ModPagespeed off
  ModPagespeedEnableCachePurge on
  ModPagespeedPurgeMethod PURGE
  ModPagespeedDisableFilters add_instrumentation
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css
  <Directory "/var/www/html/purge">
    ModPagespeed on
  </Directory>
</VirtualHost>

# For testing ModPagespeed off, but with an htaccess turning it
# back on.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName psoff-htaccess-on.example.com
  Header always set X-Test-Vhost "psoff-htaccess-on.example.com"
  DocumentRoot "/var/www/html/purge"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_mpsoff_htaccess_on"
  ModPagespeed off
  ModPagespeedEnableCachePurge on
  ModPagespeedPurgeMethod PURGE
  ModPagespeedDisableFilters add_instrumentation
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css
  <Directory "/var/www/html/purge">
    AllowOverride All
  </Directory>
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName messages-allowed.example.com
  Header always set X-Test-Vhost "messages-allowed.example.com"
  ServerAlias messages-not-allowed.example.com
  ServerAlias more-messages-allowed.example.com
  ServerAlias anything-a-wildcard.example.com
  ServerAlias anything-b-wildcard.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName messages-still-not-allowed.example.com
  Header always set X-Test-Vhost "messages-still-not-allowed.example.com"
  ServerAlias but-this-message-allowed.example.com
  ServerAlias and-this-one.example.com
  ModPagespeedMessagesDomains Allow but-this-message-allowed.example.com
  ModPagespeedMessagesDomains Allow and-this-one.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName cleared-inherited.example.com
  Header always set X-Test-Vhost "cleared-inherited.example.com"
  ServerAlias cleared-inherited-reallowed.example.com
  ServerAlias messages-allowed-at-vhost.example.com
  ServerAlias messages-not-allowed-at-vhost.example.com
  ServerAlias anything-c-wildcard.example.com
  ModPagespeedMessagesDomains Disallow *
  ModPagespeedMessagesDomains Allow cleared-inherited-reallowed.example.com
  ModPagespeedMessagesDomains Allow messages-allowed-at-vhost.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName cleared-inherited-unlisted.example.com
  Header always set X-Test-Vhost "cleared-inherited-unlisted.example.com"

  ModPagespeedMessagesDomains Allow *
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName nothing-allowed.example.com
  Header always set X-Test-Vhost "nothing-allowed.example.com"
  ModPagespeedMessagesDomains Disallow *
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName nothing-explicitly-allowed.example.com
  Header always set X-Test-Vhost "nothing-explicitly-allowed.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName everything-explicitly-allowed.example.com
  Header always set X-Test-Vhost "everything-explicitly-allowed.example.com"
  ServerAlias everything-explicitly-allowed-but-aliased.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedStatisticsDomains Allow everything-explicitly-allowed.example.com
  ModPagespeedGlobalStatisticsDomains \
    Allow everything-explicitly-allowed.example.com
  ModPagespeedMessagesDomains Allow everything-explicitly-allowed.example.com
  ModPagespeedConsoleDomains Allow everything-explicitly-allowed.example.com
  ModPagespeedAdminDomains Allow everything-explicitly-allowed.example.com
  ModPagespeedGlobalAdminDomains Allow everything-explicitly-allowed.example.com
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName debug-filters.example.com
  Header always set X-Test-Vhost "debug-filters.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters debug
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName flush.example.com
  Header always set X-Test-Vhost "flush.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_test"
  ModPagespeed on
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedRewriteDeadlinePerFlushMs 1
  ModPagespeedDisableFilters add_instrumentation
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName broken-fetch.example.com
  Header always set X-Test-Vhost "broken-fetch.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed/broken-fetch"

  # Set up a fetch proxy that will 404 every request.
  ModPagespeedFetchProxy "brokenfetch.example.com:1111"

  # Prevent loopback fetch by explicitly authorizing the domain.  Loopback
  # fetches would work, and for this test, we're trying to have fetches fail,
  # so they are instead picked up by the ipro recorder.
  ModPagespeedDomain http://broken-fetch.example.com
  ModPagespeedInPlaceResourceOptimization on

  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_javascript
  ModPagespeedDisableFilters add_instrumentation
  ModPagespeedCriticalImagesBeaconEnabled false
  ModPagespeedCriticalCssAboveTheFoldOnly off
</VirtualHost>

# Test image rewrites for parent,child nodes and flush.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName image-rewrite-with-flush.example.com
  Header always set X-Test-Vhost "image-rewrite-with-flush.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedUrlValuedAttribute li data-thumb image
  ModPagespeedEnableFilters core
</VirtualHost>

# debug.conf.template:1469-1476
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName respectvary.example.com
  Header always set X-Test-Vhost "respectvary.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedRespectVary on
  Header append Vary User-Agent
</VirtualHost>

# debug.conf.template:1083-1091
# For testing handling of redirected requests.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName redirect.example.com
  Header always set X-Test-Vhost "redirect.example.com"
  Redirect /redirect/ /
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters add_instrumentation,collapse_whitespace
</VirtualHost>

# debug.conf.template:1420-1440
# Test host for issue 809: losing extra headers with cache-control set early.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName issue809.example.com
  Header always set X-Test-Vhost "issue809.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  header add Issue809 Issue809Value
  header add Cache-Control max-age=60 early

  # The tests using this vhost scan for css-combine URLs and will not find
  # them if the combined css file is then css-minified.  So we explicitly
  # disable these and other distracting filters for easy output scanning.
  ModPagespeedDisableFilters rewrite_css,add_instrumentation,flatten_css_imports
  ModPagespeedEnableFilters rewrite_images,combine_css
  ModPagespeedCriticalImagesBeaconEnabled false
  ModPagespeedModifyCachingHeaders on
</VirtualHost>

# debug.conf.template:1621-1629
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName content-encoding.example.com
  Header always set X-Test-Vhost "content-encoding.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters debug
  ModPagespeedSlurpDirectory /var/www/html/mod_pagespeed_test/bogus_content_encoding/
  ModPagespeedSlurpReadOnly on
</VirtualHost>

# debug.conf.template:794-807
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName absolute-urls.example.com
  Header always set X-Test-Vhost "absolute-urls.example.com"

  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  # This is used for testing that we don't load resources from
  # absolute urls during resource reconstruction unless they're for
  # our own hostname.  While ModPagespeedDomain should no longer
  # have an effect on whether we load absolute urls, we need to
  # include it to be sure we're failing because of the code under
  # test and not because the domain lawyer is rejecting it.
  ModPagespeedDomain http://example.com
</VirtualHost>

# debug.conf.template:638-650
# Sets up a logical home-page server on
# max-cacheable-content-length.example.com.  This server is only used to test
# ModPagespeedMaxCacheableContentLength, i.e.,
# max_cacheable_response_content_length.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName max-cacheable-content-length.example.com
  Header always set X-Test-Vhost "max-cacheable-content-length.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedBlockingRewriteKey psatest
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_javascript
  ModPagespeedMaxCacheableContentLength 85
</VirtualHost>

# debug.conf.template:652-674
# Test how load from file handles maximum sizes.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName lff-large-files.example.com
  Header always set X-Test-Vhost "lff-large-files.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedMaxCacheableContentLength 4096
  ModPagespeedLoadFromFile "http://lff-large-files.example.com/" \
                           "/var/www/html/"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName lff-large-files-no-fallback.example.com
  Header always set X-Test-Vhost "lff-large-files-no-fallback.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedMaxCacheableContentLength 4096
  # http://lff-large-files-no-fallback.example.com/foo.css won't load, because
  # we've configured a LoadFromFile pattern that doesn't match the path the
  # webserver would take.
  ModPagespeedLoadFromFile "http://lff-large-files-no-fallback.example.com/" \
                           "/var/www/html/mod_pagespeed_example/styles/"
</VirtualHost>

# debug.conf.template:717-729
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName domain-hyperlinks-on.example.com
  Header always set X-Test-Vhost "domain-hyperlinks-on.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  # Don't actually try to rewrite any resources; the ones in
  # rewrite_domains.html don't actually exist.
  ModPagespeedRewriteLevel PassThrough

  ModPagespeedDomainRewriteHyperlinks on
  ModPagespeedMapRewriteDomain http://dst.example.com http://src.example.com
  ModPagespeedEnableFilters rewrite_domains
</VirtualHost>

# debug.conf.template:731-743
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName domain-hyperlinks-off.example.com
  Header always set X-Test-Vhost "domain-hyperlinks-off.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  # Don't actually try to rewrite any resources; the ones in
  # rewrite_domains.html don't actually exist.
  ModPagespeedRewriteLevel PassThrough

  ModPagespeedDomainRewriteHyperlinks off
  ModPagespeedMapRewriteDomain http://dst.example.com http://src.example.com
  ModPagespeedEnableFilters rewrite_domains
</VirtualHost>

# debug.conf.template:745-758
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName client-domain-rewrite.example.com
  Header always set X-Test-Vhost "client-domain-rewrite.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"

  # Don't actually try to rewrite any resources; the ones in
  # rewrite_domains.html don't actually exist.
  ModPagespeedRewriteLevel PassThrough

  ModPagespeedMapRewriteDomain http://client-domain-rewrite.example.com \
      http://localhost
  ModPagespeedClientDomainRewrite true
  ModPagespeedEnableFilters rewrite_domains
</VirtualHost>

# debug.conf.template:1034-1044
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName map-static-domain.example.com
  Header always set X-Test-Vhost "map-static-domain.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedMapRewriteDomain http://static-cdn.example.com \
                               http://map-static-domain.example.com

  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters defer_javascript,rewrite_domains
</VirtualHost>

# debug.conf.template:1261-1278
# The downstreamcacherebeacon vhost setup in the below section is used in
# apache system tests to make sure that downstream caching and rebeaconing
# interact correctly.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName downstreamcacherebeacon.example.com
  Header always set X-Test-Vhost "downstreamcacherebeacon.example.com"

  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedCriticalImagesBeaconEnabled true

  # Enable the downstream caching feature and specify a rebeaconing key.
  ModPagespeedDownstreamCachePurgeLocationPrefix "http://localhost:8020"
  ModPagespeedDownstreamCachePurgeMethod "PURGE"
  ModPagespeedDownstreamCacheRebeaconingKey random_rebeaconing_key

  # The origin marks its HTML privately cacheable: the case downstream caching
  # is about, and what the tests assert on the page's response. Only the HTML.
  # The page's stylesheet and image keep Apache's default headers so that
  # PageSpeed may fetch and use them; a resource served with
  # "Cache-Control: private" is never fetched for rewriting, and without the
  # stylesheet's selectors there is nothing for the critical-CSS beacon to
  # instrument (issue #1060).
  <FilesMatch "\.html$">
    Header set Cache-Control "private, max-age=3000"
  </FilesMatch>
</VirtualHost>

# debug.conf.template:1303-1311
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName downstreamcacheresource.example.com
  Header always set X-Test-Vhost "downstreamcacheresource.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_images
  ModPagespeedDownstreamCachePurgeLocationPrefix \
    "http://localhost:@@APACHE_SECONDARY_PORT@@/purge"
</VirtualHost>

# debug.conf.template:1884-1894
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName experiment.devicematch.example.com
  Header always set X-Test-Vhost "experiment.devicematch.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedRewriteLevel PassThrough

  ModPagespeedRunExperiment on
  ModPagespeedUseAnalyticsJs false
  ModPagespeedExperimentSpec "id=1;percent=100;matches_device_type=mobile;enable=recompress_images"
</VirtualHost>

# debug.conf.template:1896-1906
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName experiment.ajax.example.com
  Header always set X-Test-Vhost "experiment.ajax.example.com"
  DocumentRoot "/var/www/html/"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRewriteLevel CoreFilters
  ModPagespeedDisableFilters add_instrumentation
  ModPagespeedRunExperiment on
  ModPagespeedUseAnalyticsJs false
  ModPagespeedJsInlineMaxBytes 1
  ModPagespeedExperimentSpec "id=1;percent=100;level=CoreFilters;enable=collapse_whitespace;options=JsInlineMaxBytes=1"
</VirtualHost>

# debug.conf.template:1863-1870 (#EXPERIMENT_GA block, made its own vhost
# instead of a whole-server config -- see system/test_experiments.py's
# TestExperimentFramework docstring for the fold with #EXPERIMENT_NO_GA).
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName experiment.example.com
  Header always set X-Test-Vhost "experiment.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedRunExperiment on
  ModPagespeedAnalyticsID "123-45-6734"
  ModPagespeedUseAnalyticsJs false
  ModPagespeedExperimentVariable 2
  ModPagespeedExperimentSpec "id=7;enable=recompress_images,rewrite_javascript;disable=convert_jpeg_to_progressive;percent=50;options=AvoidRenamingIntrospectiveJavascript=off,JsInlineMaxBytes=4"
  ModPagespeedExperimentSpec "id=2;enable=recompress_images,rewrite_javascript;percent=50;options=AvoidRenamingIntrospectiveJavascript=on"
  ModPagespeedExperimentSpec "id=3;default;percent=0"
</VirtualHost>

# debug.conf.template:1908-1915
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName ajax.example.com
  Header always set X-Test-Vhost "ajax.example.com"
  DocumentRoot "/var/www/html/"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedJsInlineMaxBytes 1
  ModPagespeedRewriteLevel CoreFilters
  ModPagespeedDisableFilters add_instrumentation
</VirtualHost>

# debug.conf.template:1918-1956
# Support for embedded configurations, where image flags in the
# VirtualHost serving HTML is not the same as the one serving resources,
# and thus we must embed the image flags in the rewritten image URLs.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName embed-config-html.example.org
  Header always set X-Test-Vhost "embed-config-html.example.org"

  DocumentRoot "/var/www/html/mod_pagespeed_test"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedAddOptionsToUrls on
  ModPagespeedJpegRecompressionQuality 73
  ModPagespeedDisableFilters inline_css,extend_cache,inline_javascript
  ModPagespeedDomain embed-config-resources.example.com

  # Share a cache keyspace with embed-config-resources.example.com.
  ModPagespeedCacheFragment "embed-config"
  ModPagespeedRewriteDeadlinePerFlushMs 5000
  ModPagespeedLoadFromFile "http://embed-config-resources.example.com/" \
      "/var/www/html/mod_pagespeed_example/"
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName embed-config-resources.example.com
  Header always set X-Test-Vhost "embed-config-resources.example.com"

  DocumentRoot "/var/www/html/mod_pagespeed_example"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedAddOptionsToUrls on
  # Note that we do not set the jpeg quality here, but take
  # it from image URL query parameters that we synthesize in
  # from embed-config-html.example.com.

  # Share a cache keyspace with embed-config-html.example.org.
  ModPagespeedCacheFragment "embed-config"

  ModPagespeedLoadFromFile "http://embed-config-resources.example.com/" \
      "/var/www/html/mod_pagespeed_example/"
</VirtualHost>

# debug.conf.template:580-611
# Sets up a logical home-page server on www.example.com
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName www.example.com
  Header always set X-Test-Vhost "www.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedLoadFromFile http://cdn.example.com /var/www/html
  ModPagespeedMapRewriteDomain cdn.example.com origin.example.com
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css,rewrite_images
</VirtualHost>

# Sets up a logical origin for CDNs to fetch content from, on origin.example.com.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName origin.example.com
  Header always set X-Test-Vhost "origin.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedLoadFromFile http://cdn.example.com /var/www/html
  ModPagespeedMapRewriteDomain cdn.example.com origin.example.com
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css,rewrite_images
</VirtualHost>

# Sets up a logical cdn, which is where we tell browsers to fetch resources from.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName cdn.example.com
  Header always set X-Test-Vhost "cdn.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed"
  ModPagespeedLoadFromFile http://cdn.example.com /var/www/html
  ModPagespeedMapRewriteDomain cdn.example.com origin.example.com
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_css,rewrite_images
</VirtualHost>

# debug.conf.template:2049-2071
# /mod_pagespeed_test is included in our DocumentRoot and thus does
# not need to be in any resource URL paths.  This helps us verify that
# we are looping back to the corect VirtualHost -- if we hit the wrong
# one it will not work.  Also we don't have a VirtualHost for
# sharedcdn.example.com, so the default Host header used for
# origin-mapping won't work either.  Instead, we want origin-fetches
# to go back to this VirtualHost so we rely on the new third optional
# argument to MapOriginDomain.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName customhostheader.example.com
  Header always set X-Test-Vhost "customhostheader.example.com"
  DocumentRoot "/var/www/html/mod_pagespeed_test"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_test"
  ModPagespeed on
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedEnableFilters rewrite_images
  ModPagespeedMapOriginDomain \
      localhost:@@APACHE_SECONDARY_PORT@@/customhostheader \
      sharedcdn.example.com/test \
      customhostheader.example.com
  ModPagespeedJpegRecompressionQuality 50
  ModPagespeedCriticalImagesBeaconEnabled false
</VirtualHost>

# debug.conf.template:1442-1467
# Build a configuration hierarchy where at the root we have turned on
# OptimizeForBandwidth, and in various subdirectories we override settings
# to make them more aggressive.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName optimizeforbandwidth.example.com
  Header always set X-Test-Vhost "optimizeforbandwidth.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"

  ModPagespeedRewriteLevel OptimizeForBandwidth
  ModPagespeedDisableFilters add_instrumentation

  ModPagespeedBlockingRewriteKey psatest

  <Directory "/var/www/html/mod_pagespeed_test/optimize_for_bandwidth/inline_css" >
    ModPagespeedEnableFilters inline_css
  </Directory>
  <Directory "/var/www/html/mod_pagespeed_test/optimize_for_bandwidth/css_urls" >
    ModPagespeedCssPreserveURLs off
  </Directory>
  <Directory "/var/www/html/mod_pagespeed_test/optimize_for_bandwidth/image_urls" >
    ModPagespeedImagePreserveURLs off
  </Directory>
  <Directory "/var/www/html/mod_pagespeed_test/optimize_for_bandwidth/core_filters" >
    ModPagespeedRewriteLevel CoreFilters
  </Directory>
</VirtualHost>

# debug.conf.template:1006-1032
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName uses-sendfile.example.com
  Header always set X-Test-Vhost "uses-sendfile.example.com"
  ModPagespeedBlockingRewriteKey psatest
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedEnableFilters rewrite_javascript
  Header always set X-Sendfile blablabla
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName uses-xaccelredirect.example.com
  Header always set X-Test-Vhost "uses-xaccelredirect.example.com"
  ModPagespeedBlockingRewriteKey psatest
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedEnableFilters rewrite_javascript
  Header always set X-Accel-Redirect blablabla
</VirtualHost>

<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName doesnt-sendfile.example.com
  Header always set X-Test-Vhost "doesnt-sendfile.example.com"
  ModPagespeedBlockingRewriteKey psatest
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedEnableFilters rewrite_javascript
</VirtualHost>

# debug.conf.template:1351-1360
# Test host for explicit shared memory cache.
ModPagespeedCreateSharedMemoryMetadataCache "/var/cache/mod_pagespeed_with_shm" 8192
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName shmcache.example.com
  Header always set X-Test-Vhost "shmcache.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath            "/var/cache/mod_pagespeed_with_shm"

  ModPagespeedEnableFilters rewrite_images
  ModPagespeedCriticalImagesBeaconEnabled false
</VirtualHost>

# debug.conf.template:1093-1107, adapted: the template formerly proxied to a backend on a
# third port (ipro-proxy-backend.example.com, debug.conf.template:1131-1139,
# PageSpeed unplugged, mod_deflate on). The lane reaches an equivalent backend
# -- the unplugged mpsunplugged.example.com vhost, whose CSS Debian's
# deflate.conf compresses -- through the secondary port, as
# rproxy.rmcomments.example.com does.
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName ipro-proxy.example.com
  Header always set X-Test-Vhost "ipro-proxy.example.com"

  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_ipro_proxy"

  ModPagespeed on
  ModPagespeedInPlaceResourceOptimization on
  ModPagespeedEnableFilters rewrite_domains

  ProxyRemote * http://localhost:@@APACHE_SECONDARY_PORT@@
  ProxyPass / http://mpsunplugged.example.com/mod_pagespeed_test/ipro/mod_deflate/
  ProxyPassReverse / http://mpsunplugged.example.com/mod_pagespeed_test/ipro/mod_deflate/
  AddOutputFilterByType DEFLATE text/css
</VirtualHost>

# Derived from debug.conf.template:1199-1211: the statistics handler open to
# loopback clients only, which statistics_are_local_only.sh probed through the
# host's non-loopback address. The lane's own statistics Location stays open for
# the test runner, so the restricted handler lives here; "Allow from localhost /
# 127.0.0.1" becomes "Require ip" ("Require local" would also admit a client
# connecting to the server's own address, i.e. this test's request).
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName stats-local-only.example.com
  Header always set X-Test-Vhost "stats-local-only.example.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  <Location /mod_pagespeed_statistics>
    Require ip 127.0.0.1 ::1
    SetHandler mod_pagespeed_statistics
  </Location>
</VirtualHost>
EOF
    sudo sed -i "s/@@APACHE_SECONDARY_PORT@@/$APACHE_SECONDARY_PORT/g" "$conf"
    sudo a2ensite pagespeed-test-vhosts 2>/dev/null || true
    provide_fixture secondary_vhosts
    provide_fixture experiment_framework
}

# Lane fixture "external_origin": local stand-ins for the internet origins
# the bash suite fetched -- @@PAGESPEED-TEST-HOST@@ (selfsigned.modpagespeed.com)
# and www.gstatic.com. Both names resolve to this host via /etc/hosts, so
# this only runs inside the test container; a *:80 vhost answers for them
# (the site file sorts after 000-default.conf, so the primary's default
# vhost is unchanged).
configure_external_origin() {
    if [ ! -f /.dockerenv ]; then
        log_warn "Not in a container: skipping lane fixture external_origin (it edits /etc/hosts)"
        return 0
    fi
    if [ ! -e "$APACHE_CONF_DIR/sites-enabled/000-default.conf" ]; then
        log_warn "000-default.conf not enabled: skipping external_origin (its *:80 vhost would become the default)"
        return 0
    fi
    log_info "Configuring the external-origin stand-in..."
    local name
    for name in selfsigned.modpagespeed.com www.gstatic.com; do
        if [ "$(getent hosts "$name" | awk '{print $1; exit}')" != "127.0.0.1" ]; then
            echo "127.0.0.1 $name" | sudo tee -a /etc/hosts > /dev/null
        fi
    done
    sudo mkdir -p /var/www/html/psa/static
    # The bash suite's real www.gstatic.com/psa/static/1.gif was a 1x1
    # transparent pixel (GIF89a, 42 bytes) -- serve the same bytes here so a
    # proxied rewrite of it behaves the way map_proxy_domain.sh expects
    # (cache-extended, never worth format-converting).
    echo 'R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7' | base64 -d | \
        sudo tee /var/www/html/psa/static/1.gif > /dev/null
    sudo chown -R www-data:www-data /var/www/html/psa
    sudo tee "$APACHE_CONF_DIR/sites-available/pagespeed-test-external-origin.conf" > /dev/null << 'EOF'
# The origin the bash suite reached over the internet: PageSpeed passes its
# content through untouched (PassThrough, no IPRO) but still serves
# .pagespeed. URLs, and every response sets a cookie, as the real origin did
# (map_proxy_domain.sh checks both).
<VirtualHost *:80>
  ServerName selfsigned.modpagespeed.com
  ServerAlias www.gstatic.com
  Header always set X-Test-Vhost "selfsigned.modpagespeed.com"
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed_external_origin"
  ModPagespeedRewriteLevel PassThrough
  ModPagespeedInPlaceResourceOptimization off
  Header always set Set-Cookie "test-cookie=1"
  # proxying.sh: the real origin served the font with an explicit lifetime;
  # the module does not cache an unknown type heuristically.
  <Files "not_really_a_font.woff">
    Header set Cache-Control "max-age=600"
  </Files>
</VirtualHost>

# debug.conf.template: test proxying of non-.pagespeed. resources.
ModPagespeedMapProxyDomain http://localhost/modpagespeed_http \
                           http://selfsigned.modpagespeed.com/do_not_modify

# debug.conf.template: establish a proxy mapping where the current server
# proxies an image stored on gstatic.com.
ModPagespeedMapProxyDomain localhost/gstatic_images \
                           http://www.gstatic.com/psa/static
EOF
    sudo a2ensite pagespeed-test-external-origin 2>/dev/null || true
    provide_fixture external_origin
}

# Pins one name to 127.0.0.1 in /etc/hosts (the way configure_external_origin's
# loop does above) and confirms the resolver actually returns it -- the
# unauthorized-resource checks (system/test_inline_unauth.py) need this pin to
# hold for a real fetch, not just for the /etc/hosts line to exist.
pin_external_origin() {
    if [ "$(getent hosts www.modpagespeed.com | awk '{print $1; exit}')" != "127.0.0.1" ]; then
        echo "127.0.0.1 www.modpagespeed.com" | sudo tee -a /etc/hosts > /dev/null
    fi
    if [ "$(getent hosts www.modpagespeed.com | awk '{print $1; exit}')" != "127.0.0.1" ]; then
        log_error "www.modpagespeed.com does not resolve to 127.0.0.1"
        return 1
    fi
}

# Extends the "external_origin" lane fixture above with the host
# www.modpagespeed.com (system/test_inline_unauth.py). An inline-unauthorized
# fetch is session-authorized (UrlInputResource::PrepareRequest) so it bypasses
# the loopback router and really resolves the name and checks the certificate
# (see generate_tls_certs); this stand-in serves it locally over HTTPS instead
# of depending on the internet. Advertises nothing new: still "external_origin".
configure_external_origin_https() {
    if [ ! -f /.dockerenv ]; then
        log_warn "Not in a container: skipping the www.modpagespeed.com https stand-in (it edits /etc/hosts)"
        return 0
    fi
    if [ ! -e "$APACHE_CONF_DIR/sites-enabled/000-default.conf" ]; then
        log_warn "000-default.conf not enabled: skipping the https stand-in (external_origin is not advertised)"
        return 0
    fi
    log_info "Configuring the www.modpagespeed.com https stand-in..."
    pin_external_origin || return 1

    local listen_443=""
    if ! grep -qsE '^[[:space:]]*Listen[[:space:]]+443([[:space:]]|$)' "$APACHE_CONF_DIR/ports.conf"; then
        listen_443="Listen 443"
    fi
    sudo tee "$APACHE_CONF_DIR/sites-available/pagespeed-test-external-origin-https.conf" > /dev/null << EOF
$listen_443
<VirtualHost *:443>
    ServerName www.modpagespeed.com
    SSLEngine on
    SSLCertificateFile $TLS_CERT_FILE
    SSLCertificateKeyFile $TLS_KEY_FILE
    DocumentRoot /var/www/external_origin/www.modpagespeed.com
    <Directory /var/www/external_origin/www.modpagespeed.com>
        Require all granted
    </Directory>
    <IfModule pagespeed_module>
        ModPagespeed unplugged
    </IfModule>
</VirtualHost>
EOF
    sudo a2ensite pagespeed-test-external-origin-https 2>/dev/null || true
}

# Lane fixture "flush_origin": the chunked, slow-flushing origin that replaces
# the bash suite's PHP pages (pagespeed_test_framework/flush_origin.py).
# flushpackets=on makes mod_proxy pass a FLUSH bucket down the output chain
# after every backend packet -- what mod_pagespeed's flush handling reacts to.
configure_flush_origin_proxy() {
    log_info "Configuring the flush-origin proxy..."
    local conf="$APACHE_CONF_DIR/conf-available/pagespeed-test-flush-origin.conf"
    sudo tee "$conf" > /dev/null << 'EOF'
ProxyPass /mod_pagespeed_test/flush_origin/ http://127.0.0.1:@@FLUSH_ORIGIN_PORT@@/ flushpackets=on
ProxyPassReverse /mod_pagespeed_test/flush_origin/ http://127.0.0.1:@@FLUSH_ORIGIN_PORT@@/

# debug.conf.template:72-75 proxied these from a remote test host (ports 8091
# with a Content-Type, 8092 without); the flushing origin serves both paths.
ModPagespeedMapProxyDomain http://localhost/content_type_present http://127.0.0.1:@@FLUSH_ORIGIN_PORT@@/content_type_present
ModPagespeedMapProxyDomain http://localhost/content_type_absent http://127.0.0.1:@@FLUSH_ORIGIN_PORT@@/content_type_absent
EOF
    sudo sed -i "s/@@FLUSH_ORIGIN_PORT@@/$FLUSH_ORIGIN_PORT/g" "$conf"
    sudo a2enconf pagespeed-test-flush-origin 2>/dev/null || true
}

# True (exit 0) if something is already listening on
# 127.0.0.1:$FLUSH_ORIGIN_PORT. Used to decide whether to warn instead of
# act -- never to pick a target to kill (see stop_flush_origin).
flush_origin_port_is_bound() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -q ":$FLUSH_ORIGIN_PORT "
        return $?
    fi
    # ss not installed here: try to bind the port ourselves. Success (and
    # we release it again) means it was free; failure means something else
    # already holds it.
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(('127.0.0.1', $FLUSH_ORIGIN_PORT))
    s.close()
    sys.exit(1)
except OSError:
    sys.exit(0)
"
}

start_flush_origin() {
    stop_flush_origin
    if flush_origin_port_is_bound; then
        # A foreign process is never reused sight unseen: it must actually
        # answer the flush origin's health route before we advertise the
        # fixture, or a same-port process that isn't a flush origin at all
        # would be handed to every test that requires_fixture("flush_origin").
        if curl -sf "http://127.0.0.1:$FLUSH_ORIGIN_PORT/health" > /dev/null 2>&1; then
            log_warn "flush origin port $FLUSH_ORIGIN_PORT is already bound by a process this lane did not start; using it instead of starting a second copy"
            provide_fixture flush_origin
            return 0
        fi
        log_error "flush origin port $FLUSH_ORIGIN_PORT is bound but does not answer /health; refusing to reuse it"
        return 1
    fi
    mkdir -p "$LANE_STATE_DIR"
    nohup python3 "$SCRIPT_DIR/pagespeed_test_framework/flush_origin.py" \
        --port "$FLUSH_ORIGIN_PORT" \
        --image-dir "$PROJECT_ROOT/install/mod_pagespeed_test/image_rewrite_with_flush" \
        > "$LANE_STATE_DIR/flush_origin.log" 2>&1 &
    echo $! > "$FLUSH_ORIGIN_PID"
    local i
    for i in $(seq 1 50); do
        if curl -sf "http://127.0.0.1:$FLUSH_ORIGIN_PORT/health" > /dev/null 2>&1; then
            log_info "Flush origin is listening on 127.0.0.1:$FLUSH_ORIGIN_PORT"
            provide_fixture flush_origin
            return 0
        fi
        sleep 0.1
    done
    log_error "Flush origin did not start"
    cat "$LANE_STATE_DIR/flush_origin.log" 2>/dev/null || true
    return 1
}

stop_flush_origin() {
    local pid=""
    if [ -f "$FLUSH_ORIGIN_PID" ]; then
        pid="$(cat "$FLUSH_ORIGIN_PID")"
        # The PID file may outlive the process it named (a stale file from a
        # crashed or externally-reaped run) and that number may since have
        # been reused by an unrelated process; only kill it when it is
        # still, in fact, a flush_origin.py process.
        if [ -n "$pid" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q flush_origin.py; then
            kill "$pid" 2>/dev/null || true
        else
            log_warn "stale PID file, not killing"
            pid=""
        fi
        rm -f "$FLUSH_ORIGIN_PID"
    fi
    # Wait for the process we just killed (if any) to actually exit, then
    # -- non-destructively -- check whether the port is still bound. This
    # only ever kills the PID *this lane* started and tracked in
    # $FLUSH_ORIGIN_PID; it never kills by pattern or by port, because a
    # same-shaped flush origin started from ANOTHER worktree on a shared
    # host would look identical to ours and must not be touched.
    if [ -n "$pid" ]; then
        local i
        for i in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
    fi
    if flush_origin_port_is_bound; then
        log_warn "flush origin port $FLUSH_ORIGIN_PORT is still bound by a process this lane did not start; not killing it"
    fi
}

# Lane fixture "remote_config": the twelve VirtualHosts debug.conf.template's
# #REMOTE_CONFIG block formerly defined for the bash suite (pagespeed/system/
# remote_config_test.sh), lifted verbatim with @@APACHE_DOC_ROOT@@ ->
# /var/www/html, @@MOD_PAGESPEED_CACHE@@ -> /var/cache/mod_pagespeed, and
# <VirtualHost localhost:PORT> -> <VirtualHost *:PORT> (see
# configure_fixture_vhosts). Each ModPagespeedRemoteConfigurationUrl points at
# pagespeed/system/pathological_server.py, started on $RCPORT below.
configure_remote_config_vhosts() {
    log_info "Configuring remote-config vhosts on port $APACHE_SECONDARY_PORT..."
    local conf="$APACHE_CONF_DIR/sites-available/pagespeed-test-remote-config.conf"
    sudo tee "$conf" > /dev/null << 'EOF'
# debug.conf.template:911-917
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/standard"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:919-925
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-partially-invalid.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/partly-invalid"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:927-933
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-invalid.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/invalid"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:935-941
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-out-of-scope.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/out-of-scope"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:943-949
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-failed-fetch.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/fail-future"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:951-957
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-slow-fetch.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/timeout"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:959-964 (no ModPagespeedRemoteConfigurationUrl -- the
# bash sets it per-request via a .htaccess file; see system/test_remote_config.py
# for why the .htaccess cases were left out of this port)
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-with-htaccess.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:966-972
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-experiment.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/experiment"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:974-980
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-slightly-slow-fetch.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/slightly-slow"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:982-988
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-slightly-slow-expired-fetch.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/slow-expired"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:990-996
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-forbidden.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/forbidden"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>

# debug.conf.template:998-1004
<VirtualHost *:@@APACHE_SECONDARY_PORT@@>
  ServerName remote-config-initially-forbidden.example.com
  DocumentRoot "/var/www/html"
  ModPagespeedFileCachePath "/var/cache/mod_pagespeed"
  ModPagespeedRemoteConfigurationUrl "http://127.0.0.1:@@RCPORT@@/forbidden-once"
  ModPagespeedRemoteConfigurationTimeoutMs 1500
</VirtualHost>
EOF
    sudo sed -i "s/@@APACHE_SECONDARY_PORT@@/$APACHE_SECONDARY_PORT/g; s/@@RCPORT@@/$RCPORT/g" "$conf"
    sudo a2ensite pagespeed-test-remote-config 2>/dev/null || true
}

# True (exit 0) if something is already listening on 127.0.0.1:$RCPORT. Used
# to decide whether to warn instead of act -- never to pick a target to kill
# (see stop_remote_config_server).
remote_config_port_is_bound() {
    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -q ":$RCPORT "
        return $?
    fi
    # ss not installed here: try to bind the port ourselves. Success (and we
    # release it again) means it was free; failure means something else
    # already holds it.
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    s.bind(('127.0.0.1', $RCPORT))
    s.close()
    sys.exit(1)
except OSError:
    sys.exit(0)
"
}

start_remote_config_server() {
    stop_remote_config_server
    if remote_config_port_is_bound; then
        # A foreign process is never reused sight unseen: it must actually
        # answer /standard before we advertise the fixture, or a same-port
        # process that isn't pathological_server.py would be handed to every
        # test that requires_fixture("remote_config").
        if curl -sf "http://127.0.0.1:$RCPORT/standard" > /dev/null 2>&1; then
            log_warn "remote config port $RCPORT is already bound by a process this lane did not start; using it instead of starting a second copy"
            provide_fixture remote_config
            return 0
        fi
        log_error "remote config port $RCPORT is bound but does not answer /standard; refusing to reuse it"
        return 1
    fi
    mkdir -p "$LANE_STATE_DIR"
    nohup python3 "$PROJECT_ROOT/pagespeed/system/pathological_server.py" "$RCPORT" \
        > "$LANE_STATE_DIR/pathological_server.log" 2>&1 &
    echo $! > "$RCPORT_PID"
    local i
    for i in $(seq 1 50); do
        if curl -sf "http://127.0.0.1:$RCPORT/standard" > /dev/null 2>&1; then
            log_info "Remote config pathological server is listening on 127.0.0.1:$RCPORT"
            provide_fixture remote_config
            return 0
        fi
        sleep 0.1
    done
    log_error "Remote config pathological server did not start"
    cat "$LANE_STATE_DIR/pathological_server.log" 2>/dev/null || true
    return 1
}

stop_remote_config_server() {
    local pid=""
    if [ -f "$RCPORT_PID" ]; then
        pid="$(cat "$RCPORT_PID")"
        # The PID file may outlive the process it named (a stale file from a
        # crashed or externally-reaped run) and that number may since have
        # been reused by an unrelated process; only kill it when it is
        # still, in fact, a pathological_server.py process.
        if [ -n "$pid" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q pathological_server.py; then
            kill "$pid" 2>/dev/null || true
        else
            log_warn "stale PID file, not killing"
            pid=""
        fi
        rm -f "$RCPORT_PID"
    fi
    if [ -n "$pid" ]; then
        local i
        for i in $(seq 1 20); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
    fi
    if remote_config_port_is_bound; then
        log_warn "remote config port $RCPORT is still bound by a process this lane did not start; not killing it"
    fi
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
    # Message history is off without a buffer; the nginx config sets
    # MessageBufferSize too, and automatic/test_no_license_apparatus.py
    # asserts on the admin message history.
    ModPagespeedMessageBufferSize 100000
    ModPagespeedRewriteLevel CoreFilters
    ModPagespeedStatistics on
    ModPagespeedStatisticsLogging on
    ModPagespeedInPlaceResourceOptimization on
    ModPagespeedFetchHttps enable,allow_self_signed

    # The flush fixture waits for the server to notice cache.flush, and the
    # bash suite relied on that happening within a second or two.
    ModPagespeedCacheFlushPollIntervalSec 1

    # The lane image's mime table has no webp entry; the module refuses a typeless input.
    AddType image/webp .webp

    # The lane keeps the module's INFO messages in the server error log (the nginx lane logs at
    # level info too); the license apparatus tests read their startup notice from it.
    LogLevel warn pagespeed:info

    # Zero-copy aliased serving under the STOCK filter chain:
    # mod_reqtimeout stays enabled and the AddOutputFilterByType harness
    # above stays in place -- test_zerocopy_serve.py asserts the aliased
    # path fires anyway.  Main (plain-HTTP) block only: the HTTPS vhost's
    # ssl filter makes aliasing correctly ineligible there.
    ModPagespeedCycloneZeroCopy on
    ModPagespeedCycloneZeroCopyServe on

    # Match bash test configuration from debug.conf.template
    ModPagespeedBlockingRewriteKey psatest
    ModPagespeedCriticalImagesBeaconEnabled false

    # Beaconing stays off globally (so lazyload_images etc. work without
    # browser beacon data), but the prioritize_critical_images round-trip
    # test needs real beacon instrumentation on its example page. Scoped
    # stand-in for debug.conf.template's imagebeacon.example.com vhost.
    <Location /mod_pagespeed_example/prioritize_critical_images.html>
        ModPagespeedCriticalImagesBeaconEnabled true
    </Location>

    # Configure canonicalize_javascript_libraries filter
    ModPagespeedLibrary 43 1o978_K0_LNE5_ystNklf http://www.modpagespeed.com/rewrite_javascript.js

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

    # Pin a FIXED prefork pool: sixteen children are started up front and
    # MinSpareServers/MaxSpareServers keep exactly sixteen, so no child is
    # spawned or reaped while a test runs. A newly started child does start-up
    # work of its own (it fetches every remote configuration through the HTTP
    # cache) that moves the server-wide statistics, and a spawn triggered by a
    # test's own concurrent requests lands inside that test's exact-delta
    # window. ServerLimit is 32, not 16, so a graceful restart can start the
    # new generation of sixteen while the old one is still exiting; the leak
    # regression test (test_graceful_restart_no_thread_leak) still sees a
    # child that cannot exit on graceful (because a joinable Cyclone
    # HitTracker thread keeps it alive) fill the scoreboard within a few
    # cycles and Apache log AH03490. prefork gives a 1:1 process-to-slot
    # mapping so the leak is directly observable as lingering apache2 child
    # PIDs. See pagespeed/system StopCacheBackgroundThreads.
    # MaxRequestWorkers stays 16 (AP_MPMQ_MAX_DAEMONS answers 16): a
    # .pagespeed. request that arrives through the reverse-proxy vhost holds a
    # chain of four workers (proxy, origin vhost, the module's own input fetch
    # back through the proxy, and that fetch's target); with six workers the
    # chain stalled past the module's fetch deadline under light concurrent
    # load and the input was remembered as failed for minutes.
    sudo a2dismod mpm_event mpm_worker 2>/dev/null || true
    sudo a2enmod mpm_prefork 2>/dev/null || true
    sudo tee "$APACHE_CONF_DIR/mods-available/mpm_prefork.conf" > /dev/null << 'EOF'
<IfModule mpm_prefork_module>
    StartServers            16
    MinSpareServers         16
    MaxSpareServers         16
    ServerLimit             32
    MaxRequestWorkers       16
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
    Header always set X-Test-Vhost "ipro.example.com"

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

    configure_fixture_server_scope
    configure_fixture_vhosts
    configure_external_origin
    configure_external_origin_https
    configure_flush_origin_proxy
    configure_remote_config_vhosts

    # Enable required modules (including ssl for HTTPS)
    # reqtimeout is explicit so test_zerocopy_serve.py always exercises
    # the stock Debian chain, even on images where the default-enabled
    # module set was trimmed.
    sudo a2enmod rewrite headers deflate proxy proxy_http expires ssl reqtimeout include 2>/dev/null || true
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

    # Guard: the stand-in answers with a certificate valid for its
    # name, exactly as the module's fetcher will check it. Only runs where
    # configure_external_origin_https configured the stand-in (same two
    # conditions: the container guard and the 000-default.conf guard);
    # relies on the /etc/hosts pin for the name to resolve to loopback,
    # rather than curl --resolve, so this exercises the same resolution
    # path a real fetch from the module takes.
    if [ -f /.dockerenv ] && [ -e "$APACHE_CONF_DIR/sites-enabled/000-default.conf" ]; then
        if ! curl -sf --cacert "$TLS_CERT_FILE" -o /dev/null \
                https://www.modpagespeed.com/testfiles/google-cse-default.css; then
            log_error "The www.modpagespeed.com stand-in does not answer on https"
            return 1
        fi
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
    log_info ""
    log_info "run_system_tests.sh exports PAGESPEED_CACHE_DIR=/var/cache/mod_pagespeed"
    log_info "(lane fixture cache_flush); export it yourself when running pytest by hand."
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
        reset_lane_fixtures
        setup_directories
        install_module
        setup_test_content
        generate_tls_certs || exit 1
        configure_apache
        start_flush_origin || exit 1
        start_remote_config_server || exit 1
        start_apache
        ;;
    stop)
        stop_apache
        stop_flush_origin
        stop_remote_config_server
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
