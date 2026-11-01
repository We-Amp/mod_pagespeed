#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# AI-attribution gate (BLOCKING, part of the "Public-tree hygiene gate" CI
# job). A pull request fails when its description, any of ITS commit messages,
# or any of ITS commit authors carries tool attribution:
#   * a co-author trailer naming Claude or Anthropic (a model name, or an
#     address at the vendor's domain);
#   * a session trailer (the tool's session-link trailer);
#   * the "generated with" tagline of the coding tool;
#   * a link to the tool's hosted-session pages;
#   * a commit author address at the vendor's domain.
# Matching is on the SHAPE of those lines, case-insensitive; ordinary prose
# that merely mentions the vendor or the model is fine. The one sanctioned
# credit, "Discovered by <vendor>'s OSS Scanner, as vulnerability ANT-...",
# is plain prose and therefore passes without a special case.
#
# Inputs are files, never shell strings: pull request text is untrusted, so
# the workflow writes it to files (or passes it through the environment) and
# this script only ever reads those files and feeds them to grep. Nothing is
# eval'd or interpolated.
#
#   --body-from FILE      the pull request description (may be empty)
#   --commits-from FILE   JSON array as returned by the pulls/<n>/commits API
#                         (only this pull request's commits, never history)
#   --message-from FILE   one commit message to scan (merge-queue event)
#   --author-email EMAIL  one author address to scan (merge-queue event)
#   --pr-author LOGIN     the pull request author; dependabot and renovate
#                         are exempt (their commits and bodies are skipped)
#   --self-test           run the built-in cases
#
# Events: pull_request is checked in full. merge_group has no pull request
# body; the workflow passes the queue commit's message (the squash text built
# from the pull request) via --message-from, and an empty or absent message
# passes with a notice instead of failing. push events are not scanned, so
# merged history is never re-read.
set -euo pipefail

BOT_RE='^(dependabot|renovate)(\[bot\])?$'

# Rule table: <class><TAB><ERE>; all matched case-insensitively per line.
RULES=$'co-author trailer\t^[[:space:]>]*co-authored-by:.*(claude|anthropic)\n'
RULES+=$'session trailer\t^[[:space:]>]*claude-session:\n'
RULES+=$'generated-with tagline\tgenerated (with|by) \\[?claude( code)?\\]?\n'
RULES+=$'hosted-session link\tclaude\\.ai/code(/|$|[[:space:]])\n'
RULES+=$'vendor address in text trailer\t^[[:space:]>]*co-authored-by:.*@([a-z0-9-]+\\.)*anthropic\\.com'
AUTHOR_RE='@([a-z0-9-]+\.)*anthropic\.com[>[:space:]]*$'

hits=0
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

# scan_text <label> <file>
scan_text() {
  local label="$1" file="$2" cls re out
  while IFS=$'\t' read -r cls re; do
    [[ -n "$cls" ]] || continue
    out="$(grep -n -i -E -e "$re" "$file" || true)"
    if [[ -n "$out" ]]; then
      while IFS= read -r l; do
        echo "::error::${label}: ${cls} at line ${l%%:*}" >&2
      done <<<"$out"
      hits=$((hits + 1))
    fi
  done <<<"$RULES"
}

# scan_author <label> <email>
scan_author() {
  if printf '%s\n' "$2" | grep -q -i -E -e "$AUTHOR_RE"; then
    echo "::error::${1}: commit author address is at the vendor's domain (${2})" >&2
    hits=$((hits + 1))
  fi
}

is_bot() { printf '%s\n' "$1" | grep -q -i -E -e "$BOT_RE"; }

need_file() {
  if [[ ! -f "$2" ]]; then
    echo "::error::${1} '${2}' does not exist; this check cannot run, so it fails rather than passing silently" >&2
    return 1
  fi
}

run_gate() {
  local body="" commits="" message="" email="" pr_author="" tmp="$SCRATCH" i n sha login aemail
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --body-from) body="${2:-}"; shift 2 ;;
      --commits-from) commits="${2:-}"; shift 2 ;;
      --message-from) message="${2:-}"; shift 2 ;;
      --author-email) email="${2:-}"; shift 2 ;;
      --pr-author) pr_author="${2:-}"; shift 2 ;;
      *) echo "unknown argument: $1" >&2; return 2 ;;
    esac
  done
  if [[ -z "$body$commits$message$email" ]]; then
    echo "::error::nothing to check was supplied" >&2
    return 1
  fi
  if [[ -n "$pr_author" ]] && is_bot "$pr_author"; then
    echo "pull request author is a dependency bot; attribution check skipped"
    return 0
  fi
  hits=0

  if [[ -n "$body" ]]; then
    need_file "--body-from" "$body" || return 1
    scan_text "pull request description" "$body"
  fi
  if [[ -n "$message" ]]; then
    need_file "--message-from" "$message" || return 1
    if [[ -s "$message" ]]; then
      scan_text "queue commit message" "$message"
    else
      echo "::notice::no commit message in the event; nothing to scan for the queue commit"
    fi
  fi
  if [[ -n "$email" ]]; then
    scan_author "queue commit" "$email"
  fi
  if [[ -n "$commits" ]]; then
    need_file "--commits-from" "$commits" || return 1
    n="$(jq 'length' "$commits")"
    for ((i = 0; i < n; i++)); do
      sha="$(jq -r ".[$i].sha" "$commits")"
      login="$(jq -r ".[$i].author.login // \"\"" "$commits")"
      if [[ -n "$login" ]] && is_bot "$login"; then
        continue
      fi
      aemail="$(jq -r ".[$i].commit.author.email // \"\"" "$commits")"
      jq -r ".[$i].commit.message // \"\"" "$commits" > "$tmp/msg"
      scan_text "commit ${sha:0:12} message" "$tmp/msg"
      scan_author "commit ${sha:0:12}" "$aemail"
    done
  fi

  if [[ $hits -ne 0 ]]; then
    echo "::error::AI-attribution gate FAILED: remove the attribution lines from the pull request description and the commit messages, and re-author commits that carry a vendor address" >&2
    return 1
  fi
  echo "AI-attribution gate passed."
}

self_test() {
  local work="$SCRATCH" n=0 fails=0
  # Fixtures are assembled from fragments so this file never carries a
  # blocked line itself.
  local CO="Co-Authored""-By" CS="Claude""-Session" CL="Cla""ude" AN="Anthro""pic"
  local CLL="cla""ude" DOM="anthro""pic.com" CC="Claude"" Code"

  # case <expect: pass|fail> <description> <body> <commit message> <author email> [login] [pr-author]
  case_() {
    local expect="$1" desc="$2" body="$3" msg="$4" email="$5" login="${6:-}" pra="${7:-}" rc=0
    n=$((n + 1))
    printf '%s\n' "$body" > "$work/body"
    jq -n --arg m "$msg" --arg e "$email" --arg l "$login" \
      '[{sha:"0123456789abcdef", commit:{message:$m, author:{email:$e}}, author:(if $l=="" then null else {login:$l} end)}]' \
      > "$work/commits"
    run_gate --body-from "$work/body" --commits-from "$work/commits" --pr-author "$pra" \
      >/dev/null 2>&1 || rc=$?
    if [[ "$expect" == fail && $rc -eq 1 ]] || [[ "$expect" == pass && $rc -eq 0 ]]; then
      echo "  ok    ${desc}"
    else
      echo "  FAIL  ${desc} (expected ${expect}, rc=${rc})" >&2
      fails=$((fails + 1))
    fi
  }

  echo "Self-test: AI-attribution gate"
  local OK='dev@example.com'
  # blocked, in a commit message
  case_ fail "message: co-author trailer, model name" "" "fix: x

${CO}: ${CL} Sonnet 5.5 <noreply@${DOM}>" "$OK"
  case_ fail "message: co-author trailer, vendor name only" "" "fix: x

${CO}: ${AN} <bot@example.com>" "$OK"
  case_ fail "message: co-author trailer, lowercase" "" "fix: x

$(printf '%s' "$CO" | tr "[:upper:]" "[:lower:]"): ${CL} Opus <a@example.com>" "$OK"
  case_ fail "message: co-author trailer, vendor address only" "" "fix: x

${CO}: Bot <noreply@${DOM}>" "$OK"
  case_ fail "message: session trailer" "" "fix: x

${CS}: https://example.invalid/s/1" "$OK"
  case_ fail "message: generated-with tagline" "" "fix: x

Generated with ${CC}" "$OK"
  case_ fail "message: tagline with link markup" "" "fix: x

Generated with [${CC}](https://example.invalid)" "$OK"
  case_ fail "message: hosted-session link" "" "fix: x

https://${CLL}.ai/code/session_01ABC" "$OK"
  case_ fail "message: author address at the vendor" "" "fix: x" "someone@${DOM}"
  case_ fail "message: author address at a vendor subdomain" "" "fix: x" "someone@mail.${DOM}"
  # blocked, in the description
  case_ fail "body: co-author trailer" "Summary

${CO}: ${CL} <noreply@${DOM}>" "fix: x" "$OK"
  case_ fail "body: session trailer" "${CS}: abc" "fix: x" "$OK"
  case_ fail "body: tagline" "Generated with ${CC}" "fix: x" "$OK"
  case_ fail "body: hosted-session link" "see https://${CLL}.ai/code/session_01ABC" "fix: x" "$OK"
  case_ fail "body: quoted trailer" "> ${CO}: ${CL} <a@b.invalid>" "fix: x" "$OK"
  # allowed
  case_ pass "sanctioned scanner credit (body)" "Discovered by ${AN}'s OSS Scanner, as vulnerability ANT-2026-00000" "fix: x" "$OK"
  case_ pass "sanctioned scanner credit (message)" "" "fix: x

Discovered by ${AN}'s OSS Scanner, as vulnerability ANT-2026-00000" "$OK"
  case_ pass "prose naming the vendor and the model" "We compared ${CL} and other models; ${AN} publishes the docs." "docs: mention ${CL} in the tooling notes" "$OK"
  case_ pass "human co-author" "" "fix: x

${CO}: Jane Roe <jane@example.com>" "$OK"
  case_ pass "address at a lookalike domain" "" "fix: x" "someone@not${DOM}"
  case_ pass "plain commit" "Fixes a bug." "fix: a bug" "$OK"
  case_ pass "bot author, attribution-shaped text skipped" "" "chore: bump

${CO}: ${CL} <noreply@${DOM}>" "49699333+dependabot[bot]@users.noreply.github.com" "dependabot[bot]"
  case_ pass "bot pull request author skipped" "${CS}: x" "chore: bump" "$OK" "" "renovate[bot]"

  # events and inputs
  local rc
  : > "$work/empty"
  run_gate --message-from "$work/empty" >/dev/null 2>&1 && rc=0 || rc=$?; n=$((n + 1))
  if [[ $rc -eq 0 ]]; then echo "  ok    merge queue: empty message passes with a notice"; else echo "  FAIL  merge queue: empty message" >&2; fails=$((fails + 1)); fi
  printf 'fix: x\n\n%s: %s <noreply@%s>\n' "$CO" "$CL" "$DOM" > "$work/mq"
  run_gate --message-from "$work/mq" --author-email "$OK" >/dev/null 2>&1 && rc=0 || rc=$?; n=$((n + 1))
  if [[ $rc -eq 1 ]]; then echo "  ok    merge queue: attributed message fails"; else echo "  FAIL  merge queue: attributed message (rc=${rc})" >&2; fails=$((fails + 1)); fi
  run_gate --body-from "$work/nope" >/dev/null 2>&1 && rc=0 || rc=$?; n=$((n + 1))
  if [[ $rc -eq 1 ]]; then echo "  ok    missing input file fails closed"; else echo "  FAIL  missing input file (rc=${rc})" >&2; fails=$((fails + 1)); fi
  run_gate >/dev/null 2>&1 && rc=0 || rc=$?; n=$((n + 1))
  if [[ $rc -eq 1 ]]; then echo "  ok    no inputs fails closed"; else echo "  FAIL  no inputs (rc=${rc})" >&2; fails=$((fails + 1)); fi

  echo
  if [[ $fails -ne 0 ]]; then
    echo "FAILED: ${fails} of ${n} self-test cases did not behave as specified." >&2
    return 1
  fi
  echo "OK: all ${n} self-test cases behave as specified."
}

if [[ "${1:-}" == "--self-test" ]]; then
  self_test
  exit $?
fi
run_gate "$@"
