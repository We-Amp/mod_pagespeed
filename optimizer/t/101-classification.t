# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the MISS path requires a volume that exists but has no entries.
BEGIN {
    system("bash t/seed-101.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: WebP Accept header does not crash (cold miss)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-101-1.vol;

location /origin {
    pagespeed off;
    default_type image/jpeg;
    return 200 "image data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Accept: image/webp,image/png,*/*
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 2: AVIF Accept header does not crash
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-101-2.vol;

location /origin {
    pagespeed off;
    default_type image/jpeg;
    return 200 "image data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Accept: image/avif,image/webp,*/*
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]


=== TEST 3: Save-Data header does not crash
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-101-3.vol;

location /origin {
    pagespeed off;
    default_type image/jpeg;
    return 200 "image data";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers
Save-Data: on
--- response_headers
X-PageSpeed: MISS
--- no_error_log
[error]
