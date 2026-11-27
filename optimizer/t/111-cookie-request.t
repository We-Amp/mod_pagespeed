# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# A stored page, or its Markdown copy, is served to a request carrying a
# Cookie header only when the stored page is marked public or s-maxage.
# Without a cookie the same entries are served as before.  What the cache
# derives from a stored page and keeps without a mark of its own (the Early
# Hints set, the /llms.txt index) follows the same rule.  t/seed-111.sh
# stores the entries; every miss goes to the /origin location.
#
# The prologue plays the optimizer: a shared config with agent optimization
# on (so an `Accept: text/markdown` request may select the Markdown copy) and
# a notify socket nothing listens on.

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

BEGIN {
    system("bash t/seed-111.sh") == 0
        or die "Cache seeding failed";
}

{
    my $dir = "/tmp/pscookie-111";
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=true\n"
        . "agent_optimize_llms_txt_enabled=true\n";
    close($fh);
}

# The origin answers with no-store so that no miss below is recorded over a
# seeded entry: every test reads the seeds as stored.
our $config = <<'_EOC_';
server_name localhost;
pagespeed on;
pagespeed_cache_path /tmp/pscookie-111/cache.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-store";
    return 200 "from origin";
}
location ~ ^/(plain|public|smaxage|mustrev|md-plain|md-public|smaxage-zero|smaxage-dup|hints-nopage|hints-unmarked|llms\.txt)$ {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: a stored page without a sharing mark is served without a cookie
--- config eval: $::config
--- request
GET /plain
--- response_body chomp
<html><body>stored plain</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 2: a stored page without a sharing mark is not served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /plain
--- response_body chomp
from origin
--- response_headers
X-PageSpeed: MISS


=== TEST 3: the obsolete Cookie2 header counts as a cookie
--- config eval: $::config
--- more_headers
Cookie2: $Version="1"
--- request
GET /plain
--- response_body chomp
from origin


=== TEST 4: a public page is served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /public
--- response_body chomp
<html><body>stored public</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 5: an s-maxage page is served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /smaxage
--- response_body chomp
<html><body>stored smaxage</body></html>
--- response_headers
X-PageSpeed: HIT


=== TEST 6: must-revalidate does not mark a page shareable
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /mustrev
--- response_body chomp
from origin


=== TEST 7: a Markdown copy is served to an agent without a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /md-plain
--- response_body chomp
# markdown /md-plain


=== TEST 8: a Markdown copy of an unmarked page is not served to a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
Cookie: session=one
--- request
GET /md-plain
--- response_body chomp
from origin


=== TEST 9: a Markdown copy of a public page is served to a cookie
--- config eval: $::config
--- more_headers
Accept: text/markdown
Cookie: session=one
--- request
GET /md-public
--- response_body chomp
# markdown /md-public


=== TEST 10: an empty Cookie header counts as a cookie
--- config eval: $::config
--- raw_request eval
"GET /plain HTTP/1.1\r
Host: localhost\r
Cookie:\r
Connection: close\r
\r
"
--- response_body chomp
from origin


=== TEST 11: s-maxage=0 is not a sharing mark
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /smaxage-zero
--- response_body chomp
from origin


=== TEST 12: a miss without a cookie gets the stored Early Hints
--- config eval: $::config
--- raw_request eval
"GET /hints-nopage HTTP/1.1\r
Host: localhost\r
Accept: text/html\r
Connection: close\r
\r
"
--- raw_response_headers_like: ^HTTP/1\.1 103
--- error_code: 103
--- ignore_response_body


=== TEST 13: a cookie miss with no stored page gets no Early Hints
--- config eval: $::config
--- raw_request eval
"GET /hints-nopage HTTP/1.1\r
Host: localhost\r
Accept: text/html\r
Cookie: session=one\r
Connection: close\r
\r
"
--- raw_response_headers_unlike: ^HTTP/1\.1 103
--- response_body chomp
from origin


=== TEST 14: a cookie miss on an unmarked page gets no Early Hints
--- config eval: $::config
--- raw_request eval
"GET /hints-unmarked HTTP/1.1\r
Host: localhost\r
Accept: text/html\r
Cookie: session=one\r
Connection: close\r
\r
"
--- raw_response_headers_unlike: ^HTTP/1\.1 103
--- response_body chomp
from origin


=== TEST 15: the stored /llms.txt index is served without a cookie
--- config eval: $::config
--- request
GET /llms.txt
--- response_body chomp
# stored index


=== TEST 16: the stored /llms.txt index is not served to a cookie
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /llms.txt
--- response_body chomp
from origin


=== TEST 17: over HTTP/2 a stored page is served without a cookie
--- config eval: $::config
--- http2
--- request
GET /plain
--- response_body chomp
<html><body>stored plain</body></html>


=== TEST 18: over HTTP/2 an unmarked page is not served to a cookie
--- config eval: $::config
--- http2
--- more_headers
Cookie: session=one
--- request
GET /plain
--- response_body chomp
from origin


=== TEST 19: a repeated s-maxage is not a sharing mark
--- config eval: $::config
--- more_headers
Cookie: session=one
--- request
GET /smaxage-dup
--- response_body chomp
from origin
