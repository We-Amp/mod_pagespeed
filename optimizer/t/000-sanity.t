# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: module loads and nginx starts
--- config
location /t {
    return 200 "ok";
}
--- request
GET /t
--- response_body chomp
ok
--- no_error_log
[error]
[crit]
[alert]


=== TEST 2: pagespeed directive accepted
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-000-2.vol;

location /t {
    return 200 "ok";
}
--- request
GET /t
--- error_code: 200
--- no_error_log
[crit]
[alert]
