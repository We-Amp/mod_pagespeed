#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# check-floor-ratchet.sh - forward-only floor enforcement for the vendored
# kernel pins (#1104; reused by the #1130 HTML kernel vendoring
# via --main-pin-path lib/html/HTML_KERNEL_PIN).
#
# The pin file (lib/js/JS_KERNEL_PIN, or --main-pin-path's target) records a
# `floor <sha>` line: the pin-downgrade ratchet. The sync tool enforces
# pin >= floor; THIS script enforces that the floor itself only ever moves
# forward relative to origin/main, and that the floor line cannot be deleted
# or duplicated.
#
# Two callers, one logic:
#   * CI's js-kernel-differential job (--mode
#     required): the REQUIRED merge gate. Tree-local validity + equality
#     need no canonical-repo access; an actual floor MOVE verifies
#     ancestry in the canonical 1.15 repo (persistent mirror, then a
#     one-shot clone) and FAILS CLOSED if it is unreachable — floor moves
#     only happen on pin bumps, so the network dependency sits in the
#     rare path.
#   * --mode drift: a non-gating variant kept for local use (no workflow
#     calls it): same validity + equality unconditionally; for a MOVE it
#     uses the canonical source given by --canonical. With no source it
#     warn-passes on pull_request and fails otherwise.
#
# Main-side floor states (F2): ok (present + valid) / absent (no floor
# line: pull_request-only bootstrap notice-pass, every other event fails —
# once the ratchet has shipped, absent-on-main means main regressed) /
# invalid (malformed or duplicated line: always fail closed).
#
# Exit codes: 0 pass, 1 fail. All failure text goes to stderr with GitHub
# annotations (plain text when run locally).

set -euo pipefail

PIN_FILE=""
BASE_REPO=""
EVENT=""
MODE=""
CANONICAL=""
CANONICAL_URL=""
MIRROR_DIR_ARG=""
# Repo-relative path of the pin file as read from origin/main. Defaults to
# the D1 JS-kernel pin (the script's original caller); the HTML kernel
# vendoring (#1130) passes --main-pin-path lib/html/HTML_KERNEL_PIN.
MAIN_PIN_PATH="lib/js/JS_KERNEL_PIN"

usage() {
  echo "Usage: $0 --pin-file PATH --base-repo GITDIR --event EVENT --mode drift|required" >&2
  echo "          [--canonical GITDIR] [--canonical-url URL --mirror-dir PATH]" >&2
  echo "          [--main-pin-path REPO-RELATIVE-PATH]" >&2
  exit 2
}

# Every flag takes a value. A flag left value-less (e.g. --pin-file as the
# last argument) would otherwise die inside `shift 2` under set -e with
# rc=1 and no output at all; diagnose it as a mis-invocation (rc=2) instead.
need_value() {
  [[ "$2" -ge 2 ]] || { echo "Missing value for $1" >&2; usage; }
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --pin-file)      need_value "$1" $#; PIN_FILE="$2"; shift 2 ;;
    --base-repo)     need_value "$1" $#; BASE_REPO="$2"; shift 2 ;;
    --event)         need_value "$1" $#; EVENT="$2"; shift 2 ;;
    --mode)          need_value "$1" $#; MODE="$2"; shift 2 ;;
    --canonical)     need_value "$1" $#; CANONICAL="$2"; shift 2 ;;
    --canonical-url) need_value "$1" $#; CANONICAL_URL="$2"; shift 2 ;;
    --mirror-dir)    need_value "$1" $#; MIRROR_DIR_ARG="$2"; shift 2 ;;
    --main-pin-path) need_value "$1" $#; MAIN_PIN_PATH="$2"; shift 2 ;;
    *) echo "Unknown argument: $1" >&2; usage ;;
  esac
done
[[ -n "$PIN_FILE" && -n "$BASE_REPO" && -n "$EVENT" ]] || usage
[[ "$MODE" == "drift" || "$MODE" == "required" ]] || usage

# MAIN_PIN_PATH is interpolated into `git show origin/main:<path>` — require
# a plain repo-relative path of non-dot path segments: at least one
# directory plus a filename that starts alnum/underscore. This rejects
# absolute paths, empty/dot/dotdot segments (".", "..", "a/./b", "a/../b",
# "lib//x", trailing slashes) and option/whitespace smuggling alike.
if ! [[ "$MAIN_PIN_PATH" =~ ^[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*/[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]]; then
  echo "::error::floor ratchet: --main-pin-path '${MAIN_PIN_PATH}' is not a plain repo-relative dir/file path." >&2
  exit 2
fi
# Doc reference for diagnostics, derived from the pin file's directory.
DOC_REF="$(dirname "$MAIN_PIN_PATH")/CLAUDE.md"

SHA_RE='^[0-9a-f]{40}$'

# Parse a pin-file body on stdin: prints "<state> <sha>" where state is
# ok|absent|invalid (invalid covers malformed AND duplicated floor lines).
parse_floor() {
  awk '
    $1 == "floor" { n++; sha = $2 }
    END {
      if (n == 0)      { print "absent" }
      else if (n > 1)  { print "invalid duplicated" }
      else if (sha ~ /^[0-9a-f]{40}$/) { print "ok " sha }
      else             { print "invalid malformed" }
    }'
}

# ---- HEAD side: tree-local validity, enforced always ----------------------
if [[ ! -f "$PIN_FILE" ]]; then
  echo "::error::floor ratchet: pin file not found: ${PIN_FILE}" >&2
  exit 1
fi
read -r HEAD_STATE HEAD_FLOOR _ < <(parse_floor < "$PIN_FILE")
if [[ "$HEAD_STATE" != "ok" ]]; then
  echo "::error::floor ratchet: ${PIN_FILE} must contain exactly one valid 'floor <40-hex-sha>' line (found: ${HEAD_STATE}${HEAD_FLOOR:+ ${HEAD_FLOOR}}). Deleting or duplicating the floor line is rejected fail-closed; see ${DOC_REF}." >&2
  exit 1
fi
if ! [[ "$HEAD_FLOOR" =~ $SHA_RE ]]; then
  # parse_floor already guarantees this; belt-and-suspenders for the
  # ancestry commands below.
  echo "::error::floor ratchet: internal parse error on '${HEAD_FLOOR}'." >&2
  exit 1
fi

# ---- MAIN side: fetch origin/main and read its floor ----------------------
if ! git -C "$BASE_REPO" fetch --no-tags --depth=1 origin main; then
  echo "::error::floor ratchet: could not fetch origin/main in '${BASE_REPO}' — the ratchet reference is unavailable; failing closed." >&2
  exit 1
fi
MAIN_PIN_BODY="$(git -C "$BASE_REPO" show "origin/main:${MAIN_PIN_PATH}" 2>/dev/null || true)"
if [[ -z "$MAIN_PIN_BODY" ]]; then
  MAIN_STATE="absent"
  MAIN_FLOOR=""
else
  read -r MAIN_STATE MAIN_FLOOR _ < <(printf '%s\n' "$MAIN_PIN_BODY" | parse_floor)
fi

case "$MAIN_STATE" in
  absent)
    if [[ "$EVENT" == "pull_request" ]]; then
      echo "::notice::floor ratchet: origin/main has no floor line yet (bootstrap) — head floor ${HEAD_FLOOR:0:12} accepted on this pull_request; every other event fails on this state."
      exit 0
    fi
    echo "::error::floor ratchet: origin/main's ${MAIN_PIN_PATH} has no floor line on a ${EVENT} event — outside the pull_request bootstrap this means main regressed; failing closed." >&2
    exit 1 ;;
  invalid)
    echo "::error::floor ratchet: origin/main's ${MAIN_PIN_PATH} floor line is ${MAIN_FLOOR:-invalid} — the ratchet reference is unusable; failing closed." >&2
    exit 1 ;;
  ok) ;;
  *)
    echo "::error::floor ratchet: internal state '${MAIN_STATE}'." >&2
    exit 1 ;;
esac
if ! [[ "$MAIN_FLOOR" =~ $SHA_RE ]]; then
  echo "::error::floor ratchet: internal parse error on main floor '${MAIN_FLOOR}'." >&2
  exit 1
fi

# ---- Equality: the common case, no canonical-repo access needed -----------
if [[ "$HEAD_FLOOR" == "$MAIN_FLOOR" ]]; then
  echo "floor ratchet OK: floor ${HEAD_FLOOR:0:12} matches origin/main."
  exit 0
fi

# ---- Floor MOVED: ancestry must be verified in the canonical 1.15 repo ----
resolves_both() {  # $1 = git dir
  git -C "$1" cat-file -e "${MAIN_FLOOR}^{commit}" 2>/dev/null \
    && git -C "$1" cat-file -e "${HEAD_FLOOR}^{commit}" 2>/dev/null
}

SRC=""
if [[ -n "$CANONICAL" && -d "$CANONICAL" ]] && resolves_both "$CANONICAL"; then
  SRC="$CANONICAL"
fi
CLONE_TMP=""
if [[ -z "$SRC" && -n "$CANONICAL_URL" ]]; then
  # Mirror-then-clone fallback (required mode): persistent mirror first,
  # then a one-shot clone.
  if [[ -n "$MIRROR_DIR_ARG" ]]; then
    mkdir -p "$(dirname "$MIRROR_DIR_ARG")"
    # Serialize mirror writers on the CI runner fleet: both required
    # ratchet jobs (HTML and JS kernel) share this mirror path,
    # and nothing keys them to the same concurrency group — concurrent
    # `fetch --prune` calls contend on ref/FETCH_HEAD locks, and the
    # losing job would fall back to a stale mirror (slow one-shot clone,
    # or a spurious red on the required check). The lock also makes the
    # clone cleanup below safe: it only ever removes a clone THIS caller
    # created, because no other caller can observe the mirror as absent
    # while we hold the lock. Bounded wait: a refresh fetch is seconds,
    # a first full mirror clone is minutes; 600s covers the worst case.
    # Acquisition is guarded: an unopenable lock file (e.g. a stale
    # foreign-owned one — the subshell probe keeps the failed `exec`
    # redirection from killing the script) or a lock held past the wait
    # takes the same warn-and-skip path, which skips ONLY the refresh:
    # the mirror is still read as-is below, and the one-shot clone after
    # that is untouched — the required check's fail-closed semantics are
    # unchanged. flock is Linux-only, but --mirror-dir is a CI-only flag
    # (all callers run on Linux CI runners); without flock the
    # acquisition fails into the same skip path, no new failure mode.
    mirror_locked=0
    if ( exec 9>"${MIRROR_DIR_ARG}.lock" ) 2>/dev/null; then
      exec 9>"${MIRROR_DIR_ARG}.lock"
      if flock -w 600 9; then
        mirror_locked=1
      fi
    fi
    if [[ "$mirror_locked" == 1 ]]; then
      if [[ -d "$MIRROR_DIR_ARG" ]]; then
        git -C "$MIRROR_DIR_ARG" fetch --prune origin 2>/dev/null \
          || echo "::warning::floor ratchet: mirror fetch failed; trying the stale mirror state." >&2
      else
        git clone --mirror --filter=blob:none "$CANONICAL_URL" "$MIRROR_DIR_ARG" 2>/dev/null \
          || rm -rf "$MIRROR_DIR_ARG"
      fi
      flock -u 9
    else
      echo "::warning::floor ratchet: mirror lock unavailable (open failed, or held >600s by another job); skipping the mirror REFRESH — the last good mirror state is still read below." >&2
    fi
    if [[ -d "$MIRROR_DIR_ARG" ]] && resolves_both "$MIRROR_DIR_ARG"; then
      SRC="$MIRROR_DIR_ARG"
    fi
  fi
  if [[ -z "$SRC" ]]; then
    CLONE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/floor-ratchet.XXXXXX")"
    trap '[[ -n "$CLONE_TMP" ]] && rm -rf "$CLONE_TMP"' EXIT
    if git clone --filter=blob:none --no-checkout --no-single-branch \
         "$CANONICAL_URL" "${CLONE_TMP}/canonical" 2>/dev/null \
       && resolves_both "${CLONE_TMP}/canonical"; then
      SRC="${CLONE_TMP}/canonical"
    fi
  fi
fi

# ---- Canonical re-home: the previous floor has no home any more ----------
# When the canonical repository moves (the vendored kernels are re-homed to a
# repository whose history does not contain the previous floor), no ancestry
# between the two floors can exist. The move is accepted exactly when the
# fresh one-shot clone of the canonical (never a local checkout or the
# persistent mirror, which can be stale) knows nothing of the previous floor
# and the new floor is a merged commit of the canonical default branch: that
# is the forward-only property restated for the new home. A downgrade within
# one history never takes this path, because there the previous floor
# resolves and the ancestry rules below decide; without a fresh clone the
# check stays closed.
on_default_branch() {  # $1 = git dir, $2 = sha
  local ref
  for ref in refs/remotes/origin/HEAD refs/remotes/origin/master \
             refs/remotes/origin/main refs/heads/master refs/heads/main HEAD; do
    if git -C "$1" rev-parse --verify -q "${ref}^{commit}" >/dev/null 2>&1; then
      git -C "$1" merge-base --is-ancestor "$2" "$ref" 2>/dev/null
      return $?
    fi
  done
  return 1
}
if [[ -z "$SRC" && -n "$CLONE_TMP" && -d "${CLONE_TMP}/canonical" ]]; then
  fresh="${CLONE_TMP}/canonical"
  if ! git -C "$fresh" cat-file -e "${MAIN_FLOOR}^{commit}" 2>/dev/null \
     && git -C "$fresh" cat-file -e "${HEAD_FLOOR}^{commit}" 2>/dev/null \
     && on_default_branch "$fresh" "$HEAD_FLOOR"; then
    echo "::notice::floor ratchet: canonical re-homed. The previous floor ${MAIN_FLOOR:0:12} is unknown to the canonical repository and the new floor ${HEAD_FLOOR:0:12} is on its default branch; the move is accepted as the new baseline."
    exit 0
  fi
fi

if [[ -z "$SRC" ]]; then
  if [[ "$MODE" == "drift" && "$EVENT" == "pull_request" ]]; then
    echo "::warning::floor ratchet: floor moved (${MAIN_FLOOR:0:12} -> ${HEAD_FLOOR:0:12}) but no canonical source resolves both commits — ancestry NOT verified; passing only because this is a pull_request drift run. The required differential gate fails closed on this state."
    exit 0
  fi
  echo "::error::floor ratchet: floor moved (${MAIN_FLOOR:0:12} -> ${HEAD_FLOOR:0:12}) but the canonical 1.15 repo is unreachable / does not resolve both commits — ancestry cannot be verified; failing closed. A floor-moving PR needs the canonical repo reachable from CI." >&2
  exit 1
fi

if git -C "$SRC" merge-base --is-ancestor "$MAIN_FLOOR" "$HEAD_FLOOR"; then
  echo "floor ratchet OK: floor moved forward ${MAIN_FLOOR:0:12} -> ${HEAD_FLOOR:0:12} (main floor is an ancestor of the new floor)."
  exit 0
fi
# Stale-run tolerance — DRIFT MODE ONLY: outside pull_request, a delayed/
# re-run build of an OLDER commit can see a main floor that already advanced
# past it. That is not a downgrade of main (the advancing merge passed its
# own gates), so the non-gating drift mode tolerates it with a notice; on
# pull_request it stays a hard fail — a PR whose floor is behind main must
# rebase, not merge.
#
# Required mode gets NO such tolerance: the event name is caller-controlled
# (workflow_dispatch runs on an arbitrary ref chosen by the dispatcher), so
# it cannot discriminate a genuinely stale run from a regression, and the
# required gate has no legitimate stale-run case to serve. Accepted cost: a
# genuinely delayed non-PR required run of an older main commit goes red —
# that is cosmetic and intentional fail-closed behavior, whereas a pass
# here would be a hole in the merge gate.
if [[ "$MODE" == "drift" && "$EVENT" != "pull_request" ]] \
   && git -C "$SRC" merge-base --is-ancestor "$HEAD_FLOOR" "$MAIN_FLOOR"; then
  echo "::notice::floor ratchet: this tree's floor ${HEAD_FLOOR:0:12} is an ancestor of origin/main's ${MAIN_FLOOR:0:12} (stale ${EVENT} run of an older commit) — main itself has not moved backwards; passing (advisory drift mode only; the required gate fails on this state)."
  exit 0
fi
echo "::error::RATCHET violation: this tree's floor ${HEAD_FLOOR:0:12} is neither equal to nor a descendant of origin/main's floor ${MAIN_FLOOR:0:12}. The pin-downgrade floor only moves forward; see ${DOC_REF}." >&2
exit 1
