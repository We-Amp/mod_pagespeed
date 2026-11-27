# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

use Test::Nginx::Socket 'no_plan';

no_shuffle();
log_level('warn');

# Seed cache before tests
BEGIN {
    system("bash t/seed-302.sh") == 0
        or die "Cache seeding failed";
}

run_tests();

__DATA__

=== TEST 1: WebP variant is NOT served to a client that negotiated no image format
Issue #1335: stored mask 0x09 (WebP), request negotiates 0x08 (Original —
Accept names no decodable raster format).  The mismatch must fall through
to the upstream, exactly like a cache miss; the undecodable WebP bytes and
the derived image/webp Content-Type must not reach the client.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-302-1.vol;

location /origin {
    pagespeed off;
    default_type image/jpeg;
    return 200 "original-jpeg-bytes";
}
location /m1.jpg {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /m1.jpg
--- more_headers
Accept: image/jpeg
--- response_body chomp
original-jpeg-bytes
--- response_headers_like
X-PageSpeed: MISS
Content-Type: image/jpeg
--- no_error_log
[error]
[crit]


=== TEST 2: WebP variant IS served to a WebP-negotiating client (no regression)
Same stored variant as TEST 1; the request's Accept advertises image/webp,
so the negotiated mask 0x09 matches exactly and the HIT serves as today.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-302-1.vol;

location /m2.jpg {
}
--- request
GET /m2.jpg
--- more_headers
Accept: image/webp
--- response_body chomp
WEBP-BYTES-2
--- response_headers_like
X-PageSpeed: HIT
Content-Type: image/webp
--- no_error_log
[error]
[crit]


=== TEST 3: WebP variant is NOT served to an AVIF-negotiating client
Stored mask 0x09 (WebP), request negotiates 0x0A (AVIF): a raster-vs-raster
mismatch (the mask cannot prove cross-format decodability) must fall through
to the upstream like a miss.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-302-1.vol;

location /origin {
    pagespeed off;
    default_type image/jpeg;
    return 200 "original-jpeg-bytes";
}
location /m3.jpg {
    proxy_pass http://127.0.0.1:$server_port/origin;
    proxy_set_header Accept-Encoding "";
}
--- request
GET /m3.jpg
--- more_headers
Accept: image/avif
--- response_body chomp
original-jpeg-bytes
--- response_headers_like
X-PageSpeed: MISS
Content-Type: image/jpeg
--- no_error_log
[error]
[crit]


=== TEST 4: Original-format variant remains the universal fallback (no regression)
Stored mask 0x08 (Original format image), request negotiates WebP (0x09):
the stored original must still serve, with the origin Content-Type (the
format bits are 0, so no transcoded mime is derived).
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-302-1.vol;

location /m4.jpg {
}
--- request
GET /m4.jpg
--- more_headers
Accept: image/webp
--- response_body chomp
JPEG-BYTES-4
--- response_headers_like
X-PageSpeed: HIT
Content-Type: image/jpeg
--- no_error_log
[error]
[crit]


=== TEST 5: SVG variant remains universal (no regression)
Stored mask 0x0B (SVG format), request negotiates no image format (0x08):
SVG serves all clients and must keep hitting.
--- config
pagespeed on;
pagespeed_cache_path /tmp/test-ps-302-1.vol;

location /m5.svg {
}
--- request
GET /m5.svg
--- response_body chomp
<svg xmlns="http://www.w3.org/2000/svg"/>
--- response_headers_like
X-PageSpeed: HIT
Content-Type: image/svg\+xml
--- no_error_log
[error]
[crit]
