#!/bin/bash

# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2026 We-Amp B.V.
#
# classify-js-fuzz-artifacts.sh - bucket libFuzzer crash artifacts from the
# JS minify harness (pagespeed/kernel/js/js_minify_fuzz.cc) against the
# documented known-find classes, so the nightly fuzz job
# (the nightly CSS-fuzz lane, fuzz-js) posts only
# POTENTIALLY-NEW findings to the js-fuzz-finding tracking issue instead of
# re-filing the disclosed over-catch classes every night. Port of the
# optimizer's tools/ci/classify-css-fuzz-artifacts.sh to the JS harness's
# oracles and its expected artifact profile (operator-glomming over-catches
# dominate: the disclosed invalid-input umbrella class `= =`->`==`,
# `? ?`->`??`, ... — 14/16 shapes trip the token-channel oracle).
#
#   classify-js-fuzz-artifacts.sh REPLAY_BIN ARTIFACT_DIR FINDINGS_MD
#   classify-js-fuzz-artifacts.sh --self-test
#
# REPLAY_BIN is the harness's deterministic (non-libFuzzer) build. Unlike
# the 2.0 optimizer line CSS harness, its file-replay path does NOT abort on an oracle
# trip — it prints `idempotence trip: <file>` / `token-channel trip:
# <file>` to stderr and exits 1 if any trip was seen (a sanitizer report
# still aborts). Each artifact is replayed singly and bucketed by oracle
# plus two input-axis proxies:
#
#   * garbage proxy (UTF-8-aware, python3): bytes that are neither
#     ASCII-printable/tab/LF/CR nor part of a VALID UTF-8 multibyte
#     sequence. Valid JS carries high bytes (unicode idents, raw
#     U+2028/U+2029 in strings — legal since ES2019), so a high byte alone
#     is NOT proof of garbage (the CSS classifier's review lesson).
#   * validity proxy (tools/ci/js-parse-check.cjs via node, present on the
#     runners; NODE_BIN/PARSE_CHECK overridable for the self-test): an
#     HONEST two-goal parse — script goal (vm.Script raw + CJS-wrapped)
#     and module goal (vm.SourceTextModule / .mjs --check). node --check
#     itself is unusable: node v24's CJS --check path ESM-fallback returns
#     rc=0 on input unparseable in BOTH goals whenever an import/export
#     token breaks the CJS parse (the minifier-rewrite triage measured it: `export&\n&` passes
#     --check silently; that quirk misrouted an entire nightly
#     needs-triage bucket).
#
# Buckets (one proxy per oracle, matched to its contract):
#   * sanitizer report (ASan/UBSan)                    -> NEEDS TRIAGE (never
#     a known-find class).
#   * idempotence trip on garbage input (UTF-8 proxy)  -> KNOWN (invalid-
#     input-only residual family; counted, not re-filed). The idempotence
#     oracle IS the decline-path contract guardian, so its printable axis
#     stays strict: any non-garbage trip — even node-invalid — surfaces.
#   * idempotence trip on non-garbage input            -> NEEDS TRIAGE.
#   * token-channel trip on input node CANNOT parse    -> KNOWN invalid-
#     input umbrella (counted, not re-filed; sub-split into the disclosed
#     operator-glomming class vs other invalid shapes for reporting).
#     Parseability is the servability axis: a script node/V8 rejects is
#     rejected by every consumer, so a token-stream difference on it can
#     corrupt nothing servable — this covers the disclosed `= =`->`==`
#     umbrella AND the long tail of mutated-garbage over-catches without a
#     fragile shape regex on the gating path (the glom predicate only
#     sub-classifies). The decline-path contract on invalid input remains
#     guarded by the idempotence oracle above, not weakened here.
#   * token-channel trip on input node CAN parse       -> NEEDS TRIAGE,
#     ALWAYS. This is the valid-input corruption class the lane exists for; nothing
#     in the known-bucket path may swallow it. (If node itself is missing
#     on the runner, js_valid fails open: everything channels to TRIAGE.)
#   * replays clean                                    -> STALE (counted).
#
# FINDINGS_MD is written in APPEND mode — the caller owns truncation (the
# workflow truncates once at step start, then seeds its own entries, e.g.
# replay-floor red / campaign failure, BEFORE invoking this script; those
# entries must survive classification — the optimizer line re-review lesson). An
# empty file afterward means nothing needs triage (the semantics
# tools/sbom/post-findings-issue.sh keys on: empty -> auto-close the
# tracking issue). Bucket counts go to stdout (the workflow tees them into
# GITHUB_STEP_SUMMARY). Exit 0 unless mis-invoked.
#
# --self-test runs the fixture suite at the bottom (wired into the PR
# fuzz-smoke job): predicate unit checks plus end-to-end bucket assertions
# against a mock replay binary and a mock node. Exit 0 on pass, 1 with
# diagnostics on failure.

set -uo pipefail

usage() {
  echo "Usage: $0 REPLAY_BIN ARTIFACT_DIR FINDINGS_MD" >&2
  echo "       $0 --self-test" >&2
  exit 2
}

NODE_BIN="${NODE_BIN:-node}"
# The honest two-goal parse helper living next to this script (overridable
# for the self-test).
PARSE_CHECK="${PARSE_CHECK:-$(dirname "$0")/js-parse-check.cjs}"

# garbage_bytes FILE — prints the count of bytes that are neither
# ASCII-printable/tab/LF/CR nor part of a VALID UTF-8 multibyte sequence
# (python3 is guaranteed on the runners; overlong forms and lone
# continuation bytes fail to decode and count as garbage).
garbage_bytes() {
  python3 - "$1" <<'PYEOF'
import sys

data = open(sys.argv[1], "rb").read()
bad = 0
i = 0
n = len(data)
while i < n:
    b = data[i]
    if b in (9, 10, 13) or 0x20 <= b <= 0x7E:
        i += 1
        continue
    decoded = False
    for width in (2, 3, 4):
        seg = data[i:i + width]
        if len(seg) < width:
            break
        try:
            ch = seg.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if len(ch) == 1 and ord(ch) >= 0x80:
            i += width
            decoded = True
            break
    if not decoded:
        bad += 1
        i += 1
print(bad)
PYEOF
}

# js_valid FILE — rc 0 iff the input parses in ANY serving goal, per
# tools/ci/js-parse-check.cjs (script goal raw + CJS-wrapped, module goal).
# node --check itself is NOT a usable proxy: node v24's CJS --check path has
# an ESM-detection fallback that returns rc=0 for input unparseable in both
# goals whenever an import/export token breaks the CJS parse (the
# minifier-rewrite triage measured it — `export&\n&` passes --check silently; execution, vm.Script and
# .mjs --check all reject).
js_valid() {
  "$NODE_BIN" "$PARSE_CHECK" "$1" >/dev/null 2>&1
}

# has_glom_pair FILE — rc 0 iff the input contains two punctuator characters
# separated ONLY by whitespace and/or comments whose concatenation is a
# valid multi-char JS punctuator (the operator-glomming mechanism:
# `= =`->`==`, `& =`->&=, `= >`->`=>`, `. 0`->`.0`, ...). Multiline- and
# comment-aware (python): the minifier-rewrite triage measured the old line-based grep
# at a ~3x undercount (217 vs 616 mechanism-exact) because [[:space:]]
# never matches '\n' and comments between the pair chars were missed.
# HEURISTIC sub-classification only — gating runs on the parseability axis,
# never on this predicate. Consulted only on unparseable input; on valid JS
# such pairs are either impossible or already correct (`a + +b`).
has_glom_pair() {
  python3 - "$1" <<'PYEOF'
import re
import sys

data = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
# Naive comment strip (not string-aware — heuristic sub-count only).
data = re.sub(r"/\*.*?\*/", "", data, flags=re.S)
data = re.sub(r"//[^\n]*", "", data)
# Multi-char JS punctuators a whitespace/comment-separated pair can glue
# into (incl. the comment openers themselves).
MULTI = {
    "==", "!=", "<=", ">=", "&&", "||", "??", "++", "--", "**", "<<",
    ">>", ">>>", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "=>",
    "?.", "...", "//", "/*", "??=", "&&=", "||=", "<<=", ">>=", ">>>=",
    "**=",
}
for m in re.finditer(r"(\S)\s+(?=(\S))", data):
    a, b = m.group(1), m.group(2)
    if a + b in MULTI:
        sys.exit(0)
    # Number/spread glom family: `. 0` -> `.0`, `5 ...` -> `5...`.
    if a == "." and b.isdigit():
        sys.exit(0)
    if a.isdigit() and b == ".":
        sys.exit(0)
sys.exit(1)
PYEOF
}

# --- classification loop -------------------------------------------------
known_idem=0
known_glom=0
known_invalid=0
stale=0
triage=0

classify_dir() {  # $1=replay bin $2=artifact dir $3=findings md out
  local REPLAY="$1" ART_DIR="$2" OUT="$3"
  # APPEND semantics: the caller owns truncation (see header).
  [[ -e "$OUT" ]] || : > "$OUT"
  # Counters are per-call (the self-test classifies several dirs in one
  # process).
  known_idem=0; known_glom=0; known_invalid=0; stale=0; triage=0
  # Per-call (honors NODE_BIN/PARSE_CHECK overrides): fail OPEN — without
  # node or the helper there is no validity axis, and the token-channel
  # bucket must never silently swallow; every channel trip becomes TRIAGE.
  local NODE_OK=1
  command -v "$NODE_BIN" >/dev/null 2>&1 || NODE_OK=0
  [[ -f "$PARSE_CHECK" ]] || NODE_OK=0
  local artifact name stderr_file rc oracle nonprint oracle_desc
  shopt -s nullglob
  for artifact in "${ART_DIR}"/crash-* "${ART_DIR}"/oom-* "${ART_DIR}"/timeout-*; do
    name="$(basename "$artifact")"
    oracle_desc=""
    stderr_file="$(mktemp)"
    "$REPLAY" "$artifact" >/dev/null 2>"$stderr_file"
    rc=$?
    oracle=""
    if grep -q "ERROR: AddressSanitizer\|runtime error:" "$stderr_file"; then
      oracle="sanitizer"
    elif grep -q "idempotence trip: ${artifact}\|idempotence trip: .*/${name}" "$stderr_file"; then
      oracle="idempotence"
    elif grep -q "token-channel trip: ${artifact}\|token-channel trip: .*/${name}" "$stderr_file"; then
      oracle="token-channel"
    elif [[ $rc -eq 0 ]]; then
      oracle="clean"
    else
      oracle="unknown-rc${rc}"
    fi
    rm -f "$stderr_file"

    case "$oracle" in
      clean)
        stale=$((stale + 1)) ;;
      idempotence)
        nonprint="$(garbage_bytes "$artifact")"
        if [[ "$nonprint" -gt 0 ]]; then
          known_idem=$((known_idem + 1))
        else
          oracle_desc="strict idempotence oracle on NON-garbage input — possible valid-JS or decline-contract non-idempotence (new)"
          triage=$((triage + 1))
        fi ;;
      token-channel)
        if [[ "$NODE_OK" == "1" ]] && ! js_valid "$artifact"; then
          # Unparseable = unservable: invalid-input over-catch umbrella.
          if has_glom_pair "$artifact"; then
            known_glom=$((known_glom + 1))
          else
            known_invalid=$((known_invalid + 1))
          fi
        else
          oracle_desc="token-channel oracle on PARSEABLE input — possible valid-JS token-stream corruption (new; the disclosed over-catches are all invalid-input)"
          triage=$((triage + 1))
        fi ;;
      *)
        oracle_desc="${oracle} — memory-safety or unclassified failure (never a known-find class)"
        triage=$((triage + 1)) ;;
    esac

    if [[ -n "$oracle_desc" ]]; then
      {
        echo "### \`${name}\` — ${oracle_desc}"
        echo
        echo '```'
        base64 < "$artifact" | head -4
        echo '```'
        echo
      } >> "$OUT"
    fi
  done

  echo "js-fuzz triage: ${triage} needs-triage, ${known_idem} known-idempotence-garbage, ${known_glom} known-operator-glom, ${known_invalid} known-invalid-input, ${stale} stale/clean"
  if [[ "$triage" -gt 0 ]]; then
    {
      echo "_$((known_idem + known_glom + known_invalid)) further artifact(s) matched the documented known-find classes (idempotence-on-garbage: ${known_idem}; invalid-input token-channel umbrella: $((known_glom + known_invalid)), of which operator-glomming: ${known_glom}) and are NOT re-filed here; ${stale} artifact(s) replayed clean (stale). See the run summary for the full count and the \`js-fuzz-evidence\` artifact for every crash unit._"
      echo
    } >> "$OUT"
  fi
}

# --- self-test -------------------------------------------------------------
# Fixtures + mock tools (mock replay prints the harness's trip lines per
# fixture; mock node declares inputs containing MOCKINVALID unparseable).
# Asserts each fixture's bucket WITHOUT depending on oracle behavior.
self_test() {
  local tmp fails=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  mkdir -p "$tmp/artifacts"

  # The operator-glomming umbrella fixture: printable, INVALID JS, `= =`.
  printf 'a = = b; //MOCKINVALID\n' > "$tmp/artifacts/crash-glom"
  # Invalid JS WITHOUT a glom pair: invalidity alone must NOT bucket.
  printf 'a b c ){  //MOCKINVALID\n' > "$tmp/artifacts/crash-noglom"
  # Valid JS, channel trip: always triage (the lane's reason to exist).
  printf 'function f() {\n  return\n  42;\n}\n' > "$tmp/artifacts/crash-valid"
  # Module-only-parseable (import/export): script-mode --check rejects,
  # module-mode accepts — exercises the js_valid fallback (a channel trip
  # here is VALID input and must triage).
  printf "import def from './m.js';\nexport {}; //MOCKMODULE\n" > "$tmp/artifacts/crash-module"
  printf 'x:\xff\xfe\xff' > "$tmp/artifacts/crash-garbage"
  # Valid JS with multibyte content (U+00E9): NOT garbage (UTF-8 lesson).
  printf 'x = "caf\xc3\xa9";\n' > "$tmp/artifacts/crash-utf8"
  printf 'y:\x00\x01' > "$tmp/artifacts/crash-sanitizer"

  cat > "$tmp/replay-mock" <<'EOF'
#!/bin/bash
# Mock of the harness's deterministic replay: file-replay prints trip
# lines (no abort), sanitizer reports print the ASan marker.
case "$(basename "$1")" in
  crash-glom|crash-noglom|crash-valid|crash-module|crash-utf8-channel)
    echo "token-channel trip: $1" >&2; exit 1 ;;
  crash-garbage|crash-utf8)
    echo "idempotence trip: $1" >&2; exit 1 ;;
  crash-sanitizer)
    echo "ERROR: AddressSanitizer: heap-buffer-overflow" >&2; exit 1 ;;
esac
echo "js minify fuzz harness: 16 seeds clean; 1 files replayed, 0 idempotence trips, 0 token-channel trips" >&2
exit 0
EOF
  chmod +x "$tmp/replay-mock"

  cat > "$tmp/node-mock" <<'EOF'
#!/bin/bash
# Mock of the js-parse-check helper invocation (args: HELPER FILE — the
# file is the last argument). Content markers: MOCKINVALID -> unparseable
# in every goal (rc 1); anything else -> parseable (rc 0). The helper's
# contract is any-goal-accepts, so the module-fallback case is rc 0 here;
# the REAL fallback is exercised against real node below.
content="$(cat "${@: -1}")"
case "$content" in
  *MOCKINVALID*) exit 1 ;;
esac
exit 0
EOF
  chmod +x "$tmp/node-mock"

  check() {  # $1=label $2=expected $3=actual
    if [[ "$2" == "$3" ]]; then
      echo "ok: $1"
    else
      echo "FAIL: $1 — expected [$2], got [$3]" >&2
      fails=$((fails + 1))
    fi
  }

  # Predicate units.
  check "valid UTF-8 (café) is NOT garbage" "0" "$(garbage_bytes "$tmp/artifacts/crash-utf8")"
  [[ "$(garbage_bytes "$tmp/artifacts/crash-garbage")" -gt 0 ]]
  check "0xff bytes ARE garbage" "0" "$?"
  has_glom_pair "$tmp/artifacts/crash-glom"
  check "'= =' IS a glom pair" "0" "$?"
  printf 'a == b;\n' > "$tmp/glom-neg"
  has_glom_pair "$tmp/glom-neg"
  check "'==' is NOT a glom pair" "1" "$?"
  printf 'x+++++y;\n' > "$tmp/glom-neg2"
  has_glom_pair "$tmp/glom-neg2"
  check "'x+++++y' (no whitespace) is NOT a glom pair" "1" "$?"
  NODE_BIN="$tmp/node-mock" js_valid "$tmp/artifacts/crash-valid"
  check "valid JS parses (mock helper)" "0" "$?"
  NODE_BIN="$tmp/node-mock" js_valid "$tmp/artifacts/crash-glom"
  check "MOCKINVALID JS does NOT parse (mock helper)" "1" "$?"
  # Glom predicate: newline- and comment-tolerant (the measured undercount).
  printf '&\n=' > "$tmp/glom-nl"
  has_glom_pair "$tmp/glom-nl"
  check "'&<newline>=' IS a glom pair (multiline)" "0" "$?"
  printf 'var d=//\n>try' > "$tmp/glom-comment"
  has_glom_pair "$tmp/glom-comment"
  check "'=//<newline>>' IS a glom pair (comment-tolerant)" "0" "$?"
  NODE_BIN="$tmp/node-mock" PARSE_CHECK=mock js_valid "$tmp/artifacts/crash-module"
  check "module-only JS parses (helper any-goal contract)" "0" "$?"

  # End-to-end buckets through the mock replay + mock node.
  summary="$(NODE_BIN="$tmp/node-mock" classify_dir "$tmp/replay-mock" "$tmp/artifacts" "$tmp/findings.md")"
  echo "$summary"
  check "needs-triage count (valid + module + utf8 + sanitizer)" \
    "4" "$(printf '%s' "$summary" | sed -E 's/.*triage: ([0-9]+).*/\1/')"
  check "known-glom count" \
    "1" "$(printf '%s' "$summary" | sed -E 's/.*, ([0-9]+) known-operator-glom.*/\1/')"
  check "known-invalid-input count" \
    "1" "$(printf '%s' "$summary" | sed -E 's/.*, ([0-9]+) known-invalid-input.*/\1/')"
  check "known-idempotence-garbage count" \
    "1" "$(printf '%s' "$summary" | sed -E 's/.*triage: [0-9]+ needs-triage, ([0-9]+) known-idempotence.*/\1/')"
  check "findings lists crash-valid" "1" "$(grep -c 'crash-valid' "$tmp/findings.md")"
  check "findings lists crash-module" "1" "$(grep -c 'crash-module' "$tmp/findings.md")"
  check "findings lists crash-utf8" "1" "$(grep -c 'crash-utf8' "$tmp/findings.md")"
  check "findings lists crash-sanitizer" "1" "$(grep -c 'crash-sanitizer' "$tmp/findings.md")"
  check "findings OMITS crash-glom" "0" "$(grep -c 'crash-glom' "$tmp/findings.md")"
  check "findings OMITS crash-noglom" "0" "$(grep -c 'crash-noglom' "$tmp/findings.md")"
  check "findings OMITS crash-garbage" "0" "$(grep -c 'crash-garbage' "$tmp/findings.md")"

  # Fail-open: with node missing there is no validity axis, so even the
  # glom fixture must surface as TRIAGE rather than be swallowed.
  : > "$tmp/findings-nonode.md"
  NODE_BIN="$tmp/no-such-node" classify_dir "$tmp/replay-mock" "$tmp/artifacts" "$tmp/findings-nonode.md" >/dev/null
  check "node missing -> glom fixture still surfaces (fail-open)" \
    "1" "$(grep -c 'crash-glom' "$tmp/findings-nonode.md")"

  # Pre-existing findings content must SURVIVE classify_dir (the workflow
  # seeds replay-floor / campaign-failure entries before invoking the
  # classifier; an empty findings file is the auto-close signal).
  printf '### pre-existing entry (must survive)\n' > "$tmp/seeded.md"
  NODE_BIN="$tmp/node-mock" classify_dir "$tmp/replay-mock" "$tmp/artifacts" "$tmp/seeded.md" >/dev/null
  check "pre-existing findings entry survives classify_dir" \
    "1" "$(grep -c 'pre-existing entry' "$tmp/seeded.md")"

  # --- real-helper checks (gated on a real node) -------------------------
  # The mock exercises js_valid's routing; these exercise the helper's
  # actual two-goal parse, incl. the node v24 --check ESM-fallback quirk
  # that misrouted a nightly bucket: `export&\n&` passes `node --check`
  # while being unparseable in BOTH goals.
  HELPER="$(dirname "$0")/js-parse-check.cjs"
  if command -v node >/dev/null 2>&1 && [[ -f "$HELPER" ]]; then
    helper_rc() { node "$HELPER" "$1" >/dev/null 2>&1; }
    printf 'export&\n&' > "$tmp/q-export-amp.js"
    helper_rc "$tmp/q-export-amp.js"
    check "quirk case 'export&\\n&' is UNPARSEABLE (both goals)" "1" "$?"
    printf 'a&\n&' > "$tmp/q-amp.js"
    helper_rc "$tmp/q-amp.js"
    check "control 'a&\\n&' is unparseable" "1" "$?"
    printf "import def from './m.js';\nexport {};" > "$tmp/q-module.js"
    helper_rc "$tmp/q-module.js"
    check "module-only shape parses (module goal)" "0" "$?"
    printf 'await Promise.resolve(1);' > "$tmp/q-tla.js"
    helper_rc "$tmp/q-tla.js"
    check "top-level await parses (module goal)" "0" "$?"
    printf 'with(x){}' > "$tmp/q-with.js"
    helper_rc "$tmp/q-with.js"
    check "sloppy-only shape parses (script goal)" "0" "$?"
    printf 'return 1' > "$tmp/q-return.js"
    helper_rc "$tmp/q-return.js"
    check "top-level return parses (CJS wrap)" "0" "$?"
    printf 'let//\nv' > "$tmp/q-let.js"
    helper_rc "$tmp/q-let.js"
    check "RC-B control 'let//\\nv' parses" "0" "$?"

    # End-to-end must-bucket-invalid: the minimal quirk case plus
    # three minimized repro shapes from the triage (newline glom,
    # comment-induced glom, spread glom) — token-channel trips on input
    # the OLD proxy (node --check) would have called parseable for the
    # export-carrying case. Mock replay (channel trip), REAL helper.
    mkdir -p "$tmp/artifacts2"
    printf 'export&\n&' > "$tmp/artifacts2/crash-export-amp"
    printf '&\n=' > "$tmp/artifacts2/crash-nl-glom"
    printf 'var d=//\n>try{}catch{}' > "$tmp/artifacts2/crash-comment-glom"
    printf '5 ...' > "$tmp/artifacts2/crash-spread-glom"
    # RC-B control: VALID let<comment> input trips the channel oracle and
    # must STAY needs-triage until the comparator fix lands.
    printf 'let//\nv' > "$tmp/artifacts2/crash-let-comment"
    cat > "$tmp/replay-mock2" <<'EOF'
#!/bin/bash
echo "token-channel trip: $1" >&2
exit 1
EOF
    chmod +x "$tmp/replay-mock2"
    summary2="$(NODE_BIN=node classify_dir "$tmp/replay-mock2" "$tmp/artifacts2" "$tmp/findings2.md")"
    echo "$summary2"
    check "regression fixtures: none needs-triage except the RC-B control" \
      "1" "$(printf '%s' "$summary2" | sed -E 's/.*triage: ([0-9]+).*/\1/')"
    check "findings2 OMITS crash-export-amp" "0" "$(grep -c 'crash-export-amp' "$tmp/findings2.md")"
    check "findings2 OMITS crash-nl-glom" "0" "$(grep -c 'crash-nl-glom' "$tmp/findings2.md")"
    check "findings2 OMITS crash-comment-glom" "0" "$(grep -c 'crash-comment-glom' "$tmp/findings2.md")"
    check "findings2 OMITS crash-spread-glom" "0" "$(grep -c 'crash-spread-glom' "$tmp/findings2.md")"
    check "findings2 LISTS crash-let-comment (RC-B stays triage)" \
      "1" "$(grep -c 'crash-let-comment' "$tmp/findings2.md")"
  else
    echo "skip: real node or helper unavailable — helper/quirk and regression checks skipped"
  fi

  if [[ "$fails" -eq 0 ]]; then
    echo "classify-js-fuzz-artifacts self-test: all checks passed"
    return 0
  fi
  echo "classify-js-fuzz-artifacts self-test: ${fails} check(s) FAILED" >&2
  return 1
}

if [[ "${1:-}" == "--self-test" ]]; then
  self_test
  exit $?
fi

[[ $# -eq 3 ]] || usage
REPLAY="$1"
ART_DIR="$2"
OUT="$3"
[[ -x "$REPLAY" ]] || { echo "replay binary not executable: $REPLAY" >&2; usage; }
[[ -d "$ART_DIR" ]] || { echo "artifact dir missing: $ART_DIR" >&2; usage; }

classify_dir "$REPLAY" "$ART_DIR" "$OUT"
exit 0
