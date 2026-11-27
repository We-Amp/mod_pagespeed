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
    [ "${FAKE_PULL_FAIL:-0}" != "1" ] || { echo "HTTP 500" >&2; exit 1; }
    pn="${2##*/}"; pv="FAKE_PULL_${pn}"
    if [ -n "${!pv+x}" ]; then printf '%s\n' "${!pv}"; exit 0; fi
    printf '%s\n' "${FAKE_PULL-false$(printf '\t')oschaaf$(printf '\t')We-Amp/example}"
    exit 0 ;;
esac
printf '%s' "${FAKE_GH_BODY-}" | sed "s/@BASE@/${FAKE_BASE-}/g"
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

# make_repo <changed path>: a root commit and one child commit that adds the
# path. make_repo with no argument leaves only the root commit.
make_repo() {
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  git -C "${REPO}" commit -q --allow-empty -m "root (#1)"
  DEF_BEFORE="$(git -C "${REPO}" rev-parse HEAD)"
  if [ $# -gt 0 ]; then
    mkdir -p "${REPO}/$(dirname "$1")"
    echo x > "${REPO}/$1"
    git -C "${REPO}" add -A
    git -C "${REPO}" commit -q -m "change $1 (#7)"
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

PROOF_TRUE="$(body "$(suite gh-readonly-queue/master/pr-12-@BASE@ completed '"success"')")"
PROOF_MASTER="$(body "$(suite master completed '"success"')")"
PROOF_FAILED="$(body "$(suite gh-readonly-queue/master/pr-12-@BASE@ completed '"failure"')")"
PROOF_RUNNING="$(body "$(suite gh-readonly-queue/master/pr-12-@BASE@ in_progress null)")"

OUT="${WORK}/out"
ERR="${WORK}/err"

# run_scope [env assignments...]: runs the script in the repository with the
# push defaults; stdout to OUT, stderr to ERR, the exit code in RC.
run_scope() {
  : > "${WORK}/gh.log"
  : > "${WORK}/sleep.log"
  set +e
  (cd "${REPO}" && env PATH="${BIN}:${PATH}" FAKE_LOG="${WORK}/gh.log" SLEEP_LOG="${WORK}/sleep.log" \
    GITHUB_EVENT_NAME=push GITHUB_REPOSITORY=We-Amp/example SHA="${SHA_VAL}" GH_TOKEN=x \
    FORK_OR_BOT=false PR_FULL=false CI_SCOPE= BEFORE_SHA="${DEF_BEFORE}" FAKE_BASE="${DEF_BEFORE}" FAKE_GH_BODY="${PROOF_TRUE}" \
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
run_scope CI_SCOPE=push BEFORE_SHA=
check "${CUR}" true "pipeline_change=true"

# Rename detection and the multi-commit push range.
rename_repo() { # <from> <to>: a root commit holding <from>, then a commit moving it to <to>
  rm -rf "${REPO}"
  git init -q "${REPO}"
  git -C "${REPO}" config user.email t@example.invalid
  git -C "${REPO}" config user.name t
  mkdir -p "${REPO}/$(dirname "$1")" "${REPO}/$(dirname "$2")"
  echo x > "${REPO}/$1"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "root"
  git -C "${REPO}" mv "$1" "$2"
  git -C "${REPO}" commit -q -m "move"
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
}

rename_repo tools/ci/a.sh docs/a.sh
CUR=push-rename-out-of-tools-ci-runs
run_scope CI_SCOPE=push
check "${CUR}" true "pipeline_change=true" "would_skip=false"

# Three commits, the pipeline change in the first of them.
make_repo
BEFORE_VAL="$(git -C "${REPO}" rev-parse HEAD)"
for f in tools/ci/a.sh src/b.cc docs/c.md; do
  mkdir -p "${REPO}/$(dirname "${f}")"
  echo x > "${REPO}/${f}"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "change ${f} (#7)"
done
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
CUR=push-batch-nontip-pipeline-change-runs
run_scope CI_SCOPE=push BEFORE_SHA="${BEFORE_VAL}"
check "${CUR}" true "pipeline_change=true" "would_skip=false"
CUR=push-batch-before-missing-runs
run_scope CI_SCOPE=push BEFORE_SHA=
check "${CUR}" true "pipeline_change=false" "proof=error"

CUR=push-before-unreachable-runs
run_scope CI_SCOPE=push BEFORE_SHA=1111111111111111111111111111111111111111
check "${CUR}" true "pipeline_change=true"

CUR=push-before-zero-runs
run_scope CI_SCOPE=push BEFORE_SHA=0000000000000000000000000000000000000000
check "${CUR}" true "pipeline_change=true"

# The path filter of the workflow's diff step: the source of a rename is listed.
rename_repo src/x.cc docs/x.cc
CUR=pr-rename-filter-sees-source
listed="$(git -C "${REPO}" diff --no-renames --name-only HEAD^...HEAD)"
if [ "${listed}" = "$(printf 'docs/x.cc\nsrc/x.cc')" ]; then pass "${CUR}"; else fail "${CUR}: listed '${listed}'"; fi

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

if [ -z "${STDOUT_BAD}" ]; then
  pass "stdout-is-two-lines (${STDOUT_OK} of ${CASES} runs)"
else
  fail "stdout-is-two-lines: not 2 lines in:${STDOUT_BAD}"
fi

TAB="$(printf '\t')"
make_repo src/a.cc
CUR=push-proof-fork-pr-runs
run_scope CI_SCOPE=push "FAKE_PULL=true${TAB}someone${TAB}someone/example"
check "${CUR}" true "proof=false"
CUR=push-proof-foreign-head-runs
run_scope CI_SCOPE=push "FAKE_PULL=false${TAB}someone${TAB}other/example"
check "${CUR}" true "proof=false"
CUR=push-proof-dependabot-runs
run_scope CI_SCOPE=push "FAKE_PULL=false${TAB}dependabot[bot]${TAB}We-Amp/example"
check "${CUR}" true "proof=false"
CUR=push-proof-null-head-runs
run_scope CI_SCOPE=push "FAKE_PULL=null${TAB}someone${TAB}"
check "${CUR}" true "proof=error"
CUR=push-proof-pull-lookup-fails-runs
run_scope CI_SCOPE=push FAKE_PULL_FAIL=1
check "${CUR}" true "proof=error"
CUR=push-proof-same-repo-skips
run_scope CI_SCOPE=push
check "${CUR}" false "reason=queue proof"
make_repo src/a.cc
git -C "${REPO}" commit -q --amend -m "no number here"
SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
CUR=push-proof-no-pr-number-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=error"

# Push-range trust: every commit in BEFORE_SHA..SHA must map to a trusted PR.
add_commit() { # <file> <subject>
  mkdir -p "${REPO}/$(dirname "$1")"
  echo x >> "${REPO}/$1"
  git -C "${REPO}" add -A
  git -C "${REPO}" commit -q -m "$2"
  SHA_VAL="$(git -C "${REPO}" rev-parse HEAD)"
}
make_repo
add_commit src/a.cc "fork change (#551)"
add_commit src/b.cc "same-repo change (#12)"
CUR=push-range-fork-first-same-repo-tip-runs
run_scope CI_SCOPE=push "FAKE_PULL_551=true${TAB}someone${TAB}someone/example"
check "${CUR}" true "proof=false"
make_repo
add_commit src/a.cc "a (#1)"
add_commit src/b.cc "b (#2)"
add_commit src/c.cc "c (#3)"
CUR=push-range-three-trusted-skips
run_scope CI_SCOPE=push
check "${CUR}" false "reason=queue proof"
for k in 4 5 6 7 8 9 10; do add_commit "src/n${k}.cc" "n${k} (#${k})"; done
CUR=push-range-ten-trusted-skips
run_scope CI_SCOPE=push
check "${CUR}" false "reason=queue proof"
add_commit src/n11.cc "n11 (#11)"
CUR=push-range-over-limit-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=error"
CUR=push-range-before-missing-runs
run_scope CI_SCOPE=push BEFORE_SHA=
check "${CUR}" true "proof=error"
CUR=push-range-before-zero-runs
run_scope CI_SCOPE=push BEFORE_SHA=0000000000000000000000000000000000000000
check "${CUR}" true "would_skip=false"
CUR=push-range-before-unreadable-runs
run_scope CI_SCOPE=push BEFORE_SHA=1234567890123456789012345678901234567890
check "${CUR}" true "would_skip=false"
make_repo
add_commit src/a.cc "a (#1)"
add_commit src/b.cc "no number"
CUR=push-range-commit-without-number-runs
run_scope CI_SCOPE=push
check "${CUR}" true "proof=error"

# Group window: a stacked group's base is the previous entry's commit, so the
# proof is checked against the last 3 first-parent commits as well as the push.
make_repo
add_commit src/a.cc "fork change (#551)"
add_commit src/b.cc "second (#12)"
add_commit src/c.cc "tip (#13)"
TIP_PARENT="$(git -C "${REPO}" rev-parse HEAD~1)"
CUR=push-window-fork-two-below-tip-runs
run_scope CI_SCOPE=push BEFORE_SHA="${TIP_PARENT}" "FAKE_PULL_551=true${TAB}someone${TAB}someone/example"
check "${CUR}" true "proof=false"
CUR=push-window-three-trusted-skips
run_scope CI_SCOPE=push BEFORE_SHA="${TIP_PARENT}"
check "${CUR}" false "reason=queue proof"
CUR=push-window-ignores-suite-branch-base
run_scope CI_SCOPE=push BEFORE_SHA="${TIP_PARENT}" FAKE_BASE=abc "FAKE_PULL_551=true${TAB}someone${TAB}someone/example"
check "${CUR}" true "proof=false"
FORK_SHA="$(git -C "${REPO}" rev-parse HEAD~2)"
CUR=push-before-not-ancestor-runs
run_scope CI_SCOPE=push BEFORE_SHA="${FORK_SHA}" SHA="${ROOT_SHA:-${DEF_BEFORE}}"
check "${CUR}" true "would_skip=false"

make_repo
add_commit src/a.cc "fork change (#551)"
for k in 21 22 23 24; do add_commit "src/w${k}.cc" "w${k} (#${k})"; done
WIN_PARENT="$(git -C "${REPO}" rev-parse HEAD~1)"
CUR=push-window-fork-five-deep-runs
run_scope CI_SCOPE=push BEFORE_SHA="${WIN_PARENT}" "FAKE_PULL_551=true${TAB}someone${TAB}someone/example"
check "${CUR}" true "proof=false"
CUR=push-window-five-deep-trusted-skips
run_scope CI_SCOPE=push BEFORE_SHA="${WIN_PARENT}"
check "${CUR}" false "reason=queue proof"

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

if [ "${FAILS}" -ne 0 ]; then
  echo "${FAILS} case(s) failed"
  exit 1
fi
echo "All PR tier scope cases passed"
