# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# With agent optimization on, a Markdown response for agents (the page's
# Markdown copy and /llms.txt) carries x-markdown-tokens (body bytes / 4,
# rounded down) and, when pagespeed_agent_content_signal is set, the
# operator's content-signal.  Every HTML response names Accept in its Vary,
# because the same URL can answer an `Accept: text/markdown` request with the
# Markdown copy.  t/seed-113.sh stores the entries; every miss goes to the
# /origin location.
#
# The prologue plays the optimizer: a shared config with agent optimization
# on and a notify socket nothing listens on.

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
    system("bash t/seed-113.sh") == 0
        or die "Cache seeding failed";
}

{
    my $dir = "/tmp/psmdtok-113";
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/absent.sock\n"
        . "agent_optimize_entitled=true\n"
        . "agent_optimize_llms_txt_enabled=true\n";
    close($fh);
}

# The origin answers with no-store so that no miss below is recorded over a
# seeded entry.
our $config = <<'_EOC_';
server_name localhost;
pagespeed on;
pagespeed_cache_path /tmp/psmdtok-113/cache.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "no-store";
    return 200 "from origin";
}
location ~ ^/(md|html-only|not-stored|llms\.txt)$ {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
location /signal/ {
    pagespeed_agent_content_signal "search=yes, ai-input=yes, ai-train=no";
    location /signal/md {
        proxy_pass http://127.0.0.1:$server_port/origin;
        proxy_set_header Accept-Encoding "";
    }
    location /signal/off/md {
        pagespeed_agent_content_signal off;
        proxy_pass http://127.0.0.1:$server_port/origin;
        proxy_set_header Accept-Encoding "";
    }
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: the Markdown copy carries its token estimate and no content-signal
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /md
--- response_body chomp
# markdown copy for agents
--- response_headers
Content-Type: text/markdown
x-markdown-tokens: 6
content-signal:
Vary: Accept
--- no_error_log
[error]


=== TEST 2: a configured content-signal is sent with the Markdown copy
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /signal/md
--- response_body chomp
# markdown copy for agents
--- response_headers
x-markdown-tokens: 6
content-signal: search=yes, ai-input=yes, ai-train=no


=== TEST 3: off stops an inherited content-signal
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /signal/off/md
--- response_body chomp
# markdown copy for agents
--- response_headers
x-markdown-tokens: 6
content-signal:


=== TEST 4: the HTML of a page with a Markdown copy names Accept in Vary
--- config eval: $::config
--- request
GET /md
--- response_body chomp
<html><body>stored parent</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
x-markdown-tokens:


=== TEST 5: a stored page without a Markdown copy names Accept in Vary
--- config eval: $::config
--- request
GET /html-only
--- response_body chomp
<html><body>stored html only</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 6: an agent asking for a page without a Markdown copy gets the HTML, keyed on Accept
--- config eval: $::config
--- more_headers
Accept: text/markdown
--- request
GET /html-only
--- response_body chomp
<html><body>stored html only</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
x-markdown-tokens:


=== TEST 7: an HTML miss names Accept in Vary
--- config eval: $::config
--- request
GET /not-stored
--- response_body chomp
from origin
--- response_headers
X-PageSpeed: MISS
Vary: Accept-Encoding, User-Agent, Accept


=== TEST 8: the /llms.txt index carries its token estimate
--- config eval: $::config
--- request
GET /llms.txt
--- response_body chomp
# stored index
--- response_headers
Content-Type: text/markdown; charset=utf-8
x-markdown-tokens: 3


=== TEST 9: Accept with text/markdown at q=0 gets the HTML
--- config eval: $::config
--- more_headers
Accept: text/markdown;q=0
--- request
GET /md
--- response_body chomp
<html><body>stored parent</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
x-markdown-tokens:


=== TEST 10: Accept with text/markdown and text/html at equal q gets the Markdown copy
--- config eval: $::config
--- more_headers
Accept: text/markdown, text/html, */*
--- request
GET /md
--- response_body chomp
# markdown copy for agents
--- response_headers
Content-Type: text/markdown
Vary: Accept


=== TEST 11: text/markdown at q=0 is not overridden by */*
--- config eval: $::config
--- more_headers
Accept: text/markdown;q=0, */*
--- request
GET /md
--- response_body chomp
<html><body>stored parent</body></html>
--- response_headers
X-PageSpeed: HIT
Vary: Accept-Encoding, User-Agent, Accept
x-markdown-tokens:


=== TEST 12: Accept header lines are combined: text/markdown preferred over text/html
--- config eval: $::config
--- more_headers
Accept: text/html;q=0.5
Accept: text/markdown
--- request
GET /md
--- response_body chomp
# markdown copy for agents
--- response_headers
Content-Type: text/markdown
Vary: Accept
