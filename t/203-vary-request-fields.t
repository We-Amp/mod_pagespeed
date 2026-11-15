# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

# A response whose Vary names a request field must not be served from the
# cache to a client that sent a different value for that field.
#
# The cache keeps one origin response per URL, so the only request field it
# can honour in Vary is Accept-Encoding (the stored copy is the identity form
# and the coding is chosen per request).  Each test below sends two requests
# that differ only in the varied field to an origin that echoes that field in
# its body: the second client must get its own answer, not the first one's.
# The last two tests are the controls: responses that do not vary on the
# request (or vary only on Accept-Encoding) are still cached.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

BEGIN {
    system("bash t/seed-203.sh") == 0
        or die "Cache volume creation failed";
}

run_tests();

__DATA__

=== TEST 1: Vary: Accept answer is not served to a client with another Accept
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-1.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Accept" always;
    return 200 "accept=$http_accept";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["Accept: image/webp", "Accept: image/png"]
--- response_body eval
["accept=image/webp", "accept=image/png"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 2: Vary: User-Agent answer is not served to another User-Agent
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-2.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "User-Agent" always;
    return 200 "ua=$http_user_agent";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["User-Agent: client-one", "User-Agent: client-two"]
--- response_body eval
["ua=client-one", "ua=client-two"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 3: Vary: Save-Data answer is not served to a client without Save-Data
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-3.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Save-Data" always;
    return 200 "save-data=[$http_save_data]";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["Save-Data: on", ""]
--- response_body eval
["save-data=[on]", "save-data=[]"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 4: Vary: Origin answer is not served to a client from another Origin
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-4.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Origin" always;
    return 200 "origin=$http_origin";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["Origin: https://a.example", "Origin: https://b.example"]
--- response_body eval
["origin=https://a.example", "origin=https://b.example"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS"]
--- no_error_log
[crit]


=== TEST 5: control: a response without Vary is cached for every client
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-5.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "same for everyone";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["User-Agent: client-one\nAccept: image/webp", "User-Agent: client-two\nAccept: image/png"]
--- response_body eval
["same for everyone", "same for everyone"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[crit]


=== TEST 6: control: a Vary: Accept-Encoding response is cached
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-203-6.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    add_header Vary "Accept-Encoding" always;
    return 200 "coding-varied, same for everyone";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- pipelined_requests eval
["GET /t", "GET /t"]
--- more_headers eval
["User-Agent: client-one\nAccept: image/webp", "User-Agent: client-two\nAccept: image/png"]
--- response_body eval
["coding-varied, same for everyone", "coding-varied, same for everyone"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[crit]
