#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/pr_tier_scope.sh: a fake `gh` and `sleep` on PATH and a
# temporary git repository with a root commit and one child commit whose
# changed path each case sets.
#
# Run: tools/ci/test_pr_tier_scope.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCOPE="${HERE}/pr_tier_scope.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

BIN="${WORK}/bin"
mkdir -p "${BIN}"
# Fake gh: logs every call to FAKE_LOG. FAKE_GH_FAIL=1 fails every call with
# HTTP 500; otherwise it prints FAKE_GH_BODY (which may be empty) and exits 0.
cat > "${BIN}/gh" <<'GH'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_LOG}"
if [ "${FAKE_GH_FAIL:-0}" = "1" ]; then
  echo "HTTP 500" >&2
  exit 1
fi
case "$*" in
  *"/rules/branches/"*)
    [ "${FAKE_RULES_FAIL:-0}" != "1" ] || { echo "HTTP 500" >&2; exit 1; }
    printf '%s' "${FAKE_RULES-[]}"
    exit 0 ;;
  *"/pulls/"*)
    if [ "${FAKE_PR_FAIL:-0}" = "1" ]; then echo "HTTP 500" >&2; exit 1; fi
    all="$*"; n="${all##*/pulls/}"; n="${n%% *}"
    v="FAKE_PR_OUT_${n}"
    if [ -n "${!v+x}" ]; then printf '%b' "${!v}"; else printf '%b' "${FAKE_PR_OUT-}"; fi
    exit 0 ;;
esac
printf '%s' "${FAKE_GH_BODY-}"
GH
cat > "${BIN}/sleep" <<'SL'
#!/usr/bin/env bash
echo "$1" >> "${SLEEP_LOG}"
SL
chmod +x "${BIN}/gh" "${BIN}/sleep"

FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

REPO="${WORK}/repo"
SHA_VAL=""
BEFORE_VAL=""

# make_repo <changed path>: a root commit and one child commit that adds the
# path. make_repo with no argument leaves only the root commit.
make_repo() {
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  git -C "${REPO}" commit -q --allow-empty -m "root (#1)"
  BEFORE_VAL="$(git -C "${REPO}" rev-parse HEAD)"
  if [ $# -gt 0 ]; then
    mkdir -p "${REPO}/$(dirname "$1")"
    echo x > "${REPO}/$1"
    git -C "${REPO}" add -A
    git -C "${REPO}" commit -q -m "change $1 (#12)"
  fi
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
}

suite() { # <head_branch> <status> <conclusion as JSON>
  printf '{"head_branch":"%s","status":"%s","conclusion":%s}' "$1" "$2" "$3"
}
body() { # <suite>...
  local IFS=,
  printf '{"total_count":%d,"check_suites":[%s]}' "$#" "$*"
}

# proof_for <base sha>: a successful queue suite built on that base commit.
proof_for() {
  body "$(suite "gh-readonly-queue/master/pr-12-$1" completed '"success"')"
}
PROOF_MASTER="$(body "$(suite master completed '"success"')")"
PROOF_FAILED="$(body "$(suite gh-readonly-queue/master/pr-12-abc completed '"failure"')")"
PROOF_RUNNING="$(body "$(suite gh-readonly-queue/master/pr-12-abc in_progress null)")"

OUT="${WORK}/out"
ERR="${WORK}/err"

# run_scope [env assignments...]: runs the script in the repository with the
# push defaults; stdout to OUT, stderr to ERR, the exit code in RC.
run_scope() {
  : > "${WORK}/gh.log"
  : > "${WORK}/sleep.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" SLEEP_LOG="${WORK}/sleep.log" \
    GITHUB_EVENT_NAME=push GITHUB_REPOSITORY=We-Amp/example SHA="${SHA_VAL}" BEFORE_SHA="${BEFORE_VAL}" GH_TOKEN=x \
    FORK_OR_BOT=false PR_FULL=false CI_SCOPE= FAKE_GH_BODY="$(proof_for "${BEFORE_VAL}")" \
    FAKE_PR_OUT='false\toschaaf\tWe-Amp/example\n' \
    "$@" bash "${SCOPE}" > "${OUT}" 2> "${ERR}")
  RC=$?
  set -e
  CASES=$((CASES + 1))
  if [ "$(wc -l < "${OUT}" | tr -d ' ')" = 2 ]; then STDOUT_OK=$((STDOUT_OK + 1)); else STDOUT_BAD="${STDOUT_BAD} ${CUR}"; fi
}

CASES=0
STDOUT_OK=0
STDOUT_BAD=""
CUR=""

# check <name> <want fleet> [want stderr substring...]
check() {
  local name="$1" want="$2" s ok=1
  shift 2
  if [ "${RC}" -ne 0 ]; then
    fail "${name}: exit ${RC}: $(cat "${ERR}")"
    return
  fi
  if [ "$(cat "${OUT}")" != "$(printf 'fleet=%s\nheavy=%s' "${want}" "${want}")" ]; then
    fail "${name}: stdout is '$(tr '\n' ' ' < "${OUT}")', want fleet=${want} then heavy=${want}"
    return
  fi
  for s in "$@"; do
    if ! grep -qF -- "${s}" "${ERR}"; then
      fail "${name}: stderr lacks '${s}': $(cat "${ERR}")"
      ok=0
    fi
  done
  [ "${ok}" = 0 ] || pass "${name}"
}

gh_calls() { grep -c 'check-suites' "${WORK}/gh.log" || true; }

make_repo src/a.cc

CUR=scope-unset-push-runs
run_scope CI_SCOPE=
check "${CUR}" true "shadow: would_skip=true proof=true pipeline_change=false" "fleet scope: event=push scope=full fleet=true heavy=true"
if grep -qF 'repos/We-Amp/example/commits/'"${SHA_VAL}"'/check-suites?app_id=15368&per_page=100' "${WORK}/gh.log"; then
  pass "${CUR}: queried the check suites of the pushed commit"
else
  fail "${CUR}: gh log: $(cat "${WORK}/gh.log")"
fi

CUR=scope-unknown-runs
run_scope CI_SCOPE=light
check "${CUR}" true "reason=scope full" "shadow: would_skip=true"

CUR=scope-uppercase-push
run_scope CI_SCOPE=PUSH
check "${CUR}" false "scope=push fleet=false heavy=false"

CUR=push-proof-skips
run_scope CI_SCOPE=push
check "${CUR}" false "shadow: would_skip=true proof=true pipeline_change=false"

for sc in pr-build pr; do
  CUR="push-proof-skips-scope-${sc}"
  run_scope CI_SCOPE="${sc}"
  check "${CUR}" false "scope=${sc} fleet=false"
done

CUR=push-proof-false-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_MASTER}"
check "${CUR}" true "shadow: would_skip=false proof=false pipeline_change=false"

CUR=push-queue-suite-failed-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_FAILED}"
check "${CUR}" true "proof=false"

CUR=push-queue-suite-in-progress-runs
run_scope CI_SCOPE=push FAKE_GH_BODY="${PROOF_RUNNING}"
check "${CUR}" true "proof=false"

# The pushed commit's pull request must be a same-repository, non-Dependabot one.
CUR=push-queue-fork-pr-runs
run_scope CI_SCOPE=push FAKE_PR_OUT='true\tsomeone\tsomeone/example\n'
check "${CUR}" true "proof=false"

CUR=push-queue-foreign-head-runs
run_scope CI_SCOPE=push FAKE_PR_OUT='false\tsomeone\tsomeone/example\n'
check "${CUR}" true "proof=false"

CUR=push-queue-dependabot-runs
run_scope CI_SCOPE=push FAKE_PR_OUT='false\tdependabot[bot]\tWe-Amp/example\n'
check "${CUR}" true "proof=false"

CUR=push-queue-null-head-repo-runs
run_scope CI_SCOPE=push FAKE_PR_OUT='null\toschaaf\t\n'
check "${CUR}" true "proof=false"

CUR=push-queue-pr-lookup-fails-runs
run_scope CI_SCOPE=push FAKE_PR_FAIL=1
check "${CUR}" true "proof=error"

CUR=push-queue-pr-empty-output-runs
run_scope CI_SCOPE=push FAKE_PR_OUT=
check "${CUR}" true "proof=error"

CUR=push-queue-same-repo-case-skips
run_scope CI_SCOPE=push FAKE_PR_OUT='false\toschaaf\twe-amp/EXAMPLE\n'
check "${CUR}" false "proof=true"

CUR=push-subject-without-pr-number-runs
git -C "${REPO}" commit -q --amend -m "no number here"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
run_scope CI_SCOPE=push
check "${CUR}" true "proof=false"

CUR=push-subject-last-number-is-looked-up
git -C "${REPO}" commit -q --amend -m "title (#201) more (#12)"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
run_scope CI_SCOPE=push
check "${CUR}" false "proof=true"
if grep -qF 'pulls/12' "${WORK}/gh.log"; then pass "${CUR}: looked up #12"; else fail "${CUR}: gh log: $(cat "${WORK}/gh.log")"; fi

# Every commit of the push counts, not the tip alone (a merge group lands up
# to 3 entries at once).
add_commit() { # <subject>
  echo "$RANDOM$1" > "${REPO}/f-${RANDOM}-${RANDOM}.cc"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "$1"
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
}
FORK_OUT='true\tsomeone\tsomeone/example\n'

make_repo
add_commit "fork change (#551)"
add_commit "same repo change (#12)"
CUR=push-fork-first-same-repo-tip-runs
run_scope CI_SCOPE=push FAKE_PR_OUT_551="${FORK_OUT}"
check "${CUR}" true "proof=false"

make_repo
add_commit "one (#10)"
add_commit "two (#11)"
add_commit "three (#12)"
CUR=push-three-trusted-skips
run_scope CI_SCOPE=push
check "${CUR}" false "proof=true"
n="$(grep -c 'pulls/' "${WORK}/gh.log")"
if [ "${n}" = 4 ]; then pass "${CUR}: looked up all 4 pull requests (3 entries and the root)"; else fail "${CUR}: ${n} pull lookups, want 4"; fi

CUR=push-three-last-is-fork-runs
run_scope CI_SCOPE=push FAKE_PR_OUT_12="${FORK_OUT}"
check "${CUR}" true "proof=false"

CUR=push-middle-lookup-fails-runs
run_scope CI_SCOPE=push FAKE_PR_FAIL=1
check "${CUR}" true "proof=error"

make_repo
add_commit "one (#10)"
add_commit "no number"
CUR=push-one-commit-without-number-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=false"

make_repo
add_commit "one (#10)"
CUR=push-before-sha-missing-runs
run_scope CI_SCOPE=push BEFORE_SHA=
check "${CUR}" true "proof=false"
CUR=push-before-sha-zero-runs
run_scope CI_SCOPE=push BEFORE_SHA=0000000000000000000000000000000000000000
check "${CUR}" true "proof=false"
CUR=push-before-sha-unknown-runs
run_scope CI_SCOPE=push BEFORE_SHA=1234567890123456789012345678901234567890
check "${CUR}" true "proof=false"
CUR=push-before-sha-equals-tip-runs
run_scope CI_SCOPE=push BEFORE_SHA="${SHA_VAL}"
check "${CUR}" true "proof=false"

# A push range of exactly 10 trusted commits skips; 11 never does.
make_repo
i=0
while [ "${i}" -lt 10 ]; do i=$((i + 1)); add_commit "entry ${i} (#$((100 + i)))"; done
CUR=push-ten-trusted-commits-skips
run_scope CI_SCOPE=push
check "${CUR}" false "proof=true"
add_commit "entry 11 (#111)"
BEFORE_VAL="$(git -C "${REPO}" rev-parse HEAD~11)"
CUR=push-over-ten-commits-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=false"

# A merge commit in the range is never queue-shaped.
make_repo
git -C "${REPO}" checkout -q -b side
add_commit "side (#10)"
git -C "${REPO}" checkout -q -
add_commit "main (#11)"
git -C "${REPO}" merge -q --no-ff side -m "merge (#12)"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
CUR=push-merge-commit-in-range-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=false"

# A pipeline change in an earlier commit of the push is seen.
make_repo
mkdir -p "${REPO}/tools/ci"
echo x > "${REPO}/tools/ci/a.sh"
git -C "${REPO}" add -A
git -C "${REPO}" commit -q -m "pipeline (#10)"
add_commit "tip (#11)"
CUR=push-pipeline-change-in-earlier-commit-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true"

# The proof covers the whole group, and a stacked group's own base is the
# previous entry's commit, so the group is the union of the push range and the
# last 10 first-parent commits: a fork two commits below the tip, with the push
# range holding only the tip, is seen.
make_repo
add_commit "fork change (#551)"
add_commit "middle (#11)"
add_commit "tip (#12)"
BEFORE_VAL="$(git -C "${REPO}" rev-parse HEAD~1)"
CUR=push-fork-below-tip-outside-push-range-runs
run_scope CI_SCOPE=push FAKE_PR_OUT_551="${FORK_OUT}"
check "${CUR}" true "proof=false"
CUR=push-three-trusted-below-tip-skips
run_scope CI_SCOPE=push
check "${CUR}" false "proof=true"
n="$(grep -c 'pulls/' "${WORK}/gh.log")"
if [ "${n}" = 4 ]; then pass "${CUR}: looked up 4 pull requests (union, no duplicates)"; else fail "${CUR}: ${n} pull lookups, want 4"; fi
CUR=push-before-sha-not-ancestor-runs
run_scope CI_SCOPE=push BEFORE_SHA=1234567890123456789012345678901234567890
check "${CUR}" true "proof=false"

make_repo src/a.cc
CUR=push-api-error-runs
run_scope CI_SCOPE=push FAKE_GH_FAIL=1
check "${CUR}" true "proof=error"
n="$(gh_calls)"
if [ "${n}" = 3 ]; then pass "${CUR}: exactly 3 check-suites calls"; else fail "${CUR}: ${n} check-suites calls, want 3"; fi
if [ "$(tr '\n' ' ' < "${WORK}/sleep.log")" = "2 4 " ]; then pass "${CUR}: backoff 2 4"; else fail "${CUR}: backoff: $(tr '\n' ' ' < "${WORK}/sleep.log")"; fi

CUR=push-empty-body-runs
run_scope CI_SCOPE=push FAKE_GH_BODY=
check "${CUR}" true "proof=error"

CUR=push-unparseable-body-runs
run_scope CI_SCOPE=push FAKE_GH_BODY='<html>'
check "${CUR}" true "proof=error"

for p in .github/workflows/pipeline-pr.yml .github/workflows/ci.yml .github/actions/x/action.yml tools/ci/a.sh; do
  make_repo "${p}"
  CUR="push-pipeline-change-runs (${p})"
  run_scope CI_SCOPE=push
  check "${CUR}" true "pipeline_change=true" "would_skip=false"
  n="$(gh_calls)"
  if [ "${n}" = 0 ]; then pass "${CUR}: no gh call"; else fail "${CUR}: ${n} gh calls, want 0"; fi
done

# A large diff whose first path is a pipeline path: the match must survive
# grep -q exiting early (no SIGPIPE turning the match into a miss).
make_repo .github/actions/x/action.yml
mkdir -p "${REPO}/src/many"
i=0
while [ "${i}" -lt 3000 ]; do
  : > "${REPO}/src/many/file-with-a-long-name-to-fill-the-pipe-buffer-${i}.cc"
  i=$((i + 1))
done
git -C "${REPO}" add -A
git -C "${REPO}" commit -q --amend -m "large change"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
CUR=push-pipeline-change-large-diff-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true"

# A path that only looks like a pipeline path is not one.
make_repo docs/tools/ci/a.md
CUR=push-lookalike-path-skips
run_scope CI_SCOPE=push
check "${CUR}" false "pipeline_change=false"

make_repo
CUR=push-root-commit-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true"

make_repo src/a.cc
CUR=pull-request-at-push-scope-runs
run_scope CI_SCOPE=push GITHUB_EVENT_NAME=pull_request
check "${CUR}" true "event=pull_request"
if grep -q 'shadow:' "${ERR}"; then fail "${CUR}: printed a shadow line"; else pass "${CUR}: no shadow line"; fi
n="$(gh_calls)"
if [ "${n}" = 0 ]; then pass "${CUR}: no gh call"; else fail "${CUR}: ${n} gh calls, want 0"; fi

for ev in merge_group workflow_dispatch schedule; do
  CUR="${ev}-at-push-scope-runs"
  run_scope CI_SCOPE=push GITHUB_EVENT_NAME="${ev}"
  check "${CUR}" true "event=${ev}"
done

# --- The docs lane (DOCS_LANE) -------------------------------------------------
# lane_repo <op>...: a root commit with src/a.cc and docs/a.md, then the ops
# (`add:<path>`, `mv:<from>:<to>`, `commit:<subject>` to cut a commit), then a
# final commit "change (#12)". refs/remotes/origin/main holds the classifier
# the scope script reads (LANE_CLASSIFIER, default the real docs_only.sh).
lane_repo() {
  local op a b blob tree main idx="${WORK}/lane-index"
  rm -rf "${REPO}" "${idx}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  mkdir -p "${REPO}/src" "${REPO}/docs"
  echo x > "${REPO}/src/a.cc"
  echo x > "${REPO}/docs/a.md"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "root (#1)"
  LANE_BASE="$(git -C "${REPO}" rev-parse HEAD)"
  for op in "$@"; do
    case "${op}" in
      add:*)
        a="${op#add:}"
        mkdir -p "${REPO}/$(dirname "${a}")"
        echo "y${RANDOM}" >> "${REPO}/${a}"
        git -C "${REPO}" add -A ;;
      mv:*)
        a="${op#mv:}"; b="${a#*:}"; a="${a%%:*}"
        mkdir -p "${REPO}/$(dirname "${b}")"
        git -C "${REPO}" mv "${a}" "${b}" ;;
      commit:*)
        git -C "${REPO}" commit -q -m "${op#commit:}" ;;
    esac
  done
  git -C "${REPO}" commit -q --allow-empty -m "change (#12)"
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
  # The push defaults of run_scope: the range starts at the root.
  # shellcheck disable=SC2034  # run_scope reads one of the two
  DEF_BEFORE="${LANE_BASE}"
  # shellcheck disable=SC2034
  BEFORE_VAL="${LANE_BASE}"
  blob="$(git -C "${REPO}" hash-object -w "${LANE_CLASSIFIER:-${HERE}/docs_only.sh}")"
  GIT_INDEX_FILE="${idx}" git -C "${REPO}" update-index --add --cacheinfo "100755,${blob},tools/ci/docs_only.sh"
  tree="$(GIT_INDEX_FILE="${idx}" git -C "${REPO}" write-tree)"
  main="$(git -C "${REPO}" commit-tree "${tree}" -m "default branch")"
  git -C "${REPO}" update-ref refs/remotes/origin/main "${main}"
}

# run_lane [env assignments...]: a pull request against LANE_BASE with the
# lane's env; any assignment overrides.
run_lane() {
  run_scope GITHUB_EVENT_NAME=pull_request CI_SCOPE=push DEFAULT_BRANCH=main BASE="${LANE_BASE}" "$@"
}

rules_calls() { grep -c 'rules/branches/' "${WORK}/gh.log" || true; }
lane_err_has() { grep -qF -- "$1" "${ERR}"; }

lane_repo add:docs/b.md add:README.md
CUR=lane-pr-docs-shadow-runs
run_lane
check "${CUR}" true "docs lane: mode=shadow would_skip=true docs_only=true queue=n/a pr_full=false" \
  "docs lane: shadow: would skip vendor and every fleet job" "reason=event pull_request"
CUR=lane-pr-docs-shadow-other-value-runs
run_lane DOCS_LANE=yes
check "${CUR}" true "mode=shadow would_skip=true"
CUR=lane-pr-docs-on-skips
run_lane DOCS_LANE=on
check "${CUR}" false "docs lane: mode=on would_skip=true docs_only=true" "fleet=false heavy=false reason=docs only"
CUR=lane-pr-docs-on-uppercase-skips
run_lane DOCS_LANE=ON
check "${CUR}" false "reason=docs only"
CUR=lane-pr-docs-on-scope-full-skips
run_lane DOCS_LANE=on CI_SCOPE=
check "${CUR}" false "reason=docs only"
if [ "$(rules_calls)" = 0 ]; then pass "${CUR}: no rules lookup for a pull request"; else fail "${CUR}: rules looked up"; fi
CUR=lane-pr-docs-on-pr-full-runs
run_lane DOCS_LANE=on PR_FULL=true
check "${CUR}" true "would_skip=false docs_only=true queue=n/a pr_full=true"
CUR=lane-pr-docs-on-no-base-runs
run_lane DOCS_LANE=on BASE=
check "${CUR}" true "docs_only=false (no base commit)" "would_skip=false"
CUR=lane-pr-docs-on-unknown-base-runs
run_lane DOCS_LANE=on BASE=1234567890123456789012345678901234567890
check "${CUR}" true "would_skip=false"
CUR=lane-pr-docs-on-no-default-branch-runs
run_lane DOCS_LANE=on DEFAULT_BRANCH=
check "${CUR}" true "classifier unreadable" "would_skip=false"
git -C "${REPO}" update-ref -d refs/remotes/origin/main
CUR=lane-pr-docs-on-classifier-missing-runs
run_lane DOCS_LANE=on
check "${CUR}" true "classifier unreadable on origin/main" "would_skip=false"

for c in "add:src/b.cc" "add:docs/b.md add:src/b.cc" "mv:src/a.cc:docs/a.cc" "add:tools/ci/docs_only.sh" "add:.github/workflows/ci.yml"; do
  # shellcheck disable=SC2086  # the ops are separate words
  lane_repo ${c}
  CUR="lane-pr-not-docs-on-runs (${c})"
  run_lane DOCS_LANE=on
  check "${CUR}" true "would_skip=false docs_only=false" "reason=event pull_request"
done

# The classifier comes from origin/main, never from the tree under test: a
# default-branch classifier that says false wins over a docs-only change.
printf '#!/usr/bin/env bash\necho docs_only=false\n' > "${WORK}/strict.sh"
LANE_CLASSIFIER="${WORK}/strict.sh" lane_repo add:docs/b.md
CUR=lane-pr-classifier-from-default-branch
run_lane DOCS_LANE=on
check "${CUR}" true "would_skip=false docs_only=false"
# A classifier that prints anything but exactly docs_only=true is false.
printf '#!/usr/bin/env bash\necho docs_only=true; echo extra\n' > "${WORK}/noisy.sh"
LANE_CLASSIFIER="${WORK}/noisy.sh" lane_repo add:docs/b.md
CUR=lane-pr-classifier-bad-shape-runs
run_lane DOCS_LANE=on
check "${CUR}" true "would_skip=false docs_only=false"

# Merge groups: docs-only only under an ALLGREEN queue.
RULES_ALLGREEN='[{"type":"pull_request"},{"type":"merge_queue","parameters":{"grouping_strategy":"ALLGREEN"}}]'
lane_repo add:docs/b.md
CUR=lane-mg-docs-on-allgreen-skips
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES="${RULES_ALLGREEN}"
check "${CUR}" false "docs lane: mode=on would_skip=true docs_only=true queue=ALLGREEN" "event=merge_group" "reason=docs only"
if grep -qF 'repos/We-Amp/example/rules/branches/main?per_page=100' "${WORK}/gh.log"; then pass "${CUR}: read the default branch's rules"; else fail "${CUR}: no rules lookup of main"; fi
CUR=lane-mg-docs-shadow-allgreen-runs
run_lane GITHUB_EVENT_NAME=merge_group FAKE_RULES="${RULES_ALLGREEN}"
check "${CUR}" true "mode=shadow would_skip=true docs_only=true queue=ALLGREEN"
CUR=lane-mg-docs-on-headgreen-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES='[{"type":"merge_queue","parameters":{"grouping_strategy":"HEADGREEN"}}]'
check "${CUR}" true "would_skip=false docs_only=false queue=HEADGREEN"
CUR=lane-mg-docs-on-mixed-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES='[{"type":"merge_queue","parameters":{"grouping_strategy":"ALLGREEN"}},{"type":"merge_queue","parameters":{"grouping_strategy":"HEADGREEN"}}]'
check "${CUR}" true "queue=mixed"
CUR=lane-mg-docs-on-no-queue-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES='[{"type":"pull_request"}]'
check "${CUR}" true "queue=none"
CUR=lane-mg-docs-on-not-array-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES='{"message":"Not Found"}'
check "${CUR}" true "queue=error"
CUR=lane-mg-docs-on-rules-fail-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES_FAIL=1
check "${CUR}" true "queue=error"
if [ "$(rules_calls)" = 3 ]; then pass "${CUR}: three tries"; else fail "${CUR}: $(rules_calls) rules lookups, want 3"; fi
CUR=lane-mg-docs-on-no-default-branch-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on DEFAULT_BRANCH=
check "${CUR}" true "would_skip=false"
lane_repo add:src/b.cc
CUR=lane-mg-code-on-runs
run_lane GITHUB_EVENT_NAME=merge_group DOCS_LANE=on FAKE_RULES="${RULES_ALLGREEN}"
check "${CUR}" true "would_skip=false docs_only=false queue=n/a"
if [ "$(rules_calls)" = 0 ]; then pass "${CUR}: no rules lookup for a code group"; else fail "${CUR}: rules looked up"; fi

# workflow_dispatch and schedule never take the lane.
lane_repo add:docs/b.md
for ev in workflow_dispatch schedule; do
  CUR="lane-${ev}-docs-on-runs"
  run_lane GITHUB_EVENT_NAME="${ev}" DOCS_LANE=on
  check "${CUR}" true "reason=event ${ev}"
  if lane_err_has 'docs lane: mode='; then fail "${CUR}: printed a docs lane decision"; else pass "${CUR}: no docs lane decision"; fi
done

# The push burst guard: a docs-only tip proves only itself once the lane is on.
lane_repo add:docs/b.md
CUR=lane-push-docs-tip-docs-range-skips
run_scope CI_SCOPE=push DEFAULT_BRANCH=main DOCS_LANE=on
check "${CUR}" false "proof=true" "reason=queue proof"
if lane_err_has 'burst guard'; then fail "${CUR}: burst guard fired"; else pass "${CUR}: no burst guard"; fi
lane_repo add:src/b.cc "commit:code (#10)" add:docs/b.md
CUR=lane-push-docs-tip-code-range-on-runs
run_scope CI_SCOPE=push DEFAULT_BRANCH=main DOCS_LANE=on
check "${CUR}" true "docs lane: burst guard: docs-only tip with code in the push range" "proof=false"
CUR=lane-push-docs-tip-code-range-shadow-skips
run_scope CI_SCOPE=push DEFAULT_BRANCH=main
check "${CUR}" false "docs lane: shadow: the burst guard would drop the queue proof" "proof=true"
lane_repo add:docs/b.md "commit:docs (#10)" add:src/b.cc
CUR=lane-push-code-tip-on-skips
run_scope CI_SCOPE=push DEFAULT_BRANCH=main DOCS_LANE=on
check "${CUR}" false "proof=true"
if lane_err_has 'burst guard'; then fail "${CUR}: burst guard fired"; else pass "${CUR}: no burst guard"; fi

if [ -z "${STDOUT_BAD}" ]; then
  pass "stdout-is-two-lines (${STDOUT_OK} of ${CASES} runs)"
else
  fail "stdout-is-two-lines: not 2 lines in:${STDOUT_BAD}"
fi

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All PR tier scope cases passed"
