# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# Web Bot Auth observe-only wiring smoke: proves the classify ->
# verifier -> x-verified-bot header path end-to-end against a running nginx,
# mirroring the 1.15 webbotauth_smoke cases that need no signing tool:
#   * feature ON + unsigned request            -> NO verdict header (only
#     requests carrying signature material earn a label)
#   * feature ON + malformed/garbage signature -> "unknown" (fail-closed)
#   * feature ON + untagged (non-web-bot-auth) signature -> NO verdict header
#     (not web-bot-auth material; classified as if unsigned)
#   * feature ON + bare Signature header only  -> "unknown" (fail-closed)
#   * feature OFF (default)                    -> no x-verified-bot at all
#   * RSL-CAP enforcement OFF (default) + garbage Authorization: License
#     header -> 200 passthrough, no challenge (default-off pin)
# The VALID-signature end-to-end path (fresh sigs from the sign tool + a
# pre-warmed key store) lives in t/103-webbotauth-valid.t; the RSL-CAP
# enforcement-ON verdicts live in t/104-rslcap.t.
#
# The feature toggle is worker-published via pagespeed-shared.conf, so the
# prologue below plays the worker's role: it writes the shared config into a
# DEDICATED cache-parent dir (/tmp/wba-102) so no other .t suite ever sees
# web_bot_auth=true.

use File::Path qw(make_path);

my $dir = "/tmp/wba-102";
make_path($dir);
open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
print $fh <<'EOF';
version=1
web_bot_auth=true
web_bot_auth_verified_bots=test-kid=testbot
web_bot_auth_directory_hosts=keys.example.com
EOF
close($fh);

my $off_dir = "/tmp/wba-102-off";
make_path($off_dir);
open($fh, '>', "$off_dir/pagespeed-shared.conf") or die $!;
print $fh <<'EOF';
version=1
web_bot_auth=false
EOF
close($fh);

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-102.sh") == 0
        or die "Cache volume creation failed";
}

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: unsigned request gets no verdict header (human traffic unlabeled)
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102/test-ps-102-1.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
User-Agent: MyBot/1.0 (spoofed bot UA earns no trust)
--- response_headers
!x-verified-bot
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 2: garbage signature classifies as unknown, request still served
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102/test-ps-102-2.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Signature-Input: sig1=("@method" "@authority" "@path");created=1;keyid="nope";alg="ed25519";tag="web-bot-auth"
Signature: sig1=:not-a-real-signature:
--- error_code: 200
--- response_headers
x-verified-bot: unknown
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 2b: untagged signature is not web-bot-auth material -> no label
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102/test-ps-102-2b.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Signature-Input: sig1=("@method" "@authority" "@path");created=1;keyid="nope";alg="ed25519"
Signature: sig1=:not-a-real-signature:
--- error_code: 200
--- response_headers
!x-verified-bot
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 3: bare Signature header without Signature-Input is unknown
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102/test-ps-102-4.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Signature: sig1=:AAAA:
--- error_code: 200
--- response_headers
x-verified-bot: unknown
--- no_error_log
[error]


=== TEST 4: feature off (default) emits no verdict header and does no work
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102-off/test-ps-102-3.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Signature-Input: sig1=("@method");created=1;keyid="k";alg="ed25519"
Signature: sig1=:AAAA:
--- error_code: 200
--- response_headers
!x-verified-bot
--- no_error_log
[error]


=== TEST 5: RSL-CAP enforcement off (default): License header passes through
The shared config in this suite never sets rsl_cap_enforcement, so the
PREACCESS enforcement handler must decline immediately — a request carrying a
garbage Authorization: License token is served normally (200), with no RSL
processing and no challenge. Pins the default-off passthrough explicitly
(the enforcement-ON verdicts live in t/104-rslcap.t).
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-102/test-ps-102-5.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Authorization: License not.a.token
--- error_code: 200
--- response_headers
!WWW-Authenticate
--- no_error_log
[error]
