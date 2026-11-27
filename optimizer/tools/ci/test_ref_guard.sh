#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Self-test for tools/ci/ref_guard.sh (builds a throwaway repository).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=ref_guard.sh
. "$HERE/ref_guard.sh"

fail=0
expect() { # expect ok|no <desc> <cmd...>
  local want="$1" desc="$2"; shift 2
  if "$@" 2>/dev/null; then got=ok; else got=no; fi
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted $want, got $got)"; fail=1; fi
}

expect ok "main"                    ref_guard_ref_allowed refs/heads/main
expect ok "tag v1.2.3"              ref_guard_ref_allowed refs/tags/v1.2.3
expect no "ci-dev branch"           ref_guard_ref_allowed refs/heads/ci-dev
expect no "feature branch"          ref_guard_ref_allowed refs/heads/feature/x
expect no "pull ref"                ref_guard_ref_allowed refs/pull/7/merge
expect no "branch named v1"         ref_guard_ref_allowed refs/heads/v1.2.3
expect no "tag not v*"              ref_guard_ref_allowed refs/tags/optimizer-v1.0.0
expect no "empty ref"               ref_guard_ref_allowed ""
expect ok "optimizer tag, glob"     ref_guard_ref_allowed refs/tags/optimizer-v1.0.0 'optimizer-v*'
expect no "v tag, optimizer glob"   ref_guard_ref_allowed refs/tags/v1.0.0 'optimizer-v*'
expect ok "main, empty glob"        ref_guard_ref_allowed refs/heads/main ''
expect no "tag, empty glob"         ref_guard_ref_allowed refs/tags/v1.0.0 ''

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
(
  cd "$tmp" && git init -q -b main . &&
  git config user.email t@example.invalid && git config user.name t &&
  git commit -q --allow-empty -m a && git tag v1.0.0 &&
  git commit -q --allow-empty -m b && git tag v1.1.0 &&
  git checkout -q -b side && git commit -q --allow-empty -m c && git tag v9.9.9 && git tag -a -m ann v9.9.8 && git tag -a -m ann v1.0.0-ann main~1 &&
  git checkout -q main && git update-ref refs/remotes/origin/main main
) || { echo "FAIL repo setup"; exit 1; }

cd "$tmp" || exit 1
main_sha="$(git rev-parse main)"
expect ok "dispatch tag on main"        ref_guard_dispatch_tag_ok v1.0.0
expect ok "dispatch newest tag on main" ref_guard_dispatch_tag_ok v1.1.0
expect no "dispatch tag not on main"    ref_guard_dispatch_tag_ok v9.9.9
expect no "dispatch tag missing"        ref_guard_dispatch_tag_ok v7.7.7
expect no "dispatch tag empty"          ref_guard_dispatch_tag_ok ""
expect no "dispatch tag with option"    ref_guard_dispatch_tag_ok "--all"
expect no "dispatch tag not v*"         ref_guard_dispatch_tag_ok side
expect no "dispatch tag, no-tag job"    ref_guard_dispatch_tag_ok v1.0.0 ''

# check REF EVENT [INPUT_TAG] [SHA]; GITHUB_SHA defaults to main's head.
check() {
  (
    export GITHUB_REF="$1" GITHUB_EVENT_NAME="$2"
    export GITHUB_SHA="${4:-$(git rev-parse main)}"
    unset INPUT_TAG
    [ "$#" -ge 3 ] && export INPUT_TAG="$3"
    ref_guard_check
  )
}
side="$(git rev-parse side)"
ann="$(git rev-parse 'v1.0.0-ann^{commit}')"
expect ok "check: push tag on main"          check refs/tags/v1.0.0 push "" "$(git rev-parse v1.0.0)"
expect no "check: push off-main v tag"       check refs/tags/v9.9.9 push "" "$side"
expect no "check: dispatch off-main v tag"   check refs/tags/v9.9.9 workflow_dispatch v9.9.9 "$side"
expect no "check: push off-main annotated"   check refs/tags/v9.9.8 push "" "$side"
expect ok "check: push annotated on main"    check refs/tags/v1.0.0-ann push "" "$ann"
expect ok "check: dispatch on tag, input eq" check refs/tags/v1.1.0 workflow_dispatch v1.1.0
expect no "check: dispatch tag/input differ" check refs/tags/v1.1.0 workflow_dispatch v1.0.0
expect no "check: dispatch tag, empty input" check refs/tags/v1.1.0 workflow_dispatch ""
expect ok "check: dispatch main + tag"       check refs/heads/main workflow_dispatch v1.1.0
expect no "check: dispatch main + side tag"  check refs/heads/main workflow_dispatch v9.9.9
expect no "check: dispatch ci-dev branch"    check refs/heads/ci-dev workflow_dispatch v1.0.0
expect no "check: dispatch empty tag"        check refs/heads/main workflow_dispatch ""
expect ok "check: schedule on main"          check refs/heads/main schedule

# Event allowlist: a release pipeline refuses every event but its own triggers.
evcheck() { # evcheck EVENT ALLOWED
  ( export GITHUB_REF=refs/heads/main GITHUB_EVENT_NAME="$1" GITHUB_SHA="$main_sha" ALLOWED_EVENTS="$2"
    unset INPUT_TAG; ref_guard_check )
}
expect ok "events: unset list allows any"    evcheck schedule ""
expect ok "events: push allowed"             evcheck push "push workflow_dispatch"
expect no "events: pull_request refused"     evcheck pull_request "push workflow_dispatch"
expect no "events: schedule refused"         evcheck schedule "push workflow_dispatch"
expect ok "event fn: listed"                 ref_guard_event_allowed release "release workflow_dispatch"
expect no "event fn: not listed"             ref_guard_event_allowed pull_request "release"

# Composite action path: the same env the action builds for a dispatch.
act() { ( export GITHUB_REF=refs/heads/main GITHUB_EVENT_NAME=workflow_dispatch GITHUB_SHA="$main_sha" \
          ALLOWED_TAG_GLOB="$1" DISPATCH_TAG="$2"
          if [ "$GITHUB_EVENT_NAME" = "workflow_dispatch" ] && [ -n "$ALLOWED_TAG_GLOB" ]; then export INPUT_TAG="$DISPATCH_TAG"; fi
          ref_guard_check ); }
expect ok "action env: dispatch good tag"    act 'v*' v1.0.0
expect no "action env: dispatch empty tag"   act 'v*' ""
expect ok "action env: main-only, no tag"    act '' ""

# Checkout ref/sha outputs: a dispatch builds the dispatched tag, a tag push
# the pushed ref. Outputs are compared as strings, not just exit codes.
outs() { # outs REF EVENT [INPUT_TAG] [SHA]
  ( export GITHUB_REF="$1" GITHUB_EVENT_NAME="$2" GITHUB_SHA="${4:-$(git rev-parse main)}"
    unset INPUT_TAG; [ "$#" -ge 3 ] && export INPUT_TAG="$3"
    ref_guard_outputs )
}
expect_out() { # expect_out <wanted output> <desc> <cmd...>
  local want="$1" desc="$2"; shift 2
  local got; got="$("$@" 2>/dev/null | tr '\n' ' ')"
  if [ "$got" = "$want" ]; then echo "ok   $desc"; else echo "FAIL $desc (wanted '$want', got '$got')"; fail=1; fi
}
v110="$(git rev-parse v1.1.0)"; v100="$(git rev-parse v1.0.0)"
expect_out "ref=refs/tags/v1.0.0 sha=$v100 " "outputs: dispatch from main -> dispatched tag" outs refs/heads/main workflow_dispatch v1.0.0
expect_out "ref=refs/tags/v1.1.0 sha=$v110 " "outputs: dispatch on tag -> that tag"          outs refs/tags/v1.1.0 workflow_dispatch v1.1.0 "$v110"
expect_out "ref=refs/tags/v1.0.0 sha=$v100 " "outputs: tag push -> pushed ref"               outs refs/tags/v1.0.0 push "" "$v100"
expect_out "ref=refs/heads/main sha=$(git rev-parse main) " "outputs: non-dispatch main -> main" outs refs/heads/main schedule

expect no "outputs: moved tag (sha differs)" outs refs/tags/v1.0.0 push "" "$v110"

[ "$fail" = 0 ] && echo "ref_guard self-test: all passed"
exit "$fail"
