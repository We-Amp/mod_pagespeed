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
set -u
set -e

# In-place optimization is request-independent: it optimizes an image
# identically for every client and never picks a format or quality based on
# the request, so every client gets the same bytes and the response must never
# carry a Vary: header.  It may still convert to a universally supported
# format (a photographic PNG becomes a JPEG via convert_png_to_jpeg), but
# never per requester; the request-gated targets (WebP, AVIF) are chosen only
# on rewritten URLs, where the format is part of the URL itself.
#
# These tests fetch the same image with a range of user-agents and request
# headers and assert that the answer does not move.

# Hashes the response body (everything past the headers) of a
# 'wget --save-headers' dump, so responses can be compared byte-for-byte.
function ipro_body_checksum() {
  local carriage_return=$(printf "\r")
  local first_blank_line=$(
    grep --text -n -m 1 \^${carriage_return}\$ "$1" | cut -f1 -d:)
  tail --lines=+$((first_blank_line + 1)) "$1" | md5sum | cut -d' ' -f1
}

# Fetches $IMAGE from $HOST with the given user-agent and request headers.
# When $EXPECT_OPTIMIZED is true, polls until the in-place optimized response
# (marked by its W/"PSA-aj-..." ETag) appears; when false, expects the
# optimizer to leave the image alone and the poll to time out on the original
# response.  Either way, fails the test if the response carries a Vary:
# header or Cache-Control: private, then echoes
# "<content-type> <content-length> <body-md5>" as the last line.
function ipro_fetch_and_check_no_vary() {
  local HOST=$1
  local IMAGE=$2
  local EXPECT_OPTIMIZED=$3
  local USER_AGENT=$4
  local ACCEPT_WEBP=$5
  local HAS_SAVE_DATA=$6
  local HAS_VIA=$7

  local URL="http://$HOST/$IMAGE"
  local OPT="--save-headers --user-agent=$USER_AGENT"
  local TEST_ID="IPRO no-vary $URL UA=$USER_AGENT"

  if [ "$ACCEPT_WEBP" = true ]; then
    OPT+=" --header=Accept:image/webp"
    TEST_ID+=" Accept:webp"
  fi
  if [ "$HAS_SAVE_DATA" = true ]; then
    OPT+=" --header=Save-Data:on"
    TEST_ID+=" Save-Data:on"
  fi
  if [ "$HAS_VIA" = true ]; then
    OPT+=" --header=Via:proxy"
    TEST_ID+=" Via:proxy"
  fi

  start_test "$TEST_ID"
  if [ "$EXPECT_OPTIMIZED" = true ]; then
    http_proxy=$SECONDARY_HOSTNAME \
      fetch_until -save $URL 'grep -c W/\"PSA-aj-' 1 "$OPT"
  else
    # The image stays unoptimized: the in-place ETag must never appear.
    http_proxy=$SECONDARY_HOSTNAME \
      fetch_until -save -expect_time_out $URL 'grep -c W/\"PSA-aj-' 1 "$OPT"
    check_not_from "$(extract_headers $FETCH_UNTIL_OUTFILE)" \
      fgrep -qi 'Etag: W/"PSA-aj-'
  fi

  # The whole point of the change: no Vary, and no IE-specific privacy hack.
  check_not_from "$(extract_headers $FETCH_UNTIL_OUTFILE)" grep -q "^Vary: "
  check_not_from "$(extract_headers $FETCH_UNTIL_OUTFILE)" \
    grep -qi "^Cache-Control:.*private"

  local TYPE="$(extract_headers $FETCH_UNTIL_OUTFILE | \
    scrape_header 'Content-Type')"
  local LENGTH="$(extract_headers $FETCH_UNTIL_OUTFILE | scrape_content_length)"
  local CHECKSUM="$(ipro_body_checksum $FETCH_UNTIL_OUTFILE)"
  echo "$TYPE $LENGTH $CHECKSUM"
}

# Fetches $IMAGE from $HOST with every combination of user-agent, Accept,
# Save-Data and Via, and checks that the content type and the exact response
# bytes never move.  $EXPECT_OPTIMIZED says whether in-place optimization is
# expected to produce an optimized response for this image at all: animated
# GIFs stay untouched (their only conversion target, animated WebP, is
# request-gated and therefore no longer available in place), but must STILL
# come back Vary-free and byte-identical for every client.
function ipro_response_is_request_independent() {
  local HOST=$1
  local IMAGE=$2
  local EXPECTED_CONTENT_TYPE=$3
  local EXPECT_OPTIMIZED=$4

  local BASELINE=""
  local AGENT
  for AGENT in "$CHROME_MOBILE:true" "$SAFARI_MOBILE:false" \
               "$FIREFOX_DESKTOP:false"; do
    local USER_AGENT="${AGENT%:*}"
    local ACCEPT_WEBP="${AGENT##*:}"
    local SAVE_DATA
    for SAVE_DATA in true false; do
      local VIA
      for VIA in true false; do
        local RESULT="$(ipro_fetch_and_check_no_vary \
          "$HOST" "$IMAGE" "$EXPECT_OPTIMIZED" \
          "$USER_AGENT" "$ACCEPT_WEBP" "$SAVE_DATA" "$VIA" \
          | tail -n 1)"
        local TYPE="${RESULT%% *}"
        start_test "IPRO expected type $IMAGE UA=$USER_AGENT"
        check [ "$TYPE" = "$EXPECTED_CONTENT_TYPE" ]
        if [ -z "$BASELINE" ]; then
          BASELINE="$RESULT"
        else
          start_test "IPRO bytes identical $IMAGE UA=$USER_AGENT"
          check [ "$RESULT" = "$BASELINE" ]
        fi
      done
    done
  done
}

# Hosts.  They used to differ in their (now retired) AllowVaryOn settings, so
# each must now be request-independent on its own; their quality settings
# still differ, so responses are only compared within one host.
HOST_ALLOW_ACCEPT="ipro-for-browser.example.com"
HOST_ALLOW_AUTO="ipro-for-browser-vary-on-auto.example.com"
HOST_ALLOW_NONE="ipro-for-browser-vary-on-none.example.com"
# User-agents
CHROME_MOBILE="Mozilla*Android*Mobile*Chrome/44.*"
SAFARI_MOBILE="iPhone*Safari/8536.25"
FIREFOX_DESKTOP="Firefox/1.5"
# Images
# JPEG image, recompressed as a JPEG.
IMAGE_PUZZLE="images/Puzzle.jpg"
# Non-photographic PNG image, recompressed as a PNG.
IMAGE_CUPPA="images/Cuppa.png"
# Photographic PNG image, converted to JPEG for every client alike
# (convert_png_to_jpeg is request-independent, so it stays available in
# place).
IMAGE_BIKE="images/BikeCrashIcn.png"
# Animated GIF, which in-place optimization leaves alone.
IMAGE_ANIMATION="images/PageSpeedAnimationSmall.gif"
# Constants
OPTIMIZED=true
UNOPTIMIZED=false

for HOST in "$HOST_ALLOW_AUTO" "$HOST_ALLOW_ACCEPT" "$HOST_ALLOW_NONE"; do
  ipro_response_is_request_independent \
    "$HOST" "$IMAGE_PUZZLE" "image/jpeg" "$OPTIMIZED"
  ipro_response_is_request_independent \
    "$HOST" "$IMAGE_CUPPA" "image/png" "$OPTIMIZED"
  ipro_response_is_request_independent \
    "$HOST" "$IMAGE_BIKE" "image/jpeg" "$OPTIMIZED"
  ipro_response_is_request_independent \
    "$HOST" "$IMAGE_ANIMATION" "image/gif" "$UNOPTIMIZED"
done
