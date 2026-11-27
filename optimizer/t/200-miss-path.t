# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-200.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: cold miss proxies to upstream and adds X-PageSpeed: MISS
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-200-1.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "original CSS content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_body chomp
original CSS content
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]
[crit]


=== TEST 2: cold miss preserves origin Content-Type
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-200-2.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "<html><body>hi</body></html>";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_headers_like
Content-Type: text/html
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 3: second GET to same URL hits cache (pipelined)
Resolve-only still records on miss once the volume exists: first GET is a
MISS (recorded), the pipelined second GET is a HIT.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-200-3.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "cached content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- response_body chomp eval
["cached content", "cached content"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[error]
[crit]
