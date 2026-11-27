# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# Cache-directory generation check, end to end against a running nginx.
#
# The optimizer publishes the generation it was compiled with as
# cache_dir_generation= in pagespeed-shared.conf, beside the cache volume.
# The prologue below plays that optimizer: one cache-parent directory per
# case, each with a shared config stating this build's generation, an older
# one, a newer one, or none (an optimizer predating the field).  Every volume
# holds /style.css (t/seed-106.sh), so a request for it is a HIT exactly when
# the module uses the cache.
#
#   match     -> the cache is used and recorded to, as always
#   mismatch  -> one error line; requests pass through uncached (no HIT, no
#                recording) and the module keeps serving
#   none      -> one warning; the cache is used as before
#   recovery  -> the optimizer rewrites its shared config with the matching
#                generation and the running nginx re-enables the cache
#                without a reload
#
# The module's generation is read from src/worker/shared_config.h (the
# constant the module is compiled with), so a generation bump does not
# break these tests.

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
our $older = $gen - 1;
our $newer = $gen + 1;

sub write_shared_conf {
    my ($name, $body) = @_;
    my $dir = "/tmp/psgen-106-$name";
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\n$body";
    close($fh);
}

write_shared_conf('match', "cache_dir_generation=$gen\n");
write_shared_conf('older', "cache_dir_generation=$older\n");
write_shared_conf('newer', "cache_dir_generation=$newer\n");
write_shared_conf('none', "");
write_shared_conf('recover', "cache_dir_generation=$older\n");

BEGIN {
    system("bash t/seed-106.sh") == 0
        or die "Cache seeding failed";
}

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: matching generation - the cache is used and recorded to
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-match/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /gen {
    return 200 "$pagespeed_cache_generation $pagespeed_cache_generation_module $pagespeed_cache_generation_optimizer";
}
--- pipelined_requests eval
["GET /style.css", "GET /t", "GET /t", "GET /gen"]
--- response_body eval
["h1{color:red}", "from-origin", "from-origin", "match $::gen $::gen"]
--- response_headers_like eval
["X-PageSpeed: HIT", "X-PageSpeed: MISS", "X-PageSpeed: HIT", ""]
--- no_error_log
[error]
cannot verify the cache-directory generation


=== TEST 2: older optimizer - one error, pass-through, nothing read or recorded
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-older/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /gen {
    return 200 "$pagespeed_cache_generation $pagespeed_cache_generation_module $pagespeed_cache_generation_optimizer";
}
--- pipelined_requests eval
["GET /style.css", "GET /t", "GET /t", "GET /gen"]
--- response_body eval
["from-origin", "from-origin", "from-origin", "mismatch $::gen $::older"]
--- response_headers_like eval
["X-PageSpeed: MISS", "X-PageSpeed: MISS", "X-PageSpeed: MISS", ""]
--- no_error_log
failed to open cache
[crit]
[alert]


=== TEST 3: older optimizer - exactly one error line, naming both sides
(Test::Nginx checks the error log slice of each pipelined response
separately, so the line count is asserted on a single request: the slice
then holds the start-up line and anything the request added.)
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-older/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /style.css
--- response_body chomp
from-origin
--- grep_error_log eval
qr/cache-directory generation mismatch: this module is generation \d+, the optimizer \(\S+\) is generation \d+ -- the optimizer is older than this module/
--- grep_error_log_out eval
"cache-directory generation mismatch: this module is generation $::gen, the optimizer (/tmp/psgen-106-older/pagespeed-shared.conf) is generation $::older -- the optimizer is older than this module\n"
--- no_error_log
failed to open cache
[crit]
[alert]


=== TEST 4: newer optimizer - the error names the module as the older side
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-newer/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /gen {
    return 200 "$pagespeed_cache_generation $pagespeed_cache_generation_module $pagespeed_cache_generation_optimizer";
}
--- request
GET /gen
--- response_body eval
"mismatch $::gen $::newer"
--- error_log
this module is older than the optimizer
upgrade the optimizer and the nginx module to the same release
--- no_error_log
failed to open cache
[crit]


=== TEST 5: optimizer states no generation - cache used as before
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-none/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /gen {
    return 200 "$pagespeed_cache_generation $pagespeed_cache_generation_module [$pagespeed_cache_generation_optimizer]";
}
--- pipelined_requests eval
["GET /style.css", "GET /gen"]
--- response_body eval
["h1{color:red}", "unknown $::gen []"]
--- response_headers_like eval
["X-PageSpeed: HIT", ""]
--- no_error_log
[error]


=== TEST 6: optimizer states no generation - exactly one warning
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-none/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /style.css
--- response_body chomp
h1{color:red}
--- response_headers
X-PageSpeed: HIT
--- grep_error_log chop
cannot verify the cache-directory generation
--- grep_error_log_out
cannot verify the cache-directory generation
--- no_error_log
[error]


=== TEST 7: completed upgrade re-enables the cache without a reload
nginx starts while the optimizer is one generation behind (cache disabled,
see /gen); /flip then plays the optimizer being upgraded and rewriting its
shared config (appending the matching generation; the parser keeps the last
value).  The next request's shared-config poll picks that up and the seeded
entry is served from the cache again.  No pagespeed request runs before the
flip: the poll is throttled to once a second, and the pipelined requests all
land within one.  TEST 2 covers the pass-through before it.
--- http_config
log_format psgen106flip "cache_dir_generation=$pagespeed_cache_generation_module";
--- config
pagespeed on;
pagespeed_cache_path /tmp/psgen-106-recover/cache.vol;

location /origin {
    pagespeed off;
    default_type text/css;
    return 200 "from-origin";
}
location = /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /style.css {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location = /flip {
    pagespeed off;
    access_log /tmp/psgen-106-recover/pagespeed-shared.conf psgen106flip;
    return 200 "flipped";
}
location = /gen {
    return 200 "$pagespeed_cache_generation";
}
--- pipelined_requests eval
["GET /gen", "GET /flip", "GET /style.css", "GET /gen"]
--- response_body eval
["mismatch", "flipped", "h1{color:red}", "match"]
--- response_headers_like eval
["", "", "X-PageSpeed: HIT", ""]
--- no_error_log
failed to open cache
[crit]
