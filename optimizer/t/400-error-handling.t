# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volume for TEST 2 before tests. Since #1319
# the module opens the cache resolve-only (volume_size = 0) and cannot
# cold-create a volume — TEST 2 requires a volume that exists but has no
# entries. TEST 1's /nonexistent/path/cache.vol stays missing on purpose:
# it pins the misconfiguration contract.
BEGIN {
    system("bash t/seed-400.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: missing cache volume fails loud AND fail-open (resolve-only contract)
Since #1319 the module opens the cache resolve-only (volume_size = 0) and
cannot cold-create a volume. A missing volume is a misconfiguration: it must
log a loud [error] (after the delete-and-recreate recovery attempt) AND still
serve the request by proxying to upstream.
--- config
pagespeed on;
pagespeed_cache_path /nonexistent/path/cache.vol;

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
--- error_code: 200
--- error_log
cache open failed
PageSpeedCache::Create failed
--- no_error_log
[crit]
[alert]


=== TEST 2: query strings produce distinct cache entries
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-400-2.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "versioned content";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t?v=1
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[crit]
