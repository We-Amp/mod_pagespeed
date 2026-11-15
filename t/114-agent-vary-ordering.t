# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# With agent optimization on, the module adds `Accept` to the Vary of every
# HTML response.  The store side must never read that line as the origin's:
# the origin-header snapshot and the Vary classification of the miss, the
# 304 revalidation and the stale refetch all have to run before the module
# emits its own Vary.  If one of them saw the module's `Accept`, the entry
# would be stored as one the origin negotiates on Accept, and would keep
# saying so after agent optimization is turned off.
#
# Phase 1 (agent optimization on) takes each path once.  Phase 2 turns agent
# optimization off and reads the same entries back: none may name Accept.
# t/seed-114.sh stores the entries that need a validator or a stale copy.
#
# The cache only stores a response whose Vary names nothing but
# Accept-Encoding, so a store side that read our line (which names User-Agent
# and Accept) would also refuse the page outright.  /rec pins that directly:
# recorded by a miss, revalidated by a 304, served from the cache after each,
# with its stored header block read back by `seed_test_cache --dump`.

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
    system("bash t/seed-114.sh") == 0
        or die "Cache seeding failed";
}

our $dir = "/tmp/psvaryord-114";

# Write the optimizer's shared config with agent optimization on or off.
# The mtime is set explicitly so a running nginx always sees the change.
our $stamp = time() - 1000;
sub write_shared_conf {
    my ($agent) = @_;
    make_path($dir);
    my $path = "$dir/pagespeed-shared.conf";
    open(my $fh, '>', $path) or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=" . ($agent ? "true" : "false") . "\n";
    close($fh);
    $stamp += 10;
    utime($stamp, $stamp, $path) or die "utime: $!";
}

write_shared_conf(1);

# Write what the cache holds for URL to $dir/dumps/NAME.txt, which the
# /dump/ location serves.
sub dump_entry {
    my ($url, $name) = @_;
    make_path("$dir/dumps");
    my $seed = $ENV{SEED_TEST_CACHE} || "bazel-bin/tools/seed_test_cache";
    my $port = $ENV{TEST_NGINX_SERVER_PORT} || $ENV{TEST_NGINX_PORT} || 1984;
    system("$seed --dump --site_listen 0.0.0.0:$port --site_names localhost "
        . "--cache $dir/cache.vol --url $url --host localhost "
        . "--scheme http > $dir/dumps/$name.txt 2>&1") == 0
        or die "dump of $url failed";
}

# Pages that are not seeded are recorded by their first miss.
our $config = <<'_EOC_';
server_name localhost;
pagespeed on;
pagespeed_cache_path /tmp/psvaryord-114/cache.vol;

location /origin-page {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "max-age=600";
    return 200 "<html><body>from origin</body></html>";
}
location /origin-304 {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-cache";
    add_header ETag '"v1"';
    if ($http_if_none_match) {
        return 304;
    }
    return 200 "<html><body>changed</body></html>";
}
location /origin-304-vary {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-cache";
    add_header ETag '"v1"';
    add_header Vary "Accept";
    if ($http_if_none_match) {
        return 304;
    }
    return 200 "<html><body>changed</body></html>";
}
location /origin-rec {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "max-age=4";
    add_header ETag '"r1"';
    add_header Referrer-Policy "no-referrer";
    if ($http_if_none_match) {
        return 304;
    }
    return 200 "<html><body>recorded</body></html>";
}
location /origin-vary-accept {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "max-age=600";
    add_header Vary "Accept";
    return 200 "<html><body>negotiated</body></html>";
}
location /dump/ {
    pagespeed off;
    default_type text/plain;
    alias /tmp/psvaryord-114/dumps/;
}
location = /rec {
    proxy_pass http://127.0.0.1:$server_port/origin-rec;
    proxy_set_header Accept-Encoding "";
}
location = /vary-accept {
    proxy_pass http://127.0.0.1:$server_port/origin-vary-accept;
    proxy_set_header Accept-Encoding "";
}
location ~ ^/(miss|refetch)$ {
    proxy_pass http://127.0.0.1:$server_port/origin-page;
    proxy_set_header Accept-Encoding "";
}
location = /reval {
    proxy_pass http://127.0.0.1:$server_port/origin-304;
    proxy_set_header Accept-Encoding "";
}
location = /reval-vary {
    proxy_pass http://127.0.0.1:$server_port/origin-304-vary;
    proxy_set_header Accept-Encoding "";
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: phase 1, a miss is recorded and its Vary names Accept
--- config eval: $::config
--- request
GET /miss
--- response_body chomp
<html><body>from origin</body></html>
--- response_headers
X-PageSpeed: MISS
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 2: phase 1, the recorded miss is served from the cache
--- config eval: $::config
--- request
GET /miss
--- response_body chomp
<html><body>from origin</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 3: phase 1, a 304 without Vary revalidates the stored page
--- config eval: $::config
--- request
GET /reval
--- response_body chomp
<html><body>stored /reval</body></html>
--- response_headers
X-PageSpeed: REVALIDATED
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 4: phase 1, a 304 with Vary: Accept lists Accept once
The 304's own line goes out; the module's line follows it without Accept.
(The entry is then dropped: a response that varies on Accept is not stored.)
--- config eval: $::config
--- request
GET /reval-vary
--- response_body chomp
<html><body>stored /reval-vary</body></html>
--- raw_response_headers_like: \r\nVary: Accept\r\n[\s\S]*\r\nVary: Accept-Encoding, User-Agent\r\n
--- raw_response_headers_unlike: (?i)\nvary:[^\r\n]*,\s*accept\s*(?:,|\r)


=== TEST 5: phase 1, a stale page without a validator is refetched
--- config eval: $::config
--- request
GET /refetch
--- response_body chomp
<html><body>from origin</body></html>
--- response_headers
X-PageSpeed: MISS
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 5a: phase 1, a page with no origin Vary is recorded by a miss
--- config eval: $::config
--- request
GET /rec
--- response_body chomp
<html><body>recorded</body></html>
--- response_headers
X-PageSpeed: MISS
Vary: Accept-Encoding, User-Agent, Accept
Referrer-Policy: no-referrer


=== TEST 5b: phase 1, the recorded page is served from the cache, keyed on Accept
--- config eval: $::config
--- request
GET /rec
--- response_body chomp
<html><body>recorded</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
Referrer-Policy: no-referrer


=== TEST 5c: phase 1, the stored header block holds the origin's lines only
--- init
main::dump_entry("/rec", "rec-miss");
--- config eval: $::config
--- request
GET /dump/rec-miss.txt
--- response_body_like: \Aentry present flags=0x[0-9a-f]+ varies_accept=0\nsidecar present\n(?:header [^\n]*\n)*\z
--- response_body_unlike: (?i)(?:\bvary\b|\baccept\b(?!-)|user-agent)


=== TEST 5d: phase 1, the stored block carries the origin's Referrer-Policy
--- config eval: $::config
--- request
GET /dump/rec-miss.txt
--- response_body_like: (?i)\nheader referrer-policy: no-referrer\n


=== TEST 5e: phase 1, a 304 revalidates the recorded page and keeps it
--- init
sleep(5);
--- config eval: $::config
--- request
GET /rec
--- response_body chomp
<html><body>recorded</body></html>
--- response_headers
X-PageSpeed: REVALIDATED
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 5f: phase 1, after the 304 the page is served from the cache again
--- config eval: $::config
--- request
GET /rec
--- response_body chomp
<html><body>recorded</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
Referrer-Policy: no-referrer


=== TEST 5g: phase 1, after the 304 the stored block still holds the origin's lines only
--- init
main::dump_entry("/rec", "rec-304");
--- config eval: $::config
--- request
GET /dump/rec-304.txt
--- response_body_like: \Aentry present flags=0x[0-9a-f]+ varies_accept=0\nsidecar present\n(?:header [^\n]*\n)*\z
--- response_body_unlike: (?i)(?:\bvary\b|\baccept\b(?!-)|user-agent)


=== TEST 5h: phase 1, a miss whose origin varies on Accept lists Accept once
--- config eval: $::config
--- request
GET /vary-accept
--- response_body chomp
<html><body>negotiated</body></html>
--- response_headers
X-PageSpeed: MISS
--- raw_response_headers_like: \r\nVary: Accept\r\n[\s\S]*\r\nVary: Accept-Encoding, User-Agent\r\n
--- raw_response_headers_unlike: (?i)\nvary:[^\r\n]*,\s*accept\s*(?:,|\r)


=== TEST 6: phase 2, the page recorded by a miss does not name Accept
--- init
main::write_shared_conf(0);
--- config eval: $::config
--- request
GET /miss
--- response_body chomp
<html><body>from origin</body></html>
--- response_headers
X-PageSpeed: HIT
--- raw_response_headers_like: \r\nVary: Accept-Encoding, User-Agent\r\n
--- raw_response_headers_unlike: (?i)\nvary:[^\r\n]*\baccept\s*(?:,|\r)


=== TEST 7: phase 2, the page revalidated by a 304 without Vary does not name Accept
--- config eval: $::config
--- request
GET /reval
--- response_body chomp
<html><body>stored /reval</body></html>
--- response_headers
X-PageSpeed: REVALIDATED
--- raw_response_headers_like: \r\nVary: Accept-Encoding, User-Agent\r\n
--- raw_response_headers_unlike: (?i)\nvary:[^\r\n]*\baccept\s*(?:,|\r)


=== TEST 8: phase 2, the refetched page does not name Accept
--- config eval: $::config
--- request
GET /refetch
--- response_body chomp
<html><body>from origin</body></html>
--- response_headers
X-PageSpeed: HIT
--- raw_response_headers_like: \r\nVary: Accept-Encoding, User-Agent\r\n
--- raw_response_headers_unlike: (?i)\nvary:[^\r\n]*\baccept\s*(?:,|\r)
