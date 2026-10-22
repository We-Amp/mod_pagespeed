#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Workflow-shape gate (BLOCKING, part of the "Public-tree hygiene gate" CI
# job). Keeps the caller/reusable-workflow split described in
# docs/ci/pipelines.md intact: the build fleet is reachable only through this
# repository's reusable workflows pinned to master (or ci-dev), so a pull
# request can never steer its own code onto a fleet runner.
#
# Usage: tools/ci/check-workflow-shape.sh [tree-root]   (default: repo root)
#
# Rules, applied to .github/workflows/*.yml, *.yaml and *.yml.example:
#   (a) no workflow may use the pull_request_target trigger;
#   (b) a workflow whose on: block has any trigger other than workflow_call
#       is a CALLER: every job must either be a `uses:` of
#       We-Amp/mod_pagespeed/.github/workflows/<file>.yml@master or
#       @ci-dev (and carry no runs-on/steps), or be an aggregate gate job
#       pinned to a hosted label (no fleet label, no routing expression).
#       Files listed in tools/ci/workflow-shape-allowlist.txt are exempt
#       from this rule only;
#   (c) no local-path `uses: ./.github/workflows/...`: a pull request could
#       edit the callee in the same commit, defeating the pinned-ref
#       boundary;
#   (d) in a workflow_call workflow every runs-on is either a hosted label
#       (ubuntu-latest, ubuntu-24.04, ubuntu-24.04-arm, windows-2022,
#       macos-15, macos-latest) or exactly the fork/Dependabot routing
#       expression with a fleet class (fleet-heavy, fleet-light,
#       fleet-windows, fleet-iis, fleet-hyperv), or the switch routing
#       expression, which also sends the job to the hosted side while the
#       FLEET_ENABLED variable is 'false'. A job routed by the plain
#       expression must carry one of the four fleet guards of
#       docs/ci/pipelines.md (the FLEET_ENABLED kill switch in its general,
#       fleet-only or dispatch-only form, or the fleet-iis form that adds
#       FLEET_IIS_ENABLED; a job routed to fleet-iis or fleet-hyperv must carry the last) verbatim as its if:, the general form optionally
#       narrowed by `&& needs.changes.outputs.<name> == 'true'`, or
#       prefixed by `!cancelled() && needs.<job>.result == 'success' && `.
#       The fleet-only and fleet-iis guards also have a switch form that
#       tests `!= 'false'` (unset means on). A job routed by the switch
#       expression carries no guard: it always runs, hosted or on the fleet;
#       its if: is empty or just `needs.changes.outputs.<name> == 'true'`.
#       The release lane keeps the plain expression and the original guards;
#   (f) in a workflow_call workflow: a top-level `permissions:` block is
#       required and must grant no write access; write access to GitHub (a
#       `: write` permission, plain or quoted, or write-all) is allowed only
#       at job level on a job that runs on a hosted label; a fleet-routed job
#       is read-only. The permissions parser is indentation-bound (jobs at two
#       spaces, job keys at four), like the rest of this gate;
#   (g) in every workflow: a job-level `environment:` and any
#       `secrets.<NAME>` reference (the job's own token, secrets.GITHUB_TOKEN,
#       excepted) are allowed only on a job that runs on a hosted label; a
#       fleet-routed job never names an environment or reads a secret;
#       Applied to every workflow. A job that calls another workflow may pass
#       secrets only to a callee listed in SECRET_CALLEES (the module release
#       lane; the fleet-hyperv gates get none). `secrets: inherit`, `secrets[` and
#       `toJSON(secrets`, and a secret in an expression outside the jobs (a
#       workflow-level env: block), are refused;
#   (e) topology token scan of .github/ and tools/ci/: no LAN (RFC1918) IP
#       literals, no ".home" names, no ":9090", no "self-hosted", and none
#       of the retired runner-label words as standalone tokens (the bare
#       word "pool" only under .github/, see the scan below).
#
# Rule (e) exclusions (same pattern as the public-tree hygiene gate, which
# excludes its own rules file):
#   .github/hygiene-rules.txt, .github/hygiene-hashed-tokens.txt
#   tools/ci/check-workflow-shape.sh   (this file: it carries the patterns)
#   tools/ci/testdata/workflow-shape/** (fixtures violate the rules on purpose)
#
# The gate is deliberately dependency-free (bash + grep + awk, bash 3.2
# compatible) and line-oriented: runs-on values must fit on one line.
#
# Fixtures and a test live in tools/ci/testdata/workflow-shape/ and
# tools/ci/test_check_workflow_shape.sh.

set -euo pipefail

ROOT="${1:-}"
if [ -z "${ROOT}" ]; then
  ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fi
cd "${ROOT}"

ALLOWLIST="tools/ci/workflow-shape-allowlist.txt"
WF_DIR=".github/workflows"

HOSTED_LABELS="ubuntu-latest ubuntu-24.04 ubuntu-24.04-arm windows-2022 macos-15 macos-latest"
FLEET_CLASSES="fleet-heavy fleet-light fleet-windows fleet-iis fleet-hyperv"
TAB="$(printf '\t')"
# The fleet guard of docs/ci/pipelines.md, required verbatim as the if: of
# every job whose runs-on is the routing expression.
FLEET_GUARD_BODY="vars.FLEET_ENABLED == 'true' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))"
FLEET_GUARD="\${{ ${FLEET_GUARD_BODY} }}"
# Fleet-only jobs (privileged containers, a mounted container socket, a real
# kernel): the same switch, but a fork or Dependabot pull request skips them
# instead of falling back to a hosted runner.
FLEET_ONLY_GUARD="\${{ vars.FLEET_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}"
# Jobs of the fleet-iis class (no hosted twin): the fleet-only guard plus the
# FLEET_IIS_ENABLED switch, so they are skipped while no runner of the class
# is registered.
FLEET_IIS_GUARD="\${{ vars.FLEET_ENABLED == 'true' && vars.FLEET_IIS_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}"
# Dispatch-only fleet jobs (manual lanes): a workflow_dispatch is never a
# fork pull request.
FLEET_DISPATCH_GUARD="\${{ github.event_name == 'workflow_dispatch' && vars.FLEET_ENABLED == 'true' }}"

# The same guards for the tiers that move to the hosted side while the
# FLEET_ENABLED variable is 'false' (unset means on).
FLEET_ONLY_SWITCH_GUARD="\${{ vars.FLEET_ENABLED != 'false' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}"
FLEET_IIS_SWITCH_GUARD="\${{ vars.FLEET_ENABLED != 'false' && vars.FLEET_IIS_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) }}"

status=0
err() { echo "::error::$*" >&2; status=1; }

# --- allowlist (rule (b) exemption list) ------------------------------------
is_allowlisted() {
  local base="$1" line
  [ -f "${ALLOWLIST}" ] || return 1
  while IFS= read -r line || [ -n "${line}" ]; do
    line="${line%%#*}"
    line="$(printf '%s' "${line}" | tr -d '[:space:]')"
    [ -n "${line}" ] || continue
    [ "${line}" = "${base}" ] && return 0
  done < "${ALLOWLIST}"
  return 1
}

# --- parsers ------------------------------------------------------------------
# on: block trigger keys (first-level keys under a top-level on:). Prints
# INLINE and nothing else for the unsupported `on: [push]` one-line form.
on_triggers() {
  awk '
    /^[[:space:]]*#/ { next }
    /^(on|"on"|'"'"'on'"'"'):[[:space:]]*$/ { inon=1; next }
    /^(on|"on"|'"'"'on'"'"'):[[:space:]]*[^[:space:]]/ { print "INLINE"; inon=0 }
    inon && /^[^[:space:]#]/ { inon=0 }
    inon && /^  [A-Za-z_-]+:/ {
      key=$1; sub(/:$/, "", key); print key
    }
  ' "$1" | sort -u
}

# jobs: block, flattened to tab-separated records:
#   JOB <tab> <job-id> <tab> <line>
#   KEY <tab> <job-id> <tab> <key> <tab> <line> <tab> <value>
job_records() {
  awk -v tab="${TAB}" '
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs=1; next }
    injobs && /^[^[:space:]#]/ { injobs=0 }
    injobs && /^  [^[:space:]]/ {
      job=$1; sub(/:$/, "", job)
      printf "JOB%s%s%s%d\n", tab, job, tab, NR
      next
    }
    injobs && /^    [A-Za-z_-]+:/ {
      line=$0
      key=$1; sub(/:$/, "", key)
      sub(/^[[:space:]]*[^:]+:[[:space:]]*/, "", line)
      printf "KEY%s%s%s%s%s%d%s%s\n", tab, job, tab, key, tab, NR, tab, line
    }
  ' "$1"
}

# Jobs (and the workflow level, reported as job "-") that hold a `: write`
# permission, one per line: "<job> <tab> <line>". Line-oriented: a permissions
# block is the 4-space (job) or 0-space (workflow) `permissions:` key and the
# more deeply indented lines under it; `permissions: write-all` counts, and so
# does a quoted value (double or single quotes around write) or the one-line flow
# form (`permissions: { issues: write }`). Indentation-bound on purpose: jobs
# sit at two spaces and job keys at four, the style of this repository.
write_perm_records() {
  awk -v tab="${TAB}" '
    function haswrite(line) {
      sub(/[[:space:]]+#.*$/, "", line)
      return (line ~ /write-all/ || line ~ /:[[:space:]]*[\047"]?write[\047"]?[[:space:]]*([,}]|$)/)
    }
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs=1; inperm=0; job="-"; next }
    /^permissions:/ {
      inperm=1; indent=0; job="-"
      if (haswrite($0) || $0 ~ /^permissions:[[:space:]]*[\047"]?write[\047"]?[[:space:]]*(#.*)?$/) { printf "%s%s%d\n", job, tab, NR }
      next
    }
    injobs && /^[^[:space:]#]/ { injobs=0; inperm=0 }
    injobs && /^  [^[:space:]]/ { job=$1; sub(/:$/, "", job); inperm=0; next }
    injobs && /^    permissions:/ {
      inperm=1; indent=4
      if (haswrite($0) || $0 ~ /^    permissions:[[:space:]]*[\047"]?write[\047"]?[[:space:]]*(#.*)?$/) { printf "%s%s%d\n", job, tab, NR }
      next
    }
    inperm {
      match($0, /^[[:space:]]*/); cur=RLENGTH
      if (cur <= indent) { inperm=0; next }
      if (haswrite($0)) { printf "%s%s%d\n", job, tab, NR }
    }
  ' "$1"
}

# Jobs that name an environment or read a secret, one per line:
# "<job> <tab> <line> <tab> environment|secret". Line-oriented like the other
# parsers: a job is a 2-space key under jobs:, its `environment:` a 4-space key.
# secrets.GITHUB_TOKEN (the job's own token) is not a secret in this sense.
secret_env_records() {
  awk -v tab="${TAB}" '
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs=1; next }
    injobs && /^[^[:space:]#]/ { injobs=0 }
    injobs && /^  [^[:space:]]/ { job=$1; sub(/:$/, "", job); next }
    injobs && /^    environment:/ { printf "%s%s%d%senvironment\n", job, tab, NR, tab }
    injobs {
      line=$0
      while (match(line, /secrets\.[A-Za-z_][A-Za-z0-9_]*/)) {
        name=substr(line, RSTART + 8, RLENGTH - 8)
        if (name != "GITHUB_TOKEN") { printf "%s%s%d%ssecret\n", job, tab, NR, tab }
        line=substr(line, RSTART + RLENGTH)
      }
    }
  ' "$1"
}

# Callees of a call job that may be passed secrets. Each is a reusable workflow
# of this repository whose every job that reads a secret runs on a hosted label
# (rule g checks that file too). The two fleet-hyperv gates get none.
SECRET_CALLEES="pipeline-release-module.yml"
is_secret_callee() { # a `uses:` value
  local f c
  f="${1##*/}"; f="${f%%@*}"
  case "${1}" in We-Amp/mod_pagespeed/.github/workflows/*) ;; *) return 1 ;; esac
  for c in ${SECRET_CALLEES}; do [ "${f}" = "${c}" ] && return 0; done
  return 1
}

# `secrets: inherit` (any spacing) on a job: "<job> <tab> <line>".
secret_inherit_records() {
  awk -v tab="${TAB}" '
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs=1; next }
    injobs && /^[^[:space:]#]/ { injobs=0 }
    injobs && /^  [^[:space:]]/ { job=$1; sub(/:$/, "", job); next }
    /^[[:space:]]*secrets:[[:space:]]*[\047"]?inherit/ { printf "%s%s%d\n", job, tab, NR }
  ' "$1"
}

# Lines that index (`secrets[`) or serialise (`toJSON(secrets`) the secrets context.
secret_index_lines() {
  grep -nE 'secrets[[:space:]]*\[|(toJSON|toJson|tojson)[[:space:]]*\([[:space:]]*secrets' "$1" 2>/dev/null \
    | grep -vE '^[0-9]+:[[:space:]]*#' | cut -d: -f1 || true
}

# Lines outside the jobs: block that put a secret in an expression (an env:
# block at workflow level, a concurrency group, and so on).
toplevel_secret_lines() {
  awk '
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs=1; next }
    injobs && /^[^[:space:]#]/ { injobs=0 }
    !injobs && /\$\{\{[^}]*secrets/ { print NR }
  ' "$1"
}

# True when the file has a top-level (zero-indent) `permissions:` key.
has_toplevel_permissions() {
  grep -qE '^permissions:' "$1"
}

is_hosted_label() {
  local l
  for l in ${HOSTED_LABELS}; do [ "$1" = "${l}" ] && return 0; done
  return 1
}

# Exactly the routing expression of docs/ci/pipelines.md: the fork/Dependabot
# choice with a hosted label on one side and a fleet class on the other.
is_fleet_route() {
  local v="$1" h f want
  for h in ${HOSTED_LABELS}; do
    for f in ${FLEET_CLASSES}; do
      want="\${{ (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) && '${h}' || '${f}' }}"
      [ "${v}" = "${want}" ] && return 0
    done
  done
  return 1
}

# The switch routing expression: the hosted side is chosen when the kill
# switch is off as well as for a fork or Dependabot pull request, so the job
# needs no guard. Not for the classes that have no hosted twin.
is_fleet_switch_route() {
  local v="$1" h f want
  for h in ${HOSTED_LABELS}; do
    for f in fleet-heavy fleet-light fleet-windows; do
      want="\${{ (vars.FLEET_ENABLED == 'false' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]'))) && '${h}' || '${f}' }}"
      [ "${v}" = "${want}" ] && return 0
    done
  done
  return 1
}

# The only if: a switch-routed job may carry: none, or the path filter alone.
is_switch_if() {
  local v="$1" name
  [ -z "${v}" ] && return 0
  case "${v}" in
    "\${{ needs.changes.outputs."*" == 'true' }}")
      name="${v#\$\{\{ needs.changes.outputs.}"; name="${name% == \'true\' \}\}}"
      printf '%s' "${name}" | grep -qE '^[a-z][a-z_]*$' && return 0
      ;;
  esac
  return 1
}

# The fleet guard exactly, or the guard narrowed by a path filter: the guard
# in parentheses, then `&& needs.changes.outputs.<name> == 'true'`, where the
# `changes` job is the hosted job that diffs the pull request's files.
is_fleet_guard() {
  local v="$1" prefix="\${{ (${FLEET_GUARD_BODY}) && needs.changes.outputs." name
  [ "${v}" = "${FLEET_GUARD}" ] && return 0
  case "${v}" in
    "${prefix}"*" == 'true' }}")
      name="$(printf '%s' "${v#"${prefix}"}" | sed "s/ == 'true' }}\$//")"
      printf '%s' "${name}" | grep -qE '^[a-z][a-z_]*$' && return 0
      ;;
  esac
  # A job that depends on a pair of build runner and hosted twin jobs: GitHub
  # skips a job whose ancestors include a skipped job unless its if: has a
  # status check, so the guard may be prefixed by `!cancelled()` and
  # `needs.<job>.result == 'success'` terms (at least one) that restore the
  # implicit success check:
  #   ${{ !cancelled() && needs.a.result == 'success' && (<guard>) }}
  local pre="\${{ !cancelled() && " post="(${FLEET_GUARD_BODY}) }}" mid
  case "${v}" in
    "${pre}"*"${post}")
      mid="${v#"${pre}"}"; mid="${mid%"${post}"}"
      printf '%s' "${mid}" | grep -qE "^(needs\.[a-z0-9-]+\.result == 'success' && )+\$" && return 0
      ;;
  esac
  return 1
}

# A fleet-iis guard (either form) in parentheses, narrowed by a path filter:
# `&& needs.changes.outputs.<name> == 'true'`.
is_iis_narrowed() {
  local v="$1" g name
  for g in "${FLEET_IIS_GUARD}" "${FLEET_IIS_SWITCH_GUARD}"; do
    g="${g#\$\{\{ }"; g="${g% \}\}}"
    case "${v}" in
      "\${{ (${g}) && needs.changes.outputs."*" == 'true' }}")
        name="${v#"\${{ (${g}) && needs.changes.outputs."}"; name="${name% == \'true\' \}\}}"
        printf '%s' "${name}" | grep -qE '^[a-z][a-z_]*$' && return 0
        ;;
    esac
  done
  return 1
}

# A routing expression whose fleet class is fleet-iis or fleet-hyperv (both
# have no hosted twin and carry the same guard).
is_fleet_iis_route() {
  case "$1" in *"|| 'fleet-iis' }}"|*"|| 'fleet-hyperv' }}") return 0 ;; esac
  return 1
}

# --- workflow scan ------------------------------------------------------------
shopt -s nullglob
WORKFLOWS=( "${WF_DIR}"/*.yml "${WF_DIR}"/*.yaml "${WF_DIR}"/*.yml.example )
if [ ${#WORKFLOWS[@]} -eq 0 ]; then
  err "${WF_DIR}: no workflow files found"
fi

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  base="$(basename "${wf}")"

  # (a) no pull_request_target, not even in a comment.
  if grep -n 'pull_request_target' "${wf}" >/dev/null 2>&1; then
    err "${wf}: pull_request_target is forbidden (rule a): $(grep -n 'pull_request_target' "${wf}" | head -1)"
  fi

  # (c) no local-path workflow reuse.
  if grep -nE 'uses:[[:space:]]*\./\.github/workflows/' "${wf}" >/dev/null 2>&1; then
    err "${wf}: local-path reusable workflow reference is forbidden (rule c): $(grep -nE 'uses:[[:space:]]*\./\.github/workflows/' "${wf}" | head -1)"
  fi

  triggers="$(on_triggers "${wf}")"
  if printf '%s\n' "${triggers}" | grep -qx 'INLINE'; then
    err "${wf}: inline on: form is not supported by this gate; use block style"
    continue
  fi
  if [ -z "${triggers}" ]; then
    err "${wf}: could not parse the on: block"
    continue
  fi

  records="$(job_records "${wf}")"

  if printf '%s\n' "${triggers}" | grep -qx 'workflow_call'; then
    # Reusable workflow. (d) every runs-on is a hosted label or exactly the
    # routing expression.
    while IFS=$'\t' read -r kind job key lineno val; do
      { [ "${kind}" = "KEY" ] && [ "${key}" = "runs-on" ]; } || continue
      if [ -z "${val}" ]; then
        err "${wf}:${lineno}: multi-line runs-on is not supported by this gate (job '${job}')"
      elif ! is_hosted_label "${val}" && ! is_fleet_route "${val}" && ! is_fleet_switch_route "${val}"; then
        err "${wf}:${lineno}: runs-on '${val}' is neither a hosted label nor the routing expression with a fleet class (rule d)"
      elif is_fleet_switch_route "${val}"; then
        if_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="if" {print $5}')"
        if ! is_switch_if "${if_val}"; then
          err "${wf}:${lineno}: switch-routed job '${job}' may carry no if: or only a needs.changes.outputs.<name> == 'true' path filter (rule d)"
        fi
      elif is_fleet_route "${val}"; then
        # A fleet job carries the fleet guard verbatim as its if: (the
        # FLEET_ENABLED kill switch, docs/ci/pipelines.md).
        if_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="if" {print $5}')"
        if is_fleet_iis_route "${val}"; then
          route_class="${val%\' \}\}}"; route_class="${route_class##*\'}"
          if [ "${if_val}" != "${FLEET_IIS_GUARD}" ] && [ "${if_val}" != "${FLEET_IIS_SWITCH_GUARD}" ] && ! is_iis_narrowed "${if_val}"; then
            err "${wf}:${lineno}: ${route_class} job '${job}' must carry the fleet-iis guard (FLEET_IIS_ENABLED) verbatim as its if: (rule d)"
          fi
        elif ! is_fleet_guard "${if_val}" \
           && [ "${if_val}" != "${FLEET_ONLY_GUARD}" ] \
           && [ "${if_val}" != "${FLEET_ONLY_SWITCH_GUARD}" ] \
           && [ "${if_val}" != "${FLEET_DISPATCH_GUARD}" ]; then
          err "${wf}:${lineno}: fleet job '${job}' does not carry a fleet guard verbatim as its if: (rule d)"
        fi
      fi
    done <<EOF
${records}
EOF

    # (f) a reusable workflow declares a top-level permissions block (a
    # called job otherwise inherits the caller's whole token), and write
    # permissions sit only on hosted jobs.
    if ! has_toplevel_permissions "${wf}"; then
      err "${wf}: reusable workflow has no top-level permissions: block; declare a read-only one (contents: read) so a job cannot inherit the caller's token (rule f)"
    fi
    while IFS=$'\t' read -r wjob wline; do
      [ -n "${wjob}" ] || continue
      if [ "${wjob}" = "-" ]; then
        err "${wf}:${wline}: workflow-level write permission in a reusable workflow; keep it read-only and raise write on a hosted job (rule f)"
        continue
      fi
      wruns="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${wjob}" '$1=="KEY" && $2==j && $3=="runs-on" {print $5}')"
      if ! is_hosted_label "${wruns}"; then
        err "${wf}:${wline}: job '${wjob}' holds a write permission but does not run on a hosted label (rule f)"
      fi
    done <<EOF
$(write_perm_records "${wf}")
EOF
  fi

  # (g) secrets and environments, every workflow. An environment and a secret
  # belong to hosted jobs only; a call of another workflow may pass secrets
  # only to a callee on SECRET_CALLEES; no secrets: inherit; no secrets
  # indexing or serialising; no secret in a workflow-level expression.
  while IFS=$'\t' read -r sjob sline skind; do
    [ -n "${sjob}" ] || continue
    sruns="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${sjob}" '$1=="KEY" && $2==j && $3=="runs-on" {print $5}')"
    if [ -z "${sruns}" ]; then
      # A job without runs-on is a call of another workflow.
      suses="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${sjob}" '$1=="KEY" && $2==j && $3=="uses" {print $5}')"
      if ! is_secret_callee "${suses}"; then
        err "${wf}:${sline}: call job '${sjob}' passes a ${skind} to '${suses}', which is not on the secret-callee list (rule g)"
      fi
    elif ! is_hosted_label "${sruns}"; then
      err "${wf}:${sline}: job '${sjob}' names an ${skind} but does not run on a hosted label (rule g)"
    fi
  done <<EOF
$(secret_env_records "${wf}")
EOF
  while IFS=$'\t' read -r sjob sline; do
    [ -n "${sjob}" ] || continue
    err "${wf}:${sline}: 'secrets: inherit' on '${sjob}': pass secrets one by one (rule g)"
  done <<EOF
$(secret_inherit_records "${wf}")
EOF
  while IFS=$'\t' read -r sline; do
    [ -n "${sline}" ] || continue
    err "${wf}:${sline}: secrets indexed or serialised; name each secret as secrets.NAME (rule g)"
  done <<EOF
$(secret_index_lines "${wf}")
EOF
  while IFS=$'\t' read -r sline; do
    [ -n "${sline}" ] || continue
    err "${wf}:${sline}: a secret in a workflow-level expression (outside jobs); read secrets in a hosted job (rule g)"
  done <<EOF
$(toplevel_secret_lines "${wf}")
EOF

  if printf '%s\n' "${triggers}" | grep -qvx 'workflow_call'; then
    # Caller workflow. (b) unless allowlisted.
    if ! is_allowlisted "${base}"; then
      while IFS= read -r job; do
        [ -n "${job}" ] || continue
        uses_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="uses" {print $5}')"
        runson_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="runs-on" {print $5}')"
        has_steps="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="steps" {print "1"}')"
        jline="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="JOB" && $2==j {print $3}')"
        if [ -n "${uses_val}" ]; then
          if ! printf '%s' "${uses_val}" | grep -qE '^We-Amp/mod_pagespeed/\.github/workflows/[A-Za-z0-9._-]+\.yml@(master|ci-dev)$'; then
            err "${wf}:${jline}: caller job '${job}' uses '${uses_val}', not this repository's reusable workflow @master/@ci-dev (rule b)"
          fi
          if [ -n "${runson_val}" ] || [ -n "${has_steps}" ]; then
            err "${wf}:${jline}: caller job '${job}' mixes uses: with runs-on/steps (rule b)"
          fi
        else
          # Gate-job carve-out: an aggregate job (e.g. the CI gate reading
          # needs.*.result) may run inline, but only on a hosted label.
          if [ -z "${runson_val}" ]; then
            err "${wf}:${jline}: caller job '${job}' has neither uses: nor a hosted runs-on (rule b)"
          elif ! is_hosted_label "${runson_val}"; then
            err "${wf}:${jline}: caller job '${job}' runs-on '${runson_val}': callers never address the fleet, only hosted labels are allowed (rule b)"
          fi
        fi
      done <<EOF
$(printf '%s\n' "${records}" | awk -F"${TAB}" '$1=="JOB" {print $2}')
EOF
    fi
  fi
done

# --- queue-guard-reads-default-branch ----------------------------------------
# The merge-queue guard (docs/ci/pipelines.md, "Merge-queue runs") must read
# its classifier from the default branch, never from the queued tree. In
# pipeline-pr.yml (and any workflow that names the classifier):
#   - the `changes` job holds exactly one step named QG_STEP;
#   - that step has only the keys name, if, env and run, with the exact if:
#     `github.event_name == 'merge_group'`, and its run: block is exactly one
#     of the two allowed bodies below (no other command, comment or wrapper);
#   - no other non-comment line of the file names tools/ci/queue_fork_guard.sh.
QG_STEP="Refuse fork and Dependabot PRs in the merge queue"
QG_IF="        if: github.event_name == 'merge_group'"
QG_PAT='(^|[^_A-Za-z0-9])queue_fork_guard\.sh'
# shellcheck disable=SC2016  # the text to match carries literal ${...}
QG_SET='set -euo pipefail'
# shellcheck disable=SC2016
QG_REV='git rev-parse -q --verify "origin/${DEFAULT_BRANCH}^{commit}" >/dev/null ||'
# shellcheck disable=SC2016
QG_FETCH='  git fetch --no-tags --depth=1 origin "+refs/heads/${DEFAULT_BRANCH}:refs/remotes/origin/${DEFAULT_BRANCH}"'
# shellcheck disable=SC2016
QG_SHOW='git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash'
QG_BODY_FULL="$(printf '%s\n%s\n%s\n%s' "${QG_SET}" "${QG_REV}" "${QG_FETCH}" "${QG_SHOW}")"
QG_BODY_MIN="$(printf '%s\n%s' "${QG_SET}" "${QG_SHOW}")"
qg_err() { err "${wf}: $1 (queue-guard-reads-default-branch)"; }

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  mentions="$(grep -E "${QG_PAT}" "${wf}" | grep -vcE '^[[:space:]]*#' || true)"
  if [ "$(basename "${wf}")" != "pipeline-pr.yml" ] && [ "${mentions}" = "0" ]; then
    continue
  fi
  job="$(awk '/^  [A-Za-z0-9_-]+:[ ]*$/ { inj = ($1 == "changes:") } inj { print }' "${wf}")"
  n_steps="$(printf '%s\n' "${job}" | grep -cxF "      - name: ${QG_STEP}" || true)"
  if [ "${n_steps}" != "1" ]; then
    qg_err "the changes job must hold exactly one step named '${QG_STEP}' (found ${n_steps})"
    continue
  fi
  step="$(printf '%s\n' "${job}" | awk -v n="      - name: ${QG_STEP}" '
    /^      - / { instep = ($0 == n) }
    instep { print }')"
  keys="$(printf '%s\n' "${step}" | grep -E '^        [A-Za-z_-]+:' | sed -E 's/^ +([A-Za-z_-]+):.*/\1/' | sort | tr '\n' ' ')"
  if [ "${keys}" != "env if run " ]; then
    qg_err "the guard step must have exactly the keys name, if, env and run (found: name ${keys})"
  fi
  if [ "$(printf '%s\n' "${step}" | grep -cxF "${QG_IF}")" != "1" ]; then
    qg_err "the guard step must carry exactly: ${QG_IF# }"
  fi
  body="$(printf '%s\n' "${step}" | awk '
    $0 == "        run: |" { inrun = 1; next }
    inrun { if ($0 ~ /^          /) print; else if ($0 !~ /^[[:space:]]*$/) inrun = 0 }' | sed -E 's/^          //')"
  if [ "${body}" != "${QG_BODY_FULL}" ] && [ "${body}" != "${QG_BODY_MIN}" ]; then
    qg_err "the guard step's run: block must be only: ${QG_SET}, the optional default-branch fetch, and ${QG_SHOW}"
  fi
  if [ "${mentions}" != "1" ]; then
    qg_err "queue_fork_guard.sh must be named exactly once, in the guard step (found ${mentions} reads); never read the queued tree's copy"
  fi
done

# --- (e) topology token scan --------------------------------------------------
SCAN_DIRS=()
[ -d .github ] && SCAN_DIRS+=(.github)
[ -d tools/ci ] && SCAN_DIRS+=(tools/ci)

scan_hits() { # pattern [dir...]  (default: every scan dir)
  local pat="$1" dirs
  shift
  if [ $# -gt 0 ]; then dirs=("$@"); else dirs=(${SCAN_DIRS[@]+"${SCAN_DIRS[@]}"}); fi
  [ ${#dirs[@]} -gt 0 ] || return 0
  grep -rInE -I "${pat}" "${dirs[@]}" 2>/dev/null \
    | grep -v '^\.github/hygiene-rules\.txt:' \
    | grep -v '^\.github/hygiene-hashed-tokens\.txt:' \
    | grep -v '^tools/ci/check-workflow-shape\.sh:' \
    | grep -v '^tools/ci/testdata/workflow-shape/' \
    || true
}

check_token() { # description, pattern [dir...]
  local hits desc="$1" pat="$2"
  shift 2
  hits="$(scan_hits "${pat}" "$@")"
  if [ -n "${hits}" ]; then
    err "topology token scan: ${desc} (rule e):
${hits}"
  fi
}

check_token "LAN (RFC1918) IP literal" '\b(10\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}|172\.(1[6-9]|2[0-9]|3[01])\.[0-9]{1,3}\.[0-9]{1,3}|192\.168\.[0-9]{1,3}\.[0-9]{1,3})\b'
check_token "'.home' host suffix" '\.home([^A-Za-z0-9]|$)'
check_token "remote-cache port ':9090'" ':9090'
check_token "'self-hosted'" '\bself-hosted\b'
# The compound label is split across two quoted strings so that this file
# itself stays clean under the public-tree hygiene gate's label rule.
# The full word list applies to .github/, where runner labels live. Under
# tools/ci the bare word "pool" is dropped: IIS application pools are a
# product concept the Windows scripts name all the time, and a label can only
# take effect through a workflow file.
if [ -d .github ]; then
  check_token "retired runner label word" '\b(dedicated|pool|linux-x64-''pool|real-linux|macos-native|mac-linux|auxiliary|highmem|farm-controller|tr-host)\b' .github
fi
if [ -d tools/ci ]; then
  check_token "retired runner label word" '\b(dedicated|linux-x64-''pool|real-linux|macos-native|mac-linux|auxiliary|highmem|farm-controller|tr-host)\b' tools/ci
fi

if [ "${status}" -ne 0 ]; then
  echo "::error::workflow-shape gate FAILED -- see the matched paths above" >&2
  exit 1
fi
echo "Workflow-shape gate passed."
