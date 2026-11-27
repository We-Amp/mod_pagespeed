# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-002.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: server-level pagespeed on inherited by locations
Server-level directives at the top of --- config are placed inside
the default server{} block. Locations within inherit via merge_loc_conf.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-002-1.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "origin content";
}
location /t {
    # pagespeed inherited from server level
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_headers_like
X-PageSpeed: (MISS|HIT)
--- no_error_log
[error]


=== TEST 2: location-level pagespeed off overrides server-level on
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-002-2.vol;

location /t {
    pagespeed off;
    return 200 "unprocessed";
}
--- request
GET /t
--- response_headers
!X-PageSpeed
--- response_body chomp
unprocessed
--- no_error_log
[error]


=== TEST 3: pagespeed_max_age overridden at location level
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-002-3.vol;
pagespeed_max_age 86400;

location /t {
    pagespeed_max_age 300;
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]
