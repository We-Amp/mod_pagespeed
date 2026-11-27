# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-202.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: application/json not recorded (kOther content type)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-202-1.vol;

location /origin {
    pagespeed off;
    default_type application/json;
    return 200 '{"data":1}';
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_body chomp
{"data":1}
--- error_code: 200
--- no_error_log
[crit]


=== TEST 2: font/woff2 not recorded
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-202-2.vol;

location /origin {
    pagespeed off;
    default_type font/woff2;
    return 200 "font-data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- response_body chomp
font-data
--- error_code: 200
--- no_error_log
[crit]
