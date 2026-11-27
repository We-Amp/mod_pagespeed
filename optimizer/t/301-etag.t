# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The pinned ETag hex values in this file are computed by the same code
# paths pinned in test/src/nginx/etag_util_test.cc — change them together.
use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Seed cache before tests
BEGIN {
    system("bash t/seed-301.sh") == 0
        or die "Cache seeding failed";
}

run_tests();

__DATA__

=== TEST 1: HIT ETag folds the stored content identity (hash A)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-a.css {
}
--- request
GET /hash-a.css
--- response_headers_like
X-PageSpeed: HIT
ETag: W/"ps-0000000800-aaaaaaaaaaaaaaaa-000000000000000d"
--- no_error_log
[error]


=== TEST 2: same length + mask, different content -> different ETag (hash B)
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-b.css {
}
--- request
GET /hash-b.css
--- response_headers_like
X-PageSpeed: HIT
ETag: W/"ps-0000000800-bbbbbbbbbbbbbbbb-000000000000000d"
--- no_error_log
[error]


=== TEST 3: unchanged content -> stable ETag across requests
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-a.css {
}
--- request
GET /hash-a.css
--- response_headers_like
ETag: W/"ps-0000000800-aaaaaaaaaaaaaaaa-000000000000000d"
--- no_error_log
[error]


=== TEST 4: missing identity (pre-v7 metadata) -> no ETag at all
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /legacy.css {
}
--- request
GET /legacy.css
--- response_headers
X-PageSpeed: HIT
!ETag
--- no_error_log
[error]


=== TEST 5: origin-validator identity (no hash) -> tier-2 tag
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /val.css {
}
--- request
GET /val.css
--- response_headers_like
X-PageSpeed: HIT
ETag: W/"ps-0000000800-a3eff4fa2b510343-000000000000000d"
--- no_error_log
[error]


=== TEST 6: If-None-Match with the current tag -> 304
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-a.css {
}
--- request
GET /hash-a.css
--- more_headers
If-None-Match: W/"ps-0000000800-aaaaaaaaaaaaaaaa-000000000000000d"
--- error_code: 304
--- no_error_log
[error]


=== TEST 7: If-None-Match with the pre-upgrade (legacy-format) tag -> full 200
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-a.css {
}
--- request
GET /hash-a.css
--- more_headers
If-None-Match: W/"ps-0000000800-000000000000000d"
--- error_code: 200
--- response_body chomp
h1{color:red}
--- no_error_log
[error]


=== TEST 8: If-None-Match with the OTHER revision's tag -> full 200
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /hash-a.css {
}
--- request
GET /hash-a.css
--- more_headers
If-None-Match: W/"ps-0000000800-bbbbbbbbbbbbbbbb-000000000000000d"
--- error_code: 200
--- response_body chomp
h1{color:red}
--- no_error_log
[error]


=== TEST 9: legacy entry has no validator -> no 304, full 200
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-301-1.vol;

location /legacy.css {
}
--- request
GET /legacy.css
--- more_headers
If-None-Match: W/"ps-0000000800-000000000000000d"
--- error_code: 200
--- response_body chomp
h1{color:red}
--- no_error_log
[error]
