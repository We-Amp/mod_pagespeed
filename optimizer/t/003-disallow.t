# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-003.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: disallowed URL prefix bypasses pagespeed
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-003-1.vol;
pagespeed_disallow /api/;

location /api/data {
    return 200 '{"ok":true}';
}
--- request
GET /api/data
--- response_headers
!X-PageSpeed
--- response_body chomp
{"ok":true}
--- no_error_log
[error]


=== TEST 2: non-disallowed URL is processed
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-003-2.vol;
pagespeed_disallow /api/;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "body";
}
location /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /style.css
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 3: multiple disallow patterns
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-003-3.vol;
pagespeed_disallow /api/;
pagespeed_disallow /admin/;

location /api/data {
    return 200 "api";
}
location /admin/dashboard {
    return 200 "admin";
}
--- request
GET /admin/dashboard
--- response_headers
!X-PageSpeed
--- response_body chomp
admin
--- no_error_log
[error]
