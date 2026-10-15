#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# Public-tree hygiene gate (BLOCKING) -- the "Public-tree hygiene gate" CI
# job. This repository is public, so the tracked tree must stay free of
# private-infrastructure and private-process vocabulary: internal
# decision-record references, private build-machine names, the private CI
# runner-pool label scheme, pre-publication repository slugs, internal
# tracker identifiers, retired-license markers and private build-fleet
# topology words. Direct commits to this repository never pass through the
# private source repo's export-time filters, so the classes are re-checked
# here, against public-safe patterns only: a rule may name a CLASS of
# unwanted content, it may never carry an actual private value (hostnames
# and topology coinages are fair game). Credential literals are covered
# by a second pass with its own rules file (see below), whose patterns match
# shapes only.
#
# The rules live in .github/hygiene-rules.txt (one "<flags><TAB><ERE>" per
# line; its header documents each class). Every rule is run over the
# tracked tree with git grep; ANY hit fails the job and prints path:line.
#
# Excluded from the scan:
#   * .git -- git grep visits tracked files only, so it is never scanned.
#   * the rules file itself -- it necessarily contains every blocked
#     pattern.
# There are NO content exclusions in this first pass: any hit anywhere else
# in the tree fails.
#
# Second pass: credential literals (PowerShell secure-string and PSCredential
# literals, password-like assignments, private key headers, well-known token
# shapes). Its rules, placeholder list and path allowlist live in
# .github/hygiene-credential-rules.txt (header documents the format); a line
# carrying the marker "hygiene-allow-credential" is exempt. It reports
# path:line and the rule class only, never the matched text. Limitation: the
# path:line report format assumes tracked file names without a colon.
#
# Usage: tools/ci/check-public-hygiene.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"

RULES=".github/hygiene-rules.txt"
[[ -f "$RULES" ]] || { echo "::error::${RULES} not found" >&2; exit 1; }

EXCLUDES=(
  ":(exclude)${RULES}"
)

status=0
lineno=0
while IFS=$'\t' read -r flags pattern || [[ -n "$flags" ]]; do
  lineno=$((lineno + 1))
  case "$flags" in
    ''|\#*) continue ;;
  esac
  if [[ "$flags" != *E* ]]; then
    echo "::error::${RULES}:${lineno}: only extended-regex (E) rules are supported" >&2
    exit 1
  fi
  if [[ -z "$pattern" ]]; then
    echo "::error::${RULES}:${lineno}: rule has flags but no pattern" >&2
    exit 1
  fi
  grep_args=(-n -I -E)
  [[ "$flags" == *i* ]] && grep_args+=(-i)
  set +e
  out="$(git grep "${grep_args[@]}" -e "$pattern" -- . "${EXCLUDES[@]}")"
  rc=$?
  set -e
  if [[ $rc -gt 1 ]]; then
    echo "::error::${RULES}:${lineno}: git grep failed (status ${rc}): ${out}" >&2
    exit 1
  fi
  if [[ $rc -eq 0 ]]; then
    echo "::error::hygiene rule ${lineno} matched (${pattern}):" >&2
    echo "$out" >&2
    status=1
  fi
done < "$RULES"

if [[ $status -ne 0 ]]; then
  echo "::error::public-tree hygiene gate FAILED -- see the matched paths above" >&2
  exit 1
fi

echo "Public-tree hygiene gate passed: no rule matched the tracked tree."

# ---------------------------------------------------------------------------
# Second pass: credential literals.
# ---------------------------------------------------------------------------
CRED_RULES=".github/hygiene-credential-rules.txt"
CRED_MARKER="hygiene-allow-credential"
[[ -f "$CRED_RULES" ]] || { echo "::error::${CRED_RULES} not found" >&2; exit 1; }

cred_excludes=(":(exclude)${RULES}" ":(exclude)${CRED_RULES}")
placeholders=()
placeholders_dq=()
allowed_paths=()
cred_rules=()
lineno=0
while IFS=$'\t' read -r kind a b c d || [[ -n "$kind" ]]; do
  lineno=$((lineno + 1))
  case "$kind" in
    ''|\#*) continue ;;
    placeholder|placeholder-dq)
      if [[ -z "$a" ]]; then
        echo "::error::${CRED_RULES}:${lineno}: empty ${kind} entry refused" >&2
        exit 1
      fi
      if [[ "$kind" == placeholder ]]; then placeholders+=("$a"); else placeholders_dq+=("$a"); fi
      ;;
    allow-path)
      if [[ -z "$a" || "$a" == . || "$a" == .. || "$a" == *[\*\?\[]* \
            || "$a" == :* || "$a" == /* || "$a" == *..* ]]; then
        echo "::error::${CRED_RULES}:${lineno}: allow-path must be the exact path of one tracked file (no glob characters, not empty, not . or ..)" >&2
        exit 1
      fi
      if [[ "$(git ls-files -- ":(literal)${a}")" != "$a" ]]; then
        echo "::error::${CRED_RULES}:${lineno}: allow-path '${a}' is not a tracked file" >&2
        exit 1
      fi
      cred_excludes+=(":(exclude,literal)${a}")
      allowed_paths+=("$a")
      ;;
    rule)
      if [[ "$a" != *E* || -z "$b" || -z "$c" ]]; then
        echo "::error::${CRED_RULES}:${lineno}: malformed rule" >&2
        exit 1
      fi
      cred_rules+=("${a}"$'\t'"${b}"$'\t'"${c}"$'\t'"${d:-}")
      ;;
    *)
      echo "::error::${CRED_RULES}:${lineno}: unknown line kind '${kind}'" >&2
      exit 1
      ;;
  esac
done < "$CRED_RULES"

is_placeholder() { # <value> <quote char>
  local p
  for p in "${placeholders[@]}"; do
    if printf '%s\n' "$1" | grep -Eiq -e "$p"; then
      return 0
    fi
  done
  if [[ "$2" == '"' ]]; then
    for p in "${placeholders_dq[@]}"; do
      if printf '%s\n' "$1" | grep -Eiq -e "$p"; then
        return 0
      fi
    done
  fi
  return 1
}

# Self-check: a realistic random-looking value must never read as a
# placeholder, whichever quote it is wrapped in. Catches an over-broad entry.
CANARY='Qw7rTz9pLm2x'
for q in '"' "'"; do
  if is_placeholder "$CANARY" "$q"; then
    echo "::error::${CRED_RULES}: placeholder set matches the canary value; an entry is too broad" >&2
    exit 1
  fi
done

cred_status=0
for entry in "${cred_rules[@]}"; do
  IFS=$'\t' read -r flags name pattern options <<<"$entry"
  grep_args=(-n -I -o -E)
  [[ "$flags" == *i* ]] && grep_args+=(-i)
  pathspecs=(.)
  filter=0
  read -r -a opts <<<"$options"
  for opt in ${opts[@]+"${opts[@]}"}; do
    case "$opt" in
      value-filter) filter=1 ;;
      paths=*)
        IFS=',' read -r -a pathspecs <<<"${opt#paths=}"
        ;;
    esac
  done
  set +e
  out="$(git grep "${grep_args[@]}" -e "$pattern" -- "${pathspecs[@]}" "${cred_excludes[@]}")"
  rc=$?
  set -e
  if [[ $rc -gt 1 ]]; then
    echo "::error::${CRED_RULES}: rule '${name}': git grep failed (status ${rc})" >&2
    exit 1
  fi
  [[ $rc -eq 0 ]] || continue
  while IFS= read -r hit; do
    [[ -n "$hit" ]] || continue
    rest="${hit#*:}"
    path="${hit%%:*}"
    n="${rest%%:*}"
    match="${rest#*:}"
    if sed -n "${n}p" "$path" | grep -qF -e "$CRED_MARKER"; then
      continue
    fi
    if [[ $filter -eq 1 ]]; then
      value="$(printf '%s' "$match" | sed -E "s/^[^:=]*[:=][[:space:]]*[\"']//; s/[\"']\$//")"
      quote="$(printf '%s' "$match" | sed -E "s/^[^:=]*[:=][[:space:]]*(.).*/\1/")"
      if is_placeholder "$value" "$quote"; then
        continue
      fi
    fi
    echo "::error file=${path},line=${n}::credential literal (${name}); move it to the environment or a secret store" >&2
    echo "${path}:${n}: ${name}" >&2
    cred_status=1
  done <<<"$out"
done

if [[ $cred_status -ne 0 ]]; then
  echo "::error::public-tree hygiene gate FAILED -- credential literals found; see ${CRED_RULES} for the rules and the allowlist" >&2
  exit 1
fi

echo "Public-tree credential-literal pass passed: no rule matched the tracked tree (allowed paths: ${allowed_paths[*]:-none})."
