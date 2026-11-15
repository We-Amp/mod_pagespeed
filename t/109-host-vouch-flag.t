# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

# Every notification the module sends says whether the server block stands
# behind its hostname (wire flag HOST_VOUCHED).  The worker runs the work that
# fetches from that hostname, the agent render and the /llms.txt build, only
# for a notification that carries the flag, so a listed name turns agent
# optimization on and any other name leaves it off.
#
# The prologue plays the optimizer: a shared config with both agent options
# on and a listener on the notify socket that writes one line per frame it
# receives (url, mask and flags byte).  Each test then checks the line for
# its own request.  t/seed-109.sh stores a stylesheet under both hostnames
# for the hit-path cases.

use File::Path qw(make_path);
use IO::Socket::UNIX;
use Fcntl qw(:flock);
use Time::HiRes qw(sleep);
use Digest::SHA qw(sha256_hex);

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

our $dir = "/tmp/psvouch-109";

BEGIN {
    system("bash t/seed-109.sh") == 0
        or die "Cache seeding failed";
}

{
    make_path($dir);
    open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
    print $fh "version=1\ncache_dir_generation=$gen\n"
        . "socket_path=$dir/worker.sock\n"
        . "agent_optimize_entitled=true\n"
        . "agent_optimize_llms_txt_enabled=true\n";
    close($fh);
    open(my $hf, '>', "$dir/pagespeed-hosts.conf") or die $!;
    close($hf);
}

# The site of the test's server block (src/nginx/site_identity.cc): its
# server names (Test::Nginx's own and the config's) and its listener.
our $port = $ENV{TEST_NGINX_SERVER_PORT} || $ENV{TEST_NGINX_PORT} || 1984;
our $site = do {
    my $in = "nginx-server-block-site-v1\n";
    $in .= 'n' . length($_) . ":$_\n" for sort ('localhost', 'site.test');
    my $addr = "0.0.0.0:$port";
    $in .= 'a' . length($addr) . ":$addr\n";
    sha256_hex($in);
};

# The listener: one child per connection, each frame appended as
# "url=<url> host=<host> mask=<hex> site=<site, or - for none> flags=<n>".
our $frames = "$dir/frames.log";
unlink $frames;
unlink "$dir/worker.sock";
our $listener_pid = fork();
die "fork: $!" unless defined $listener_pid;
if ($listener_pid == 0) {
    $SIG{CHLD} = 'IGNORE';
    my $srv = IO::Socket::UNIX->new(
        Type => SOCK_STREAM(), Local => "$dir/worker.sock", Listen => 16)
        or die "listen: $!";
    while (my $conn = $srv->accept()) {
        my $pid = fork();
        next if !defined $pid || $pid != 0;
        close($srv);
        while (1) {
            my $hdr = '';
            last unless read($conn, $hdr, 4) == 4;
            my $len = unpack('N', $hdr);
            my $p = '';
            last unless read($conn, $p, $len) == $len;
            my $o = 1;  # after the version byte
            my $ul = unpack('N', substr($p, $o, 4)); $o += 4;
            my $url = substr($p, $o, $ul); $o += $ul;
            my $hl = unpack('N', substr($p, $o, 4)); $o += 4;
            my $host = substr($p, $o, $hl); $o += $hl;
            $o += 1;  # content type
            my $mask = unpack('N', substr($p, $o, 4)); $o += 4;
            $o += 2;  # scheme, agent request
            my $ocl = unpack('N', substr($p, $o, 4)); $o += 4 + $ocl;
            my $osl = ord(substr($p, $o, 1)); $o += 1 + $osl;
            my $sl = ord(substr($p, $o, 1)); $o += 1;
            my $site = $sl ? substr($p, $o, $sl) : '-';
            my $flags = ord(substr($p, -1, 1));
            open(my $out, '>>', $frames) or exit 1;
            flock($out, LOCK_EX);
            printf $out "url=%s host=%s mask=%08x site=%s flags=%d\n",
                $url, $host, $mask, $site, $flags;
            close($out);
        }
        exit 0;
    }
    exit 0;
}
END {
    kill 'TERM', $listener_pid if $listener_pid;
}

# The frames recorded for `url` under `host`, waiting up to `secs` for one.
sub frames_for {
    my ($url, $host, $secs) = @_;
    my @lines;
    for (my $t = 0; $t < $secs; $t += 0.05) {
        if (open(my $in, '<', $frames)) {
            @lines = grep { /^url=\Q$url\E host=\Q$host\E / } <$in>;
            close($in);
        }
        last if @lines;
        sleep(0.05);
    }
    return @lines;
}

# Per test: [url, host, expected flags of the frame, or undef for no frame].
our %expect = (
    'an HTML miss under a listed name is sent vouched'
        => ['/vouch-page.html', 'site.test', 1],
    'a stylesheet miss under a listed name is sent vouched'
        => ['/vouch-miss.css', 'site.test', 1],
    'a stylesheet miss under an unlisted name is sent unvouched'
        => ['/vouch-miss.css', 'unlisted.test', 0],
    'an llms.txt build request under a listed name is sent vouched'
        => ['/llms.txt', 'site.test', 1],
    'a hit that asks for another variant, under a listed name, is sent vouched'
        => ['/hit.css', 'site.test', 1],
    'a hit that asks for another variant, under an unlisted name, is sent unvouched'
        => ['/hit.css', 'unlisted.test', 0],
    'an HTML miss under an unlisted name is not sent'
        => ['/vouch-page.html', 'unlisted.test', undef],
);

add_response_body_check(sub {
    my ($block, $body, $req_idx, $repeated_req_idx, $dry_run) = @_;
    return if $dry_run;
    (my $title = $block->name) =~ s/^.*?TEST \d+:\s*//;
    my $e = $expect{$title};
    return unless $e;
    my ($url, $host, $flags) = @$e;
    if (!defined $flags) {
        my @lines = frames_for($url, $host, 1);
        is(scalar(@lines), 0, $block->name . " - no frame");
        return;
    }
    my @lines = frames_for($url, $host, 5);
    is(scalar(@lines), 1, $block->name . " - one frame");
    like($lines[0] // '', qr/ flags=$flags$/, $block->name . " - flags");
    # Every frame names the server block's keyspace.
    like($lines[0] // '', qr/ site=$site /, $block->name . " - site");
});

our $config = <<'_EOC_';
server_name site.test;
pagespeed on;
pagespeed_cache_path /tmp/psvouch-109/cache.vol;

location = /llms.txt {
    proxy_pass http://127.0.0.1:$server_port/llms-origin;
}
location /llms-origin {
    pagespeed off;
    return 404 "origin";
}
location /origin-html {
    pagespeed off;
    default_type text/html;
    add_header Cache-Control "public, max-age=600";
    return 200 "<html><head><title>t</title></head><body>page</body></html>";
}
location /origin-css {
    pagespeed off;
    default_type text/css;
    add_header Cache-Control "public, max-age=600";
    return 200 "h1{color:red}";
}
location ~ \.html$ {
    proxy_pass http://127.0.0.1:$server_port/origin-html;
    proxy_set_header Accept-Encoding "";
}
location ~ \.css$ {
    proxy_pass http://127.0.0.1:$server_port/origin-css;
    proxy_set_header Accept-Encoding "";
}
_EOC_

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: an HTML miss under a listed name is sent vouched
--- config eval: $::config
--- more_headers
Host: site.test
--- request
GET /vouch-page.html
--- error_code: 200


=== TEST 2: a stylesheet miss under a listed name is sent vouched
--- config eval: $::config
--- more_headers
Host: site.test
--- request
GET /vouch-miss.css
--- error_code: 200


=== TEST 3: a stylesheet miss under an unlisted name is sent unvouched
--- config eval: $::config
--- more_headers
Host: unlisted.test
--- request
GET /vouch-miss.css
--- error_code: 200


=== TEST 4: an llms.txt build request under a listed name is sent vouched
--- config eval: $::config
--- more_headers
Host: site.test
--- request
GET /llms.txt
--- error_code: 404


=== TEST 5: a hit that asks for another variant, under a listed name, is sent vouched
--- config eval: $::config
--- more_headers
Host: site.test
Accept-Encoding: gzip, br
--- request
GET /hit.css
--- response_body chomp
h1{color:red}
--- error_code: 200


=== TEST 6: a hit that asks for another variant, under an unlisted name, is sent unvouched
--- config eval: $::config
--- more_headers
Host: unlisted.test
Accept-Encoding: gzip, br
--- request
GET /hit.css
--- response_body chomp
h1{color:red}
--- error_code: 200


=== TEST 7: an HTML miss under an unlisted name is not sent
--- config eval: $::config
--- more_headers
Host: unlisted.test
--- request
GET /vouch-page.html
--- error_code: 200
