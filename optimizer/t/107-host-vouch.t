# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# Agent optimization works only for hostnames the server block stands behind.
#
# The worker builds /llms.txt and renders pages by fetching from the request's
# hostname.  The module keeps that work to names the block lists as server
# names (exact, wildcard, regex) and names in the operator's host alias table:
#
#   - /llms.txt is served from the cache only under such a name, and with a
#     port only the one the request arrived on; under any other name or port
#     the request goes to the origin
#   - while agent optimization is on, an HTML response under any other name
#     is served and stored but not sent to the worker
#
# The prologue plays the optimizer: a shared config with both agent options
# on and a notify socket that nothing listens on, so every attempted
# notification logs a warning.  t/seed-107.sh stores an index under each
# hostname used below.

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

# The port the test server listens on, as t/seed-107.sh computes it.
our $port = $ENV{TEST_NGINX_SERVER_PORT} || 1984;
our $other_port = $port == 65000 ? 65001 : 65000;

BEGIN {
    system("bash t/seed-107.sh") == 0
        or die "Cache seeding failed";
}

{
    my $dir = "/tmp/psvouch-107";
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=true\n"
        . "agent_optimize_llms_txt_enabled=true\n";
    close($fh);
    open(my $hf, '>', "$dir/pagespeed-hosts.conf") or die $!;
    print $hf "aka.test=canon.test\n";
    close($hf);
}

our $config = <<'_EOC_';
server_name site.test *.wild.test ~^r[0-9]+\.regex\.test$;
pagespeed on;
pagespeed_cache_path /tmp/psvouch-107/cache.vol;

location = /llms.txt {
    proxy_pass http://127.0.0.1:$server_port/llms-origin;
}
location /llms-origin {
    pagespeed off;
    return 404 "origin";
}
location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "public, max-age=600";
    return 200 "<html><head><title>t</title></head><body>page</body></html>";
}
location /page {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: an exact server name gets the stored index
--- config eval: $::config
--- more_headers
Host: site.test
--- request
GET /llms.txt
--- response_body chomp
# index for site.test
--- error_code: 200


=== TEST 2: a wildcard server name gets the stored index
--- config eval: $::config
--- more_headers
Host: a.wild.test
--- request
GET /llms.txt
--- response_body chomp
# index for a.wild.test
--- error_code: 200


=== TEST 3: a regex server name gets the stored index
--- config eval: $::config
--- more_headers
Host: r7.regex.test
--- request
GET /llms.txt
--- response_body chomp
# index for r7.regex.test
--- error_code: 200


=== TEST 4: a name from the host alias table gets the stored index
--- config eval: $::config
--- more_headers
Host: aka.test
--- request
GET /llms.txt
--- response_body chomp
# index for canon.test
--- error_code: 200


=== TEST 5: a name no server name lists goes to the origin
--- config eval: $::config
--- more_headers
Host: unlisted.test
--- request
GET /llms.txt
--- response_body chomp
origin
--- error_code: 404


=== TEST 6: an address literal goes to the origin
--- config eval: $::config
--- more_headers
Host: 10.0.0.9
--- request
GET /llms.txt
--- response_body chomp
origin
--- error_code: 404


=== TEST 7: a listed name with the server's own port gets the stored index
--- config eval: $::config
--- more_headers eval
"Host: site.test:$::port"
--- request
GET /llms.txt
--- response_body eval
"# index for site.test:$::port"
--- error_code: 200


=== TEST 8: a listed name with another port goes to the origin
--- config eval: $::config
--- more_headers eval
"Host: site.test:$::other_port"
--- request
GET /llms.txt
--- response_body chomp
origin
--- error_code: 404


=== TEST 9: an HTML response under a listed name is sent to the worker
--- config eval: $::config
--- more_headers
Host: site.test
--- request
GET /page-listed.html
--- response_body chomp
<html><head><title>t</title></head><body>page</body></html>
--- error_code: 200
--- error_log
failed to notify worker


=== TEST 10: an HTML response under an unlisted name is served, not sent
--- config eval: $::config
--- more_headers
Host: unlisted.test
--- request
GET /page-unlisted.html
--- response_body chomp
<html><head><title>t</title></head><body>page</body></html>
--- error_code: 200
--- no_error_log
failed to notify worker
