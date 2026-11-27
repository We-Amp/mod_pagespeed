# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Seed cache before tests
BEGIN {
    system("bash t/seed-300.sh") == 0
        or die "Cache seeding failed";
}

run_tests();

__DATA__

=== TEST 1: HIT serves cached content with correct headers
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-300-1.vol;

location /style.css {
    # No proxy needed — cache HIT serves directly from mmap
}
--- request
GET /style.css
--- response_body chomp
h1{color:red}
--- response_headers_like
X-PageSpeed: HIT
Content-Type: text/css
Cache-Control: .*max-age=\d+.*
ETag: W/"ps-.*
--- no_error_log
[error]


=== TEST 2: HIT includes Vary header for CSS
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-300-1.vol;

location /style.css {
}
--- request
GET /style.css
--- response_headers_like
Vary: Accept-Encoding
--- no_error_log
[error]


=== TEST 3: HIT includes Age header
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-300-1.vol;

location /style.css {
}
--- request
GET /style.css
--- response_headers_like
Age: \d+
--- no_error_log
[error]
