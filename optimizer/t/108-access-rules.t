# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# The server's access rules (allow/deny, auth_request, auth_basic) decide
# before a cached response is served: a refused client gets the refusal, an
# admitted client still gets the cached copy.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

BEGIN {
    system("bash t/seed-108.sh") == 0
        or die "Cache seeding failed";
}

run_tests();

__DATA__

=== TEST 1: deny refuses a client for a cached URL
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-1.vol;

location /style.css {
    deny all;
}
--- request
GET /style.css
--- error_code: 403
--- response_headers
!X-PageSpeed
--- error_log
access forbidden by rule


=== TEST 2: an allowed client still gets the cached copy
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-1.vol;

location /style.css {
    allow 127.0.0.1;
    deny all;
}
--- request
GET /style.css
--- error_code: 200
--- response_body chomp
p{color:blue}
--- response_headers
X-PageSpeed: HIT
--- no_error_log
[error]


=== TEST 3: auth_request refuses a client for a cached URL
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-1.vol;

location /style.css {
    auth_request /access-check;
}
location = /access-check {
    internal;
    pagespeed off;
    return 403;
}
--- request
GET /style.css
--- error_code: 403
--- response_headers
!X-PageSpeed
--- no_error_log
[crit]
[alert]


=== TEST 4: auth_request admitting a client still gets the cached copy
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-1.vol;

location /style.css {
    auth_request /access-check;
}
location = /access-check {
    internal;
    pagespeed off;
    return 204;
}
--- request
GET /style.css
--- error_code: 200
--- response_body chomp
p{color:blue}
--- response_headers
X-PageSpeed: HIT
--- no_error_log
[error]


=== TEST 5: auth_basic refuses a client without credentials for a cached URL
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-1.vol;

location /style.css {
    auth_basic "members";
    auth_basic_user_file ../html/htpasswd;
}
--- user_files
>>> htpasswd
reader:{PLAIN}letmein
--- request
GET /style.css
--- error_code: 401
--- response_headers
!X-PageSpeed
WWW-Authenticate: Basic realm="members"
--- no_error_log
[crit]
[alert]


=== TEST 6: auth_basic refuses a cached response the origin marked public
The first two requests carry valid credentials: the origin's public response
is stored and then served from the cache.  A wrong password and a missing one
are still refused.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-108-6.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Cache-Control "public, max-age=600";
    return 200 "p{margin:0}";
}
location /shared.css {
    auth_basic "members";
    auth_basic_user_file ../html/htpasswd;
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- user_files
>>> htpasswd
reader:{PLAIN}letmein
--- pipelined_requests eval
["GET /shared.css", "GET /shared.css", "GET /shared.css", "GET /shared.css"]
--- more_headers eval
["Authorization: Basic cmVhZGVyOmxldG1laW4=",
 "Authorization: Basic cmVhZGVyOmxldG1laW4=",
 "Authorization: Basic cmVhZGVyOndyb25ncGFzcw==",
 ""]
--- error_code eval
[200, 200, 401, 401]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT",
 "WWW-Authenticate: Basic realm=\"members\"",
 "WWW-Authenticate: Basic realm=\"members\""]
--- no_error_log
[crit]
[alert]
