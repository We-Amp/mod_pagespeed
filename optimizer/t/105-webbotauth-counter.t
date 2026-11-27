# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Web Bot Auth opt-in counter endpoint (EXPERIMENTAL) end-to-end
# smoke. Proves a RUNNING nginx exposes /.well-known/webbotauth-counter per the
# operator-configured mode (worker-role pagespeed-shared.conf prologue) and the
# env-carried secret bearer token, and honors the response-hardening contract:
#
#   * mode off (default)          -> endpoint invisible (404, normal handling)
#   * mode public, no/bad token   -> COARSE doc (200, Cache-Control max-age=300,
#                                    bucketed tiers, NO boot_id)
#   * mode public, valid token    -> EXACT doc (200, Cache-Control no-store,
#                                    boot_id present)
#   * mode private, no token      -> 404 (existence hidden; surface B only)
#   * mode private, valid token   -> EXACT doc (200, no-store)
#   * HEAD                         -> headers only, no body
#   * non-GET/HEAD method         -> not served (declined -> normal handling)
#
# Needs no worker and no network: the counter reads the serve-stats mmap when
# present (absent here => zeroed doc), and the mode/token come from the shared
# conf + the `env` directive. The secret bearer token is injected via nginx's
# `env VAR=VALUE;` directive (main context) so the worker process sees it.
#
# JSON-escaping of operator-controlled signer NAMES in the exact doc is covered
# by inspection of the C++ builder (CounterJsonEscape); exercising it e2e would
# require a worker to mint the serve-stats file and a verified signed request to
# claim a per-signer slot, which this worker-less smoke cannot set up.

use Test::Nginx::Socket 'no_plan';
use File::Path qw(make_path);

my $token = "secrettoken105";

# Three parent dirs, one per mode (the shared conf lives in the cache path's
# parent, so distinct modes need distinct parents).
my %dirs = (
    off     => "/tmp/wba-105-off",
    public  => "/tmp/wba-105-public",
    private => "/tmp/wba-105-private",
);
for my $mode (keys %dirs) {
    my $d = $dirs{$mode};
    make_path("$d/www");
    open(my $fh, '>', "$d/pagespeed-shared.conf") or die $!;
    print $fh "version=1\nweb_bot_auth_public_counter=$mode\n";
    close($fh);
}

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: mode off -> endpoint invisible (404)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-off/test-ps-105-1.vol;
root /tmp/wba-105-off/www;
--- request
GET /.well-known/webbotauth-counter
--- error_code: 404


=== TEST 2: mode public, no token -> coarse doc (200, max-age=300, no boot_id)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-public/test-ps-105-2.vol;
root /tmp/wba-105-public/www;
--- request
GET /.well-known/webbotauth-counter
--- error_code: 200
--- response_headers
Content-Type: application/json
Cache-Control: public, max-age=300
Vary: Authorization
X-Content-Type-Options: nosniff
--- response_body_like: "verified_total":"\d
--- response_body_unlike: boot_id|"since"|"kid"


=== TEST 3: mode public, valid token -> exact doc (200, no-store, boot_id)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-public/test-ps-105-3.vol;
root /tmp/wba-105-public/www;
--- request
GET /.well-known/webbotauth-counter
--- more_headers
Authorization: Bearer secrettoken105
--- error_code: 200
--- response_headers
Content-Type: application/json
Cache-Control: no-store
X-Content-Type-Options: nosniff
--- response_body_like: "boot_id":"


=== TEST 4: mode public, WRONG token -> coarse doc (falls back, not exact)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-public/test-ps-105-4.vol;
root /tmp/wba-105-public/www;
--- request
GET /.well-known/webbotauth-counter
--- more_headers
Authorization: Bearer wrong-token
--- error_code: 200
--- response_headers
Cache-Control: public, max-age=300
Vary: Authorization
--- response_body_unlike: boot_id|"since"|"kid"


=== TEST 5: mode private, no token -> 404 (existence hidden)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-private/test-ps-105-5.vol;
root /tmp/wba-105-private/www;
--- request
GET /.well-known/webbotauth-counter
--- error_code: 404


=== TEST 6: mode private, valid token -> exact doc (200, no-store)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-private/test-ps-105-6.vol;
root /tmp/wba-105-private/www;
--- request
GET /.well-known/webbotauth-counter
--- more_headers
Authorization: Bearer secrettoken105
--- error_code: 200
--- response_headers
Cache-Control: no-store
--- response_body_like: "since":"\d\d\d\d-\d\d-\d\d"


=== TEST 7: HEAD returns headers only, no body
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-public/test-ps-105-7.vol;
root /tmp/wba-105-public/www;
--- request
HEAD /.well-known/webbotauth-counter
--- error_code: 200
--- response_headers
Content-Type: application/json
--- response_body chomp


=== TEST 8: non-GET/HEAD method is not served (declined -> normal handling)
--- main_config
env PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=secrettoken105;
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-105-public/test-ps-105-8.vol;
root /tmp/wba-105-public/www;
--- request
POST /.well-known/webbotauth-counter
--- error_code: 404
--- response_body_unlike: verified_total
