# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Web Bot Auth VALID-signature end-to-end smoke: mints FRESH
# RFC 9421 signatures at test time with webbotauth_sign_tool (the same
# serializer the verifier uses), pre-warms the nginx key store with the
# tool's --emit=keystore document (playing the worker's role, exactly like
# the pagespeed-shared.conf prologue below), and proves a running nginx
# classifies them:
#   * scanner-probe shape (("@authority" "signature-agent"), created/expires/
#     nonce/tag, quoted sf-string Signature-Agent) -> "testbot,
#     ed25519-verified"  — this is the shape our own signed-agent probes send
#   * multi-signature (untagged CDN member + tagged member) -> verified
#   * escaped path (%XX) + mixed-case Host, signed over the wire bytes ->
#     verified (the classify hook binds @path to unparsed_uri and lowercases
#     @authority)
#   * expired signature -> "unknown" (fail-closed)
#   * wrong-tag signature -> NO label (not web-bot-auth material)
#   * warmed key but unregistered kid -> "signed-agent"
#
# Requires WEBBOTAUTH_SIGN_TOOL (path to a built webbotauth_sign_tool);
# skipped otherwise (the harness scripts export it after building the tool).

BEGIN {
    unless ($ENV{WEBBOTAUTH_SIGN_TOOL} && -x $ENV{WEBBOTAUTH_SIGN_TOOL}) {
        print "1..0 # SKIP WEBBOTAUTH_SIGN_TOOL not set or not executable\n";
        exit 0;
    }
}

# Pre-create the empty cache volumes before tests. Since #1319 the module
# opens the cache resolve-only (volume_size = 0) and cannot cold-create a
# volume — the tests require volumes that exist but have no entries.
# Placed after the skip-check BEGIN above: a skipped run must not require
# the seed tool.
BEGIN {
    system("bash t/seed-103.sh") == 0
        or die "Cache volume creation failed";
}

use Test::Nginx::Socket 'no_plan';
use File::Path qw(make_path);

my $tool = $ENV{WEBBOTAUTH_SIGN_TOOL};
my $dir  = "/tmp/wba-103";
make_path($dir);

# Worker-role prologue: feature toggle + registry + directory host ...
open(my $fh, '>', "$dir/pagespeed-shared.conf") or die $!;
print $fh <<'EOF';
version=1
web_bot_auth=true
web_bot_auth_verified_bots=test-kid=testbot
web_bot_auth_directory_hosts=keys.example.com
EOF
close($fh);

# ... plus the warmed key store (worker-published file, here minted by the
# sign tool so the suite needs no worker and no network). Two kids under the
# same deterministic keypair: one registered as a verified bot, one not.
my $store  = qx($tool --emit=keystore --kid=test-kid --directory-host=keys.example.com);
die "sign tool --emit=keystore failed" if $? != 0 || $store !~ /^k=P,/m;
my $store2 = qx($tool --emit=keystore --kid=unreg-kid --directory-host=keys.example.com);
die "sign tool --emit=keystore failed" if $? != 0 || $store2 !~ /^k=P,/m;
open($fh, '>', "$dir/pagespeed-webbotauth-keys.conf") or die $!;
print $fh $store, $store2;
close($fh);

# Mint headers via the sign tool; returns (signature_input, signature).
sub mint {
    my ($args) = @_;
    my $out = qx($tool --emit=headers $args);
    die "sign tool failed ($args): rc=$?" if $? != 0;
    my ($si) = $out =~ /^SIGINPUT=(.*)$/m;
    my ($s)  = $out =~ /^SIG=(.*)$/m;
    die "unexpected sign tool output: $out" unless defined $si && defined $s;
    return ($si, $s);
}

my $now = time();
my $sig_agent = '"https://keys.example.com/.well-known/http-message-signatures-directory"';
my $scanner_args = "--kid=test-kid --authority=localhost"
    . " --components=\@authority,signature-agent"
    . " --field='signature-agent=$sig_agent'"
    . " --created=$now --expires=" . ($now + 300)
    . " --nonce=probe-nonce-103 --tag=web-bot-auth";

# TEST 1: scanner-probe shape.
my ($si1, $s1) = mint($scanner_args);
$::hdrs1 = "Signature-Agent: $sig_agent\nSignature-Input: $si1\nSignature: $s1";

# TEST 2: multi-signature — an untagged CDN-style member ahead of the tagged
# one, in both headers. Selection must pick the tagged member.
$::hdrs2 = "Signature-Agent: $sig_agent\n"
    . 'Signature-Input: cdn=("@method");created=1;keyid="cdn-key";alg="rsa-v1_5-sha256", ' . "$si1\n"
    . "Signature: cdn=:QUJD:, $s1";

# TEST 3: escaped path + mixed-case Host over the classic derived components.
# Signed over the WIRE path (/t%41 — decodes to /tA) and the lowercased
# authority; sent with a deliberately mixed-case Host header.
my ($si3, $s3) = mint("--kid=test-kid --method=GET --authority=localhost"
    . " --path=/t%41 --created=$now --tag=web-bot-auth");
$::raw3 = "GET /t%41 HTTP/1.1\r\n"
    . "Host: LocalHost\r\n"
    . "Signature-Input: $si3\r\n"
    . "Signature: $s3\r\n"
    . "Connection: close\r\n\r\n";

# TEST 4: expired (created 10h ago, no expires; max age is 1h).
my ($si4, $s4) = mint("--kid=test-kid --authority=localhost"
    . " --components=\@authority,signature-agent"
    . " --field='signature-agent=$sig_agent'"
    . " --created=" . ($now - 36000) . " --tag=web-bot-auth");
$::hdrs4 = "Signature-Agent: $sig_agent\nSignature-Input: $si4\nSignature: $s4";

# TEST 5: valid crypto but a NON-web-bot-auth tag: not our material -> no
# label at all (classified as if unsigned).
my ($si5, $s5) = mint("--kid=test-kid --authority=localhost"
    . " --components=\@authority,signature-agent"
    . " --field='signature-agent=$sig_agent'"
    . " --created=$now --tag=other-protocol");
$::hdrs5 = "Signature-Agent: $sig_agent\nSignature-Input: $si5\nSignature: $s5";

# TEST 6: warmed key, kid NOT in the verified-bot registry -> signed-agent.
my ($si6, $s6) = mint("--kid=unreg-kid --authority=localhost"
    . " --components=\@authority,signature-agent"
    . " --field='signature-agent=$sig_agent'"
    . " --created=$now --tag=web-bot-auth");
$::hdrs6 = "Signature-Agent: $sig_agent\nSignature-Input: $si6\nSignature: $s6";

# TEST 7: tampered signature (one bit flipped) -> unknown, fail-closed.
my ($si7, $s7) = mint("$scanner_args --tamper");
$::hdrs7 = "Signature-Agent: $sig_agent\nSignature-Input: $si7\nSignature: $s7";

# TEST 8: the intermediary case from RFC 9421 section 4.1 -- a CDN adds its
# signature as SEPARATE Signature-Input/Signature field lines, so the tagged
# member sits on the SECOND Signature-Input line and its signature bytes on a
# SECOND Signature line. Only a verifier that joins repeated field lines
# (RFC 9110) can verify this.
$::hdrs8 = "Signature-Agent: $sig_agent\n"
    . 'Signature-Input: cdn=("@method");created=1;keyid="cdn-key";alg="rsa-v1_5-sha256"' . "\n"
    . "Signature: cdn=:QUJD:\n"
    . "Signature-Input: $si1\n"
    . "Signature: $s1";

no_shuffle();
log_level('warn');
run_tests();

__DATA__

=== TEST 1: scanner-probe-shaped signature verifies as the registered bot
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-1.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs1
--- error_code: 200
--- response_headers
x-verified-bot: testbot, ed25519-verified


=== TEST 2: tagged member selected out of a multi-signature dictionary
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-2.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs2
--- error_code: 200
--- response_headers
x-verified-bot: testbot, ed25519-verified


=== TEST 3: escaped path + mixed-case Host verify (wire-path binding)
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-3.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- raw_request eval
$::raw3
--- error_code: 200
--- response_headers
x-verified-bot: testbot, ed25519-verified


=== TEST 4: expired signature fails closed to unknown
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-4.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs4
--- error_code: 200
--- response_headers
x-verified-bot: unknown


=== TEST 5: wrong-tag signature is treated as unsigned (no label)
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-5.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs5
--- error_code: 200
--- response_headers
!x-verified-bot


=== TEST 6: warmed key with unregistered kid classifies as signed-agent
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-6.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs6
--- error_code: 200
--- response_headers
x-verified-bot: signed-agent


=== TEST 7: tampered signature fails closed to unknown
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-7.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs7
--- error_code: 200
--- response_headers
x-verified-bot: unknown


=== TEST 8: tagged signature on a second field line still verifies
--- config
pagespeed on;
pagespeed_cache_path /tmp/wba-103/test-ps-103-8.vol;

location /origin {
    pagespeed off;
    default_type text/html;
    return 200 "hello";
}
location /t {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /t
--- more_headers eval
$::hdrs8
--- error_code: 200
--- response_headers
x-verified-bot: testbot, ed25519-verified
