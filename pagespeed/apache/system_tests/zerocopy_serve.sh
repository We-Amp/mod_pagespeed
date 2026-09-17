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

# Zero-copy aliased serving E2E (CycloneZeroCopyServe).
#
# Asserts that with CycloneZeroCopy(+Serve) enabled, a warm cache-hit
# resource serve takes the ALIASED path on the stock Debian/Ubuntu filter
# chain -- mod_reqtimeout enabled and mod_filter by-type harnesses
# (AddOutputFilterByType for both MOD_PAGESPEED_OUTPUT_FILTER and DEFLATE)
# present.  The regression this guards: eligibility silently fail-closing
# on every default install while the serve degrades to the copy path with
# no counter movement.
#
# Self-gated on the conf actually enabling zero-copy, so the snippet is a
# no-op in rigs that run with the shipped (opt-in) Apache defaults.

if egrep -qi '^[[:space:]]*ModPagespeedCycloneZeroCopy[[:space:]]+on' \
     "$APACHE_DEBUG_PAGESPEED_CONF" && \
   egrep -qi '^[[:space:]]*ModPagespeedCycloneZeroCopyServe[[:space:]]+on' \
     "$APACHE_DEBUG_PAGESPEED_CONF"; then
  start_test Zero-copy aliased serve fires on the stock filter chain
  # zerocopy_big_image.html resizes Puzzle.jpg to 800x600: a genuine
  # REWRITTEN .pagespeed.ic. artifact, stored in the HTTP cache and well
  # above the engine's 16KB aliasing floor.  See the fixture page for why
  # the obvious alternatives cannot alias (below-floor 256x192 example,
  # no-saving full-size recompression, on-the-fly .ce. outputs).
  URL="$TEST_ROOT/zerocopy_big_image.html"
  fetch_until -save "$URL" 'fgrep -c Puzzle.jpg.pagespeed.ic' 1
  IMG_ABS=$(grep -o '/[^"]*Puzzle\.jpg\.pagespeed\.ic\.[^"]*\.jpg' \
    "$FETCH_UNTIL_OUTFILE" | head -n 1)
  check [ -n "$IMG_ABS" ]
  IMG_URL="$PRIMARY_SERVER$IMG_ABS"
  echo "Rewritten image: $IMG_URL"
  BASELINE_ALIASED=$(scrape_stat zerocopy_serve_aliased)
  # Fetch the artifact twice: whatever the first fetch's cache state, the
  # second is a warm Cyclone hit and must be served aliased.
  check $WGET_DUMP "$IMG_URL" > /dev/null
  check $WGET_DUMP "$IMG_URL" > /dev/null
  ALIASED=$(scrape_stat zerocopy_serve_aliased)
  INELIGIBLE=$(scrape_stat zerocopy_serve_ineligible)
  echo "zerocopy_serve_aliased: $BASELINE_ALIASED -> $ALIASED" \
       "(ineligible: $INELIGIBLE)"
  check [ "$ALIASED" -gt "$BASELINE_ALIASED" ]
else
  echo "skipping zerocopy_serve: CycloneZeroCopy(+Serve) not on in conf"
fi
