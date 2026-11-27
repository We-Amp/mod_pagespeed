# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Each server block keeps its own cache entries, also when several blocks
# share one cache path and list the same server name.
#
# Tests 1-2: the Test::Nginx block stores /doc.css; other blocks on their own
# listeners, one with the same server name and one with another, have an
# origin of their own for the same URL.  The Test::Nginx block reaches them
# through /via-same/ and /via-other/, with the Host header the test chooses.
#
# The other tests run each case twice against the Test::Nginx block: with a
# cache holding its own entries (t/seed-112.sh, /tmp/ps-112-own), and with a
# cache holding the same entries for the other block that shares the path
# (/tmp/ps-112-other).  Only its own entries are used: a stored Markdown copy,
# the /llms.txt index, Early Hints, and an expired entry served when the
# origin fails.  The prologue writes a shared config with agent optimization
# on and a notify socket nothing listens on.

use Test::Nginx::Socket 'no_plan';

our $port = $ENV{TEST_NGINX_SERVER_PORT} || $ENV{TEST_NGINX_PORT} || 1984;
our $same_port = $port + 31;
our $other_port = $port + 32;

BEGIN {
    system("bash t/seed-112.sh") == 0
        or die "Cache seeding failed";
}

use File::Path qw(make_path);

open(my $sc, '<', 'src/worker/shared_config.h') or die "shared_config.h: $!";
our $gen;
while (<$sc>) {
    if (/kCacheDirGeneration\s*=\s*(\d+)\s*;/) {
        $gen = $1;
        last;
    }
}
close($sc);
die "kCacheDirGeneration not found" unless $gen;

for my $dir ('/tmp/ps-112-own', '/tmp/ps-112-other') {
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=true\n"
        . "agent_optimize_llms_txt_enabled=true\n";
    close($fh);
}

sub other_blocks {
    my ($vol) = @_;
    my $out = '';
    for my $b ([$same_port, 'localhost', 'p{color:green}'],
               [$other_port, 'other.test', 'p{color:red}']) {
        my ($p, $name, $body) = @$b;
        $out .= <<"_EOB_";
server {
    listen 127.0.0.1:$p;
    server_name $name;
    pagespeed on;
    pagespeed_cache_path $vol;
    location /origin {
        pagespeed off;
        default_type text/css;
        add_header Cache-Control "public, max-age=600";
        return 200 "$body";
    }
    location /doc.css {
        proxy_pass http://127.0.0.1:$p/origin;
        proxy_set_header Accept-Encoding "";
    }
}
_EOB_
    }
    return $out;
}

sub main_config {
    my ($vol) = @_;
    return <<"_EOC_";
pagespeed on;
pagespeed_cache_path $vol;
location /origin {
    pagespeed off;
    default_type text/css;
    add_header Cache-Control "public, max-age=600";
    return 200 "p{color:blue}";
}
location /doc.css {
    proxy_pass http://127.0.0.1:\$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location /via-same/ {
    pagespeed off;
    proxy_pass http://127.0.0.1:$same_port/;
    proxy_set_header Host \$http_x_test_host;
}
location /via-other/ {
    pagespeed off;
    proxy_pass http://127.0.0.1:$other_port/;
    proxy_set_header Host \$http_x_test_host;
}
_EOC_
}

# Test 11: both blocks store /v.css; an origin that answers with a Vary the
# cache does not store (when the request carries X-Vary-Now) makes a block
# drop the URL's entries.  The other block listens on $same_port with the
# same server name and has pagespeed_force_refresh on, so a no-cache request
# reaches its origin.
sub vary_blocks {
    my ($vol) = @_;
    return <<"_EOB_";
server {
    listen 127.0.0.1:$same_port;
    server_name localhost;
    pagespeed on;
    pagespeed_cache_path $vol;
    pagespeed_force_refresh on;
    location /origin {
        pagespeed off;
        default_type text/css;
        add_header Cache-Control "public, max-age=600";
        if (\$http_x_vary_now) {
            add_header Cache-Control "public, max-age=600";
            add_header Vary "Cookie";
        }
        return 200 "p{color:green}";
    }
    location /v.css {
        proxy_pass http://127.0.0.1:$same_port/origin;
        proxy_set_header Accept-Encoding "";
    }
}
_EOB_
}

sub vary_config {
    my ($vol) = @_;
    return <<"_EOC_";
pagespeed on;
pagespeed_cache_path $vol;
location /origin {
    pagespeed off;
    default_type text/css;
    add_header Cache-Control "public, max-age=600";
    return 200 "p{color:blue}";
}
location /v.css {
    proxy_pass http://127.0.0.1:\$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location /via-same/ {
    pagespeed off;
    proxy_pass http://127.0.0.1:$same_port/;
    proxy_set_header Host localhost;
}
_EOC_
}

# The stored-entry tests: the origin answers no-store, and fails for the
# stylesheet, so nothing a miss fetches is stored over a seeded entry.
sub stored_config {
    my ($dir) = @_;
    return <<"_EOC_";
pagespeed on;
pagespeed_cache_path $dir/cache.vol;
location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-store";
    return 200 "from origin";
}
location /failing {
    pagespeed off;
    return 500 "origin failed";
}
location ~ ^/(md|llms\\.txt|hints)\$ {
    proxy_pass http://127.0.0.1:\$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /stale.css {
    proxy_pass http://127.0.0.1:\$server_port/failing;
    proxy_set_header Accept-Encoding "";
}
_EOC_
}

our $http_config_1 = other_blocks('/tmp/test-ps-112-1.vol');
our $config_1 = main_config('/tmp/test-ps-112-1.vol');
our $http_config_2 = other_blocks('/tmp/test-ps-112-2.vol');
our $config_2 = main_config('/tmp/test-ps-112-2.vol');
our $http_config_vary = vary_blocks('/tmp/test-ps-112-3.vol');
our $config_vary = vary_config('/tmp/test-ps-112-3.vol');
our $http_config_own = other_blocks('/tmp/ps-112-own/cache.vol');
our $config_own = stored_config('/tmp/ps-112-own');
our $http_config_other = other_blocks('/tmp/ps-112-other/cache.vol');
our $config_other = stored_config('/tmp/ps-112-other');

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: a block with the same server name on another listener keeps its own entries
The first two requests store and serve the entry in the Test::Nginx block.
The block on the other listener then answers from its own origin, and serves
its own stored copy after that.
--- http_config eval: $::http_config_1
--- config eval: $::config_1
--- pipelined_requests eval
["GET /doc.css", "GET /doc.css", "GET /via-same/doc.css", "GET /via-same/doc.css"]
--- more_headers
X-Test-Host: localhost
--- error_code eval
[200, 200, 200, 200]
--- response_body eval
["p{color:blue}", "p{color:blue}", "p{color:green}", "p{color:green}"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT", "X-PageSpeed: MISS",
 "X-PageSpeed: HIT"]
--- no_error_log
[error]


=== TEST 2: a block with another server name keeps its own entries
The block on the other listener lists another server name and answers every
request on its listener; the request names the Test::Nginx block's host.
--- http_config eval: $::http_config_2
--- config eval: $::config_2
--- pipelined_requests eval
["GET /doc.css", "GET /doc.css", "GET /via-other/doc.css", "GET /via-other/doc.css"]
--- more_headers
X-Test-Host: localhost
--- error_code eval
[200, 200, 200, 200]
--- response_body eval
["p{color:blue}", "p{color:blue}", "p{color:red}", "p{color:red}"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: HIT", "X-PageSpeed: MISS",
 "X-PageSpeed: HIT"]
--- no_error_log
[error]


=== TEST 3: the block's own Markdown copy is served to an agent
--- http_config eval: $::http_config_own
--- config eval: $::config_own
--- more_headers
Accept: text/markdown
--- request
GET /md
--- response_body chomp
# markdown


=== TEST 4: a Markdown copy stored for another block is not used
--- http_config eval: $::http_config_other
--- config eval: $::config_other
--- more_headers
Accept: text/markdown
--- request
GET /md
--- response_body chomp
from origin


=== TEST 5: the block's own /llms.txt index is served
--- http_config eval: $::http_config_own
--- config eval: $::config_own
--- request
GET /llms.txt
--- response_body chomp
# stored index


=== TEST 6: a /llms.txt index stored for another block is not used
--- http_config eval: $::http_config_other
--- config eval: $::config_other
--- request
GET /llms.txt
--- response_body chomp
from origin


=== TEST 7: a miss gets the block's own Early Hints
--- http_config eval: $::http_config_own
--- config eval: $::config_own
--- raw_request eval
"GET /hints HTTP/1.1\r
Host: localhost\r
Accept: text/html\r
Connection: close\r
\r
"
--- raw_response_headers_like: ^HTTP/1\.1 103
--- error_code: 103
--- ignore_response_body


=== TEST 8: Early Hints stored for another block are not used
--- http_config eval: $::http_config_other
--- config eval: $::config_other
--- raw_request eval
"GET /hints HTTP/1.1\r
Host: localhost\r
Accept: text/html\r
Connection: close\r
\r
"
--- raw_response_headers_unlike: ^HTTP/1\.1 103
--- response_body chomp
from origin


=== TEST 9: the block's own expired entry is served when the origin fails
--- http_config eval: $::http_config_own
--- config eval: $::config_own
--- request
GET /stale.css
--- error_code: 200
--- response_body chomp
p{color:gray}


=== TEST 10: an expired entry stored for another block is not used when the origin fails
--- http_config eval: $::http_config_other
--- config eval: $::config_other
--- request
GET /stale.css
--- error_code: 500
--- response_body_like: origin failed


=== TEST 11: a block that drops a URL for its origin's Vary drops only its own entries
Both blocks store /v.css.  The other block's origin then answers with a Vary
the cache does not store: that block drops its entries and fetches again,
while the Test::Nginx block still serves its own.
--- http_config eval: $::http_config_vary
--- config eval: $::config_vary
--- pipelined_requests eval
["GET /v.css", "GET /via-same/v.css", "GET /via-same/v.css",
 "GET /via-same/v.css", "GET /v.css"]
--- more_headers eval
["", "", "X-Vary-Now: 1\nCache-Control: no-cache", "", ""]
--- error_code eval
[200, 200, 200, 200, 200]
--- response_body eval
["p{color:blue}", "p{color:green}", "p{color:green}", "p{color:green}",
 "p{color:blue}"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS", "X-PageSpeed: MISS",
 "X-PageSpeed: MISS", "X-PageSpeed: HIT"]
--- no_error_log
[error]
