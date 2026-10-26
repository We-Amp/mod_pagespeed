#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Host-local bisect of a red Linux full-tier job (docs/ci/pipelines.md,
# "Bisecting a red full-tier job"). The same file in every repository; the
# per-repository data (job names, configs, container flags, services, the
# vendor command) is in tools/ci/bisect-job-table.sh next to it.
#
#   bisect-job.sh start --job <linux-asan|linux-tsan|linux-build>
#       --bad <sha> --good <sha> (--run <run-id> | --targets-file <file>)
#       --repo <git url or path> [--cpus N] [--memory-gb N]
#       [--runs-per-test N] [--remote-cache-url URL]
#     Prints the run id (bisect-<repo>-<job>-<utc>-<pid>) and returns at
#     once; the work runs detached and writes BISECT_ROOT/<id>/{state,log,
#     result.json,exit}.
#   bisect-job.sh status <id>    state, the last log lines, exit when done
#   bisect-job.sh resume <id>    take the lock again and continue
#   bisect-job.sh _step <id>     one step (what `git bisect run` calls):
#                                exit 0 good, 1 bad, 125 skip
#
# start/resume exit: 0 launched; 2 refused (BISECT_ROOT unset, relative or
# inside an actions-runner*/_work tree, unknown job, usage); 3 another
# bisect holds BISECT_ROOT/.lock.
#
# result.json: status (culprit | inconclusive | env-dependent |
# external-drift-or-stale-good | no-targets | too-many-targets), first_bad,
# skips, targets, timeout_only, rebuilt_images, steps, bad, good, job.
#
# Method: the failing targets come from the failed job's log (or a file)
# through failing_targets.sh; more than BISECT_MAX_TARGETS (default 10) or
# none means no bisect. The bad endpoint must fail and the good one pass
# before `git bisect start --first-parent <bad> <good>` and `git bisect
# run`. A step builds or tests only those targets, with the commit's own
# .bazelrc config, vendored tree and dev image, in a capped container.
#
# Host safety: BISECT_ROOT is required and never defaulted. Every container
# and network name starts with the run id and is recorded in
# BISECT_ROOT/<id>/names before it is created; only those names are ever
# removed; a resumed worker first removes the containers an earlier worker
# recorded. Host Bazel (vendoring) runs with HOME and --output_user_root
# (so the install base) under BISECT_ROOT/<id>, through a PATH shim that
# keeps the vendor script's own --output_base (the script may read it),
# records it, and shuts that server down when each call returns (a call
# without one gets BISECT_ROOT/<id>/vendor-ob). The exit trap shuts down any
# recorded output base that still exists and removes only the trees under
# BISECT_ROOT/<id>; a vendor script's own directory stays its own. Images
# are only ever added (content-hashed tags from the commit's
# ensure-dev-image.sh), never retagged or removed.
# Host-side work (docker clients, vendoring, image builds) runs under
# nice/ionice. That does not carry into a container: a step container's
# share comes from --cpu-shares 128 and its --cpus/--memory caps, and the
# service containers get a small fixed cap. One bisect per host: flock on
# BISECT_ROOT/.lock, held by the detached worker.
#
# Disk: each step copies the vendored tree into the work tree (cp -a), and
# BISECT_ROOT (runs, vendor trees, disk caches) only grows: this script
# never removes old runs from it; operators do.
#
# Environment: BISECT_ROOT (required, absolute), BISECT_MAX_TARGETS,
# BISECT_STOP_AFTER=endpoints (stop after the endpoint checks; `resume`
# continues).

set -uo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd -P)"
SELF="${SELF_DIR}/$(basename "$0")"
FAILING_TARGETS="${SELF_DIR}/failing_targets.sh"
IMAGE_ARCH=x64
MAX_TARGETS="${BISECT_MAX_TARGETS:-10}"

# shellcheck source=tools/ci/bisect-job-table.sh
. "${SELF_DIR}/bisect-job-table.sh"

say() { printf '%s bisect-job: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
err() { echo "bisect-job: $*" >&2; }
usage() { sed -n '9,24p' "${SELF}" | sed 's/^# \{0,1\}//' >&2; }

# --- Table access ----------------------------------------------------------

known_job() {
  case "$1" in ''|*[!a-z0-9-]*) return 1 ;; esac
  case " ${BISECT_JOBS} " in *" $1 "*) return 0 ;; esac
  return 1
}

# job_var JOB FIELD: BISECT_<JOB>_<FIELD> from the table (JOB is checked
# by known_job first, so the eval sees [A-Z0-9_] only).
job_var() {
  local key
  key="BISECT_$(printf '%s' "$1" | tr 'a-z-' 'A-Z_')_$2"
  eval "printf '%s' \"\${${key}:-}\""
}

# --- BISECT_ROOT and the lock --------------------------------------------

in_runner_tree() {
  case "$1/" in */actions-runner*/_work/*) return 0 ;; esac
  return 1
}

# check_root: refuse an unusable BISECT_ROOT, else export its physical path.
check_root() {
  local resolved
  if [ -z "${BISECT_ROOT:-}" ]; then
    err "BISECT_ROOT is not set (a fresh directory for bisect runs; never defaulted)"
    return 2
  fi
  case "${BISECT_ROOT}" in
    /*) ;;
    *) err "BISECT_ROOT must be an absolute path: ${BISECT_ROOT}"; return 2 ;;
  esac
  case "/${BISECT_ROOT}/" in
    */../*|*/./*) err "BISECT_ROOT must not contain . or .. components: ${BISECT_ROOT}"; return 2 ;;
  esac
  if in_runner_tree "${BISECT_ROOT}"; then
    err "refusing BISECT_ROOT inside a runner work tree: ${BISECT_ROOT}"
    return 2
  fi
  # Before creating anything: resolve the nearest existing ancestor, so a
  # not-yet-existing path behind a symlink into a runner tree is refused.
  local anc="${BISECT_ROOT}" rest=""
  while [ ! -d "${anc}" ]; do
    rest="/$(basename "${anc}")${rest}"
    anc="$(dirname "${anc}")"
  done
  resolved="$(cd -P "${anc}" && pwd -P)" || return 2
  if in_runner_tree "${resolved}${rest}"; then
    err "refusing BISECT_ROOT inside a runner work tree: ${resolved}${rest}"
    return 2
  fi
  mkdir -p "${BISECT_ROOT}" || return 2
  resolved="$(cd -P "${BISECT_ROOT}" && pwd -P)" || return 2
  if in_runner_tree "${resolved}"; then
    err "refusing BISECT_ROOT inside a runner work tree: ${resolved}"
    return 2
  fi
  BISECT_ROOT="${resolved}"
  export BISECT_ROOT
}

# take_lock: fd 9 holds an exclusive flock on BISECT_ROOT/.lock. A child
# inherits it, so the detached worker keeps the lock for its lifetime.
take_lock() {
  if ! command -v flock >/dev/null 2>&1; then
    err "flock(1) is required (one bisect per host)"
    return 2
  fi
  if ! { true >&9; } 2>/dev/null; then
    exec 9>>"${BISECT_ROOT}/.lock" || return 2
  fi
  if ! flock -n 9; then
    err "another bisect holds ${BISECT_ROOT}/.lock"
    return 3
  fi
}

valid_id() {
  case "$1" in bisect-*) ;; *) return 1 ;; esac
  case "$1" in */*|*..*) return 1 ;; esac
  [ -f "${BISECT_ROOT}/$1/config" ]
}

# --- Small helpers ---------------------------------------------------------

# lowprio CMD...: a host command at the lowest CPU (and, when available,
# I/O) priority. A container started by a docker client does not inherit it.
lowprio() {
  if command -v ionice >/dev/null 2>&1; then
    nice -n 19 ionice -c3 "$@"
  else
    nice -n 19 "$@"
  fi
}

dk() { lowprio docker "$@"; }

# posint VALUE: a positive whole number without leading zeros.
posint() {
  case "$1" in ''|0*|*[!0-9]*) return 1 ;; esac
  return 0
}

host_cpus() {
  local n=""
  n="$(nproc 2>/dev/null)" || n=""
  [ -n "${n}" ] || n="$(getconf _NPROCESSORS_ONLN 2>/dev/null)" || n=""
  [ -n "${n}" ] || n="$(sysctl -n hw.ncpu 2>/dev/null)" || n=""
  case "${n}" in ''|*[!0-9]*) n=4 ;; esac
  echo "${n}"
}

sha256_stdin() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum | cut -c1-64
  else
    shasum -a 256 | cut -c1-64
  fi
}

json_str() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '"%s"' "${s}"
}

# json_list FILE: a JSON array of the file's unique non-empty lines.
json_list() {
  local first=1 line out="["
  if [ -f "$1" ]; then
    while IFS= read -r line; do
      [ -n "${line}" ] || continue
      [ "${first}" = 1 ] || out="${out}, "
      out="${out}$(json_str "${line}")"
      first=0
    done < <(awk '!seen[$0]++' "$1")
  fi
  printf '%s]' "${out}"
}

remove_tree() {
  [ -e "$1" ] || return 0
  chmod -R u+w "$1" 2>/dev/null
  rm -rf "$1"
}

# record KIND NAME: note a docker object before creating it.
record() { echo "$1 $2" >> "${RUN_DIR}/names"; }

set_state() { echo "$1" > "${RUN_DIR}/state"; }

# load_run ID: RUN_DIR, the run's config and its job's table values.
load_run() {
  RUN_ID="$1"
  RUN_DIR="${BISECT_ROOT}/${RUN_ID}"
  SRC="${RUN_DIR}/src"
  # shellcheck source=/dev/null
  . "${RUN_DIR}/config"
  KIND="$(job_var "${JOB}" KIND)"
  CONFIG="$(job_var "${JOB}" CONFIG)"
}

# --- start / status / resume -----------------------------------------------

cmd_start() {
  local job="" bad="" good="" run="" targets_file="" repo="" cpus="" mem=""
  local runs_per_test="" remote_cache_url="" rc
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --job|--bad|--good|--run|--targets-file|--repo|--cpus|--memory-gb|--runs-per-test|--remote-cache-url)
        [ "$#" -ge 2 ] || { err "$1 needs a value"; return 2; }
        case "$1" in
          --job) job="$2" ;;
          --bad) bad="$2" ;;
          --good) good="$2" ;;
          --run) run="$2" ;;
          --targets-file) targets_file="$2" ;;
          --repo) repo="$2" ;;
          --cpus) cpus="$2" ;;
          --memory-gb) mem="$2" ;;
          --runs-per-test) runs_per_test="$2" ;;
          --remote-cache-url) remote_cache_url="$2" ;;
        esac
        shift 2 ;;
      -h|--help) usage; return 0 ;;
      *) err "unknown argument: $1"; usage; return 2 ;;
    esac
  done
  check_root || return 2
  if ! known_job "${job}"; then
    err "unknown job '${job}' (v1 jobs: ${BISECT_JOBS})"
    return 2
  fi
  if [ -z "${bad}" ] || [ -z "${good}" ] || [ -z "${repo}" ]; then
    err "--bad, --good and --repo are required"; usage; return 2
  fi
  if [ -n "${run}" ] && [ -n "${targets_file}" ] || [ -z "${run}${targets_file}" ]; then
    err "give exactly one of --run and --targets-file"; return 2
  fi
  if [ -n "${targets_file}" ] && [ ! -r "${targets_file}" ]; then
    err "cannot read ${targets_file}"; return 2
  fi
  local v
  for v in "${cpus}" "${mem}" "${runs_per_test}"; do
    [ -z "${v}" ] || posint "${v}" \
      || { err "--cpus, --memory-gb and --runs-per-test take positive whole numbers (got '${v}')"; return 2; }
  done
  if [ -z "${cpus}" ]; then
    cpus=$(( $(host_cpus) / 4 ))
    [ "${cpus}" -ge 1 ] || cpus=1
    [ "${cpus}" -le 16 ] || cpus=16
  fi
  if [ -z "${mem}" ]; then
    mem=$(( 2 * cpus ))
    [ "${mem}" -ge 16 ] || mem=16
  fi
  if [ -d "${repo}" ]; then
    repo="$(cd "${repo}" && pwd -P)"
  fi

  take_lock
  rc=$?
  [ "${rc}" = 0 ] || return "${rc}"

  local id dir
  id="bisect-${BISECT_REPO_NAME}-${job}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
  dir="${BISECT_ROOT}/${id}"
  mkdir -p "${dir}" || return 2
  if [ -n "${targets_file}" ]; then
    cp "${targets_file}" "${dir}/targets.input" || return 2
  fi
  {
    printf 'JOB=%q\n' "${job}"
    printf 'BAD=%q\n' "${bad}"
    printf 'GOOD=%q\n' "${good}"
    printf 'RUN=%q\n' "${run}"
    printf 'REPO=%q\n' "${repo}"
    printf 'CPUS=%q\n' "${cpus}"
    printf 'MEM_GB=%q\n' "${mem}"
    printf 'RUNS_PER_TEST=%q\n' "${runs_per_test}"
    printf 'REMOTE_CACHE_URL=%q\n' "${remote_cache_url}"
  } > "${dir}/config"
  echo "created" > "${dir}/state"
  echo "${id}"
  launch "${id}"
}

launch() {
  local dir="${BISECT_ROOT}/$1"
  rm -f "${dir}/exit"
  if command -v setsid >/dev/null 2>&1; then
    nohup setsid -f "${SELF}" _worker "$1" </dev/null >>"${dir}/log" 2>&1
  else
    nohup "${SELF}" _worker "$1" </dev/null >>"${dir}/log" 2>&1 &
  fi
}

cmd_status() {
  check_root || return 2
  valid_id "${1:-}" || { err "no run '${1:-}' under ${BISECT_ROOT}"; return 2; }
  local dir="${BISECT_ROOT}/$1"
  echo "state: $(cat "${dir}/state" 2>/dev/null)"
  if [ -f "${dir}/log" ]; then
    echo "log (last 20 lines):"
    tail -n 20 "${dir}/log" | sed 's/^/  /'
  fi
  [ -f "${dir}/exit" ] && echo "exit: $(cat "${dir}/exit")"
  [ -f "${dir}/result.json" ] && cat "${dir}/result.json"
  return 0
}

cmd_resume() {
  local rc
  check_root || return 2
  valid_id "${1:-}" || { err "no run '${1:-}' under ${BISECT_ROOT}"; return 2; }
  take_lock
  rc=$?
  [ "${rc}" = 0 ] || return "${rc}"
  echo "resumed" >> "${BISECT_ROOT}/$1/state.history"
  echo "$1"
  launch "$1"
}

# --- The detached worker -------------------------------------------------

worker_exit() {
  local rc="$1"
  cleanup
  echo "${rc}" > "${RUN_DIR}/exit"
  say "exit ${rc}"
}

# remove_recorded [containers]: remove the run's own docker objects, by the
# names recorded in <id>/names only (containers, then networks).
remove_recorded() {
  local name
  if [ -f "${RUN_DIR}/names" ]; then
    for name in $(awk '$1 == "container" {print $2}' "${RUN_DIR}/names" | awk '!seen[$0]++'); do
      dk rm -f "${name}" >/dev/null 2>&1
    done
    for name in $(awk '$1 == "network" {print $2}' "${RUN_DIR}/names" | awk '!seen[$0]++'); do
      [ "${1:-}" = containers ] || dk network rm "${name}" >/dev/null 2>&1
    done
  fi
}

# Remove the run's docker objects and its host Bazel server and trees.
cleanup() {
  local real ob
  remove_recorded
  real="$(cat "${RUN_DIR}/vendor-bin/real-bazel" 2>/dev/null)"
  # A vendoring call that was interrupted may have left its server running:
  # shut down every recorded output base that still exists (never delete
  # one outside the run's own directory).
  if [ -n "${real}" ] && [ -f "${RUN_DIR}/vendor-obs" ]; then
    while IFS= read -r ob; do
      [ -n "${ob}" ] && [ -d "${ob}" ] || continue
      (cd "${SRC}" 2>/dev/null || cd "${RUN_DIR}" || exit; HOME="${RUN_DIR}/home" \
        "${real}" "--output_base=${ob}" shutdown) >/dev/null 2>&1 9>&-
    done < "${RUN_DIR}/vendor-obs"
  fi
  remove_tree "${RUN_DIR}/vendor-ob"
  remove_tree "${RUN_DIR}/bazel-root"
}

write_result() {
  local status="$1" first_bad="${2:-}" fb="null" tmp="${RUN_DIR}/result.json.tmp"
  [ -n "${first_bad}" ] && fb="$(json_str "${first_bad}")"
  {
    echo "{"
    echo "  \"status\": $(json_str "${status}"),"
    echo "  \"first_bad\": ${fb},"
    echo "  \"skips\": $(json_list "${RUN_DIR}/skips"),"
    echo "  \"targets\": $(json_list "${RUN_DIR}/targets"),"
    echo "  \"timeout_only\": $(cat "${RUN_DIR}/timeout_only" 2>/dev/null || echo false),"
    echo "  \"rebuilt_images\": $(json_list "${RUN_DIR}/rebuilt_images"),"
    echo "  \"steps\": $(cat "${RUN_DIR}/steps" 2>/dev/null || echo 0),"
    echo "  \"bad\": $(json_str "$(cat "${RUN_DIR}/bad.sha" 2>/dev/null || echo "${BAD}")"),"
    echo "  \"good\": $(json_str "$(cat "${RUN_DIR}/good.sha" 2>/dev/null || echo "${GOOD}")"),"
    echo "  \"job\": $(json_str "${JOB}")"
    echo "}"
  } > "${tmp}" && mv "${tmp}" "${RUN_DIR}/result.json"
  set_state "done: ${status}"
  say "result: ${status}${first_bad:+ ${first_bad}}"
}

phase_targets() {
  [ -f "${RUN_DIR}/targets" ] && return 0
  set_state "targets"
  local extracted="${RUN_DIR}/targets.extracted"
  if [ -f "${RUN_DIR}/targets.input" ]; then
    awk '!/^[[:space:]]*#/ && NF {print $1 "\tlisted"}' "${RUN_DIR}/targets.input" > "${extracted}"
  else
    local name job_id
    name="$(job_var "${JOB}" NAME)"
    job_id="$(gh run view "${RUN}" -R "${BISECT_GH_REPO}" --json jobs \
      --jq ".jobs[] | select(.name == \"${name}\" or (.name | endswith(\" / ${name}\"))) | .databaseId" \
      | head -n 1)" || job_id=""
    if [ -z "${job_id}" ]; then
      say "no job '${name}' in run ${RUN}"
      return 1
    fi
    say "fetching the failed log of run ${RUN} job ${job_id}"
    gh run view "${RUN}" -R "${BISECT_GH_REPO}" --job "${job_id}" --log-failed --allow-escape-sequences \
      > "${RUN_DIR}/job.log" || { say "cannot fetch the job log"; return 1; }
    bash "${FAILING_TARGETS}" --mode "${KIND}" "${RUN_DIR}/job.log" > "${extracted}" || return 1
  fi
  cut -f1 "${extracted}" | LC_ALL=C sort -u > "${RUN_DIR}/targets.tmp"
  if [ -s "${extracted}" ] && ! awk -F '\t' '$2 != "test-timeout" {found = 1} END {exit found ? 0 : 1}' "${extracted}"; then
    echo true > "${RUN_DIR}/timeout_only"
  else
    echo false > "${RUN_DIR}/timeout_only"
  fi
  mv "${RUN_DIR}/targets.tmp" "${RUN_DIR}/targets"
  local count
  count="$(grep -c . "${RUN_DIR}/targets")"
  say "${count} target(s): $(tr '\n' ' ' < "${RUN_DIR}/targets")"
  if [ "${count}" = 0 ]; then
    write_result no-targets
  elif [ "${count}" -gt "${MAX_TARGETS}" ]; then
    write_result too-many-targets
  fi
  return 0
}

resolve_sha() {
  git -C "${SRC}" rev-parse --verify -q "$1^{commit}" 2>/dev/null && return 0
  git -C "${SRC}" fetch -q origin "$1" >/dev/null 2>&1 \
    && git -C "${SRC}" rev-parse --verify -q "$1^{commit}" 2>/dev/null
}

phase_clone() {
  if [ ! -d "${SRC}/.git" ]; then
    set_state "clone"
    remove_tree "${SRC}.partial"
    git clone -q -c core.autocrlf=false "${REPO}" "${SRC}.partial" || return 1
    mv "${SRC}.partial" "${SRC}" || return 1
  fi
  local b g
  b="$(resolve_sha "${BAD}")" || { say "unknown bad commit ${BAD}"; return 1; }
  g="$(resolve_sha "${GOOD}")" || { say "unknown good commit ${GOOD}"; return 1; }
  echo "${b}" > "${RUN_DIR}/bad.sha"
  echo "${g}" > "${RUN_DIR}/good.sha"
}

# endpoint WHICH: run one step at the bad or good commit (once; kept).
endpoint() {
  local f="${RUN_DIR}/endpoint-$1" sha code
  if [ ! -f "${f}" ]; then
    sha="$(cat "${RUN_DIR}/$1.sha")"
    set_state "endpoint $1"
    git -C "${SRC}" checkout -q --detach "${sha}" || return 1
    (cd "${SRC}" && "${SELF}" _step "${RUN_ID}") >&2
    code=$?
    echo "${code}" > "${f}"
  fi
  cat "${f}"
}

phase_endpoints() {
  local code
  code="$(endpoint bad)" || return 1
  case "${code}" in
    1) ;;
    0) write_result env-dependent; return 0 ;;
    *) write_result inconclusive; return 0 ;;
  esac
  code="$(endpoint good)" || return 1
  case "${code}" in
    0) ;;
    1) write_result external-drift-or-stale-good; return 0 ;;
    *) write_result inconclusive; return 0 ;;
  esac
}

phase_bisect() {
  local out="${RUN_DIR}/bisect-run.out" first rc
  set_state "bisect"
  if [ ! -f "${RUN_DIR}/bisect-started" ]; then
    (cd "${SRC}" && git bisect reset -q >/dev/null 2>&1; \
      git bisect start --first-parent "$(cat "${RUN_DIR}/bad.sha")" "$(cat "${RUN_DIR}/good.sha")") || return 1
    touch "${RUN_DIR}/bisect-started"
  fi
  (cd "${SRC}" && git bisect run "${SELF}" _step "${RUN_ID}") 2>&1 | tee "${out}"
  rc="${PIPESTATUS[0]}"
  git -C "${SRC}" bisect log > "${RUN_DIR}/bisect.log" 2>/dev/null
  first="$(sed -n 's/^\([0-9a-f]\{40,64\}\) is the first bad commit$/\1/p' "${out}" | head -n 1)"
  git -C "${SRC}" bisect reset -q >/dev/null 2>&1
  if [ -n "${first}" ]; then
    write_result culprit "${first}"
  else
    say "git bisect run ended (${rc}) without a single first bad commit"
    write_result inconclusive
  fi
}

cmd_worker() {
  local rc
  check_root || exit 2
  valid_id "${1:-}" || { err "no run '${1:-}'"; exit 2; }
  load_run "$1"
  take_lock || exit $?
  trap 'worker_exit $?' EXIT
  # A resumed run: containers an earlier worker started (and could not
  # remove, e.g. after it was killed) go first, by recorded name.
  remove_recorded containers
  say "worker for ${RUN_ID}: job ${JOB} (${KIND}, --config=${CONFIG}), bad ${BAD}, good ${GOOD}, ${CPUS} cpus, ${MEM_GB} GiB"
  [ -f "${RUN_DIR}/result.json" ] && exit 0
  phase_targets || exit 1
  [ -f "${RUN_DIR}/result.json" ] && exit 0
  phase_clone || exit 1
  phase_endpoints || exit 1
  [ -f "${RUN_DIR}/result.json" ] && exit 0
  if [ "${BISECT_STOP_AFTER:-}" = endpoints ]; then
    set_state "stopped after the endpoints (resume continues)"
    exit 0
  fi
  phase_bisect || exit 1
  exit 0
}

# --- One step ----------------------------------------------------------------

STEP_CODE=125
STEP_REASON=""
verdict() { STEP_CODE="$1"; STEP_REASON="$2"; }

# service_image SERVICE: the image tag the commit's workflow starts it with.
service_image() {
  sed -n 's/^.*for img in \(.*\); do.*$/\1/p' "${SRC}/.github/workflows/pipeline-full.yml" 2>/dev/null \
    | head -n 1 | tr ' ' '\n' | grep -m1 "^$1:"
}

stop_services() {
  local s
  for s in ${SERVICES}; do dk rm -f "${RUN_ID}-${s}" >/dev/null 2>&1; done
  [ -n "${NET}" ] && dk network rm "${NET}" >/dev/null 2>&1
  return 0
}

start_services() {
  local s img c
  NET=""
  [ -n "${SERVICES}" ] || return 0
  for s in ${SERVICES}; do
    service_image "${s}" >/dev/null || { verdict 125 "no ${s} image in this commit's pipeline-full.yml"; return 1; }
  done
  NET="${RUN_ID}-net"
  record network "${NET}"
  dk network rm "${NET}" >/dev/null 2>&1
  dk network create "${NET}" >/dev/null || { verdict 125 "cannot create network ${NET}"; return 1; }
  for s in ${SERVICES}; do
    img="$(service_image "${s}")"
    c="${RUN_ID}-${s}"
    record container "${c}"
    dk rm -f "${c}" >/dev/null 2>&1
    # The test hosts reach it by the short alias (a run-id name can exceed
    # the 63-character DNS label limit); the alias lives on the run's own
    # network only.
    dk run -d --init --name "${c}" --network "${NET}" --network-alias "${s}" \
      --cpus 1 --memory 1g --memory-swap 1g --cpu-shares 128 "${img}" >/dev/null \
      || { verdict 125 "cannot start ${c}"; return 1; }
  done
  case " ${SERVICES} " in
    *" redis "*)
      for _ in $(seq 1 30); do
        dk exec "${RUN_ID}-redis" redis-cli ping 2>/dev/null | grep -q PONG && return 0
        sleep 1
      done
      verdict 125 "redis never became ready"
      return 1 ;;
  esac
  return 0
}

prepare_home() {
  mkdir -p "${RUN_DIR}/home"
  {
    echo "startup --output_user_root=${RUN_DIR}/bazel-root"
    # A server this run starts outside that root (a vendor script's own
    # --output_base) exits on its own soon after.
    echo "startup --max_idle_secs=300"
  } > "${RUN_DIR}/home/.bazelrc"
}

# prepare_vendor_shim: <id>/vendor-bin/bazel, first on PATH while
# vendoring. It reads only the leading startup options (bazel [startup
# options] <command> ...). It keeps a caller's --output_base (a vendor
# script may read that directory itself) and gives a call without one
# <id>/vendor-ob. It records the output base in <id>/vendor-obs and shuts
# its server down when the call returns, so no vendoring server outlives
# the call that started it.
prepare_vendor_shim() {
  local bin="${RUN_DIR}/vendor-bin" real
  real="$(command -v bazel)" || return 1
  case "${real}" in "${bin}"/*) real="$(cat "${bin}/real-bazel")" ;; esac
  mkdir -p "${bin}" || return 1
  echo "${real}" > "${bin}/real-bazel"
  {
    echo '#!/usr/bin/env bash'
    echo '# Vendoring-only bazel (written by bisect-job.sh).'
    printf 'real=%q\n' "${real}"
    printf 'default_ob=%q\n' "${RUN_DIR}/vendor-ob"
    printf 'record=%q\n' "${RUN_DIR}/vendor-obs"
    cat <<'EOF'
args=("$@")
ob=""
cmd=""
i=0
while [ "${i}" -lt "$#" ]; do
  a="${args[${i}]}"
  case "${a}" in
    --output_base=*) ob="${a#--output_base=}" ;;
    --output_base) i=$((i + 1)); ob="${args[${i}]:-}" ;;
    --*) ;;
    *) cmd="${a}"; break ;;
  esac
  i=$((i + 1))
done
if [ -z "${ob}" ]; then
  ob="${default_ob}"
  mkdir -p "${ob}"
  set -- "--output_base=${ob}" "$@"
fi
case "${ob}" in /*) abs="${ob}" ;; *) abs="${PWD}/${ob}" ;; esac
grep -qxF -- "${abs}" "${record}" 2>/dev/null || echo "${abs}" >> "${record}"
"${real}" "$@"
rc=$?
[ "${cmd}" = shutdown ] || "${real}" "--output_base=${abs}" shutdown >/dev/null 2>&1
exit "${rc}"
EOF
  } > "${bin}/bazel"
  chmod +x "${bin}/bazel"
}

# step_vendor STEPLOG: the commit's vendored tree in SRC/vendor, built once
# per vendor key and kept under BISECT_ROOT/vendor/<key>.
step_vendor() {
  local key store
  # shellcheck disable=SC2086 # the path list is intentionally split
  key="$(git ls-tree -r HEAD -- ${BISECT_VENDOR_KEY_PATHS} | sha256_stdin)"
  store="${BISECT_ROOT}/vendor/${key}"
  remove_tree "${SRC}/vendor"
  if [ -d "${store}" ]; then
    cp -a "${store}" "${SRC}/vendor" || { verdict 125 "cannot copy the vendored tree"; return 1; }
    return 0
  fi
  prepare_home
  prepare_vendor_shim || { verdict 125 "no host bazel for vendoring"; return 1; }
  say "vendoring (key ${key})"
  if ! (cd "${SRC}" && HOME="${RUN_DIR}/home" && PATH="${RUN_DIR}/vendor-bin:${PATH}" \
    && export HOME PATH \
    && lowprio bash -c '. "$1" && bisect_vendor_cmd' bisect-vendor "${SELF_DIR}/bisect-job-table.sh") \
    >>"$1" 2>&1 9>&-; then
    verdict 125 "vendoring failed"
    return 1
  fi
  [ -d "${SRC}/vendor" ] || { verdict 125 "vendoring left no vendor/ tree"; return 1; }
  mkdir -p "${BISECT_ROOT}/vendor"
  remove_tree "${store}.partial"
  cp -a "${SRC}/vendor" "${store}.partial" && mv "${store}.partial" "${store}" \
    || { verdict 125 "cannot store the vendored tree"; return 1; }
}

# step_image STEPLOG: IMAGE = the commit's content-hashed dev image, built
# only when the host does not have it (and then recorded).
step_image() {
  IMAGE="$(cd "${SRC}" && bash tools/ci/ensure-dev-image.sh --name-only "${IMAGE_ARCH}" docker 2>>"$1")" \
    || { verdict 125 "ensure-dev-image.sh --name-only failed"; return 1; }
  [ -n "${IMAGE}" ] || { verdict 125 "no dev image name"; return 1; }
  if dk image inspect "${IMAGE}" >/dev/null 2>&1; then
    return 0
  fi
  say "building ${IMAGE}"
  (cd "${SRC}" && lowprio bash tools/ci/ensure-dev-image.sh "${IMAGE_ARCH}" docker) >>"$1" 2>&1 9>&- \
    || { verdict 125 "dev image build failed"; return 1; }
  grep -qxF "${IMAGE}" "${RUN_DIR}/rebuilt_images" 2>/dev/null || echo "${IMAGE}" >> "${RUN_DIR}/rebuilt_images"
}

# classify STEPLOG RC: the step's verdict from the bazel output.
classify() {
  local failed="$1.targets" rec_test rec_build other_build
  if [ "$2" = 0 ]; then
    verdict 0 "all recorded targets pass"
    return
  fi
  bash "${FAILING_TARGETS}" --mode "${KIND}" "$1" > "${failed}" 2>/dev/null || : > "${failed}"
  rec_test="$(awk -F '\t' 'NR == FNR {rec[$1] = 1; next} ($1 in rec) && ($2 == "test-failed" || $2 == "test-timeout") {print $1}' \
    "${RUN_DIR}/targets" "${failed}" | head -n 1)"
  rec_build="$(awk -F '\t' 'NR == FNR {rec[$1] = 1; next} ($1 in rec) && $2 == "build-failed" {print $1}' \
    "${RUN_DIR}/targets" "${failed}" | head -n 1)"
  other_build="$(awk -F '\t' 'NR == FNR {rec[$1] = 1; next} !($1 in rec) && $2 == "build-failed" {print $1}' \
    "${RUN_DIR}/targets" "${failed}" | head -n 1)"
  if [ "${KIND}" = test ] && [ -n "${rec_test}" ]; then
    verdict 1 "recorded target fails: ${rec_test}"
  elif [ -n "${other_build}" ]; then
    verdict 125 "build error outside the recorded targets: ${other_build}"
  elif [ -n "${rec_build}" ]; then
    verdict 1 "recorded target fails to build: ${rec_build}"
  else
    verdict 125 "failed (exit $2) without a recorded target failure"
  fi
}

run_step() {
  local n="$1" steplog="$2" rc t
  SERVICES="$(job_var "${JOB}" SERVICES)"
  NET=""
  if ! grep -qE "^[a-z]+:${CONFIG}[[:space:]]" "${SRC}/.bazelrc" 2>/dev/null; then
    verdict 125 ".bazelrc has no :${CONFIG} line (commit predates the config)"
    return
  fi
  step_vendor "${steplog}" || return
  step_image "${steplog}" || return

  local name="${RUN_ID}-step-${n}"
  local args=(run --rm --init --name "${name}" --cpus "${CPUS}"
    --memory "${MEM_GB}g" --memory-swap "${MEM_GB}g" --cpu-shares 128)
  set -f
  # shellcheck disable=SC2207 # table values are flag lists
  args+=($(job_var "${JOB}" DOCKER_FLAGS))
  set +f
  start_services || { stop_services; return; }
  [ -n "${NET}" ] && args+=(--network "${NET}")
  args+=(--user "$(id -u):$(id -g)")
  if [ "$(job_var "${JOB}" PASSWD)" = 1 ]; then
    { echo "dev:x:$(id -u):$(id -g)::/workspace:/bin/bash"; cat /etc/passwd; } > "${RUN_DIR}/passwd"
    args+=(-v "${RUN_DIR}/passwd:/etc/passwd:ro")
  fi
  set -f
  for t in $(job_var "${JOB}" ENV); do args+=(-e "${t}"); done
  set +f
  mkdir -p "${RUN_DIR}/disk-cache"
  args+=(-v "${SRC}:/workspace" -v "${RUN_DIR}/disk-cache:/disk-cache" -w /workspace "${IMAGE}")
  args+=(bazel "--output_user_root=$(job_var "${JOB}" OUTPUT_ROOT)" "${KIND}")
  set -f
  # shellcheck disable=SC2207
  args+=($(job_var "${JOB}" BAZEL_FLAGS))
  set +f
  args+=("--config=${CONFIG}")
  [ "${KIND}" = test ] && args+=(--nocache_test_results)
  args+=(--disk_cache=/disk-cache "--jobs=${CPUS}")
  [ "${KIND}" = test ] && [ -n "${RUNS_PER_TEST}" ] && args+=("--runs_per_test=${RUNS_PER_TEST}")
  if [ -n "${REMOTE_CACHE_URL}" ]; then
    args+=("--remote_cache=${REMOTE_CACHE_URL}" --noremote_upload_local_results)
  else
    args+=(--remote_cache=)
  fi
  if [ "${KIND}" = test ]; then
    local s S
    for s in ${SERVICES}; do
      S="$(printf '%s' "${s}" | tr 'a-z' 'A-Z')"
      args+=("--test_env=${S}_HOST=${s}")
    done
    set -f
    for t in $(job_var "${JOB}" SERVICE_TEST_ENV); do args+=("--test_env=${t}"); done
    set +f
  fi
  args+=(--)
  while IFS= read -r t; do [ -n "${t}" ] && args+=("${t}"); done < "${RUN_DIR}/targets"

  record container "${name}"
  say "step ${n}: running ${KIND} of $(grep -c . "${RUN_DIR}/targets") target(s) in ${name}"
  dk "${args[@]}" >>"${steplog}" 2>&1
  rc=$?
  stop_services
  classify "${steplog}" "${rc}"
}

cmd_step() {
  check_root || exit 125
  valid_id "${1:-}" || { err "no run '${1:-}'"; exit 125; }
  load_run "$1"
  local sha n steplog
  sha="$(git -C "${SRC}" rev-parse HEAD 2>/dev/null)" || exit 125
  n=$(( $(cat "${RUN_DIR}/steps" 2>/dev/null || echo 0) + 1 ))
  echo "${n}" > "${RUN_DIR}/steps"
  steplog="${RUN_DIR}/step-${n}.log"
  : > "${steplog}"
  say "step ${n} at ${sha}"
  # In a subshell: a shell error there must read as skip, never as bad.
  (
    run_step "${n}" "${steplog}"
    echo "${STEP_CODE} ${STEP_REASON}" > "${steplog}.verdict"
  )
  if [ -s "${steplog}.verdict" ]; then
    read -r STEP_CODE STEP_REASON < "${steplog}.verdict"
  else
    verdict 125 "internal error in the step"
  fi
  case "${STEP_CODE}" in 0|1|125) ;; *) verdict 125 "internal: unexpected verdict" ;; esac
  echo "${n} ${sha} ${STEP_CODE} ${STEP_REASON}" >> "${RUN_DIR}/steps.log"
  [ "${STEP_CODE}" = 125 ] && echo "${sha}" >> "${RUN_DIR}/skips"
  say "step ${n} at ${sha}: ${STEP_CODE} (${STEP_REASON})"
  tail -n 40 "${steplog}" | sed 's/^/    | /'
  exit "${STEP_CODE}"
}

# --- Dispatch ----------------------------------------------------------------

case "${1:-}" in
  start) shift; cmd_start "$@"; exit $? ;;
  status) shift; cmd_status "$@"; exit $? ;;
  resume) shift; cmd_resume "$@"; exit $? ;;
  _step) shift; cmd_step "$@" ;;
  _worker) shift; cmd_worker "$@" ;;
  -h|--help) usage; exit 0 ;;
  *) usage; exit 2 ;;
esac
