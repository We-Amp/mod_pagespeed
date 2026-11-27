# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# The worker's agent fetches (agent_optimize render, llms.txt synthesis) carry
# its User-Agent marker and, when the worker shares the host, arrive from
# loopback.  A rule that admits local clients must not admit them: they are
# presented to access rules, $remote_addr and the access log as 224.0.0.0 for
# the request, and the connection's next request gets its real address back.
# A local client without the marker is unaffected.  The held page is a static
# file: `return` would answer in the rewrite phase, before any access rule.  (Test::Nginx connects from
# 127.0.0.1; a remote client copying the marker is covered by the unit test
# test/src/nginx/agent_self_fetch_test.cc.)

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

our $AgentUA = 'Mozilla/5.0 (compatible; mod_pagespeed agent_optimize)';

run_tests();

__DATA__

=== TEST 1: a local-only location refuses the agent fetch from loopback
--- config
location /local-only/ {
    allow 127.0.0.1;
    deny all;
}
--- user_files
>>> local-only/page.html
held-af-110
--- request
GET /local-only/page.html
--- more_headers eval
"User-Agent: $::AgentUA"
--- error_code: 403
--- response_body_unlike: held-af-110
--- error_log
access forbidden by rule


=== TEST 2: a local-only location admits a loopback request without the marker
--- config
location /local-only/ {
    allow 127.0.0.1;
    deny all;
}
--- user_files
>>> local-only/page.html
held-af-110
--- request
GET /local-only/page.html
--- more_headers
User-Agent: Mozilla/5.0 (X11; Linux x86_64) Firefox/130.0
--- error_code: 200
--- response_body
held-af-110
--- no_error_log
[error]


=== TEST 3: the same refusal with pagespeed on in the location
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-110-3.vol;

location /local-only/ {
    allow 127.0.0.1;
    deny all;
}
--- user_files
>>> local-only/page.html
held-af-110
--- request
GET /local-only/page.html
--- more_headers eval
"User-Agent: $::AgentUA"
--- error_code: 403
--- response_body_unlike: held-af-110
--- error_log
access forbidden by rule
--- no_error_log
[crit]
[alert]


=== TEST 4: the agent fetch reads as 224.0.0.0, and the next request on the connection as itself
--- config
location /addr {
    default_type text/plain;
    return 200 "$remote_addr\n";
}
--- pipelined_requests eval
["GET /addr", "GET /addr", "GET /addr"]
--- more_headers eval
["User-Agent: $::AgentUA",
 "User-Agent: curl/8.5.0",
 "User-Agent: $::AgentUA"]
--- error_code eval
[200, 200, 200]
--- response_body eval
["224.0.0.0\n", "127.0.0.1\n", "224.0.0.0\n"]
--- no_error_log
[error]


=== TEST 5: the access log records the agent fetch as 224.0.0.0
--- http_config
log_format af110 "af110 $remote_addr $http_user_agent";
--- config
location /logged {
    access_log logs/error.log af110;
    default_type text/plain;
    return 200 "ok";
}
--- request
GET /logged
--- more_headers eval
"User-Agent: $::AgentUA"
--- error_code: 200
--- error_log eval
qr/af110 224\.0\.0\.0 Mozilla\/5\.0 \(compatible; mod_pagespeed agent_optimize\)/
--- no_error_log
af110 127.0.0.1
