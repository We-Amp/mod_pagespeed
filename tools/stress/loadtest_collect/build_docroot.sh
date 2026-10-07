#!/bin/bash
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
#
# Build a path-mirrored docroot from a collected corpus URL list, by fetching
# each URL with curl and writing it to <docroot>/<path>. This is the robust
# replay path for the shutdown/restart stress harness when the slurp proxy can't
# be used (see README "Realistic corpus replay" for why: a 103-Early-Hints
# origin defeats the 1.x serf slurp fetcher for HTML; curl handles 103 fine).
#
# Pair it with loadtest_collect/collect.js, which ENUMERATES the URL set (it
# drives a real headless browser, so JS/lazy sub-resources are discovered):
#   1. collect.js fills a slurp dir + corpus_all_urls.txt (its %r access log).
#   2. this script turns corpus_all_urls.txt into a servable docroot.
#   3. serve the docroot with mod_pagespeed (RewriteLevel CoreFilters, IPRO on,
#      LoadFromFile mapping the serving host to the docroot) and point the
#      harness at the emitted paths file (--url-file).
#
# Usage:
#   build_docroot.sh <urls-file> <origin-base-url> <docroot-dir> <out-paths-file>
# e.g.
#   build_docroot.sh corpus_all_urls.txt https://modpagespeed.com \
#       /corpus/modpagespeed.com /tmp/corpus_replay_paths.txt

set -u

if [ $# -ne 4 ]; then
  echo "Usage: $0 <urls-file> <origin-base-url> <docroot-dir> <out-paths-file>" >&2
  exit 1
fi

URLS_FILE="$1"          # corpus_all_urls.txt (full http://host/path lines)
ORIGIN="${2%/}"         # https origin to actually fetch from, e.g. https://example.com
DOCROOT="$3"
OUT_PATHS="$4"
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Safari/537.36"

# The host whose URLs we mirror = the origin host (strip scheme).
HOST="${ORIGIN#*://}"

mkdir -p "$DOCROOT"
: > "$OUT_PATHS"

ok=0; bad=0
while read -r u; do
  # Only mirror URLs for this host (http:// or https://).
  case "$u" in
    http://"$HOST"/*|https://"$HOST"/*) ;;
    *) continue;;
  esac
  path="/${u#*://*/}"          # everything after host, with a leading slash
  base="${path%%\?*}"          # drop any query string
  case "$base" in
    */)   file="$DOCROOT${base}index.html";;   # directory page
    *.*)  file="$DOCROOT${base}";;             # has extension => resource
    *)    file="$DOCROOT${base}/index.html";;  # extensionless page
  esac
  mkdir -p "$(dirname "$file")" 2>/dev/null
  code=$(curl -s --max-time 30 -A "$UA" -w "%{http_code}" -o "$file" "${ORIGIN}${base}")
  if [ "$code" = "200" ]; then
    ok=$((ok + 1))
    echo "$base" >> "$OUT_PATHS"
  else
    bad=$((bad + 1))
    rm -f "$file"
  fi
done < "$URLS_FILE"

echo "[build_docroot] fetched ok=$ok bad=$bad -> $DOCROOT"
echo "[build_docroot] files=$(find "$DOCROOT" -type f | wc -l) size=$(du -sh "$DOCROOT" | cut -f1)"
echo "[build_docroot] harness --url-file=$OUT_PATHS ($(wc -l < "$OUT_PATHS") paths)"
