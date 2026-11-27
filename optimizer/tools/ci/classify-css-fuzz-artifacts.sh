#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# classify-css-fuzz-artifacts.sh - bucket libFuzzer crash artifacts from the
# CSS minify harness (lib/css/css_minify_fuzz.cc) against the audit-documented
# known-find classes, so the scheduled CSS-fuzz lane
# (job fuzz-css) posts only POTENTIALLY-NEW findings to the tracking issue
# instead of re-filing the documented residuals every night.
#
#   classify-css-fuzz-artifacts.sh REPLAY_BIN ARTIFACT_DIR FINDINGS_MD
#   classify-css-fuzz-artifacts.sh --self-test
#
# REPLAY_BIN is the harness's deterministic (non-libFuzzer) build; each
# artifact is replayed through it and bucketed by which oracle fired plus an
# input-validity proxy (the documented residual classes are all invalid-input
# garbage):
#
#   * "idempotence violation" on garbage input
#       -> KNOWN garbage-rejoin class (fixed on main — the strict
#          oracle's last documented red-line; the bucket is a legacy
#          safety net for pre-fix artifacts and future idempotence
#          regressions on garbage; counted, not re-filed).
#   * "token-stream violation" on garbage input OR with an unterminated
#     url( at EOF OR the PROVEN escape-glue shape (input/output differ
#     only in removed whitespace AND every removed-whitespace run
#     immediately follows escape content in the minifier's
#     space-removal set), AND the
#     region is outside strings and unquoted-url() interiors (the
#     minifier removes no whitespace there — any diff inside a string
#     or url interior is real corruption, e.g. "content:'; x'" ->
#     "';x'" and "url(a; b.png)" -> "url(a;b.png)" stay needs-triage)
#     OR the custom-region ';'-drop shape (input/output differ ONLY in
#     deleted ';' chars immediately before '}' inside custom-property
#     regions — Phase 3's trailing-';' trim reaching into a malformed
#     custom value, invalid-input by construction; run 30913256122's
#     crash-dc9b2139, #1238 triage)
#       -> KNOWN token-oracle over-catch (safety net: the disclosed
#          shapes — NormalizeUrl's escape-unaware ')' strip, collapse
#          reorder/dedup, and the v2 escape-glue tokenization
#          disagreement — are behavior-preserving by construction; the
#          bucket stays so a future over-catch shape is counted rather
#          than re-filed nightly).
#   * a sanitizer report (ASan/UBSan), or EITHER oracle on non-garbage input
#       -> NEEDS TRIAGE: potential new finding; listed individually in
#          FINDINGS_MD with the artifact name and a base64 of the first
#          bytes. (A non-garbage token-oracle trip can still be the
#          disclosed string-reorder over-catch — the issue text says so;
#          better one false triage than a missed #1158-class corruption.)
#   * replays clean -> STALE (fixed between the fuzz run and triage, or a
#     fork-mode duplicate); counted, not reported.
#
# "Garbage" is UTF-8-AWARE (review #1176): valid CSS carries high bytes —
# non-ASCII code points are legal in idents (CSS Syntax 3) and in
# content/font-family strings ("content:'café'") — so a byte outside
# printable ASCII is NOT proof of garbage. garbage_bytes() counts only
# bytes that are neither ASCII-printable/tab/LF/CR nor part of a VALID
# UTF-8 multibyte sequence; multibyte handling is a classic minifier bug
# class and a valid-UTF-8 trip must reach a human.
#
# FINDINGS_MD is written in APPEND mode — the caller owns truncation (the
# workflow truncates once at step start, then seeds its own entries, e.g.
# replay-floor-red / campaign-failed, BEFORE invoking this script; those
# entries must survive classification). An empty file afterward means
# nothing needs triage — the semantics tools/sbom/post-findings-issue.sh
# keys on (empty -> auto-close the tracking issue). Known-class and stale
# counts go to stdout (the workflow tees them into GITHUB_STEP_SUMMARY).
# Exit 0 unless mis-invoked; the lane never hard-fails on findings.
#
# --self-test runs the fixture suite at the bottom (wired into the PR smoke
# lane): predicate unit checks plus end-to-end bucket assertions against a
# mock replay binary. Exit 0 on pass, 1 with diagnostics on failure.

set -uo pipefail

usage() {
  echo "Usage: $0 REPLAY_BIN ARTIFACT_DIR FINDINGS_MD" >&2
  echo "       $0 --self-test" >&2
  exit 2
}

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

# is_unterminated_url FILE — rc 0 iff the input contains "url(" with NO ')'
# anywhere after the LAST "url(" (an unterminated url( at EOF — the token
# oracle's disclosed printable over-catch class). A ')' before the last
# url( is irrelevant; a ')' after it terminates the token.
is_unterminated_url() {
  grep -q 'url(' "$1" || return 1
  # Everything after the last "url(" (greedy .*): unterminated iff no ')'.
  local rest
  rest="$(tr -d '\n' < "$1" | sed 's/.*url(//')"
  [[ "$rest" != *")"* ]]
}

# is_ws_only_diff ARTIFACT STDERR_FILE — rc 0 iff the token-stream
# violation is the PROVEN escape-glue over-catch class and nothing
# else: input and output bytes differ only in removed whitespace, AND
# every removed-whitespace run is immediately after escape content in
# the minifier's space-removal set ('{' '}' ';' ':' ',' '>' '+' '~',
# odd backslash run) — EXTENDED with '/' and '*', the calc math-mode
# operators, whose adjacent whitespace the minifier removes inside
# calc() and siblings even when it is escape content (the review's
# "sin(////...\/ 0" class: escape glue around calc operators is the
# same behavior-preserving tokenization-only shape).  The tight shape
# is the whole correctness proof: whitespace removal elsewhere is
# SEMANTIC (custom values, strings, url interiors, between numbers)
# and must stay needs-triage — e.g. "a{--x:a b;width:1\/2}" ->
# "--x:ab" is real corruption, the removed space follows 'a' (not in
# the set), so it can never slip this net.
is_ws_only_diff() {
  python3 - "$1" "$2" <<'PYEOF'
import re, sys

data = open(sys.argv[1], "rb").read()
msg = open(sys.argv[2], "rb").read().decode("utf-8", "replace")
m = re.search(r"token-stream violation: \[(.*)\] -> \[(.*)\]", msg, re.S)
if not m:
    sys.exit(1)
a, b = m.group(1), m.group(2)
ws = re.compile(r"\s")
if ws.sub("", a) != ws.sub("", b):
    sys.exit(1)  # Content differs: real catch, not an over-catch.

# String / unquoted-url context mask: positions inside a quoted string
# or an unquoted url() interior.  Mirrors the comparator's own
# tokenization (escape pairs inside strings and url() are consumed).
inside = [False] * (len(a) + 1)
in_str = None
in_url = False
k = 0
while k < len(a):
    c = a[k]
    if in_str is not None:
        inside[k] = True
        if c == "\\" and k + 1 < len(a):
            inside[k + 1] = True
            k += 2
            continue
        if c == in_str:
            in_str = None
        k += 1
        continue
    if in_url:
        inside[k] = True
        if c == "\\" and k + 1 < len(a):
            inside[k + 1] = True
            k += 2
            continue
        if c == ")":
            in_url = False
        k += 1
        continue
    if c in "'\"":
        inside[k] = True
        in_str = c
        k += 1
        continue
    if a[k:k + 3].lower() == "url" and k + 3 < len(a) and a[k + 3] == "(":
        m = k + 4
        while m < len(a) and a[m] in " \t\n\r\f":
            m += 1
        if m >= len(a) or a[m] not in "'\"":
            for q in range(k, k + 4):
                inside[q] = True
            in_url = True
            k += 4
            continue
    k += 1

# Sanctioned whitespace-removal positions, mirroring the minifier:
# AFTER_SET — ws after these chars is removed (CanRemoveSpaceAfter,
# plus calc math-mode's '/' and '*'), so a removed run FOLLOWING one
# (bare or escape content with an odd backslash run) is sanctioned or
# comparator-covered escape glue.
# BEFORE_SET — ws before these chars is removed (CanRemoveSpaceBefore
# plus calc's '/' and '*'), so a removed run PRECEDING one is
# sanctioned.  Stylesheet edges (leading/trailing trims) are always
# sanctioned.  Whitespace anywhere else is SEMANTIC (custom values,
# strings, url interiors, between numbers/idents) and stays
# needs-triage.
AFTER_SET = set("{};:,>+~/*")
BEFORE_SET = set("{};,>+~/*")
i = j = 0
ok = True
def allowed_around(a, i, e):
    # i: start of the removed run; e: end of it.  Returns True iff the
    # removal is at a stylesheet edge or adjacent to a set char.
    if i == 0 or e == len(a):
        return True  # Leading/trailing stylesheet trims.
    # Look back: the char before the run is a bare set char, or the
    # content of an escape pair (set or not — escape glue is
    # comparator-covered).  But a run DIRECTLY after a backslash is
    # escaped whitespace — content, never removable (#1154's class).
    k = i - 1
    if k >= 0 and a[k] == "\\":
        return False
    if a[k] in AFTER_SET:
        return True
    # Look ahead: the char after the run, bare or escape content.
    if e < len(a) and a[e] in BEFORE_SET:
        return True
    if e + 1 < len(a) and a[e] == "\\" and a[e + 1] in BEFORE_SET:
        return True
    return False


while i < len(a):
    if a[i].isspace():
        if inside[i]:
            # Inside a string or url() interior the minifier preserves
            # whitespace verbatim — only EXACT identity is allowed;
            # any change (removal, collapse, reformat) is corruption.
            while i < len(a) and a[i].isspace():
                if j >= len(b) or a[i] != b[j]:
                    ok = False
                    break
                i += 1
                j += 1
            if not ok:
                break
            continue
        e = i
        while e < len(a) and a[e].isspace():
            e += 1
        if j < len(b) and b[j].isspace():
            # Both sides have whitespace here: a collapse or keep,
            # always sanctioned (calc/selector spacing).
            while j < len(b) and b[j].isspace():
                j += 1
            i = e
            continue
        if not allowed_around(a, i, e):
            ok = False
            break
        i = e
        continue
    if j >= len(b) or a[i] != b[j]:
        ok = False  # Non-whitespace difference: real catch.
        break
    i += 1
    j += 1
sys.exit(0 if ok else 1)
PYEOF
}

# is_custom_semi_drop ARTIFACT STDERR_FILE — rc 0 iff every input->output
# difference in the token-stream violation is one of the oracle's
# SANCTIONED diff classes, with AT LEAST ONE custom-region ';'-drop site
# as the anchor (the class the oracle actually flags on).  The custom
# ';'-drop is Phase 3's trailing-semicolon trim reaching into a custom
# region: the oracle's sequence-strict custom channel counts the ';' as
# region content and flags, but the construct is invalid CSS by
# construction — a top-level ';' terminates the declaration, so a custom
# value never legitimately contains one (the region is already
# unclosed-bracket garbage).  Disclosed invalid-input over-catch (run
# 30913256122 crash-dc9b2139; run 30947209282's "--::{c;}" four; run
# 30983672391's two; #1238 triage).  Honest boundary: VALID contexts
# preserve the ';' — the probe set "a{--::{c;}}", "a{--x:{c;}}",
# "a{--x:{c;d;}}", "a{b:{L;}}" (the #1167 value-group tracking) and bare
# "--::{c;}" all replay clean; the trim only fires on malformed input
# (e.g. inside an unclosed paren, where the #1167 protection does not
# reach).
#
# Sanctioned diff classes (each with its justification; every diff must
# match one — there is NO misc escape):
#   1. Custom ';'-before-'}' drop — the anchor class above.  The site is
#      "custom" iff scanning back to the nearest declaration-END
#      boundary (';' or '}' — NOT '{', which a custom value legitimately
#      contains, e.g. "--::{c;}"; run 30947209282) is followed by
#      optional ws and then '--'.
#   2. Plain ';'-before-'}' drop with no custom opener — the oracle's
#      own inherent trailing-';' sanction; it only ever RIDES ALONG
#      (rule: at least one site must be class 1).
#   3. ';'-after-'{' drop — the empty-declaration trim at block start;
#      probed oracle-sanctioned ("x{;p:v}" -> "x{p:v}" replays clean;
#      run 30983672391's crash-210129bf carries one).  An escaped '\{'
#      is content, not a block start — excluded via the odd-backslash
#      lookback.
#   4. Whitespace collapse/removal at the minifier's sanctioned
#      positions — mirrors is_ws_only_diff's allowed_around EXACTLY
#      (both-sides-ws collapse always; removal only at a stylesheet edge
#      or adjacent to a removal-set char, with the escaped-whitespace
#      guard).  Keep the mask/sets/helper in sync with is_ws_only_diff.
#
# HARD REJECTION (the load-bearing insurance): any non-identity
# difference inside a quoted string or an unquoted url() interior is
# corruption — never sanctioned — even when a legitimate custom ';'-drop
# is present elsewhere in the artifact (a real bug riding a known shape
# must still file; cf. the crash-strsemi counter-fixture and run
# 30983672391's triage note).  Any difference matching NO class above
# stays needs-triage ("a{b:c;;}", "a{--x:v;b:{c;}}", "a{--x:c;;d}",
# string/url-interior edits, content changes).
is_custom_semi_drop() {
  python3 - "$1" "$2" <<'PYEOF'
import re, sys

data = open(sys.argv[1], "rb").read()
msg = open(sys.argv[2], "rb").read().decode("utf-8", "replace")
m = re.search(r"token-stream violation: \[(.*)\] -> \[(.*)\]", msg, re.S)
if not m:
    sys.exit(1)
a, b = m.group(1), m.group(2)

# True iff s[pos] is escape content (preceded by an odd backslash run).
def is_escaped(s, pos):
    n = 0
    while pos > n and s[pos - 1 - n] == "\\":
        n += 1
    return n % 2 == 1

# String / unquoted-url context mask — mirrors is_ws_only_diff (keep in
# sync): positions inside a quoted string or an unquoted url() interior.
inside = [False] * (len(a) + 1)
in_str = None
in_url = False
k = 0
while k < len(a):
    c = a[k]
    if in_str is not None:
        inside[k] = True
        if c == "\\" and k + 1 < len(a):
            inside[k + 1] = True
            k += 2
            continue
        if c == in_str:
            in_str = None
        k += 1
        continue
    if in_url:
        inside[k] = True
        if c == "\\" and k + 1 < len(a):
            inside[k + 1] = True
            k += 2
            continue
        if c == ")":
            in_url = False
        k += 1
        continue
    if c in "'\"":
        inside[k] = True
        in_str = c
        k += 1
        continue
    if a[k:k + 3].lower() == "url" and k + 3 < len(a) and a[k + 3] == "(":
        m2 = k + 4
        while m2 < len(a) and a[m2] in " \t\n\r\f":
            m2 += 1
        if m2 >= len(a) or a[m2] not in "'\"":
            for q in range(k, k + 4):
                inside[q] = True
            in_url = True
            k += 4
            continue
    k += 1

# Sanctioned whitespace-removal positions — mirrors is_ws_only_diff
# (keep in sync).
AFTER_SET = set("{};:,>+~/*")
BEFORE_SET = set("{};,>+~/*")
def allowed_around(a, i, e):
    if i == 0 or e == len(a):
        return True  # Leading/trailing stylesheet trims.
    k = i - 1
    if k >= 0 and a[k] == "\\":
        return False  # Escaped whitespace is content (#1154's class).
    if a[k] in AFTER_SET:
        return True
    if e < len(a) and a[e] in BEFORE_SET:
        return True
    if e + 1 < len(a) and a[e] == "\\" and a[e + 1] in BEFORE_SET:
        return True
    return False

i = j = 0
saw_custom = False
ok = True
while i < len(a):
    if j < len(b) and a[i] == b[j]:
        i += 1
        j += 1
        continue
    # A difference.  Inside a string or url() interior: hard rejection.
    if inside[i]:
        ok = False
        break
    c = a[i]
    if c.isspace():
        # Class 4: collapse or removal at sanctioned positions.
        e = i
        while e < len(a) and a[e].isspace():
            e += 1
        if j < len(b) and b[j].isspace():
            while j < len(b) and b[j].isspace():
                j += 1
            i = e
            continue
        if not allowed_around(a, i, e):
            ok = False
            break
        i = e
        continue
    if c == ";" and i + 1 < len(a) and a[i + 1] == "}":
        # Class 1/2: ';'-before-'}' drop.  Custom iff the nearest
        # declaration-END boundary (';' '}') scans back to a '--'
        # opener.
        k = i - 1
        while k >= 0 and a[k] not in ";}":
            k -= 1
        k += 1
        while k < i and a[k] in " \t\n\r\f":
            k += 1
        if a[k:k + 2] == "--":
            saw_custom = True
        i += 1
        continue
    if (c == ";" and i > 0 and a[i - 1] == "{" and not is_escaped(a, i - 1)):
        # Class 3: empty-declaration trim at block start.
        i += 1
        continue
    ok = False
    break
# Trailing output must be fully consumed, and at least one custom site
# must have anchored the flag.
if ok and (j != len(b) or not saw_custom):
    ok = False
sys.exit(0 if ok else 1)
PYEOF
}

# --- classification loop -------------------------------------------------
known_rejoin=0
known_token=0
stale=0
triage=0

classify_dir() {  # $1=replay bin $2=artifact dir $3=findings md out
  local REPLAY="$1" ART_DIR="$2" OUT="$3"
  # APPEND semantics (#1176 re-review): the caller owns truncation. The
  # workflow truncates $FINDINGS at step start, then writes its own
  # entries (replay-floor red, fuzzer campaign failure) BEFORE invoking
  # this classifier — truncating here wiped them, and an empty findings
  # file is the auto-close signal: the gate died on exactly the nights it
  # was built for. Create-if-absent only guarantees the file exists.
  [[ -e "$OUT" ]] || : > "$OUT"
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
    elif grep -q "idempotence violation" "$stderr_file"; then
      oracle="idempotence"
    elif grep -q "token-stream violation" "$stderr_file"; then
      oracle="token-stream"
    elif [[ $rc -eq 0 ]]; then
      oracle="clean"
    else
      oracle="unknown-rc${rc}"
    fi
    nonprint="$(garbage_bytes "$artifact")"

    case "$oracle" in
      clean)
        stale=$((stale + 1)) ;;
      idempotence)
        if [[ "$nonprint" -gt 0 ]]; then
          known_rejoin=$((known_rejoin + 1))
        else
          oracle_desc="strict idempotence oracle on NON-garbage input — possible valid-CSS non-idempotence (new)"
          triage=$((triage + 1))
        fi ;;
      token-stream)
        if [[ "$nonprint" -gt 0 ]] || is_unterminated_url "$artifact" ||
           is_ws_only_diff "$artifact" "$stderr_file" ||
           is_custom_semi_drop "$artifact" "$stderr_file"; then
          known_token=$((known_token + 1))
        else
          oracle_desc="token-stream oracle on NON-garbage input — possible valid-CSS served-bytes corruption (new; the disclosed collapse reorder/dedup over-catches are the only known false-positive shapes)"
          triage=$((triage + 1))
        fi ;;
      *)
        oracle_desc="${oracle} — memory-safety or unclassified failure (never a known-find class)"
        triage=$((triage + 1)) ;;
    esac

    rm -f "$stderr_file"

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

  echo "css-fuzz triage: ${triage} needs-triage, ${known_rejoin} known-rejoin-class, ${known_token} known-token-overcatch, ${stale} stale/clean"
  if [[ "$triage" -gt 0 ]]; then
    {
      echo "_$((known_rejoin + known_token)) further artifact(s) matched the audit-documented known-find classes (garbage rejoin: ${known_rejoin}; token-oracle invalid-input over-catches: ${known_token}) and are NOT re-filed here; ${stale} artifact(s) replayed clean (stale). See the run summary for the full count and the \`css-fuzz-evidence\` artifact for every crash unit._"
      echo
    } >> "$OUT"
  fi
}

# --- self-test -------------------------------------------------------------
# Fixtures + a mock replay (filename -> oracle line); asserts each fixture's
# bucket WITHOUT depending on oracle behavior. The terminated-url and UTF-8
# fixtures are the #1176-review regression tests: the broken predicates
# bucketed both as known/expected, silently swallowing the #1158-class.
self_test() {
  local tmp fails=0
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  mkdir -p "$tmp/artifacts"

  printf 'a{background:url(foo.png);color:red}' > "$tmp/artifacts/crash-terminated"
  printf 'a{background:url(foo'                 > "$tmp/artifacts/crash-unterminated"
  printf 'a{b:\xff\xfe\xff}'                    > "$tmp/artifacts/crash-garbage"
  printf "a{content:'caf\xc3\xa9'}"             > "$tmp/artifacts/crash-utf8"
  printf 'x:\x00\x01'                           > "$tmp/artifacts/crash-sanitizer"
  # Escape-glue over-catch: printable, backslash, whitespace-only diff.
  printf '0\\;\n00\\;\n00..'                > "$tmp/artifacts/crash-escapeglue"
  # Counter-fixture: backslash present but token CONTENT changes — must
  # stay needs-triage (real-corruption shape).
  printf 'a{--x:\\/*y*/;b:c}'                   > "$tmp/artifacts/crash-realcatch"
  # Semantic whitespace deletion with a backslash present (review's
  # demonstrated slip): escape content is NOT in the space-removal set
  # at the removed run, so it must stay needs-triage.
  printf 'a{--x:a b;width:1\\/2}'               > "$tmp/artifacts/crash-wscorrupt"
  # Escaped whitespace is CONTENT (#1154's class): a removed run
  # directly after a backslash must stay needs-triage.
  printf 'a{b:c\\ d}'                           > "$tmp/artifacts/crash-wsescape"
  # String-interior ws deletion with a set-char neighbor: inside a
  # string every ws diff is corruption — must stay needs-triage even
  # though ';' is in the removal set.
  printf "a{content:'; x'}"                       > "$tmp/artifacts/crash-strsemi"
  # Custom-region ';'-drop (run 30913256122, crash-dc9b2139): the ONLY
  # diff is a ';' deleted before '}' inside a '--' declaration — the
  # disclosed invalid-input over-catch, buckets known_token.
  printf 'a{pad:l(x);--z:[;}}x'                  > "$tmp/artifacts/crash-custsemi"
  # Counter-fixture: the same ';'-before-'}' deletion WITHOUT a custom
  # opener in scope — must stay needs-triage.
  printf 'a{b:c;;}'                               > "$tmp/artifacts/crash-semireal"
  # The run-30947209282 class (4 artifacts): ';'-drop inside a '{...}'
  # group in a custom value inside an unclosed-paren context — the '{'
  # must not cut the '--' scan-back.  All four bucket known_token.
  printf '/(;--::{Y;}}'                           > "$tmp/artifacts/crash-custbrace1"
  printf ';S}0/(;--::{c;}I'                       > "$tmp/artifacts/crash-custbrace2"
  printf ';}(;--::{c;}I'                          > "$tmp/artifacts/crash-custbrace3"
  # ...including a leading plain ';}'->'}' trim in the same artifact
  # (the oracle's inherent sanction) alongside the custom drop.
  printf ';}:SYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY(;--::{c;} ' > "$tmp/artifacts/crash-custbrace4"
  # Counter-fixtures (stay needs-triage): the same deletion inside a
  # nested block with no custom anywhere; a custom property elsewhere
  # in the artifact but the drop in a nested block; a ';' deletion with
  # a custom opener but NOT before '}'.
  printf 'a{b:{c;}}'                              > "$tmp/artifacts/crash-seminested"
  printf 'a{--x:v;b:{c;}}'                        > "$tmp/artifacts/crash-semifar"
  printf 'a{--x:c;;d}'                            > "$tmp/artifacts/crash-semimid"
  # Sanctioned-noise tolerance (run 30983672391): the custom ';'-drop
  # riding with the oracle's OTHER sanctioned diff classes must bucket:
  # the minimized repro (';'-after-'{' trim + custom site), the exact
  # crash-42adb26b bytes (leading plain trim + ws collapse + custom
  # drop), and a ws-run removal before ';' + custom drop (the
  # crash-210129bf hunk shape).
  printf 'x{;--r:[;}}'                            > "$tmp/artifacts/crash-semimin"
  printf ';}:S0-\r/(;--::{c;}I'                   > "$tmp/artifacts/crash-semilead"
  printf 'a{b:rever\n\n\n;--r:[;}}'               > "$tmp/artifacts/crash-semicollapse"
  # Counter-fixtures (stay needs-triage): a hard-rejection class riding
  # a legitimate custom site — string-interior ws deletion, url()
  # interior ws deletion, a content change — and a ';' dropped after an
  # ESCAPED '{' (content, not a block start: the class-3 guard).
  printf "a{content:'; x';--z:[;}}"               > "$tmp/artifacts/crash-semistr"
  printf 'a{background:url(a; b.png);--z:[;}}'    > "$tmp/artifacts/crash-semiurl"
  printf 'a{--z:[;}};b:cx}'                       > "$tmp/artifacts/crash-semicontent"
  printf 'a{b:\\{;c};--z:[;}}'                    > "$tmp/artifacts/crash-semiesc"

  cat > "$tmp/replay-mock" <<'EOF'
#!/bin/bash
case "$(basename "$1")" in
  crash-terminated|crash-unterminated)
    echo "token-stream violation: [x] -> [y]" >&2; exit 134 ;;
  crash-garbage|crash-utf8)
    echo "idempotence violation: [x] -> [y] -> [z]" >&2; exit 134 ;;
  crash-sanitizer)
    echo "ERROR: AddressSanitizer: heap-buffer-overflow" >&2; exit 1 ;;
  crash-escapeglue)
    printf 'token-stream violation: [0\\;\n00\\;\n00..] -> [0\\;00\\;00..]\n' >&2; exit 134 ;;
  crash-realcatch)
    echo "token-stream violation: [a{--x:\\/*y*/;b:c}] -> [a{--x:\\;b:c}]" >&2; exit 134 ;;
  crash-wscorrupt)
    echo "token-stream violation: [a{--x:a b;width:1\\/2}] -> [a{--x:ab;width:1\\/2}]" >&2; exit 134 ;;
  crash-wsescape)
    echo "token-stream violation: [a{b:c\\ d}] -> [a{b:c\\d}]" >&2; exit 134 ;;
  crash-strsemi)
    echo "token-stream violation: [a{content:'; x'}] -> [a{content:';x'}]" >&2; exit 134 ;;
  crash-custsemi)
    echo "token-stream violation: [a{pad:l(x);--z:[;}}x] -> [a{pad:l(x);--z:[}}x]" >&2; exit 134 ;;
  crash-semireal)
    echo "token-stream violation: [a{b:c;;}] -> [a{b:c;}]" >&2; exit 134 ;;
  crash-custbrace1)
    echo "token-stream violation: [/(;--::{Y;}}] -> [/(;--::{Y}}]" >&2; exit 134 ;;
  crash-custbrace2)
    echo "token-stream violation: [;S}0/(;--::{c;}I] -> [;S}0/(;--::{c}I]" >&2; exit 134 ;;
  crash-custbrace3)
    echo "token-stream violation: [;}(;--::{c;}I] -> [;}(;--::{c}I]" >&2; exit 134 ;;
  crash-custbrace4)
    echo "token-stream violation: [;}:SYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY(;--::{c;} ] -> [}:SYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY(;--::{c} ]" >&2; exit 134 ;;
  crash-seminested)
    echo "token-stream violation: [a{b:{c;}}] -> [a{b:{c}}]" >&2; exit 134 ;;
  crash-semifar)
    echo "token-stream violation: [a{--x:v;b:{c;}}] -> [a{--x:v;b:{c}}]" >&2; exit 134 ;;
  crash-semimid)
    echo "token-stream violation: [a{--x:c;;d}] -> [a{--x:c;d}]" >&2; exit 134 ;;
  crash-semimin)
    echo "token-stream violation: [x{;--r:[;}}] -> [x{--r:[}}]" >&2; exit 134 ;;
  crash-semilead)
    printf 'token-stream violation: [;}:S0-\r/(;--::{c;}I] -> [}:S0- /(;--::{c}I]\n' >&2; exit 134 ;;
  crash-semicollapse)
    printf 'token-stream violation: [a{b:rever\n\n\n;--r:[;}}] -> [a{b:rever;--r:[}}]\n' >&2; exit 134 ;;
  crash-semistr)
    echo "token-stream violation: [a{content:'; x';--z:[;}}] -> [a{content:';x';--z:[}}]" >&2; exit 134 ;;
  crash-semiurl)
    echo "token-stream violation: [a{background:url(a; b.png);--z:[;}}] -> [a{background:url(a;b.png);--z:[}}]" >&2; exit 134 ;;
  crash-semicontent)
    echo "token-stream violation: [a{--z:[;}};b:cx}] -> [a{--z:[}};b:cy}]" >&2; exit 134 ;;
  crash-semiesc)
    printf 'token-stream violation: [a{b:\\{;c};--z:[;}}] -> [a{b:\\{c};--z:[}}]\n' >&2; exit 134 ;;
esac
exit 0
EOF
  chmod +x "$tmp/replay-mock"

  check() {  # $1=label $2=expected $3=actual
    if [[ "$2" == "$3" ]]; then
      echo "ok: $1"
    else
      echo "FAIL: $1 — expected [$2], got [$3]" >&2
      fails=$((fails + 1))
    fi
  }

  # Predicate units.
  garbage_bytes "$tmp/artifacts/crash-utf8" > "$tmp/g1"
  check "valid UTF-8 (café) is NOT garbage" "0" "$(cat "$tmp/g1")"
  [[ "$(garbage_bytes "$tmp/artifacts/crash-garbage")" -gt 0 ]]
  check "0xff bytes ARE garbage" "0" "$?"
  is_unterminated_url "$tmp/artifacts/crash-terminated"
  check "terminated url( is NOT unterminated" "1" "$?"
  is_unterminated_url "$tmp/artifacts/crash-unterminated"
  check "url( at EOF IS unterminated" "0" "$?"
  printf 'a{color:red}' > "$tmp/nourl"
  is_unterminated_url "$tmp/nourl"
  check "no url( is NOT unterminated" "1" "$?"

  # End-to-end buckets through the mock replay.
  summary="$(classify_dir "$tmp/replay-mock" "$tmp/artifacts" "$tmp/findings.md")"
  echo "$summary"
  check "needs-triage count (terminated + utf8 + sanitizer + realcatch + wscorrupt + wsescape + strsemi + semireal + seminested + semifar + semimid + semistr + semiurl + semicontent + semiesc)" \
    "15" "$(printf '%s' "$summary" | sed -E 's/.*triage: ([0-9]+).*/\1/')"
  check "known_token count (unterminated url + escapeglue + custom semi-drop x8)" \
    "10" "$(printf '%s' "$summary" | sed -E 's/.*, ([0-9]+) known-token-overcatch.*/\1/')"
  check "findings lists crash-terminated" "1" "$(grep -c 'crash-terminated' "$tmp/findings.md")"
  check "findings lists crash-utf8" "1" "$(grep -c 'crash-utf8' "$tmp/findings.md")"
  check "findings lists crash-sanitizer" "1" "$(grep -c 'crash-sanitizer' "$tmp/findings.md")"
  check "findings OMITS crash-unterminated" "0" "$(grep -c 'crash-unterminated' "$tmp/findings.md")"
  check "findings OMITS crash-garbage" "0" "$(grep -c 'crash-garbage' "$tmp/findings.md")"
  check "findings OMITS crash-escapeglue" "0" "$(grep -c 'crash-escapeglue' "$tmp/findings.md")"
  check "findings lists crash-realcatch" "1" "$(grep -c 'crash-realcatch' "$tmp/findings.md")"
  check "findings lists crash-wscorrupt (semantic ws deletion stays triage)" \
    "1" "$(grep -c 'crash-wscorrupt' "$tmp/findings.md")"
  check "findings lists crash-wsescape (escaped-ws deletion stays triage)" \
    "1" "$(grep -c 'crash-wsescape' "$tmp/findings.md")"
  check "findings lists crash-strsemi (string-interior deletion stays triage)" \
    "1" "$(grep -c 'crash-strsemi' "$tmp/findings.md")"
  check "findings OMITS crash-custsemi (custom-region ';'-drop is known)" \
    "0" "$(grep -c 'crash-custsemi' "$tmp/findings.md")"
  check "findings lists crash-semireal (';'-drop without custom opener stays triage)" \
    "1" "$(grep -c 'crash-semireal' "$tmp/findings.md")"
  check "findings OMITS crash-custbrace1 ('{' in custom value does not cut scan-back)" \
    "0" "$(grep -c 'crash-custbrace1' "$tmp/findings.md")"
  check "findings OMITS crash-custbrace2" "0" "$(grep -c 'crash-custbrace2' "$tmp/findings.md")"
  check "findings OMITS crash-custbrace3" "0" "$(grep -c 'crash-custbrace3' "$tmp/findings.md")"
  check "findings OMITS crash-custbrace4 (leading plain trim + custom drop)" \
    "0" "$(grep -c 'crash-custbrace4' "$tmp/findings.md")"
  check "findings lists crash-seminested (nested-block drop, no custom)" \
    "1" "$(grep -c 'crash-seminested' "$tmp/findings.md")"
  check "findings lists crash-semifar (custom elsewhere, drop in nested block)" \
    "1" "$(grep -c 'crash-semifar' "$tmp/findings.md")"
  check "findings lists crash-semimid (';' deletion not before '}')" \
    "1" "$(grep -c 'crash-semimid' "$tmp/findings.md")"
  check "findings OMITS crash-semimin (';'-after-'{' + custom site buckets)" \
    "0" "$(grep -c 'crash-semimin' "$tmp/findings.md")"
  check "findings OMITS crash-semilead (plain trim + ws collapse + custom site)" \
    "0" "$(grep -c 'crash-semilead' "$tmp/findings.md")"
  check "findings OMITS crash-semicollapse (ws-run removal + custom site)" \
    "0" "$(grep -c 'crash-semicollapse' "$tmp/findings.md")"
  check "findings lists crash-semistr (string-interior edit riding a custom site)" \
    "1" "$(grep -c 'crash-semistr' "$tmp/findings.md")"
  check "findings lists crash-semiurl (url-interior edit riding a custom site)" \
    "1" "$(grep -c 'crash-semiurl' "$tmp/findings.md")"
  check "findings lists crash-semicontent (content change riding a custom site)" \
    "1" "$(grep -c 'crash-semicontent' "$tmp/findings.md")"
  check "findings lists crash-semiesc (';' after ESCAPED '{' stays triage)" \
    "1" "$(grep -c 'crash-semiesc' "$tmp/findings.md")"

  # Pre-existing findings content must SURVIVE classify_dir (#1176
  # re-review): the workflow seeds entries (replay-floor red, fuzzer
  # campaign failure) into $FINDINGS before invoking the classifier, and
  # an empty findings file is the auto-close signal — a classifier-side
  # truncation wipes the seed and auto-closes a live issue on a night the
  # campaign never ran.
  printf '### pre-existing entry (must survive)\n' > "$tmp/seeded.md"
  classify_dir "$tmp/replay-mock" "$tmp/artifacts" "$tmp/seeded.md" >/dev/null
  check "pre-existing findings entry survives classify_dir" \
    "1" "$(grep -c 'pre-existing entry' "$tmp/seeded.md")"
  check "surviving file still gets triage entries appended" \
    "1" "$(grep -c 'crash-terminated' "$tmp/seeded.md")"

  if [[ "$fails" -eq 0 ]]; then
    echo "classify-css-fuzz-artifacts self-test: all checks passed"
    return 0
  fi
  echo "classify-css-fuzz-artifacts self-test: ${fails} check(s) FAILED" >&2
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
