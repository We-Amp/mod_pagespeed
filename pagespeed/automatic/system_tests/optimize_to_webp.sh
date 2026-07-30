#!/bin/bash
#
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
start_test Optimize images to webp
# Which WebP flavours a client gets is decided by the Accept request header and
# by the enabled filters -- never by the browser version. The user-agent column
# below therefore proves the opposite of what it used to: requests that differ
# only in user agent must produce identical output.
function test_optimize_to_webp() {
  HTML="$TEST_ROOT/optimize_for_bandwidth/$1"
  URL="$HTML?PageSpeedFilters=$2"
  OUT=$($WGET -q -O - --header=X-PSA-Blocking-Rewrite:psatest \
    --user-agent=$3 --header=Accept:image/webp $URL)
  check_from "$OUT" grep -q "$4"
  check_from "$OUT" grep -q "$5"
}
# Same as test_optimize_to_webp, but the request does not advertise WebP.
function test_optimize_without_webp_accept() {
  HTML="$TEST_ROOT/optimize_for_bandwidth/$1"
  URL="$HTML?PageSpeedFilters=$2"
  OUT=$($WGET -q -O - --header=X-PSA-Blocking-Rewrite:psatest \
    --user-agent=$3 $URL)
  check_from "$OUT" grep -q "$4"
  check_from "$OUT" grep -q "$5"
}
# Current Chrome, three-digit major version. Both lossless and animated WebP
# will be used.
test_optimize_to_webp webp_urls/rewrite_webp.html \
  "convert_to_webp_lossless,convert_to_webp_animated,recompress_png" \
  "Chrome/137." \
  "/xCuppa.png.pagespeed.ic.*.webp\"/>" \
  "/xPageSpeedAnimationSmall.gif.pagespeed.ic.*.webp\"/>"
# Oldest Chrome with animated WebP support. Identical output.
test_optimize_to_webp webp_urls/rewrite_webp.html \
  "convert_to_webp_lossless,convert_to_webp_animated,recompress_png" \
  "Chrome/32." \
  "/xCuppa.png.pagespeed.ic.*.webp\"/>" \
  "/xPageSpeedAnimationSmall.gif.pagespeed.ic.*.webp\"/>"
# A browser version far below anything the old allow lists covered. Because it
# advertises WebP it is taken at its word, so the output is identical again.
test_optimize_to_webp webp_urls/rewrite_webp.html \
  "convert_to_webp_lossless,convert_to_webp_animated,recompress_png" \
  "Chrome/22." \
  "/xCuppa.png.pagespeed.ic.*.webp\"/>" \
  "/xPageSpeedAnimationSmall.gif.pagespeed.ic.*.webp\"/>"
# Without convert_to_webp_animated only lossless WebP is used; the animated GIF
# is left alone. The enabled filter set is what draws this line now.
test_optimize_to_webp webp_urls/rewrite_webp.html \
  "convert_to_webp_lossless,recompress_png" \
  "Chrome/137." \
  "/xCuppa.png.pagespeed.ic.*.webp\"/>" \
  "/PageSpeedAnimationSmall.gif\"/>"
# No Accept:image/webp. No WebP will be used. Single frame image will be
# converted to PNG.
test_optimize_without_webp_accept webp_urls/rewrite_webp.html \
  "convert_to_webp_lossless,convert_to_webp_animated,recompress_png" \
  "Chrome/137." \
  "/xCuppa.png.pagespeed.ic.*.png\"/>" \
  "/PageSpeedAnimationSmall.gif\"/>"
