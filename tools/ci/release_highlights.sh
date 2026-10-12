#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Print the release-body sections of one release from RELEASE_NOTES.md: the
# "## Highlights" section, then the "## Action required when upgrading" section
# when the release has one.
#
#   release_highlights.sh NOTES_FILE TAG
#   release_highlights.sh --self-test
#
# The release's entry starts at "# mod_pagespeed <version> Release Notes".
# <version> is tried as the tag without its leading 'v' (1.15.0+r22), then with
# the pre-release part dropped (1.16.0-rc.8+r1 -> 1.16.0+r1), then -- only for
# a pre-release tag (one containing '-') -- the bare version (1.16.0). A rebuild
# tag such as v1.15.0+r23 therefore never picks up the 1.15.0 base entry.
# A section runs from its "## " heading to the next "## " or "# " heading.
# Lines inside code fences (``` or ~~~, optionally indented) are ignored for
# heading detection.
#
# Fails closed (non-zero, nothing on stdout) when the release has no entry,
# has no Highlights, or Highlights / Action required is present but empty.
# An absent Action required section is fine.
set -euo pipefail

# section FILE HEADLINE SECTION_HEADING -> prints the section body (may be empty);
# returns 3 when the section heading is absent.
section() {
  awk -v head="$2" -v want="$3" '
    /^[ \t]*(```|~~~)/ { if (insec) fence = !fence }
    !fence && /^# / { if ($0 == head) { insec = 1; next } else if (insec) exit }
    insec && !fence && /^## / { if ($0 == want) { inhl = 1; seen = 1; next } else if (inhl) exit }
    inhl { print }
    END { exit seen ? 0 : 3 }
  ' "$1"
}

trim() {
  awk 'NF { p = 1 } p { buf[++n] = $0 } END { while (n && buf[n] ~ /^[ \t]*$/) n--; for (i = 1; i <= n; i++) print buf[i] }'
}

extract() {
  local notes="$1" tag="$2" ver core build cand found="" head hl ar rc
  [ -f "$notes" ] || { echo "release_highlights: no such file: $notes" >&2; return 1; }
  ver=${tag#v}
  core=${ver%%[-+]*}
  build=""
  case "$ver" in *+*) build="+${ver#*+}" ;; esac
  local cands=("$ver")
  case "$ver" in
    *-*) cands+=("${core}${build}" "$core") ;;
  esac
  for cand in "${cands[@]}"; do
    if grep -qxF "# mod_pagespeed ${cand} Release Notes" "$notes"; then found="$cand"; break; fi
  done
  if [ -z "$found" ]; then
    echo "release_highlights: no '# mod_pagespeed <version> Release Notes' entry in $notes matches tag $tag" >&2
    return 1
  fi
  head="# mod_pagespeed ${found} Release Notes"
  rc=0; hl=$(section "$notes" "$head" "## Highlights" | trim) || rc=$?
  if [ "$rc" != 0 ] || [ -z "$(printf '%s' "$hl" | tr -d '[:space:]')" ]; then
    echo "release_highlights: '$found' has a missing or empty Highlights section in $notes" >&2
    return 1
  fi
  rc=0; ar=$(section "$notes" "$head" "## Action required when upgrading" | trim) || rc=$?
  printf '%s\n' "$hl"
  if [ "$rc" = 0 ]; then
    if [ -z "$(printf '%s' "$ar" | tr -d '[:space:]')" ]; then
      echo "release_highlights: '$found' has an empty 'Action required when upgrading' section in $notes" >&2
      return 1
    fi
    printf '\n## Action required when upgrading\n\n%s\n' "$ar"
  fi
}

self_test() {
  local t fail=0 got
  t=$(mktemp -d)
  trap 'rm -rf "$t"' RETURN
  cat > "$t/n.md" <<'FIX'
# mod_pagespeed 1.17.0 Release Notes

## Highlights

- new thing

### Sub heading

```
# not a heading
```

## Action required when upgrading

other

# mod_pagespeed 1.16.0 Release Notes

## Highlights

- old thing 100%

# mod_pagespeed 1.15.0+r22 Release Notes

## Highlights

- plus build

# mod_pagespeed 1.15.0+r21 Release Notes

## Overview

no highlights here

# mod_pagespeed 1.14.0 Release Notes

## Highlights

## Next

x

# mod_pagespeed 1.13.0 Release Notes

## Highlights

- h13

## Action required when upgrading

## Next

x

# mod_pagespeed 1.12.0 Release Notes

  ~~~
  ## Highlights
  ~~~

## Highlights

- h12

```
## Action required when upgrading
```

## Packaging

p
FIX
  got=$(extract "$t/n.md" v1.17.0) || { echo "FAIL: 1.17.0"; fail=1; }
  case "$got" in "- new thing"*"# not a heading"*) ;; *) echo "FAIL: 1.17.0 content"; fail=1 ;; esac
  case "$got" in *"old thing"*) echo "FAIL: leaked past section"; fail=1 ;; esac
  [ "$(extract "$t/n.md" v1.16.0-rc.8+r1)" = "- old thing 100%" ] || { echo "FAIL: rc tag"; fail=1; }
  [ "$(extract "$t/n.md" v1.15.0+r22)" = "- plus build" ] || { echo "FAIL: +r tag"; fail=1; }
  # Action required section is appended when present
  got=$(extract "$t/n.md" v1.17.0)
  case "$got" in *"## Action required when upgrading"*"other"*) ;; *) echo "FAIL: action section missing"; fail=1 ;; esac
  case "$got" in *"old thing"*) echo "FAIL: action leaked"; fail=1 ;; esac
  # Middle step: version plus build, pre-release part dropped (1.15.0+r22 exists)
  [ "$(extract "$t/n.md" v1.15.0-rc.1+r22)" = "- plus build" ] || { echo "FAIL: middle step"; fail=1; }
  # Rebuild tag must not fall back to the base entry
  extract "$t/n.md" v1.16.0+r5 >/dev/null 2>&1 && { echo "FAIL: rebuild tag took base entry"; fail=1; }
  # Empty (present) Action required is refused
  extract "$t/n.md" v1.13.0 >/dev/null 2>&1 && { echo "FAIL: empty action accepted"; fail=1; }
  # Fenced headings (indented ~~~, ```) are not sections
  got=$(extract "$t/n.md" v1.12.0) || { echo "FAIL: 1.12.0"; fail=1; }
  case "$got" in "- h12"*) ;; *) echo "FAIL: fence content"; fail=1 ;; esac
  extract "$t/n.md" v1.15.0+r21 >/dev/null 2>&1 && { echo "FAIL: no-highlights accepted"; fail=1; }
  extract "$t/n.md" v1.14.0 >/dev/null 2>&1 && { echo "FAIL: empty accepted"; fail=1; }
  extract "$t/n.md" v9.9.9 >/dev/null 2>&1 && { echo "FAIL: missing release accepted"; fail=1; }
  extract "$t/missing.md" v1.17.0 >/dev/null 2>&1 && { echo "FAIL: missing file accepted"; fail=1; }
  [ "$fail" = 0 ] && echo "release_highlights self-test: ok"
  return "$fail"
}

case "${1:-}" in
  --self-test) self_test ;;
  "") echo "usage: $0 NOTES_FILE TAG | --self-test" >&2; exit 2 ;;
  *) [ $# -eq 2 ] || { echo "usage: $0 NOTES_FILE TAG" >&2; exit 2; }; extract "$1" "$2" ;;
esac
