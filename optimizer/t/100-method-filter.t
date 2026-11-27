# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-100.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: POST request declined (no X-PageSpeed)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-100-1.vol;

location /origin {
    pagespeed off;
    default_type text/plain;
    return 200 "origin";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
POST /t
hello
--- response_headers
!X-PageSpeed
--- error_code: 200
--- no_error_log
[error]


=== TEST 2: HEAD request processed (gets MISS on cold cache)
The handler accepts HEAD. Body filter skips recording for HEAD.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-100-2.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "origin";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
HEAD /t
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 3: GET request processed (cold miss)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-100-3.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "body";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_headers
X-PageSpeed: MISS
--- error_code: 200
--- no_error_log
[error]
