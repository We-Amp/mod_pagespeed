# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: pagespeed off by default — no X-PageSpeed header
--- config
location /t {
    return 200 "ok";
}
--- request
GET /t
--- response_headers
!X-PageSpeed
--- response_body chomp
ok
--- no_error_log
[error]


=== TEST 2: pagespeed_hot_threshold accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-2.vol;
pagespeed_hot_threshold 10;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 3: pagespeed_synthesize_swr accepts on/off
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-3.vol;
pagespeed_synthesize_swr off;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 4: pagespeed_conditional_revalidation accepts on/off
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-4.vol;
pagespeed_conditional_revalidation off;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 5: pagespeed_html_max_age accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-6.vol;
pagespeed_html_max_age 600;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 6: pagespeed_css_max_age accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-7.vol;
pagespeed_css_max_age 3600;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 7: pagespeed_image_max_age accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-8.vol;
pagespeed_image_max_age 7200;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 8: pagespeed_max_age accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-9.vol;
pagespeed_max_age 43200;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]


=== TEST 9: pagespeed_immutable_max_age accepts integer
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-001-10.vol;
pagespeed_immutable_max_age 2592000;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[error]
