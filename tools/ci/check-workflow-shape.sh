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
#       The release lane keeps the plain expression and the original guards.
#       In pipeline-pr.yml only, the queue routing expression replaces both:
#       the same choice plus `|| needs.changes.outputs.route != 'fleet'`
#       (a missing or empty route means hosted), and every build runner job
#       there lists `changes` in its `needs:` (the queue-route check); the
#       fleet-iis job also carries `needs.changes.outputs.route == 'fleet'`
#       in its guard;
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
#   (yaml-alias) in every workflow's jobs: section, no YAML anchor or alias
#       on a job or on a job-level key other than steps: and env:, and no
#       `<<:` merge key: the gate reads the text and cannot resolve them;
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
# The queue variants (pipeline-pr.yml): a merge group that holds a fork or
# Dependabot pull request is routed to hosted runners, so the fleet-iis job,
# which has no hosted twin, also requires the fleet route.
FLEET_IIS_QUEUE_GUARD="\${{ vars.FLEET_ENABLED != 'false' && vars.FLEET_IIS_ENABLED == 'true' && !(github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) && needs.changes.outputs.route == 'fleet' }}"
# 1 while a pipeline-pr.yml is being checked (queue routing forms only).
PR_WF=0

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
      job=$0; sub(/^  /, "", job); sub(/[ \t]*:.*$/, "", job); gsub(/["\047]/, "", job)
      printf "JOB%s%s%s%d\n", tab, job, tab, NR
      next
    }
    injobs && /^    ["\047]?[A-Za-z_-]+["\047]?[ \t]*:/ {
      line=$0
      key=$0; sub(/^    /, "", key); sub(/[ \t]*:.*$/, "", key); gsub(/["\047]/, "", key)
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
  local v="$1" g name guards
  if [ "${PR_WF}" = 1 ]; then guards=( "${FLEET_IIS_QUEUE_GUARD}" ); else guards=( "${FLEET_IIS_GUARD}" "${FLEET_IIS_SWITCH_GUARD}" ); fi
  for g in "${guards[@]}"; do
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

# The queue routing expression of pipeline-pr.yml: the switch expression plus
# `|| needs.changes.outputs.route != 'fleet'` inside the hosted condition, so a
# missing or empty route means hosted. Not for the classes with no hosted twin.
is_fleet_queue_route() {
  local v="$1" h f want
  for h in ${HOSTED_LABELS}; do
    for f in fleet-heavy fleet-light fleet-windows; do
      want="\${{ (vars.FLEET_ENABLED == 'false' || (github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) || needs.changes.outputs.route != 'fleet') && '${h}' || '${f}' }}"
      [ "${v}" = "${want}" ] && return 0
    done
  done
  return 1
}

# The queue form of the fork routing expression, for the fleet-iis and
# fleet-hyperv classes.
is_fleet_iis_queue_route() {
  local v="$1" h f want
  for h in ${HOSTED_LABELS}; do
    for f in fleet-iis fleet-hyperv; do
      want="\${{ ((github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]')) || needs.changes.outputs.route != 'fleet') && '${h}' || '${f}' }}"
      [ "${v}" = "${want}" ] && return 0
    done
  done
  return 1
}

# YAML anchors, aliases and merge keys the gate cannot resolve, one line number
# per hit, in the jobs: section: on a job (its key or value), on any job-level
# key other than steps: and env: (their value or its continuation lines), and
# every `<<:` merge key. Anchored step lists and env maps stay allowed.
yaml_alias_lines() {
  awk '
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs = 1; next }
    injobs && /^[^[:space:]#]/ { injobs = 0 }
    !injobs { next }
    /^[[:space:]-]*["\047]?<<["\047]?[[:space:]]*:/ { print NR; next }
    /^  [^[:space:]]/ { key = ""; if ($0 ~ /^  [&*]/ || $0 ~ /:[[:space:]]*[&*]/) print NR; next }
    /^    [^[:space:]]/ {
      key = $0; sub(/^    /, "", key); sub(/[ \t]*:.*$/, "", key); gsub(/["\047]/, "", key)
      if (key ~ /^[&*-]/) { print NR; next }
      v = $0; sub(/^    [^:]*:/, "", v)
      if (key != "steps" && key != "env" && v ~ /(^|[[:space:],\[{])[&*][A-Za-z0-9_-]/) print NR
      next
    }
    /^     / && key != "" && key != "steps" && key != "env" && $0 ~ /^[[:space:]]*(- )?[&*][A-Za-z0-9_-]|:[[:space:]]*[&*][A-Za-z0-9_-]/ { print NR }
  ' "$1"
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
  if grep -nE 'uses["'\'']?[[:space:]]*:[[:space:]]*["'\'']?\./\.github/workflows/' "${wf}" >/dev/null 2>&1; then
    err "${wf}: local-path reusable workflow reference is forbidden (rule c): $(grep -nE 'uses["'\'']?[[:space:]]*:[[:space:]]*["'\'']?\./\.github/workflows/' "${wf}" | head -1)"
  fi

  # (yaml-alias) anchors, aliases and merge keys the gate cannot resolve.
  while IFS= read -r al; do
    [ -n "${al}" ] || continue
    err "${wf}:${al}: YAML anchor, alias or merge key on a job or on a job key other than steps/env; the gate cannot resolve it (yaml-alias)"
  done <<EOT
$(yaml_alias_lines "${wf}")
EOT

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

  PR_WF=0
  [ "${base}" = "pipeline-pr.yml" ] && PR_WF=1
  if printf '%s\n' "${triggers}" | grep -qx 'workflow_call'; then
    # Reusable workflow. (d) every runs-on is a hosted label or exactly the
    # routing expression.
    while IFS=$'\t' read -r kind job key lineno val; do
      { [ "${kind}" = "KEY" ] && [ "${key}" = "runs-on" ]; } || continue
      if [ -z "${val}" ]; then
        err "${wf}:${lineno}: multi-line runs-on is not supported by this gate (job '${job}')"
      elif [ "${PR_WF}" = 1 ] && { is_fleet_route "${val}" || is_fleet_switch_route "${val}"; }; then
        err "${wf}:${lineno}: runs-on '${val}' is the routing expression without the queue route term; pipeline-pr.yml requires the queue routing expression (rule d)"
      elif ! is_hosted_label "${val}" && ! is_fleet_route "${val}" && ! is_fleet_switch_route "${val}" \
           && ! { [ "${PR_WF}" = 1 ] && { is_fleet_queue_route "${val}" || is_fleet_iis_queue_route "${val}"; }; }; then
        err "${wf}:${lineno}: runs-on '${val}' is neither a hosted label nor the routing expression with a fleet class (rule d)"
      elif is_fleet_switch_route "${val}" || is_fleet_queue_route "${val}"; then
        if_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="if" {print $5}')"
        if ! is_switch_if "${if_val}"; then
          err "${wf}:${lineno}: switch-routed job '${job}' may carry no if: or only a needs.changes.outputs.<name> == 'true' path filter (rule d)"
        fi
      elif is_fleet_route "${val}" || is_fleet_iis_queue_route "${val}"; then
        # A fleet job carries the fleet guard verbatim as its if: (the
        # FLEET_ENABLED kill switch, docs/ci/pipelines.md).
        if_val="$(printf '%s\n' "${records}" | awk -F"${TAB}" -v j="${job}" '$1=="KEY" && $2==j && $3=="if" {print $5}')"
        if is_fleet_iis_route "${val}"; then
          route_class="${val%\' \}\}}"; route_class="${route_class##*\'}"
          if [ "${PR_WF}" = 1 ]; then
            if [ "${if_val}" != "${FLEET_IIS_QUEUE_GUARD}" ] && ! is_iis_narrowed "${if_val}"; then
              err "${wf}:${lineno}: ${route_class} job '${job}' must carry the fleet-iis queue guard (FLEET_IIS_ENABLED and the fleet route) verbatim as its if: (rule d)"
            fi
          elif [ "${if_val}" != "${FLEET_IIS_GUARD}" ] && [ "${if_val}" != "${FLEET_IIS_SWITCH_GUARD}" ] && ! is_iis_narrowed "${if_val}"; then
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
#   - the `changes` job holds exactly one step named QG_STEP; the first three
#     steps after the actions/checkout step (which carries no env:) are the
#     push-ref step (QG_PUSH_STEP: a push is accepted only on the default
#     branch or ci-dev), the dispatch-actor step (QG_DISP_STEP: a dispatch made
#     by github-actions[bot] is refused) and QG_STEP, in this order, each in
#     its exact spec form (qg_push_expected, qg_disp_expected);
#   - that step has only the keys name, id (queue), if, env and run (keys compared after
#     unquoting), the exact if: `github.event_name == 'merge_group'`, a run:
#     block that is exactly one of the two allowed bodies below, and env keys
#     from the allowed set only; the `changes` job defines no other env keys;
#   - outside that step, no non-comment line names `queue_fork_gua...` (the
#     test script excepted), and the `changes` job has no glob or brace
#     expansion under tools/ci/ (path-filter grep patterns excepted), no
#     find/xargs/source/dot-invocation and no GITHUB_ENV/GITHUB_PATH write;
#   - BASH_ENV, PATH and GIT_DIR/GIT_WORK_TREE/GIT_CONFIG* are not set at
#     workflow level or in the `changes` job;
#   - the `vendor` job has `needs:` containing `changes` and either no `if:`
#     or exactly VENDOR_IF (the fleet scope, see the fleet-scope rule).
QG_STEP="Classify the merge group"
QG_IF="        if: github.event_name == 'merge_group'"
QG_PAT='(^|[^_A-Za-z0-9])queue_fork_guard\.sh'
QG_ENV_ALLOWED=" GH_TOKEN BASE HEAD_SHA HEAD_REF DEFAULT_BRANCH GITHUB_REPOSITORY "
# shellcheck disable=SC2016  # the text to match carries literal ${...}
QG_SET='set -euo pipefail'
# shellcheck disable=SC2016
QG_REV='git rev-parse -q --verify "origin/${DEFAULT_BRANCH}^{commit}" >/dev/null ||'
# shellcheck disable=SC2016
QG_FETCH='  git fetch --no-tags --depth=1 origin "+refs/heads/${DEFAULT_BRANCH}:refs/remotes/origin/${DEFAULT_BRANCH}"'
# shellcheck disable=SC2016
QG_SHOW='rc=0; git show "origin/${DEFAULT_BRANCH}:tools/ci/queue_fork_guard.sh" | bash || rc=$?'
# Exit 0 = trusted (fleet), 3 = untrusted (hosted), anything else refuses.
# shellcheck disable=SC2016
QG_CASE='case "${rc}" in 0) echo route=fleet;; 3) echo route=hosted;; *) exit 1;; esac >> "$GITHUB_OUTPUT"'
QG_BODY_FULL="$(printf '%s\n%s\n%s\n%s\n%s' "${QG_SET}" "${QG_REV}" "${QG_FETCH}" "${QG_SHOW}" "${QG_CASE}")"
QG_BODY_MIN="$(printf '%s\n%s\n%s' "${QG_SET}" "${QG_SHOW}" "${QG_CASE}")"
# shellcheck disable=SC2016
VENDOR_IF='    if: ${{ needs.changes.outputs.fleet == '"'true'"' }}'
QG_PUSH_STEP="Refuse pushes that are not on the default branch"
QG_DISP_STEP="Refuse dispatches made with a workflow token"
# Expected text of the push-ref step and of the dispatch-actor step.
qg_push_expected() {
  # shellcheck disable=SC2016  # the text to match carries literal ${...}
  printf '%s\n' \
    "      - name: ${QG_PUSH_STEP}" \
    "        if: github.event_name == 'push'" \
    '        env:' \
    '          REF: ${{ github.ref }}' \
    '          DEFAULT_BRANCH: ${{ github.event.repository.default_branch }}' \
    '        run: |' \
    '          set -euo pipefail' \
    '          case "${REF}" in "refs/heads/${DEFAULT_BRANCH}"|refs/heads/ci-dev) ;; *) echo "::error::push on ${REF} is not allowed in this pipeline"; exit 1;; esac'
}
qg_disp_expected() {
  # shellcheck disable=SC2016
  printf '%s\n' \
    "      - name: ${QG_DISP_STEP}" \
    "        if: github.event_name == 'workflow_dispatch'" \
    '        env:' \
    '          ACTOR: ${{ github.actor }}' \
    '        run: |' \
    '          set -euo pipefail' \
    '          [ "${ACTOR}" != "github-actions[bot]" ] || { echo "::error::dispatches made with a workflow token are not allowed"; exit 1; }'
}
qg_err() { err "${wf}: $1 (queue-guard-reads-default-branch)"; }

# job_block <file> <job>: the lines of one top-level job.
job_block() {
  awk -v j="  $2:" '/^  [A-Za-z0-9_-]+:[ ]*$/ { inj = ($0 == j) } inj { print }' "$1"
}
# env_keys <indent>: the keys of an `env:` block at that indent (stdin).
env_keys() {
  awk -v ind="$1" '
    $0 == ind "env:" { e = 1; next }
    e {
      if ($0 ~ /^[[:space:]]*#/ || $0 ~ /^[[:space:]]*$/) next
      if (substr($0, 1, length(ind) + 2) == ind "  ") {
        k = $0; sub(/^ +/, "", k); sub(/:.*/, "", k); gsub(/["\047]/, "", k); print k
      } else e = 0
    }'
}

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  mentions="$(grep -E "${QG_PAT}" "${wf}" | grep -vcE '^[[:space:]]*#' || true)"
  if [ "$(basename "${wf}")" != "pipeline-pr.yml" ] && [ "${mentions}" = "0" ]; then
    continue
  fi
  job="$(job_block "${wf}" changes)"
  n_steps="$(printf '%s\n' "${job}" | grep -cxF "      - name: ${QG_STEP}" || true)"
  if [ "${n_steps}" != "1" ]; then
    qg_err "the changes job must hold exactly one step named '${QG_STEP}' (found ${n_steps})"
    continue
  fi
  step="$(printf '%s\n' "${job}" | awk -v n="      - name: ${QG_STEP}" '
    /^      - / { instep = ($0 == n) }
    instep { print }')"
  keys="$(printf '%s\n' "${step}" | grep -E '^        ["'\'']?[A-Za-z_-]+["'\'']?:' | sed -E 's/^ +["'\'']?([A-Za-z_-]+)["'\'']?:.*/\1/' | sort | tr '\n' ' ')"
  if [ "${keys}" != "env id if run " ]; then
    qg_err "the guard step must have exactly the keys name, id, if, env and run (found: name ${keys})"
  fi
  if [ "$(printf '%s\n' "${step}" | grep -cxF "        id: queue")" != "1" ]; then
    qg_err "the guard step must carry exactly: id: queue"
  fi
  if [ "$(printf '%s\n' "${step}" | grep -cxF "${QG_IF}")" != "1" ]; then
    qg_err "the guard step must carry exactly: ${QG_IF# }"
  fi
  body="$(printf '%s\n' "${step}" | awk '
    $0 == "        run: |" { inrun = 1; next }
    inrun { if ($0 ~ /^          /) print; else if ($0 !~ /^[[:space:]]*$/) inrun = 0 }' | sed -E 's/^          //')"
  if [ "${body}" != "${QG_BODY_FULL}" ] && [ "${body}" != "${QG_BODY_MIN}" ]; then
    qg_err "the guard step's run: block must be only: ${QG_SET}, the optional default-branch fetch, ${QG_SHOW} and ${QG_CASE}"
  fi

  # Position: right after actions/checkout come the push-ref step, the
  # dispatch-actor step and the guard step, in this order, in the spec form.
  after_checkout="$(printf '%s\n' "${job}" | grep -E '^      - ' | awk '/^      - uses: actions\/checkout@/ { f = 1; next } f { print; if (++n == 3) exit }' | tr '\n' '|')"
  if [ "${after_checkout}" != "      - name: ${QG_PUSH_STEP}|      - name: ${QG_DISP_STEP}|      - name: ${QG_STEP}|" ]; then
    qg_err "the first steps after the actions/checkout step of the changes job must be '${QG_PUSH_STEP}', '${QG_DISP_STEP}' and '${QG_STEP}', in this order"
  fi
  for spec in push disp; do
    if [ "${spec}" = push ]; then sname="${QG_PUSH_STEP}"; sexp="$(qg_push_expected)"; else sname="${QG_DISP_STEP}"; sexp="$(qg_disp_expected)"; fi
    sblk="$(printf '%s\n' "${job}" | awk -v n="      - name: ${sname}" '/^      - / { instep = ($0 == n) } instep { print }' | grep -vE '^[[:space:]]*(#|$)' || true)"
    if [ "${sblk}" != "${sexp}" ]; then
      qg_err "the step '${sname}' must be present in the changes job in exactly the spec form (no other key or command)"
    fi
  done
  if printf '%s\n' "${job}" | awk '/^      - / { c = ($0 ~ /^      - uses: actions\/checkout@/) } c { print }' | grep -qE '^        ["'\'']?env["'\'']?:'; then
    qg_err "the actions/checkout step of the changes job must not set env:"
  fi

  # env: only the spec's keys on the job and on the guard step.
  for k in $(printf '%s\n' "${job}" | env_keys "    ") $(printf '%s\n' "${step}" | env_keys "        "); do
    case "${QG_ENV_ALLOWED}" in *" ${k} "*) ;; *) qg_err "env key '${k}' is not one of the guard's inputs (changes job or guard step)" ;; esac
  done
  top="$(awk '/^jobs:/ { exit } { print }' "${wf}")"
  if printf '%s\n%s\n' "${top}" "${job}" | grep -qE '^[[:space:]]*["'\'']?(BASH_ENV|PATH|GIT_DIR|GIT_WORK_TREE|GIT_CONFIG[A-Za-z_]*)["'\'']?[[:space:]]*:'; then
    qg_err "BASH_ENV, PATH and GIT_DIR/GIT_WORK_TREE/GIT_CONFIG* must not be set at workflow level or in the changes job"
  fi

  # No other read of the script, in any spelling, outside the guard step.
  rest="$(awk -v n="      - name: ${QG_STEP}" '
    $0 == n { skip = 1; next }
    skip && (/^      - / || /^  [A-Za-z0-9_-]+:/ || /^[A-Za-z]/) { skip = 0 }
    !skip { print }' "${wf}" | grep -vE '^[[:space:]]*#')"
  if printf '%s\n' "${rest}" | sed 's/test_queue_fork_guard\.sh//g' | grep -q 'queue_fork_gua'; then
    qg_err "queue_fork_gua... is named outside the guard step; never read the queued tree's copy"
  fi
  cjob="$(printf '%s\n' "${rest}" | awk '/^  [A-Za-z0-9_-]+:[ ]*$/ { inj = ($0 == "  changes:") } inj { print }')"
  if printf '%s\n' "${cjob}" | grep -vE 'grep -q?E ' | grep -qE 'tools/ci/[^[:space:]]*([*?{[]|\$[{(])'; then
    qg_err "a glob, brace or interpolated path under tools/ci/ in the changes job"
  fi
  if printf '%s\n' "${cjob}" | grep -qE '(^|[^A-Za-z0-9_./-])(find|xargs|source)([[:space:]]|$)|(^|[[:space:];&|(]|run:[[:space:]]*)\.[[:space:]]+[^[:space:]]|GITHUB_ENV|GITHUB_PATH'; then
    qg_err "find, xargs, source, a dot-invocation or a GITHUB_ENV/GITHUB_PATH write in the changes job"
  fi

  # vendor waits for changes, with no if: that could skip it.
  if [ "$(basename "${wf}")" = "pipeline-pr.yml" ]; then
    vjob="$(job_block "${wf}" vendor)"
    if [ -z "${vjob}" ]; then
      qg_err "no vendor job"
    else
      if ! printf '%s\n' "${vjob}" | grep -qE '^    needs:[[:space:]]*(changes|\[.*\bchanges\b.*\])[[:space:]]*$' &&
        ! { printf '%s\n' "${vjob}" | grep -qE '^    needs:[[:space:]]*$' && printf '%s\n' "${vjob}" | grep -qxE '      - changes'; }; then
        qg_err "the vendor job must have needs: changes"
      fi
      # The only if: vendor may carry is the fleet scope of the changes job
      # (the fleet-scope rule below pins the step that sets it).
      vifs="$(printf '%s\n' "${vjob}" | grep -E '^    ["'\'']?if["'\'']?[[:space:]]*:' || true)"
      if [ -n "${vifs}" ] && [ "${vifs}" != "${VENDOR_IF}" ]; then
        qg_err "the vendor job may have no if: or exactly: ${VENDOR_IF# }"
      fi
    fi
  fi
done

# --- fleet-scope ---------------------------------------------------------------
# The fleet scope of the PR tier (docs/ci/pipelines.md, "CI_SCOPE") decides
# whether `vendor`, and with it every fleet job, runs. Its decision script must
# be read from the default branch, never from the tree under test, and
# merge_group and workflow_dispatch must never reach it. In pipeline-pr.yml (and
# any workflow that names the script):
#   - the `changes` job holds exactly one step named FS_STEP, in exactly the
#     spec form of fs_expected (comment and blank lines aside): no other key,
#     env key or command, and the inline merge_group|workflow_dispatch branch;
#   - the step comes right after the merge-queue guard step (so after the
#     three pinned steps and before the path-filter diff step);
#   - the `changes` job publishes `fleet` and `heavy` from that step;
#   - outside that step, no non-comment line names `pr_tier_scope.sh` (the
#     test script excepted).
FS_STEP="Decide the fleet scope"
FS_PAT='(^|[^_A-Za-z0-9])pr_tier_scope\.sh'
fs_expected() {
  # shellcheck disable=SC2016  # the text to match carries literal ${...}
  printf '%s\n' \
    "      - name: ${FS_STEP}" \
    '        id: scope' \
    '        env:' \
    '          GH_TOKEN: ${{ github.token }}' \
    '          CI_SCOPE: ${{ vars.CI_SCOPE }}' \
    '          DEFAULT_BRANCH: ${{ github.event.repository.default_branch }}' \
    '          SHA: ${{ github.sha }}' \
    "          FORK_OR_BOT: \${{ github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]') }}" \
    "          PR_FULL: \${{ github.event_name == 'pull_request' && contains(github.event.pull_request.labels.*.name, 'ci:pr-full') }}" \
    '        run: |' \
    '          set -euo pipefail' \
    '          out="${RUNNER_TEMP}/fleet-scope.txt"' \
    '          case "${GITHUB_EVENT_NAME}" in' \
    "            merge_group|workflow_dispatch) printf 'fleet=true\\nheavy=true\\n' > \"\${out}\" ;;" \
    '            *)' \
    '              if git show "origin/${DEFAULT_BRANCH}:tools/ci/pr_tier_scope.sh" > "${RUNNER_TEMP}/pr_tier_scope.sh" 2>/dev/null &&' \
    '                bash "${RUNNER_TEMP}/pr_tier_scope.sh" > "${out}" &&' \
    "                [ \"\$(grep -cxE '(fleet|heavy)=(true|false)' \"\${out}\")\" = 2 ] && [ \"\$(wc -l < \"\${out}\")\" -eq 2 ]; then" \
    '                :' \
    '              else' \
    '                echo "::warning::fleet scope undecided; running the full PR tier"' \
    "                printf 'fleet=true\\nheavy=true\\n' > \"\${out}\"" \
    '              fi ;;' \
    '          esac' \
    '          cat "${out}" >> "$GITHUB_OUTPUT"'
}
fs_err() { err "${wf}: $1 (fleet-scope)"; }

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  fs_mentions="$(grep -E "${FS_PAT}" "${wf}" | grep -vcE '^[[:space:]]*#' || true)"
  qg_mentions="$(grep -E "${QG_PAT}" "${wf}" | grep -vcE '^[[:space:]]*#' || true)"
  if [ "$(basename "${wf}")" != "pipeline-pr.yml" ] && [ "${fs_mentions}" = "0" ] && [ "${qg_mentions}" = "0" ]; then
    continue
  fi
  job="$(job_block "${wf}" changes)"
  n_steps="$(printf '%s\n' "${job}" | grep -cxF "      - name: ${FS_STEP}" || true)"
  if [ "${n_steps}" != "1" ]; then
    fs_err "the changes job must hold exactly one step named '${FS_STEP}' (found ${n_steps})"
  else
    sblk="$(printf '%s\n' "${job}" | awk -v n="      - name: ${FS_STEP}" '/^      - / { instep = ($0 == n) } instep { print }' | grep -vE '^[[:space:]]*(#|$)' || true)"
    if [ "${sblk}" != "$(fs_expected)" ]; then
      fs_err "the step '${FS_STEP}' must be in exactly the spec form (no other key, env key or command; merge_group and workflow_dispatch decided inline)"
    fi
    # Right after the merge-queue guard step, when the job has one (its own
    # rule reports a missing guard step).
    if printf '%s\n' "${job}" | grep -qxF "      - name: ${QG_STEP}"; then
      next_step="$(printf '%s\n' "${job}" | grep -E '^      - ' | awk -v g="      - name: ${QG_STEP}" 'f { print; exit } $0 == g { f = 1 }')"
      if [ "${next_step}" != "      - name: ${FS_STEP}" ]; then
        fs_err "the step '${FS_STEP}' must come right after '${QG_STEP}' (before the path-filter diff)"
      fi
    fi
  fi
  for o in fleet heavy; do
    if ! printf '%s\n' "${job}" | grep -qxF "      ${o}: \${{ steps.scope.outputs.${o} }}"; then
      fs_err "the changes job must publish '${o}: \${{ steps.scope.outputs.${o} }}'"
    fi
  done
  # No other read of the script, in any spelling, outside the scope step.
  rest="$(awk -v n="      - name: ${FS_STEP}" '
    $0 == n { skip = 1; next }
    skip && (/^      - / || /^  [A-Za-z0-9_-]+:/ || /^[A-Za-z]/) { skip = 0 }
    !skip { print }' "${wf}" | grep -vE '^[[:space:]]*#')"
  if printf '%s\n' "${rest}" | sed 's/test_pr_tier_scope\.sh//g' | grep -q 'pr_tier_sco'; then
    fs_err "pr_tier_sco... is named outside the '${FS_STEP}' step; never read the tree's copy"
  fi
done

# --- queue-route ----------------------------------------------------------------
# The runner route of pipeline-pr.yml (docs/ci/pipelines.md, "Merge-queue
# runs"). The `changes` job decides `fleet` or `hosted` once, in the step
# RT_STEP, from the classifier's published route and the event; a missing or
# invalid merge-group route fails the step. Every build runner job reads the
# result from `needs.changes.outputs.route` directly, never through `vendor`
# (which runs queued code), so each one lists `changes` in its `needs:`. In
# pipeline-pr.yml (and any workflow that names the classifier):
#   - the `changes` job holds exactly one step named RT_STEP, in exactly the
#     spec form of rt_expected (comment and blank lines aside), right after
#     the fleet-scope step, and exactly one `id: route`;
#   - the `changes` job publishes `route: ${{ steps.route.outputs.route }}`
#     and no other job defines an output named `route`;
#   - in pipeline-pr.yml every job whose runs-on is not a plain hosted label
#     lists `changes` in its `needs:`.
RT_STEP="Decide the runner route"
rt_expected() {
  # shellcheck disable=SC2016  # the text to match carries literal ${...}
  printf '%s\n' \
    "      - name: ${RT_STEP}" \
    '        id: route' \
    '        env:' \
    '          QUEUE_ROUTE: ${{ steps.queue.outputs.route }}' \
    "          FORK_OR_BOT: \${{ github.event_name == 'pull_request' && (github.event.pull_request.head.repo.fork == true || github.actor == 'dependabot[bot]') }}" \
    '        run: |' \
    '          set -euo pipefail' \
    '          case "${GITHUB_EVENT_NAME}" in' \
    '            merge_group)' \
    '              case "${QUEUE_ROUTE}" in fleet|hosted) route="${QUEUE_ROUTE}" ;; *) echo "::error::the merge group has no valid route"; exit 1 ;; esac ;;' \
    '            pull_request)' \
    '              if [ "${FORK_OR_BOT}" = true ]; then route=hosted; else route=fleet; fi ;;' \
    '            push|workflow_dispatch) route=fleet ;;' \
    '            *) echo "::error::event ${GITHUB_EVENT_NAME} has no route"; exit 1 ;;' \
    '          esac' \
    '          echo "route=${route}" >> "$GITHUB_OUTPUT"'
}
rt_err() { err "${wf}: $1 (queue-route)"; }

# job_needs_changes <job-block>: true when its needs: lists `changes`.
job_needs_changes() {
  printf '%s\n' "$1" | grep -qE '^    needs:[[:space:]]*(changes|\[.*\bchanges\b.*\])[[:space:]]*$' && return 0
  if printf '%s\n' "$1" | grep -qE '^    needs:[[:space:]]*$' && printf '%s\n' "$1" | grep -qxE '      - changes'; then return 0; fi
  return 1
}

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  qg_mentions="$(grep -E "${QG_PAT}" "${wf}" | grep -vcE '^[[:space:]]*#' || true)"
  if [ "$(basename "${wf}")" != "pipeline-pr.yml" ] && [ "${qg_mentions}" = "0" ]; then
    continue
  fi
  job="$(job_block "${wf}" changes)"
  n_steps="$(printf '%s\n' "${job}" | grep -cxF "      - name: ${RT_STEP}" || true)"
  if [ "${n_steps}" != "1" ]; then
    rt_err "the changes job must hold exactly one step named '${RT_STEP}' (found ${n_steps})"
  else
    sblk="$(printf '%s\n' "${job}" | awk -v n="      - name: ${RT_STEP}" '/^      - / { instep = ($0 == n) } instep { print }' | grep -vE '^[[:space:]]*(#|$)' || true)"
    if [ "${sblk}" != "$(rt_expected)" ]; then
      rt_err "the step '${RT_STEP}' must be in exactly the spec form (no other key, env key or command)"
    fi
    if printf '%s\n' "${job}" | grep -qxF "      - name: ${FS_STEP}"; then
      next_step="$(printf '%s\n' "${job}" | grep -E '^      - ' | awk -v g="      - name: ${FS_STEP}" 'f { print; exit } $0 == g { f = 1 }')"
      if [ "${next_step}" != "      - name: ${RT_STEP}" ]; then
        rt_err "the step '${RT_STEP}' must come right after '${FS_STEP}'"
      fi
    fi
  fi
  if [ "$(printf '%s\n' "${job}" | grep -cE '^        ["'\'']?id["'\'']?:[[:space:]]*["'\'']?route["'\'']?[[:space:]]*$')" != "1" ]; then
    rt_err "the changes job must have exactly one step with id: route"
  fi
  if ! printf '%s\n' "${job}" | grep -qxF "      route: \${{ steps.route.outputs.route }}"; then
    rt_err "the changes job must publish 'route: \${{ steps.route.outputs.route }}'"
  fi
  if [ "$(basename "${wf}")" = "pipeline-pr.yml" ]; then
    # No other job defines a `route` output; every fleet-capable job needs changes.
    while IFS= read -r oj; do
      [ -n "${oj}" ] || continue
      [ "${oj}" != "changes" ] || continue
      ojb="$(job_block "${wf}" "${oj}")"
      if printf '%s\n' "${ojb}" | grep -qE '^      ["'\'']?route["'\'']?[[:space:]]*:'; then
        rt_err "job '${oj}' defines an output named route; only the changes job decides it"
      fi
      oruns="$(printf '%s\n' "${ojb}" | sed -n -E 's/^    runs-on:[[:space:]]*//p' | head -1)"
      if [ -n "${oruns}" ] && ! is_hosted_label "${oruns}" && ! job_needs_changes "${ojb}"; then
        rt_err "job '${oj}' is routed to the fleet but does not list changes in its needs: (it reads needs.changes.outputs.route)"
      fi
    done <<EOT
$(awk '/^jobs:[[:space:]]*$/ { j = 1; next } j && /^[^[:space:]#]/ { j = 0 } j && /^  [A-Za-z0-9_-]+:[ ]*$/ { k = $1; sub(/:$/, "", k); print k }' "${wf}")
EOT
  fi
done

# --- event-guard --------------------------------------------------------------
# Every workflow other than pipeline-pr.yml that has a fleet runs-on (any
# runs-on that is not a plain hosted label) must carry the hosted job
# `event-guard`, in exactly the form below, and every fleet job must reach it
# through `needs`. A merge_group run takes its caller workflow from the queued
# tree, and the queued tree can also produce push and workflow_dispatch events
# of its own, so the guard checks the event, the ref and the actor: it refuses
# merge-queue refs, any ref outside the pipeline's ALLOWED_REFS, a dispatch
# made with a workflow token and a pull request from a fork or another
# repository. ALLOWED and ALLOWED_REFS are fixed per pipeline (eg_allowed_for,
# eg_refs_for); a file not named there may list only push, schedule and
# workflow_dispatch events and exact refs/heads refs, refs/tags/* and refs/pull/*/merge.
# pipeline-full.yml also allows pull_request (the caller runs it for the
# ci:full label) and schedule (the nightly calls it), and no other event.
#   - "Reaches" means a path of needs edges to event-guard along which no job
#     can run after a failed guard. Job keys (if, needs, runs-on,
#     continue-on-error, uses) match quoted or with space before the colon.
#     Before matching, each if: is normalised: quotes stripped, all lines
#     joined (a plain value continues on every line indented more than its
#     key; a #-led line in a block scalar is content), lowercased,
#     whitespace collapsed, and the space between a function name and its (
#     removed.
#   - Relay rule: for every job on a needs path between event-guard and a
#     fleet job (and the fleet jobs themselves, unless rule d pins their if:,
#     that is in a workflow_call file), an if: that contains always(,
#     failure(, cancelled(, ||, ! (a negation anywhere, != included),
#     == false, or success( (unless the whole if: is a bare success()) must
#     be exactly
#       needs.event-guard.result == 'success' && (<inner>)
#     (optionally inside ${{ }}), the parentheses around <inner> one balanced
#     group (string literals ignored) and <inner> free of event-guard, and
#     the job must need event-guard, and a block scalar holding it must strip
#     its final newline (|- or >-). Otherwise the job is rejected by name.
#     Any other if: is accepted as before (GitHub adds success() implicitly
#     unless the expression names a status function).
#   - A pinned fleet job whose if: has always( or failure( does not reach; one
#     whose if: has !cancelled() needs a `needs.<job>.result == 'success'`
#     term for a reaching job. A job with continue-on-error other than false
#     does not pass the guard on to its dependents. The scan covers every
#     workflow file with a fleet runs-on, not a fixed list.
EG_DEFAULT_BRANCH="master"
eg_allowed_for() { # workflow file base name
  case "$1" in
    pipeline-full.yml) printf '%s' "push workflow_dispatch pull_request schedule" ;;
    pipeline-nightly.yml) printf '%s' "schedule workflow_dispatch" ;;
    pipeline-tidy-sweep.yml) printf '%s' "workflow_dispatch" ;;
    gate-*.yml|pipeline-release-*.yml) printf '%s' "push workflow_dispatch" ;;
    *) printf '%s' "" ;;
  esac
}
eg_refs_for() { # workflow file base name
  case "$1" in
    pipeline-full.yml) printf '%s' "refs/heads/${EG_DEFAULT_BRANCH} refs/heads/ci-dev refs/pull/*/merge" ;;
    pipeline-nightly.yml|pipeline-tidy-sweep.yml) printf '%s' "refs/heads/${EG_DEFAULT_BRANCH}" ;;
    gate-*.yml|pipeline-release-*.yml) printf '%s' "refs/tags/* refs/heads/${EG_DEFAULT_BRANCH}" ;;
    *) printf '%s' "" ;;
  esac
}

# Expected text of the event-guard job for an ALLOWED and an ALLOWED_REFS value.
eg_expected() {
  # shellcheck disable=SC2016  # the text to match carries literal ${{ ... }}
  printf '%s\n' \
    '  event-guard:' \
    '    name: Event guard (refuses merge_group, unexpected events, refs and actors)' \
    '    runs-on: ubuntu-latest' \
    '    steps:' \
    '      - name: Refuse events, refs and actors this pipeline must never run on' \
    '        env:' \
    '          EVENT: ${{ github.event_name }}' \
    '          REF: ${{ github.ref }}' \
    '          ACTOR: ${{ github.actor }}' \
    '          PR_FORK: ${{ github.event.pull_request.head.repo.fork }}' \
    '          PR_HEAD_REPO: ${{ github.event.pull_request.head.repo.full_name }}' \
    '          REPOSITORY: ${{ github.repository }}' \
    "          ALLOWED: \"$1\"" \
    "          ALLOWED_REFS: \"$2\"" \
    '        run: |' \
    '          set -euo pipefail' \
    '          set -f' \
    '          case " ${ALLOWED} " in *" ${EVENT} "*) ;; *) echo "::error::event ${EVENT} is not allowed in this pipeline"; exit 1;; esac' \
    '          case "${REF}" in refs/heads/gh-readonly-queue/*) echo "::error::merge queue refs never run this pipeline"; exit 1;; esac' \
    '          ok=no' \
    '          # shellcheck disable=SC2254  # the allowed refs are globs on purpose' \
    '          for r in ${ALLOWED_REFS}; do case "${REF}" in ${r}) ok=yes;; esac; done' \
    '          [ "${ok}" = yes ] || { echo "::error::ref ${REF} is not allowed in this pipeline"; exit 1; }' \
    '          if [ "${EVENT}" = workflow_dispatch ] && [ "${ACTOR}" = "github-actions[bot]" ]; then echo "::error::dispatches made with a workflow token are not allowed"; exit 1; fi' \
    '          if [ "${EVENT}" = pull_request ] && { [ "${PR_FORK}" = true ] || [ "${PR_HEAD_REPO}" != "${REPOSITORY}" ]; }; then echo "::error::pull requests from forks are not allowed in this pipeline"; exit 1; fi' \
    '          echo "event ${EVENT} on ${REF} accepted"'
}

# Fleet jobs that do not reach event-guard, one per line (see the rules above).
eg_unreached() { # workflow file, 1 if it is a workflow_call file (rule d pins its fleet jobs)
  awk -v hosted="${HOSTED_LABELS}" -v wcall="$2" '
    BEGIN { n = split(hosted, h, " "); for (i = 1; i <= n; i++) ishosted[h[i]] = 1 }
    # The parenthesised guard conjunct: exactly
    # needs.event-guard.result == \047success\047 && (<inner>), the parentheses
    # around <inner> one balanced group (string literals ignored) and <inner>
    # free of event-guard.
    function guardform(t,   p, inner, q, d, c, i) {
      if (t ~ /^\$\{\{/) { if (t !~ /\}\}$/) return 0; sub(/^\$\{\{ ?/, "", t); sub(/ ?\}\}$/, "", t) }
      if (index(t, "${{") || index(t, "}}")) return 0
      p = "needs.event-guard.result == \047success\047 && ("
      if (index(t, p) != 1 || substr(t, length(t), 1) != ")") return 0
      inner = substr(t, length(p) + 1, length(t) - length(p) - 1)
      if (index(inner, "event-guard")) return 0
      q = inner; gsub(/\047[^\047]*\047/, "", q)
      if (index(q, "\047")) return 0
      d = 0
      for (i = 1; i <= length(q); i++) {
        c = substr(q, i, 1)
        if (c == "(") d++
        else if (c == ")") { d--; if (d < 0) return 0 }
      }
      return d == 0
    }
    # An if: value continues on every following line indented more than its
    # key (column 4), a #-led line included (block scalar content).
    injobs && inif {
      if ($0 ~ /^     / || $0 ~ /^[[:space:]]*$/) { v = $0; gsub(/^[[:space:]]+/, "", v); iff[job] = iff[job] " " v; next }
      inif = 0
    }
    /^[[:space:]]*#/ { next }
    /^jobs:[[:space:]]*$/ { injobs = 1; next }
    injobs && /^[^[:space:]#]/ { injobs = 0 }
    injobs && /^  [^[:space:]]/ {
      job = $0; sub(/^  /, "", job); sub(/[ \t]*:.*$/, "", job); gsub(/["\047]/, "", job)
      order[++nj] = job; inneeds = 0; next
    }
    injobs && /^    ["\047]?needs["\047]?[ \t]*:/ {
      v = $0; sub(/^    [^:]+:[[:space:]]*/, "", v); gsub(/[\[\],]/, " ", v)
      needs[job] = v; inneeds = (v ~ /^[[:space:]]*$/); next
    }
    injobs && inneeds && /^      - / { v = $0; sub(/^      - /, "", v); needs[job] = needs[job] " " v; next }
    injobs && /^    ["\047]?[A-Za-z_-]+["\047]?[ \t]*:/ {
      inneeds = 0
      k = $0; sub(/^    /, "", k); sub(/[ \t]*:.*$/, "", k); gsub(/["\047]/, "", k); v = $0; sub(/^    [^:]+:[[:space:]]*/, "", v)
      if (k == "if") { iff[job] = v; blk[job] = ""; if (v ~ /^[>|][-+0-9]*[[:space:]]*$/) { iff[job] = ""; blk[job] = v }; inif = 1 }
      else if (k == "continue-on-error") { gsub(/["\047]/, "", v); coe[job] = tolower(v) }
      else if (k == "runs-on") ro[job] = v
    }
    END {
      for (i = 1; i <= nj; i++) reach[order[i]] = 0
      changed = 1
      while (changed) {
        changed = 0
        for (i = 1; i <= nj; i++) {
          j = order[i]
          if (reach[j]) continue
          t = tolower(iff[j]); gsub(/[[:space:]]+/, " ", t); sub(/^ /, "", t); sub(/ $/, "", t)
          if (t ~ /^".*"$/ || t ~ /^\047.*\047$/) t = substr(t, 2, length(t) - 2)
          while (match(t, /[a-z0-9_] \(/)) t = substr(t, 1, RSTART) substr(t, RSTART + 2)
          u = t; gsub(/ /, "", u)
          # An explicit success() turns off the implicit one as well; only a
          # bare success() equals the default.
          w = u; sub(/^\$\{\{/, "", w); sub(/\}\}$/, "", w)
          risky = (u ~ /always\(|failure\(|cancelled\(|\|\||!|==false/ || (u ~ /success\(/ && w != "success()"))
          m = split(needs[j], nd, " "); ok = 0
          hasg = 0
          for (x = 1; x <= m; x++) if (nd[x] == "event-guard") hasg = 1
          fleet = (ro[j] != "" && !(ro[j] in ishosted))
          pinned = (fleet && wcall == 1)
          # A block scalar without strip chomping (|, >, |+, >+) keeps a final
          # newline: text around ${{ }}, which GitHub reads as a non-empty string.
          kept = (blk[j] != "" && blk[j] !~ /-/ && index(t, "${{"))
          if (!pinned && risky) { ok = (hasg && !kept && guardform(t)); if (!ok) badform[j] = 1 }
          else if (t ~ /always\(|failure\(/) ok = 0
          else {
            for (x = 1; x <= m; x++) {
              d = nd[x]
              if (d != "event-guard" && (!(d in reach) || !reach[d])) continue
              if (coe[d] != "" && coe[d] != "false") continue
              if (t ~ /cancelled\(\)/ && index(t, tolower("needs." d ".result == \047success\047")) == 0) continue
              ok = 1; break
            }
          }
          if (ok) { reach[j] = 1; changed = 1 }
        }
      }
      # Jobs below event-guard (a needs path up to it).
      changed = 1
      while (changed) {
        changed = 0
        for (i = 1; i <= nj; i++) {
          j = order[i]
          if (below[j]) continue
          m = split(needs[j], nd, " ")
          for (x = 1; x <= m; x++) if (nd[x] == "event-guard" || below[nd[x]]) { below[j] = 1; changed = 1; break }
        }
      }
      # Unreached fleet jobs ("F <job>"), and every job on a needs path between
      # event-guard and a fleet job whose if: breaks the parenthesised guard
      # rule ("R <job>").
      for (i = 1; i <= nj; i++) {
        j = order[i]
        if (j == "event-guard" || ro[j] == "" || (ro[j] in ishosted)) continue
        if (!reach[j]) print "F " j
        sp = 0; stk[++sp] = j
        while (sp > 0) {
          a = stk[sp--]
          if (a in seen) continue
          seen[a] = 1
          if (badform[a] && below[a]) print "R " a
          m = split(needs[a], nd, " ")
          for (x = 1; x <= m; x++) if (nd[x] != "event-guard") stk[++sp] = nd[x]
        }
      }
    }
  ' "$1"
}

for wf in ${WORKFLOWS[@]+"${WORKFLOWS[@]}"}; do
  [ -n "${wf}" ] || continue
  base="$(basename "${wf}")"
  [ "${base}" != "pipeline-pr.yml" ] || continue
  # Any runs-on that is not a plain hosted label is a fleet job.
  fleet_jobs="$(job_records "${wf}" | awk -F"${TAB}" '$1=="KEY" && $3=="runs-on" {print $2 "\t" $5}' | while IFS="${TAB}" read -r j v; do
    if [ -n "${v}" ] && ! is_hosted_label "${v}"; then printf '%s\n' "${j}"; fi
  done)"
  [ -n "${fleet_jobs}" ] || continue
  eg_block="$(awk '/^(  )?#/ { next } /^  [A-Za-z0-9_-]+:[ ]*$/ { inj = ($1 == "event-guard:") } /^[^[:space:]]/ { inj = 0 } inj { print }' "${wf}")"
  if [ -z "${eg_block}" ]; then
    err "${wf}: a fleet pipeline needs the hosted job 'event-guard' (event-guard)"
    continue
  fi
  eg_allowed="$(eg_allowed_for "${base}")"
  eg_refs="$(eg_refs_for "${base}")"
  eg_val="$(printf '%s\n' "${eg_block}" | sed -n 's/^          ALLOWED: "\([^"]*\)"$/\1/p')"
  eg_rval="$(printf '%s\n' "${eg_block}" | sed -n 's/^          ALLOWED_REFS: "\([^"]*\)"$/\1/p')"
  set -f
  if [ -n "${eg_allowed}" ]; then
    [ "${eg_val}" = "${eg_allowed}" ] || err "${wf}: the event-guard ALLOWED list must be exactly '${eg_allowed}' (event-guard)"
    [ "${eg_rval}" = "${eg_refs}" ] || err "${wf}: the event-guard ALLOWED_REFS list must be exactly '${eg_refs}' (event-guard)"
  else
    for t in ${eg_val}; do
      case "${t}" in push|schedule|workflow_dispatch) ;; *) err "${wf}: event '${t}' may not be allowed by the event-guard (event-guard)" ;; esac
    done
    for t in ${eg_rval}; do
      case "${t}" in refs/heads/gh-readonly-queue/*|refs/heads/*[*?[]*) err "${wf}: ref '${t}' may not be allowed by the event-guard (event-guard)" ;; refs/heads/*|"refs/tags/*"|"refs/pull/*/merge") ;; *) err "${wf}: ref '${t}' may not be allowed by the event-guard (event-guard)" ;; esac
    done
  fi
  set +f
  if [ -z "${eg_val}" ] || [ -z "${eg_rval}" ] || [ "${eg_block}" != "$(eg_expected "${eg_val}" "${eg_rval}")" ]; then
    err "${wf}: the event-guard job must match the spec form exactly: no other key, step or command (event-guard)"
  fi
  while read -r ukind uj; do
    [ -n "${uj}" ] || continue
    if [ "${ukind}" = R ]; then
      err "${wf}: job '${uj}' has an if: with always(, failure(, cancelled(, ||, ! or == false that is not exactly needs.event-guard.result == 'success' && (<inner>) (event-guard)"
    else
      err "${wf}: fleet job '${uj}' does not reach event-guard through needs, or could run after it failed (event-guard)"
    fi
  done <<EOT
$(eg_unreached "${wf}" "$(on_triggers "${wf}" | grep -qx workflow_call && echo 1 || echo 0)")
EOT
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
