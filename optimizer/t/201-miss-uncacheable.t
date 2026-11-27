# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-201.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: Cache-Control no-store response served but not recorded
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-1.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Cache-Control "no-store" always;
    return 200 "private data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_body chomp
private data
--- error_code: 200
--- no_error_log
[crit]


=== TEST 2: Cache-Control private response served but not recorded
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-2.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "private, max-age=300" always;
    return 200 "user-specific content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_body chomp
user-specific content
--- error_code: 200
--- no_error_log
[crit]


=== TEST 3: Vary: Cookie response served but not cached (always MISS)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-3.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Cookie" always;
    return 200 "session-dependent data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["session-dependent data", "session-dependent data"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 4: Vary: * response served but not cached (always MISS)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-4.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "*" always;
    return 200 "wildcard vary data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["wildcard vary data", "wildcard vary data"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 5: Vary: Accept-Encoding response is cached (MISS then HIT)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-5.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Accept-Encoding" always;
    return 200 "encoding-varied content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["encoding-varied content", "encoding-varied content"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[crit]


=== TEST 6: Vary: User-Agent, Accept response is not cached (always MISS)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-6.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "User-Agent, Accept" always;
    return 200 "ua-accept-varied content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["ua-accept-varied content", "ua-accept-varied content"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 7: Vary: Accept-Encoding, Cookie — mixed allowed/disallowed not cached
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-7.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Accept-Encoding, Cookie" always;
    return 200 "mixed vary data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["mixed vary data", "mixed vary data"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 8: No origin Vary — module emits its own and response is cached
This test catches ordering regression: if emit_vary runs before vary_uncacheable,
the module's own User-Agent token would poison the check.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-8.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "no-vary content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["no-vary content", "no-vary content"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[crit]


=== TEST 9: Vary token matching is case-insensitive
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-9.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "accept-encoding, ACCEPT-ENCODING" always;
    return 200 "lowercase-vary content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["lowercase-vary content", "lowercase-vary content"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[crit]


=== TEST 10: Multiple Vary headers — disallowed token in second header not cached
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-201-10.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Accept-Encoding" always;
    add_header Vary "Cookie" always;
    return 200 "multi-vary data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["multi-vary data", "multi-vary data"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]
