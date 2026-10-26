#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Test for tools/ci/bisect-job.sh (docs/ci/pipelines.md, "Bisecting a red
# full-tier job"). Hermetic: real git on a temporary repository of 12
# commits (commit 7 introduces the failure; commit 4 has no ci-* line in
# its .bazelrc), and fakes on PATH for docker, bazel, gh, nohup, setsid,
# nproc, ionice and sleep. The fake docker logs every call and refuses
# (exit 97) any container or network name outside the run's own prefix,
# and any verb it does not know (exit 98); a
# step container reads the mounted work tree's commit index and a per-case
# map (<index>=pass|testfail|timeout|buildfail|otherbuild|vendorfail).
# The fake nohup and setsid run in the foreground, so every case is
# synchronous. The repository's own table and workflow are checked too
# (table-matches-workflow, no-host-specifics-in-source).
#
# Needs flock(1) from util-linux. Run: tools/ci/test_bisect_job.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${HERE}/../.." && pwd)"

if ! command -v flock >/dev/null 2>&1; then
  echo "test_bisect_job: needs flock(1) (util-linux)" >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT

FAILS=0
PASSES=0
pass() { PASSES=$((PASSES + 1)); echo "ok   $1"; }
fail() { FAILS=$((FAILS + 1)); echo "FAIL $1: $2"; }

# --- The driver under test, next to its table and target parser ---------
TOOLS="${WORK}/tools/ci"
mkdir -p "${TOOLS}"
cp "${HERE}/bisect-job.sh" "${HERE}/bisect-job-table.sh" "${TOOLS}/"
if [ -f "${HERE}/failing_targets.sh" ]; then
  cp "${HERE}/failing_targets.sh" "${TOOLS}/"
else
  # Stand-in for tools/ci/failing_targets.sh when this tree does not carry
  # it yet: the same output contract for the lines the fakes print.
  cat > "${TOOLS}/failing_targets.sh" <<'EOF'
#!/usr/bin/env bash
mode=""; file=""
while [ "$#" -gt 0 ]; do
  case "$1" in --mode) mode="$2"; shift 2 ;; *) file="$1"; shift ;; esac
done
LC_ALL=C awk -v mode="${mode}" '
  { line = $0
    gsub(/\033\[[0-9;]*[A-Za-z]/, "", line)
    sub(/^[^\t]*\t[^\t]*\t/, "", line)
    sub(/^[0-9-]+T[0-9:.]+Z /, "", line)
    if (match(line, /Target \/\/[^ \t]+ failed to build/)) {
      print substr(line, RSTART + 7, RLENGTH - 23) "\tbuild-failed"; next }
    if (!match(line, /^\/\/[^ \t]+:[^ \t]+[ \t]+/)) next
    rest = substr(line, RLENGTH + 1); label = substr(line, 1, RLENGTH)
    sub(/[ \t]+$/, "", label)
    if (rest ~ /^(FAILED TO BUILD|NO STATUS)/) print label "\tbuild-failed"
    else if (mode == "test" && rest ~ /^FAILED( in|$)/) print label "\ttest-failed"
    else if (mode == "test" && rest ~ /^TIMEOUT in/) print label "\ttest-timeout"
  }' ${file:+"${file}"} | LC_ALL=C sort -u
EOF
fi
chmod +x "${TOOLS}"/*.sh
DRIVER="${TOOLS}/bisect-job.sh"

# --- Fakes --------------------------------------------------------------
BIN="${WORK}/bin"
mkdir -p "${BIN}"

# Status of commit index $1 in the case's map (default pass).
cat > "${BIN}/fixture-status" <<'EOF'
#!/usr/bin/env bash
s=$(sed -n "s/^$1=//p" "${FAKE_STATE}/map" 2>/dev/null | head -1)
echo "${s:-pass}"
EOF

cat > "${BIN}/docker" <<'EOF'
#!/usr/bin/env bash
log="${FAKE_STATE}/docker.log"
echo "docker $*" >> "${log}"
refuse() { echo "REFUSED(97) $*" >> "${log}"; exit 97; }
# A name is ours when it is a run id under BISECT_ROOT or starts with one.
owned() {
  local d id
  for d in "${BISECT_ROOT}"/bisect-*; do
    [ -d "${d}" ] || continue
    id=$(basename "${d}")
    case "$1" in "${id}"|"${id}"-*) return 0 ;; esac
  done
  return 1
}
sub="${1:-}"; shift || true
case "${sub}" in
  run)
    name=""; ws=""; detached=0; targets=""; after=0; prev=""
    for a in "$@"; do
      if [ "${after}" = 1 ]; then targets="${targets} ${a}"; continue; fi
      case "${prev}" in
        --name) name="${a}" ;;
        -v) case "${a}" in *:/workspace) ws="${a%:/workspace}" ;; esac ;;
      esac
      case "${a}" in
        --name=*) name="${a#--name=}" ;;
        -d) detached=1 ;;
        --) after=1 ;;
      esac
      prev="${a}"
    done
    [ -n "${name}" ] && owned "${name}" || refuse "run without an owned --name"
    [ "${detached}" = 1 ] && { echo "cid-${name}"; exit 0; }
    n=$(cat "${ws}/n")
    st=$(fixture-status "${n}")
    first=""; for t in ${targets}; do first="${t}"; break; done
    case "${st}" in
      pass)
        for t in ${targets}; do printf '%s   PASSED in 0.4s\n' "${t}"; done; exit 0 ;;
      testfail)
        printf '%s   FAILED in 1.2s\n' "${first}"; exit 3 ;;
      timeout)
        printf '%s   TIMEOUT in 300.0s\n' "${first}"; exit 3 ;;
      buildfail)
        echo "ERROR: /workspace/rec/BUILD:1:1: Compiling rec/a.cc failed: (Exit 1)"
        printf 'ERROR: Target %s failed to build\n' "${first}"
        printf '%s   FAILED TO BUILD\n' "${first}"; exit 1 ;;
      otherbuild)
        echo "ERROR: Target //other:lib failed to build"
        for t in ${targets}; do printf '%s   NO STATUS\n' "${t}"; done; exit 1 ;;
      *) echo "fixture: unexpected status ${st}"; exit 1 ;;
    esac ;;
  rm)
    for a in "$@"; do
      case "${a}" in -*) continue ;; esac
      owned "${a}" || refuse "rm ${a}"
    done; exit 0 ;;
  network)
    case "${1:-}" in
      create|rm) owned "${2:-}" || refuse "network $*"; exit 0 ;;
      *) refuse "network $*" ;;
    esac ;;
  exec)
    owned "${1:-}" || refuse "exec $*"; echo PONG; exit 0 ;;
  image)
    [ "${1:-}" = inspect ] || refuse "image $*"
    grep -qxF -- "${2:-}" "${FAKE_STATE}/images" 2>/dev/null; exit $? ;;
  rmi|kill|system|prune|stop|container|volume) refuse "${sub} $*" ;;
  *) echo "UNKNOWN(98) ${sub} $*" >> "${log}"; exit 98 ;;
esac
EOF

cat > "${BIN}/bazel" <<'EOF'
#!/usr/bin/env bash
echo "HOME=${HOME} $*" >> "${FAKE_STATE}/bazel.log"
ob=""
for a in "$@"; do
  case "${a}" in --output_base=*) ob="${a#--output_base=}" ;; --*) ;; *) break ;; esac
done
case " $* " in
  *" vendor "*)
    st=$(fixture-status "$(cat n 2>/dev/null)")
    [ "${st}" = vendorfail ] && { echo "ERROR: fetch failed" >&2; exit 1; }
    [ -n "${ob}" ] && mkdir -p "${ob}/external/protobuf++protoc+linux-x86_64"
    dir=vendor
    for a in "$@"; do case "${a}" in --vendor_dir=*) dir="${a#--vendor_dir=}" ;; esac; done
    mkdir -p "${dir}" && echo "vendored $(cat n)" > "${dir}/marker"; exit 0 ;;
esac
exit 0
EOF

# Fake gh, shaped like the real one: `run view` has no
# --allow-escape-sequences flag (unknown flag, exit 1); `api .../logs`
# prints a short refusal unless it gets that flag. FAKE_GH_RUN_VIEW=fail
# fails `run view --log-failed`; FAKE_GH_API=refuse refuses even with it.
cat > "${BIN}/gh" <<'EOF'
#!/usr/bin/env bash
echo "gh $*" >> "${FAKE_STATE}/gh.log"
case " $* " in
  " run view "*" --allow-escape-sequences "*)
    echo "unknown flag: --allow-escape-sequences" >&2; exit 1 ;;
  " run view "*" --log-failed "*)
    [ "${FAKE_GH_RUN_VIEW:-}" = fail ] && { echo "failed to get run log" >&2; exit 1; }
    cat "${FAKE_STATE}/gh-log"; exit 0 ;;
  " run view "*" --json jobs "*) echo 4242; exit 0 ;;
  " api "*"/actions/jobs/4242/logs "*)
    case " $* " in
      *" --allow-escape-sequences "*) [ "${FAKE_GH_API:-}" = refuse ] || { cat "${FAKE_STATE}/gh-log"; exit 0; } ;;
    esac
    echo "the output contains terminal escape sequences; use --allow-escape-sequences to print them"
    exit 0 ;;
esac
echo "fake gh: unexpected call: $*" >&2
exit 1
EOF

cat > "${BIN}/nohup" <<'EOF'
#!/usr/bin/env bash
echo "nohup $*" >> "${FAKE_STATE}/detach.log"
exec "$@"
EOF

cat > "${BIN}/setsid" <<'EOF'
#!/usr/bin/env bash
echo "setsid $*" >> "${FAKE_STATE}/detach.log"
case "${1:-}" in -f|--fork) shift ;; esac
exec "$@"
EOF

cat > "${BIN}/nproc" <<'EOF'
#!/usr/bin/env bash
echo "${FAKE_NPROC:-8}"
EOF

cat > "${BIN}/ionice" <<'EOF'
#!/usr/bin/env bash
echo "ionice $*" >> "${FAKE_STATE}/ionice.log"
[ "${1:-}" = -c3 ] || { echo "fake ionice: unexpected $*" >&2; exit 1; }
shift; exec "$@"
EOF

cat > "${BIN}/sleep" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "${BIN}"/*
export PATH="${BIN}:${PATH}"

# --- Fixture repository: commits 1..12 ----------------------------------
ORIGIN="${WORK}/origin"
git init -q "${ORIGIN}"
git -C "${ORIGIN}" config user.email fixture@example.com
git -C "${ORIGIN}" config user.name fixture
mkdir -p "${ORIGIN}/tools/ci" "${ORIGIN}/.github/workflows" "${ORIGIN}/bazel"
cat > "${ORIGIN}/tools/ci/ensure-dev-image.sh" <<'EOF'
#!/usr/bin/env bash
name=fixture-dev:x64-0123abcd
for a in "$@"; do
  [ "${a}" = --tag-latest ] && { echo "tag-latest $*" >> "${FAKE_STATE}/ensure.log"; exit 9; }
done
[ "${1:-}" = --name-only ] && { echo "${name}"; exit 0; }
echo "build $*" >> "${FAKE_STATE}/ensure.log"
echo "${name}" >> "${FAKE_STATE}/images"
echo "${name}"
EOF
# Like the real vendor script: its own output base (a mktemp dir), which it
# reads after the bazel call; it leaves the dir behind here, so the test can
# see that the driver never deletes it.
cat > "${ORIGIN}/tools/vendor-deps.sh" <<'EOF'
#!/usr/bin/env bash
ob="$(mktemp -d "${FAKE_TMP}/bazel-vendor-XXXXXX")"
bazel --output_base="${ob}" vendor --vendor_dir="${1:-vendor}" //... || exit 1
ls -d "${ob}"/external/protobuf++protoc+* >/dev/null 2>&1 \
  || { echo "MISSING protoc repos under ${ob}" >&2; exit 1; }
EOF
cat > "${ORIGIN}/.github/workflows/pipeline-full.yml" <<'EOF'
jobs:
  linux-asan:
    steps:
      - run: |
          for img in redis:7-alpine memcached:1.6-alpine; do
            docker pull "$img"
          done
EOF
echo "8.4.2" > "${ORIGIN}/.bazelversion"
echo "# fixture" > "${ORIGIN}/bazel/BUILD"
SHA=()
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
  echo "${i}" > "${ORIGIN}/n"
  echo "module(name = \"fixture\")  # commit ${i}" > "${ORIGIN}/MODULE.bazel"
  if [ "${i}" = 4 ]; then
    echo "build:other --keep_going" > "${ORIGIN}/.bazelrc"
  else
    printf 'build:ci-asan --keep_going\nbuild:ci-tsan --keep_going\nbuild:ci-build --keep_going\n' > "${ORIGIN}/.bazelrc"
  fi
  git -C "${ORIGIN}" add -A
  git -C "${ORIGIN}" commit -qm "commit ${i}"
  SHA[${i}]=$(git -C "${ORIGIN}" rev-parse HEAD)
done

# --- Case helpers ------------------------------------------------------
# new_case NAME: a fresh state dir and BISECT_ROOT; default log = one
# failing test target, map = commits 7..12 testfail.
new_case() {
  CASE="$1"
  FAKE_STATE="${WORK}/cases/${CASE}"
  mkdir -p "${FAKE_STATE}/root"
  BISECT_ROOT="$(cd -P "${FAKE_STATE}/root" && pwd -P)"
  FAKE_TMP="${FAKE_STATE}/tmp"
  mkdir -p "${FAKE_TMP}"
  export FAKE_STATE BISECT_ROOT FAKE_TMP
  : > "${FAKE_STATE}/docker.log"; : > "${FAKE_STATE}/bazel.log"; : > "${FAKE_STATE}/images"
  printf 'linux-asan\tRun ASan\t2026-10-26T02:10:11.1234567Z \033[31m//rec:a_test\033[0m   FAILED in 2.1s\n' \
    > "${FAKE_STATE}/gh-log"
  set_map 7 12 testfail
  unset FAKE_NPROC BISECT_STOP_AFTER BISECT_MAX_TARGETS
}
# set_map FROM TO STATUS (replaces the map); add_map INDEX STATUS
set_map() { : > "${FAKE_STATE}/map"; local i; for i in $(seq "$1" "$2"); do echo "${i}=$3" >> "${FAKE_STATE}/map"; done; }
add_map() { local tmp="${FAKE_STATE}/map.tmp"; grep -v "^$1=" "${FAKE_STATE}/map" > "${tmp}" || true; echo "$1=$2" >> "${tmp}"; mv "${tmp}" "${FAKE_STATE}/map"; }

# start JOB [ARGS]: runs start with --bad 12 --good 1 --run 99 unless the
# args name their own; sets RC and ID; RESULT points at result.json.
start() {
  local job="$1"; shift
  local args=(--job "${job}" --repo "${ORIGIN}")
  case " $* " in *" --bad "*) ;; *) args+=(--bad "${SHA[12]}") ;; esac
  case " $* " in *" --good "*) ;; *) args+=(--good "${SHA[1]}") ;; esac
  case " $* " in *" --run "*|*" --targets-file "*) ;; *) args+=(--run 99) ;; esac
  RC=0
  ID=$("${DRIVER}" start "${args[@]}" "$@" 2>"${FAKE_STATE}/start.err") || RC=$?
  RESULT="${BISECT_ROOT}/${ID}/result.json"
}
# jget KEY: the value of a result.json key (quotes and trailing comma off).
jget() { sed -n "s/^  \"$1\": \(.*\)$/\1/p" "${RESULT}" | sed 's/,$//; s/^"//; s/"$//'; }
steplog() { cat "${BISECT_ROOT}/${ID}/steps.log" 2>/dev/null; }
expect_eq() { if [ "$2" = "$3" ]; then return 0; fi; fail "${CASE}" "$1: want '$3', got '$2'"; return 1; }

# --- Behaviour cases -----------------------------------------------------
new_case culprit-found-test-bisect
start linux-asan
if expect_eq rc "${RC}" 0 && expect_eq status "$(jget status)" culprit \
  && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}" \
  && expect_eq targets "$(jget targets)" '["//rec:a_test"]' \
  && expect_eq timeout_only "$(jget timeout_only)" false \
  && expect_eq rebuilt_images "$(jget rebuilt_images)" '["fixture-dev:x64-0123abcd"]' \
  && expect_eq steps "$(jget steps)" 6 \
  && expect_eq exit "$(cat "${BISECT_ROOT}/${ID}/exit")" 0; then
  case "${ID}" in
    bisect-*-linux-asan-[0-9]*T[0-9]*Z-[0-9]*) pass "${CASE}" ;;
    *) fail "${CASE}" "id ${ID} does not have the bisect-<repo>-<job>-<utc>-<pid> shape" ;;
  esac
fi
CULPRIT_RESULT="${RESULT}"
CULPRIT_STATE="${FAKE_STATE}"

# The log was fetched with escape sequences kept, then parsed.
CASE=log-fetched-with-run-view
if grep -q -- '^gh run view 99 -R [^ ]* --job 4242 --log-failed$' "${CULPRIT_STATE}/gh.log" \
  && ! grep -q '^gh api' "${CULPRIT_STATE}/gh.log"; then
  pass "${CASE}"; else fail "${CASE}" "$(cat "${CULPRIT_STATE}/gh.log")"; fi

CASE=every-docker-call-carries-the-prefix
FAKE_STATE="${CULPRIT_STATE}"
if grep -qE 'REFUSED|UNKNOWN' "${FAKE_STATE}/docker.log"; then
  fail "${CASE}" "$(grep -E 'REFUSED|UNKNOWN' "${FAKE_STATE}/docker.log" | head -3)"
elif ! grep -q -- "--name bisect-.*-step-" "${FAKE_STATE}/docker.log"; then
  fail "${CASE}" "no step container was named"
else
  pass "${CASE}"
fi

CASE=no-destructive-docker-verbs
if grep -E '^docker (rmi|kill|system|prune|stop|volume|container)( |$)|prune' "${FAKE_STATE}/docker.log" \
  || grep -nE '(^|[^-])\b(pkill|killall)\b|docker (rmi|kill|system)|\bprune\b' "${DRIVER}"; then
  fail "${CASE}" "a destructive docker verb or signal appears"
elif grep -q -- '--tag-latest' "${FAKE_STATE}/ensure.log" 2>/dev/null; then
  fail "${CASE}" "ensure-dev-image.sh was run with --tag-latest"
else
  pass "${CASE}"
fi

CASE=host-bazel-home-is-under-bisect-root
FAKE_STATE="${CULPRIT_STATE}"
BR="$(cd -P "${FAKE_STATE}/root" && pwd -P)"
CID=$(basename "$(dirname "${CULPRIT_RESULT}")")
if [ ! -s "${FAKE_STATE}/bazel.log" ]; then
  fail "${CASE}" "host bazel was never run"
elif grep -v "^HOME=${BR}/" "${FAKE_STATE}/bazel.log" >/dev/null; then
  fail "${CASE}" "$(grep -v "^HOME=${BR}/" "${FAKE_STATE}/bazel.log" | head -2)"
elif ! grep -qxF "startup --output_user_root=${BR}/${CID}/bazel-root" "${BR}/${CID}/home/.bazelrc"; then
  fail "${CASE}" "the run's HOME/.bazelrc does not set the output root"
elif ! grep -q ' shutdown' "${FAKE_STATE}/bazel.log"; then
  fail "${CASE}" "no bazel shutdown on exit"
elif [ -e "${BR}/${CID}/bazel-root" ]; then
  fail "${CASE}" "the run's output root was left behind"
else
  pass "${CASE}"
fi

CASE=vendor-servers-shut-down-nothing-deleted-outside-root
# Every host vendoring call names an output base; each one gets a shutdown
# after its last use; the install base and HOME stay under BISECT_ROOT; an
# output base outside BISECT_ROOT (the vendor script's own) is left alone;
# the run's own vendor-ob is removed.
BLOG="${FAKE_STATE}/bazel.log"
problems=""
if grep -v -- ' --output_base=' "${BLOG}" | grep -q .; then
  problems="${problems} [a call without an output base]"
fi
# Each vendoring call is followed directly by the shutdown of its server.
calls=$(awk '!/ shutdown$/ {ob = $0; sub(/.* --output_base=/, "", ob); sub(/ .*/, "", ob)
  if ((getline nxt) <= 0 || nxt !~ (" --output_base=" ob " shutdown$")) print ob}' "${BLOG}")
[ -z "${calls}" ] || problems="${problems} [no shutdown right after a call on: $(echo ${calls})]"
for ob in $(grep -v ' shutdown$' "${BLOG}" | sed -n 's/.* --output_base=\([^ ]*\) .*/\1/p' | sort -u); do
  case "${ob}" in
    "${BR}"/*) ;;
    *) [ -d "${ob}" ] || problems="${problems} [deleted outside the root: ${ob}]" ;;
  esac
done
grep -qxF "startup --output_user_root=${BR}/${CID}/bazel-root" "${BR}/${CID}/home/.bazelrc" \
  || problems="${problems} [install base not under the root]"
[ -e "${BR}/${CID}/vendor-ob" ] && problems="${problems} [vendor-ob left behind]"
if [ -n "${problems}" ]; then fail "${CASE}" "${problems}"; else pass "${CASE}"; fi

CASE=host-vendoring-is-low-priority
if grep -q 'bash -c .*bisect_vendor_cmd' "${FAKE_STATE}/ionice.log" 2>/dev/null; then
  pass "${CASE}"; else fail "${CASE}" "vendoring did not run under ionice/nice"; fi

CASE=services-and-step-command
FAKE_STATE="${CULPRIT_STATE}"
. "${TOOLS}/bisect-job-table.sh"
step_line=$(grep -- "--name ${CID}-step-1 " "${FAKE_STATE}/docker.log" || true)
want="--config=${BISECT_LINUX_ASAN_CONFIG} --nocache_test_results --disk_cache=/disk-cache"
case "${step_line}" in
  *"docker run --rm --init --name ${CID}-step-1 --cpus "*"--cpu-shares 128 ${BISECT_LINUX_ASAN_DOCKER_FLAGS} "*"-v ${BR}/${CID}/src:/workspace -v ${BR}/${CID}/disk-cache:/disk-cache -w /workspace fixture-dev:x64-0123abcd bazel --output_user_root=${BISECT_LINUX_ASAN_OUTPUT_ROOT} test "*"${want}"*" --remote_cache= "*"-- //rec:a_test") ok=1 ;;
  *) ok=0 ;;
esac
if [ "${ok}" != 1 ]; then
  fail "${CASE}" "step command: ${step_line}"
elif [ -n "${BISECT_LINUX_ASAN_SERVICES}" ]; then
  if grep -q -- "docker run -d --init --name ${CID}-redis --network ${CID}-net --network-alias redis --cpus 1 --memory 1g --memory-swap 1g --cpu-shares 128 redis:7-alpine" "${FAKE_STATE}/docker.log" \
    && grep -q -- " --test_env=REDIS_HOST=redis " <<<"${step_line}" \
    && grep -q -- " --test_env=MEMCACHED_HOST=memcached " <<<"${step_line}" \
    && grep -q -- "docker network rm ${CID}-net" "${FAKE_STATE}/docker.log" \
    && grep -q -- "docker rm -f ${CID}-memcached" "${FAKE_STATE}/docker.log"; then
    pass "${CASE}"
  else
    fail "${CASE}" "service containers not started/removed by name"
  fi
else
  if grep -q -- '-d --init' "${FAKE_STATE}/docker.log"; then fail "${CASE}" "unexpected service container"; else pass "${CASE}"; fi
fi

# A run id with a 7-digit pid makes "<id>-memcached" longer than a DNS
# label (63); the test hosts must not depend on the id at all.
CASE=service-hosts-are-short-aliases
FAKE_STATE="${CULPRIT_STATE}"
long_id="bisect-${BISECT_REPO_NAME}-linux-asan-20261026T020000Z-1234567"
hosts=$(grep -oE -- '--test_env=[A-Z]+_HOST=[^ ]+' "${FAKE_STATE}/docker.log" | sed 's/.*_HOST=//' | sort -u || true)
bad_hosts=""
for h in ${hosts}; do
  case " ${BISECT_LINUX_ASAN_SERVICES} " in *" ${h} "*) ;; *) bad_hosts="${bad_hosts} ${h}" ;; esac
done
if [ "${#long_id}" -le 53 ]; then
  fail "${CASE}" "the long-id premise does not hold (${#long_id} chars)"
elif [ -n "${BISECT_LINUX_ASAN_SERVICES}" ] && [ -z "${hosts}" ]; then
  fail "${CASE}" "no --test_env host values"
elif [ -n "${bad_hosts}" ]; then
  fail "${CASE}" "host values are not the short aliases:${bad_hosts}"
elif grep '^docker run -d ' "${FAKE_STATE}/docker.log" | grep -qv -- '--network-alias '; then
  fail "${CASE}" "a service container without --network-alias"
else
  pass "${CASE}"
fi

new_case culprit-found-build-bisect
printf 'linux-build\tBuild all targets\t2026-10-26T02:10:11.1Z ERROR: Target //rec:lib failed to build\n' > "${FAKE_STATE}/gh-log"
set_map 7 12 buildfail
start linux-build
if expect_eq status "$(jget status)" culprit && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}" \
  && expect_eq targets "$(jget targets)" '["//rec:lib"]'; then
  if grep -q -- ' build --config=ci-build ' "${FAKE_STATE}/docker.log" \
    || grep -q -- ' build .*--config=ci-build ' "${FAKE_STATE}/docker.log"; then
    if grep -q -- 'nocache_test_results' "${FAKE_STATE}/docker.log"; then
      fail "${CASE}" "a build step carries test flags"
    else pass "${CASE}"; fi
  else fail "${CASE}" "no bazel build --config=ci-build step"; fi
fi

new_case compile-failure-counts-as-bad-in-test-bisect
set_map 7 12 buildfail
start linux-asan
expect_eq status "$(jget status)" culprit && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}" && pass "${CASE}"

new_case other-target-build-error-is-skip
add_map 8 otherbuild
start linux-asan
if expect_eq status "$(jget status)" culprit && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}"; then
  if steplog | grep -q "^[0-9]* ${SHA[8]} 125 "; then pass "${CASE}"
  else fail "${CASE}" "commit 8 was not a 125 step: $(steplog)"; fi
fi

new_case bad-endpoint-passes-is-env-dependent
set_map 1 12 pass
start linux-asan
expect_eq status "$(jget status)" env-dependent && expect_eq steps "$(jget steps)" 1 \
  && expect_eq first_bad "$(jget first_bad)" null && pass "${CASE}"

new_case good-endpoint-fails-is-external-drift
set_map 1 12 testfail
start linux-asan
expect_eq status "$(jget status)" external-drift-or-stale-good && expect_eq steps "$(jget steps)" 2 && pass "${CASE}"

new_case no-targets-extracted
: > "${FAKE_STATE}/gh-log"
start linux-asan
if expect_eq rc "${RC}" 0 && expect_eq status "$(jget status)" no-targets; then
  if [ -e "${BISECT_ROOT}/${ID}/src" ] || grep -q '^docker run' "${FAKE_STATE}/docker.log"; then
    fail "${CASE}" "a bisect was started"; else pass "${CASE}"; fi
fi

new_case more-than-ten-targets
: > "${FAKE_STATE}/gh-log"
for i in $(seq 1 11); do
  printf 'linux-asan\tRun ASan\t2026-10-26T02:10:11.1Z //rec:t%s_test   FAILED in 1s\n' "${i}" >> "${FAKE_STATE}/gh-log"
done
start linux-asan
if expect_eq status "$(jget status)" too-many-targets; then
  if [ -e "${BISECT_ROOT}/${ID}/src" ] || grep -q '^docker run' "${FAKE_STATE}/docker.log"; then
    fail "${CASE}" "a bisect was started"; else pass "${CASE}"; fi
fi

new_case log-falls-back-to-jobs-api
export FAKE_GH_RUN_VIEW=fail
start linux-asan
unset FAKE_GH_RUN_VIEW
if expect_eq status "$(jget status)" culprit && expect_eq targets "$(jget targets)" '["//rec:a_test"]'; then
  if grep -q -- '^gh api --allow-escape-sequences repos/[^ ]*/actions/jobs/4242/logs$' "${FAKE_STATE}/gh.log"; then
    pass "${CASE}"; else fail "${CASE}" "no jobs API fallback: $(cat "${FAKE_STATE}/gh.log")"; fi
fi

new_case api-refusal-is-no-log
export FAKE_GH_RUN_VIEW=fail FAKE_GH_API=refuse
start linux-asan
unset FAKE_GH_RUN_VIEW FAKE_GH_API
if [ -e "${RESULT}" ]; then
  fail "${CASE}" "a refusal body was parsed as a log: $(jget status)"
elif ! expect_eq exit "$(cat "${BISECT_ROOT}/${ID}/exit" 2>/dev/null)" 1; then
  :
elif ! grep -q 'cannot fetch the job log' "${BISECT_ROOT}/${ID}/log"; then
  fail "${CASE}" "the log does not say why"
else
  pass "${CASE}"
fi

new_case targets-file-input
printf '# recorded by hand\n//rec:a_test\n\n' > "${FAKE_STATE}/targets.txt"
start linux-asan --targets-file "${FAKE_STATE}/targets.txt"
if expect_eq status "$(jget status)" culprit && expect_eq targets "$(jget targets)" '["//rec:a_test"]'; then
  if [ -s "${FAKE_STATE}/gh.log" ]; then fail "${CASE}" "gh was called"; else pass "${CASE}"; fi
fi

new_case timeout-only-is-labelled
printf 'linux-tsan\tRun TSan\t2026-10-26T02:10:11.1Z //rec:a_test   TIMEOUT in 900.0s\n' > "${FAKE_STATE}/gh-log"
set_map 7 12 timeout
start linux-tsan
expect_eq status "$(jget status)" culprit && expect_eq timeout_only "$(jget timeout_only)" true && pass "${CASE}"

new_case pre-config-commit-is-skip
start linux-asan --bad "${SHA[8]}"
if expect_eq status "$(jget status)" culprit && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}"; then
  if steplog | grep -q "^[0-9]* ${SHA[4]} 125 .*ci-asan"; then pass "${CASE}"
  else fail "${CASE}" "commit 4 was not a 125 step: $(steplog)"; fi
fi

new_case vendor-failure-is-skip
add_map 8 vendorfail
start linux-asan
if expect_eq status "$(jget status)" culprit && expect_eq first_bad "$(jget first_bad)" "${SHA[7]}"; then
  if steplog | grep -q "^[0-9]* ${SHA[8]} 125 .*vendor"; then pass "${CASE}"
  else fail "${CASE}" "commit 8 was not a 125 vendor step: $(steplog)"; fi
fi

new_case skips-around-culprit-is-inconclusive
add_map 6 otherbuild
add_map 7 otherbuild
start linux-asan
skips=$(jget skips)
if expect_eq status "$(jget status)" inconclusive && expect_eq first_bad "$(jget first_bad)" null; then
  case "${skips}" in
    *"${SHA[6]}"*"${SHA[7]}"*|*"${SHA[7]}"*"${SHA[6]}"*) pass "${CASE}" ;;
    *) fail "${CASE}" "skips ${skips} do not list commits 6 and 7" ;;
  esac
fi

new_case endpoint-skip-is-inconclusive
add_map 12 vendorfail
start linux-asan
expect_eq status "$(jget status)" inconclusive && expect_eq skips "$(jget skips)" "[\"${SHA[12]}\"]" && pass "${CASE}"

new_case resume-keeps-state
export BISECT_STOP_AFTER=endpoints
start linux-asan
unset BISECT_STOP_AFTER
if [ -e "${RESULT}" ]; then
  fail "${CASE}" "result written before the stop"
elif ! grep -q stopped "${BISECT_ROOT}/${ID}/state"; then
  fail "${CASE}" "state after the stop: $(cat "${BISECT_ROOT}/${ID}/state")"
else
  st_out=$("${DRIVER}" status "${ID}" 2>&1 || true)
  # An orphan an earlier (killed) worker recorded: resume removes it first.
  echo "container ${ID}-step-99" >> "${BISECT_ROOT}/${ID}/names"
  # And a vendoring output base an interrupted call left: cleanup shuts its
  # server down and leaves the directory alone.
  mkdir -p "${FAKE_TMP}/orphan-ob"
  echo "${FAKE_TMP}/orphan-ob" >> "${BISECT_ROOT}/${ID}/vendor-obs"
  before=$(wc -l < "${FAKE_STATE}/docker.log")
  RC=0; "${DRIVER}" resume "${ID}" >/dev/null 2>"${FAKE_STATE}/resume.err" || RC=$?
  after_log=$(tail -n +"$((before + 1))" "${FAKE_STATE}/docker.log")
  rm_at=$(grep -n -- "^docker rm -f ${ID}-step-99\$" <<<"${after_log}" | head -1 | cut -d: -f1)
  run_at=$(grep -n '^docker run' <<<"${after_log}" | head -1 | cut -d: -f1)
  if expect_eq rc "${RC}" 0 && expect_eq status "$(jget status)" culprit; then
    if ! grep -q 'state' <<<"${st_out}"; then
      fail "${CASE}" "status printed no state: ${st_out}"
    elif [ -z "${rm_at}" ] || [ -z "${run_at}" ] || [ "${rm_at}" -gt "${run_at}" ]; then
      fail "${CASE}" "the recorded orphan was not removed before new containers started"
    elif ! grep -q -- "--output_base=${FAKE_TMP}/orphan-ob shutdown" "${FAKE_STATE}/bazel.log"; then
      fail "${CASE}" "cleanup did not shut down a recorded vendoring output base"
    elif [ ! -d "${FAKE_TMP}/orphan-ob" ]; then
      fail "${CASE}" "cleanup deleted a directory outside BISECT_ROOT"
    elif cmp -s "${RESULT}" "${CULPRIT_RESULT}"; then
      pass "${CASE}"
    else
      fail "${CASE}" "resumed result differs: $(diff "${RESULT}" "${CULPRIT_RESULT}" | head -5)"
    fi
  fi
fi

new_case budget-defaults
export FAKE_NPROC=128
start linux-asan
if grep -q -- '--cpus 16 --memory 32g --memory-swap 32g --cpu-shares 128' "${FAKE_STATE}/docker.log" \
  && grep -q -- '--jobs=16 ' "${FAKE_STATE}/docker.log"; then
  new_case budget-defaults-small-host
  export FAKE_NPROC=2
  start linux-asan
  if grep -q -- '--cpus 1 --memory 16g --memory-swap 16g' "${FAKE_STATE}/docker.log" \
    && grep -q -- '--jobs=1 ' "${FAKE_STATE}/docker.log"; then pass budget-defaults
  else fail budget-defaults "small host: $(grep -m1 -- '-step-' "${FAKE_STATE}/docker.log")"; fi
else
  fail "${CASE}" "$(grep -m1 -- '-step-' "${FAKE_STATE}/docker.log")"
fi
unset FAKE_NPROC

new_case flags-override-budget
export FAKE_NPROC=128
start linux-asan --cpus 3 --memory-gb 20 --runs-per-test 4 --remote-cache-url http://cache.invalid:1
line=$(grep -m1 -- '-step-1 ' "${FAKE_STATE}/docker.log" || true)
case "${line}" in
  *"--cpus 3 --memory 20g --memory-swap 20g "*"--jobs=3 --runs_per_test=4 --remote_cache=http://cache.invalid:1 --noremote_upload_local_results "*) pass "${CASE}" ;;
  *) fail "${CASE}" "${line}" ;;
esac
unset FAKE_NPROC

# --- Refusals and the lock -----------------------------------------------
new_case refuses-bad-budget-values
bad_values=""
for v in "--cpus 00" "--cpus 0" "--cpus -1" "--cpus 1.5" "--memory-gb 0" "--memory-gb 08" "--runs-per-test 0"; do
  # shellcheck disable=SC2086 # flag and value
  start linux-asan ${v}
  [ "${RC}" = 2 ] || bad_values="${bad_values} [${v}: ${RC}]"
done
if [ -n "${bad_values}" ]; then fail "${CASE}" "accepted:${bad_values}"
elif ls -d "${BISECT_ROOT}"/bisect-* >/dev/null 2>&1; then fail "${CASE}" "a run dir was created"
else pass "${CASE}"; fi

new_case refuses-without-bisect-root
unset BISECT_ROOT
RC=0; "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1 || RC=$?
RC2=0; BISECT_ROOT=relative/dir "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1 || RC2=$?
expect_eq rc "${RC}" 2 && expect_eq rc-relative "${RC2}" 2 && pass "${CASE}"

new_case refuses-unknown-job
start linux-arm64-build
expect_eq rc "${RC}" 2 && pass "${CASE}"

new_case refuses-runner-work-tree
RUNNER_DIR="${WORK}/srv/actions-runner-3/_work/x"
RC=0; BISECT_ROOT="${RUNNER_DIR}" "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1 || RC=$?
mkdir -p "${WORK}/srv/actions-runner-3/_work/y"
ln -s "${WORK}/srv/actions-runner-3/_work/y" "${WORK}/link-root"
RC2=0; BISECT_ROOT="${WORK}/link-root" "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1 || RC2=$?
RC3=0; BISECT_ROOT="${WORK}/link-root/new/deeper" "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1 || RC3=$?
if expect_eq rc "${RC}" 2 && expect_eq rc-symlink "${RC2}" 2 && expect_eq rc-symlink-new "${RC3}" 2; then
  if [ -e "${RUNNER_DIR}" ] || [ -n "$(ls -A "${WORK}/srv/actions-runner-3/_work/y")" ]; then
    fail "${CASE}" "something was written inside the runner tree"; else pass "${CASE}"; fi
fi

new_case refuses-when-lock-held
RC=0
(
  flock -n 8 || exit 99
  "${DRIVER}" start --job linux-asan --bad "${SHA[12]}" --good "${SHA[1]}" --run 1 --repo "${ORIGIN}" >/dev/null 2>&1
) 8>"${BISECT_ROOT}/.lock" || RC=$?
if expect_eq rc "${RC}" 3; then
  if ls -d "${BISECT_ROOT}"/bisect-* >/dev/null 2>&1; then fail "${CASE}" "a run dir was created"; else pass "${CASE}"; fi
fi

# --- The repository's own table and workflow -------------------------------
CASE=table-matches-workflow
WF="${REPO_ROOT}/.github/workflows/pipeline-full.yml"
(
  # shellcheck source=/dev/null
  . "${REPO_ROOT}/tools/ci/bisect-job-table.sh"
  bad=0
  report() { echo "     $1"; bad=1; }
  # has_token TOKEN: TOKEN appears in the job block as a whole word
  # (--config=ci does not match inside --config=ci-asan).
  has_token() {
    local re
    re=$(printf '%s' "$1" | sed 's#[].*^$+?(){}|\\/[]#\\&#g')
    grep -qE -- "(^|[^A-Za-z0-9_=-])${re}([^A-Za-z0-9_-]|$)" <<<"${block}"
  }
  for job in ${BISECT_JOBS}; do
    block=$(awk -v j="  ${job}:" '$0 == j {on = 1; next} on && /^  [a-z0-9-]+:$/ {exit} on {print}' "${WF}")
    [ -n "${block}" ] || { report "${job}: no job block in pipeline-full.yml"; continue; }
    J=$(printf '%s' "${job}" | tr 'a-z-' 'A-Z_')
    v() { eval "printf '%s' \"\${BISECT_${J}_$1:-}\""; }
    grep -qxF "    name: $(v NAME)" <<<"${block}" || report "${job}: name '$(v NAME)'"
    has_token "--config=$(v CONFIG)" || report "${job}: --config=$(v CONFIG)"
    grep -qF -- "--output_user_root=$(v OUTPUT_ROOT)" <<<"${block}" || report "${job}: output root $(v OUTPUT_ROOT)"
    for t in $(v BAZEL_FLAGS) $(v DOCKER_FLAGS); do
      has_token "${t}" || report "${job}: flag ${t}"
    done
    # And the other way: every docker flag, -e key and --test_env name of the
    # job's docker runs is in the table (or one the driver sets itself).
    run_lines=$(awk '/docker run --rm --init/ {on = 1} on {print} on && /DOCKER_IMAGE/ {on = 0}' <<<"${block}")
    for t in $(grep -oE '(^|[[:space:]])--[a-z][a-z-]*(=[^[:space:]\\]*)?' < <(grep -v -- ' -e ' <<<"${run_lines}") | tr -d ' \t'); do
      case "${t}" in
        --rm|--init|--user|--memory=*|--memory-swap=*|--network=*) continue ;;
        --security-opt) t="--security-opt" ;;
      esac
      case " $(v DOCKER_FLAGS) " in *" ${t} "*) ;; *) report "${job}: workflow docker flag ${t} not in the table" ;; esac
    done
    services_up=$(printf '%s' "$(v SERVICES)" | tr 'a-z' 'A-Z')
    for k in $(grep -oE '(^|[[:space:]])-e [A-Z_][A-Z0-9_]*=' <<<"${run_lines}" | sed 's/.*-e //; s/=$//' | sort -u); do
      case "${k}" in BAZEL_CONFIG) continue ;; esac
      case " ${services_up} " in *" ${k%_HOST} "*) continue ;; esac
      case " $(v ENV) " in *" ${k}="*) ;; *) report "${job}: workflow env ${k} not in the table" ;; esac
    done
    for k in $(grep -oE -- '--test_env=[A-Z_][A-Z0-9_]*' <<<"${block}" | sed 's/--test_env=//' | sort -u); do
      case " ${services_up} " in *" ${k%_HOST} "*) continue ;; esac
      case " $(v SERVICE_TEST_ENV) " in *" ${k}="*) ;; *) report "${job}: workflow --test_env=${k} not in the table" ;; esac
    done
    for e in $(v ENV); do
      grep -qF -- "-e ${e}" <<<"${block}" || report "${job}: env ${e}"
    done
    if [ "$(v PASSWD)" = 1 ]; then
      grep -qF -- ':/etc/passwd:ro' <<<"${block}" || report "${job}: passwd mount"
    else
      grep -qF -- ':/etc/passwd:ro' <<<"${block}" && report "${job}: job mounts a passwd file, table says 0"
    fi
    for s in $(v SERVICES); do
      grep -qE "for img in .*${s}:" <<<"${block}" || report "${job}: service ${s}"
      S=$(printf '%s' "${s}" | tr 'a-z' 'A-Z')
      grep -qF -- "--test_env=${S}_HOST=" <<<"${block}" || report "${job}: ${S}_HOST"
    done
    for e in $(v SERVICE_TEST_ENV); do
      grep -qF -- "--test_env=${e}" <<<"${block}" || report "${job}: --test_env=${e}"
    done
    if [ -z "$(v SERVICES)" ] && grep -q 'for img in ' <<<"${block}"; then
      report "${job}: job starts services, table lists none"
    fi
    case "$(v KIND)" in
      test) grep -qE "(^|[[:space:]])test .*--config=$(v CONFIG)([^A-Za-z0-9_-]|$)" <<<"${block}" || report "${job}: not a bazel test" ;;
      build) grep -qE "build .*--config=$(v CONFIG)([^A-Za-z0-9_-]|$)" <<<"${block}" || report "${job}: not a bazel build" ;;
      *) report "${job}: kind '$(v KIND)'" ;;
    esac
    keys=$(sed -n 's/.*vendor-.*hashFiles(\([^)]*\)).*/\1/p' <<<"${block}" | head -1 \
      | tr -d "' " | tr ',' '\n' | sed 's#/\*\*$##')
    want=$(printf '%s\n' ${BISECT_VENDOR_KEY_PATHS})
    [ "${keys}" = "${want}" ] || report "${job}: vendor key paths '$(echo ${keys})' != '$(echo ${want})'"
  done
  exit "${bad}"
) && pass "${CASE}" || fail "${CASE}" "table and pipeline-full.yml disagree (above)"

CASE=no-host-specifics-in-source
if grep -nE '[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|[.]home\b|/Users/|/home/[a-z]' \
  "${REPO_ROOT}/tools/ci/bisect-job.sh" "${REPO_ROOT}/tools/ci/bisect-job-table.sh"; then
  fail "${CASE}" "host specifics in the source (above)"
else
  pass "${CASE}"
fi

echo
echo "test_bisect_job: ${PASSES} passed, ${FAILS} failed"
[ "${FAILS}" = 0 ]
