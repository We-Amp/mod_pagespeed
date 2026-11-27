# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# RSL-CAP enforcement (experimental) end-to-end smoke. Ports the
# 1.15 rsl_cap_enforcement_smoke.sh semantics onto the 2.0 module: mints signed
# capability tokens with webbotauth_sign_tool (--emit=rslcap, the SAME
# deterministic key the --emit=keystore document publishes, so the bytes the
# validator checks are bit-identical), pre-warms the nginx RSL key store with a
# realm-"rsl" keystore document (playing the worker's role, exactly like the
# pagespeed-shared.conf prologue below), and proves a RUNNING nginx maps the
# validator verdict to inline HTTP status:
#   * enabled + valid token granting the required lic+scope -> 200 (allow)
#   * enabled + NO Authorization token                      -> 401
#   * enabled + valid token but lic/scope NOT granted       -> 402
#   * enabled + EXPIRED token                               -> 401
#   * enabled + TAMPERED signature                          -> 401
#   * enabled + malformed token                             -> 401
#   * enabled + valid token but iss != pinned issuer        -> 401 (issuer pin)
#   * enabled + oversized (>8KB) Authorization header       -> 401 (header cap)
#
# Every 401 carries the RFC 7235 challenge (WWW-Authenticate: License);
# asserted on the no-token case.
#
# Default-OFF passthrough (enforcement disabled => zero-cost, all requests pass)
# is exercised by every other t/*.t file — none of them set
# rsl_cap_enforcement, so the PREACCESS handler returns NGX_DECLINED
# immediately and never touches a request — and pinned explicitly by the
# garbage-Authorization passthrough case in t/102-webbotauth.t.
#
# Requires WEBBOTAUTH_SIGN_TOOL (path to a built webbotauth_sign_tool);
# skipped otherwise (the harness scripts export it after building the tool).

BEGIN {
    unless ($ENV{WEBBOTAUTH_SIGN_TOOL} && -x $ENV{WEBBOTAUTH_SIGN_TOOL}) {
        print "1..0 # SKIP WEBBOTAUTH_SIGN_TOOL not set or not executable\n";
        exit 0;
    }
}

use Test::Nginx::Socket 'no_plan';
use File::Path qw(make_path);

my $tool = $ENV{WEBBOTAUTH_SIGN_TOOL};
my $dir  = "/tmp/rslcap-104";
my $kid  = "test-key-1";
my $iss  = "issuer.example";
my $host = "keys.example.com";
make_path("$dir/www");

# A file the static content handler serves once enforcement passes (the
# PREACCESS handler runs before the CONTENT phase).
open(my $probe, '>', "$dir/www/rce-probe") or die $!;
print $probe "ok\n";
close($probe);

# Worker-role prologue: enforcement toggle + required license/scope + issuer
# pin + the RSL issuer directory host (realm "rsl").
open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
print $fh <<"EOF";
version=1
rsl_cap_enforcement=true
rsl_cap_directory_hosts=$host
rsl_cap_requested_license=premium
rsl_cap_requested_scope=render
rsl_cap_issuer=$iss
EOF
close($fh);

# ... plus the warmed key store (worker-published file, here minted by the sign
# tool so the suite needs no worker and no network). Realm "rsl" keeps it a
# distinct trust domain from the observe-only "wba" verifier keys.
my $store = qx($tool --emit=keystore --kid=$kid --directory-host=$host --realm=rsl);
die "sign tool --emit=keystore failed" if $? != 0 || $store !~ /^k=P,/m;
open($fh, '>', "$dir/pagespeed-rslcap-keys.conf") or die $!;
print $fh $store;
close($fh);

# Mint capability tokens (each prints "TOKEN=License <token>"); keep the full
# Authorization header value (the "License " scheme prefix included).
sub mint {
    my ($args) = @_;
    my $out = qx($tool --emit=rslcap --kid=$kid $args);
    die "sign tool --emit=rslcap failed ($args): rc=$?" if $? != 0;
    my ($t) = $out =~ /^TOKEN=(.*)$/m;
    die "unexpected sign tool output: $out" unless defined $t;
    return $t;
}

$::T_OK    = mint("--iss=$iss --license=premium --scope=render");
$::T_UNLIC = mint("--iss=$iss --license=basic --scope=other");
$::T_EXP   = mint("--iss=$iss --license=premium --scope=render --exp-in=-3600");
$::T_TAMP  = mint("--iss=$iss --license=premium --scope=render --tamper");
$::T_WISS  = mint("--iss=evil.example --license=premium --scope=render");
# Oversized Authorization value: beyond the handler's 8KB cap, within the
# enlarged nginx header buffer configured in TEST 8 (so it reaches the
# handler's cap instead of being rejected by nginx itself with 400/414).
$::T_BIG   = "License " . ("A" x 9000);

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: valid token granting the required license+scope -> 200
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-1.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_OK"
--- error_code: 200


=== TEST 2: no Authorization token -> 401 with the RFC 7235 challenge
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-2.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- error_code: 401
--- response_headers
WWW-Authenticate: License


=== TEST 3: valid identity but license/scope not granted -> 402
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-3.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_UNLIC"
--- error_code: 402


=== TEST 4: expired token -> 401
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-4.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_EXP"
--- error_code: 401


=== TEST 5: tampered signature -> 401
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-5.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_TAMP"
--- error_code: 401


=== TEST 6: malformed token -> 401
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-6.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers
Authorization: License not-a-valid-token
--- error_code: 401


=== TEST 7: valid token but issuer != pinned issuer -> 401 (issuer pin)
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-7.vol;
root /tmp/rslcap-104/www;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_WISS"
--- error_code: 401


=== TEST 8: oversized (>8KB) Authorization header -> 401 (header cap)
The enlarged client-header buffer lets the oversized value through nginx's own
line limit so it reaches the handler's 8KB RSL-CAP cap instead of being
rejected by nginx with 400/414 first (same setup as the 1.15 smoke).
--- config
pagespeed on;
pagespeed_cache_path /tmp/rslcap-104/test-ps-104-8.vol;
root /tmp/rslcap-104/www;
large_client_header_buffers 4 64k;
location = /rce-probe { }
--- request
GET /rce-probe
--- more_headers eval
"Authorization: $::T_BIG"
--- error_code: 401
