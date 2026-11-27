#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The 2.1 version-drift guard of website.yml (see the comment there). Run from
# the repository root, or set ROOT.
#
# Two checks:
#   1. LIVE STATE: the 2.1 manifest must not lag the latest stable v2.* tag of
#      the public mod_pagespeed repository (a warning for GRACE_HOURS after
#      the tag, an error after that). A module-only release is the exception:
#      a newer tag on the manifest's major.minor line that has no matching
#      v* tag in this repository (no optimizer release, so no new images,
#      chart or NuGet packages) passes when MODULE_PACKAGE_VERSION in
#      src/data/product-facts.mjs already names it;
#   2. COMMITTED FILES: hand-pinned image tags in non-frozen content must equal
#      the manifest.
#
# RELEASE_WINDOW=1 (set by release-window.sh for the sync and release/* pull
# requests) turns a manifest lag into a warning at any tag age. Stale image
# tags are softened ONLY when RELEASE_SYNC=1 (the auto/sync-release-manifests
# pull request): that bot edits release.yaml alone (see PR #666), so it cannot
# move the literals in the same commit, and the check would block its own fix.
# On release/* pull requests and everywhere else they stay errors. Also errors
# in a window: an unreadable manifest and no stable tag at all. An unreadable
# tag date counts as a stale tag, so inside a window it is a warning too.

set -euo pipefail

cd "${ROOT:-.}"
grace="${GRACE_HOURS:-6}"
window="${RELEASE_WINDOW:-0}"
sync="${RELEASE_SYNC:-0}"

# Date to epoch seconds: GNU date on the runner, BSD date on a Mac.
to_epoch() {
  date -u -d "$1" +%s 2>/dev/null \
    || date -u -j -f '%Y-%m-%dT%H:%M:%SZ' "$1" +%s 2>/dev/null \
    || echo 0
}

# complain soft|sync MESSAGE: an error, or a warning when the softening applies
# (soft: any release window; sync: the sync pull request only).
fail=0
warned=0
complain() {
  if { [ "$1" = soft ] && [ "$window" = 1 ]; } || { [ "$1" = sync ] && [ "$sync" = 1 ]; }; then
    warned=1
    echo "::warning::$2 -- warning only: this is a release window (the sync or release/* pull request); the same finding fails on main and on ordinary pull requests."
  else
    echo "::error::$2"
    fail=1
  fi
}

manifest=$(grep -E '^[[:space:]]*semver:' website/src/content/releases-2.1/release.yaml \
  | head -1 | sed -E 's/.*"([^"]+)".*/\1/')
if [ -z "$manifest" ]; then
  echo "::error::could not read release.semver from website/src/content/releases-2.1/release.yaml"
  exit 1
fi
latest_stable=$(git ls-remote --tags --refs https://github.com/We-Amp/mod_pagespeed.git 'v2.*' \
  | awk '{ print $2 }' | sed 's#^refs/tags/##' \
  | grep -vE -- '-(rc|beta|alpha|preview)' | sort -V | tail -1 | sed 's/^v//')
if [ -z "$latest_stable" ]; then
  echo "::error::no stable v2.* tag found on We-Amp/mod_pagespeed -- cannot verify the manifest (network?)"
  exit 1
fi

# MODULE_PACKAGE_VERSION: the module packages, which lead the manifest after
# a module-only release. Empty when unreadable, which never matches a tag.
module_pkg=$(grep -E '^export const MODULE_PACKAGE_VERSION *=' website/src/data/product-facts.mjs 2>/dev/null \
  | head -1 | sed -E "s/.*['\"]([0-9]+\.[0-9]+\.[0-9]+)['\"].*/\1/" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' || true)

# A module-only release: the newer tag stays on the manifest's major.minor
# line, and this repository (the optimizer worker, the images, the chart and
# the NuGet packages) has no release tag for it. A failed lookup is not proof
# of a module-only release, so it falls through to the lag check below.
module_only=0
if [ "$latest_stable" != "$manifest" ] \
  && [ "${latest_stable%.*}" = "${manifest%.*}" ] \
  && [ -n "$module_pkg" ] && [ "$module_pkg" = "$latest_stable" ]; then
  if opt_refs=$(git ls-remote --tags --refs https://github.com/We-Amp/pagespeed-optimizer.git "v${latest_stable}" 2>/dev/null); then
    case "$(printf '%s\n' "$opt_refs" | awk '{ print $2 }')" in
      *"refs/tags/v${latest_stable}"*) ;;
      *) module_only=1 ;;
    esac
  fi
fi

if [ "$module_only" = 1 ]; then
  echo "module-only release: v${latest_stable} has no optimizer release and MODULE_PACKAGE_VERSION is ${module_pkg}; the manifest (and the image tags) stay ${manifest}."
elif [ "$(printf '%s\n%s\n' "$latest_stable" "$manifest" | sort -V | tail -1)" != "$manifest" ]; then
  # Resolve the tag's date: an annotated tag carries its own tagger date, a
  # lightweight tag borrows its commit's.
  tag_date=""
  ref_json=$(gh api "repos/We-Amp/mod_pagespeed/git/refs/tags/v${latest_stable}" 2>/dev/null || true)
  if [ -n "$ref_json" ]; then
    obj_type=$(printf '%s' "$ref_json" | jq -r '.object.type // empty')
    obj_sha=$(printf '%s' "$ref_json" | jq -r '.object.sha // empty')
    case "$obj_type" in
      tag)    tag_date=$(gh api "repos/We-Amp/mod_pagespeed/git/tags/${obj_sha}" --jq '.tagger.date // empty' 2>/dev/null || true) ;;
      commit) tag_date=$(gh api "repos/We-Amp/mod_pagespeed/git/commits/${obj_sha}" --jq '.committer.date // empty' 2>/dev/null || true) ;;
    esac
  fi
  tag_ct=0
  if [ -n "$tag_date" ]; then
    tag_ct=$(to_epoch "$tag_date")
  fi
  now=$(date -u +%s)
  tag_age_h=$(( (now - tag_ct) / 3600 ))
  if [ "$tag_ct" -gt 0 ] && [ "$tag_age_h" -lt "$grace" ]; then
    echo "::warning::the 2.1 manifest (${manifest}) lags the latest stable tag v${latest_stable}, tagged ${tag_age_h}h ago -- within the ${grace}h grace window. Passing; this fails once the tag is older than that. Bump release.yaml, or, for a module-only release, MODULE_PACKAGE_VERSION in website/src/data/product-facts.mjs."
  else
    complain soft "the 2.1 manifest (${manifest}) lags the latest stable tag v${latest_stable} (tag age ${tag_age_h}h, or tag date unreadable). Bump website/src/content/releases-2.1/release.yaml, or, for a module-only release (no optimizer v${latest_stable} release), MODULE_PACKAGE_VERSION in website/src/data/product-facts.mjs."
  fi
fi

# Hand-pinned image tags: a ghcr.io/we-amp/pagespeed-*:2.X.Y literal in
# non-frozen content (same scope as the URL guard: blog exempt) must equal the
# manifest semver. These are copy-paste install commands, and a stale one pulls
# an old image. .md cannot use the <ImageTag /> component, so this keeps those
# literals honest. The same holds for a Compose default
# (`${PAGESPEED_VERSION:-2.X.Y}`) or an .env value (`PAGESPEED_VERSION=2.X.Y`),
# and `latest` is never a valid tag for the worker or nginx image (only the
# combined image publishes one), so those fail too.
esc="${manifest//./\\.}"
stale=$(find website/src/content website/src/pages website/src/components website/src/layouts \
  -type f \( -name '*.md' -o -name '*.mdx' -o -name '*.astro' \) \
  -not -path '*/content/blog/*' \
  -exec grep -Eoh 'ghcr\.io/we-amp/pagespeed-[a-z]+:2\.[0-9]+\.[0-9]+|PAGESPEED_VERSION(:-|=)(2\.[0-9]+\.[0-9]+|latest)|pagespeed-(worker|nginx):(\$\{PAGESPEED_VERSION:-)?latest' {} + 2>/dev/null \
  | sort -u | grep -vE "(:|:-|=)${esc}\$" || true)
if [ -n "$stale" ]; then
  complain sync "stale image tag(s) in non-frozen content (must be :${manifest}): $(printf '%s' "$stale" | tr '\n' ' ')"
fi

if [ "$fail" = 0 ] && [ "$warned" = 0 ]; then
  echo "2.1 version-drift guard OK: manifest=${manifest} latest_stable_tag=v${latest_stable} module_packages=${module_pkg:-unknown}; window=${window}."
fi
exit "$fail"
