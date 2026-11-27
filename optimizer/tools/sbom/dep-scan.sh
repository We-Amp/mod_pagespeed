#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
# Comprehensive, report-only dependency CVE scan — phase 1.
#
# Generates SBOMs from DETERMINISTIC, COMMITTED inputs and scans them with grype:
#   - npm    : EVERY committed npm lockfile in the tree, in both lockfile
#              dialects (pnpm-lock.yaml / package-lock.json). The
#              surface list (NPM_LOCKFILES) is COVERAGE-CHECKED against
#              `git ls-files` (see assert_npm_coverage): a committed lockfile
#              that is not registered is a loud failure, not a silent hole.
#   - cargo  : lib/image/vtracer_ffi/Cargo.lock (committed; the Bazel build
#              consumes it via crate_universe from_cargo, so build == scan)
#   - images : (only with --images) the BUILT product images the build machine
#              produces — modpagespeed/worker, modpagespeed/nginx,
#              pagespeed2-base-runtime, pagespeed2-dev — scanned from the local
#              docker cache. This is the OS / apt / Chromium / .so layer grype
#              matches reliably; the part of the C/C++ surface syft-on-source
#              cannot see.
#
# Determinism: every source ecosystem is scanned from a COMMITTED lockfile,
# copied into an isolated temp dir so syft sees ONLY that file. We never
# `syft dir:.` a project tree (that pulls in whatever node_modules / obj / bin
# happen to be present; a .NET project dir was observed cataloging 744 npm
# pkgs) and we never generate a lockfile at scan time (a generated lockfile can
# resolve differently than what the product build pins — the exact drift this
# is meant to kill). Images are scanned by the build machine where they exist.
#
# DEV DEPENDENCIES ARE IN SCOPE (SYFT_JAVASCRIPT_INCLUDE_DEV_DEPENDENCIES below).
# syft's package-lock.json cataloger SKIPS `"dev": true` entries by default, so
# a package-lock.json that is (nearly) all devDependencies catalogues almost
# nothing and reports a false 0/0/0/0 — a gate that reports "clean" over
# packages it never examined is worse than no gate. Dev deps are therefore
# included everywhere; the empty-catalog guard in run_scan turns any surface
# that still catalogues 0 packages into a scan failure rather than a clean row.
# (pnpm-lock.yaml is unaffected — syft's pnpm cataloger already reports the full
# `packages:` section.)
#
# NOT covered here (tracked in the README): .NET (its real surface is the
# built .nupkg, not a dev-restore — deferred to a post-build scan); transitive
# vendored C/C++ Bazel deps (Envoy CPE+NVD model, lands in mod_pagespeed).
#
# Two modes:
#   REPORT-ONLY (default): prints a per-source severity histogram, writes SBOMs
#     + grype reports under $OUT_DIR, and ALWAYS exits 0. This is the daily /
#     scheduled posture — surfaces newly-disclosed CVEs without ever reddening CI.
#   BLOCKING (--fail-on <sev>): same scan, exits NON-ZERO on gate hits (after
#     VEX suppression). Gate shape differs by surface (see run_scan):
#     npm/cargo gate UNCONDITIONALLY at <sev>+; images gate at <sev>+ when a
#     fix is available (fix.state "fixed") and at High/Critical always.
#     Used by the per-PR + push gate (npm + cargo, clean at medium+ today)
#     and the scheduled image gate. Restrict the scanned set with --surfaces
#     to keep the still-report-only surfaces (.NET) out of the gate.
#     See tools/sbom/README.md.
#
# syft + grype must be on PATH at the pinned versions below. On the blocking
# path the grype DB is pinned (GRYPE_DB_AUTO_UPDATE=false + a provisioned
# dated archive — see the CI dependency gate + tools/sbom/grype-db-pin.json)
# so a newly-published CVE cannot non-deterministically break an in-flight PR.
set -uo pipefail

# See the DEV DEPENDENCIES note above — without this, package-lock.json
# surfaces that are all-devDependencies scan as empty and report a false clean
# (syft's package-lock cataloger skips `"dev": true` entries by default).
export SYFT_JAVASCRIPT_INCLUDE_DEV_DEPENDENCIES=true

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/sbom/scan}"
GRYPE_SEVERITY="${GRYPE_SEVERITY:-medium}"   # reported threshold (NOT enforced)
SYFT_PIN="1.44.0"
GRYPE_PIN="0.121.2"
SCAN_IMAGES=0
PUBLISHED_VERSION=""
SYFT_ARGS=()
SUMMARY_MD=""
WORK=""
VEX_ARGS=()
# --fail-on <sev>: blocking mode. Empty = report-only (default, always exit 0).
FAIL_ON=""
# --surfaces <csv>: restrict the scanned set (e.g. "npm,cargo"). Empty = all.
SURFACES=""
GATE_HITS=0   # total over-threshold findings across all scanned surfaces

# bash 3.2 (macOS) safe: no associative arrays, no ${var,,} — lower via tr.
lc() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

# sev_rank <sev> -> numeric rank (low<medium<high<critical); unknown -> 0.
sev_rank() {
  case "$(lc "$1")" in
    low) echo 1 ;; medium) echo 2 ;; high) echo 3 ;; critical) echo 4 ;;
    *) echo 0 ;;
  esac
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --images) SCAN_IMAGES=1 ;;
    --published-images) shift; PUBLISHED_VERSION="${1:-}" ;;
    --published-images=*) PUBLISHED_VERSION="${1#*=}" ;;
    --fail-on) shift; FAIL_ON="${1:-}" ;;
    --fail-on=*) FAIL_ON="${1#*=}" ;;
    --surfaces) shift; SURFACES="${1:-}" ;;
    --surfaces=*) SURFACES="${1#*=}" ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
  shift
done

if [ -n "$FAIL_ON" ]; then
  case "$(lc "$FAIL_ON")" in
    low|medium|high|critical) ;;
    *) echo "::error::--fail-on must be one of low|medium|high|critical (got '$FAIL_ON')" >&2; exit 2 ;;
  esac
fi

# want_surface <kind> — is this surface in the requested --surfaces set?
# Empty SURFACES means "all". Matches on the scan kind (npm/cargo/image).
want_surface() {
  [ -z "$SURFACES" ] && return 0
  case ",$(lc "$SURFACES")," in
    *",$(lc "$1"),"*) return 0 ;;
    *) return 1 ;;
  esac
}

# Built product images (local docker tags from docker/build-release.sh). ":latest"
# is what the build machine retags after each build; absent images skip cleanly.
# Published images (ghcr.io/we-amp/<name>:<version>), for --published-images.
PUBLISHED_NAMES="pagespeed-worker pagespeed-nginx pagespeed-combined"
PUBLISHED_PLATFORMS="linux/amd64 linux/arm64"

PRODUCT_IMAGES="
modpagespeed/worker:latest
modpagespeed/nginx:latest
pagespeed2-base-runtime:latest
pagespeed2-dev:latest
"

# Surfaces that are scanned + reported (SBOM, tracking issue, summary row) but
# never bump GATE_HITS in --fail-on mode. Owner decision 2026-06-11 (the CVE
# ratchet): pagespeed2-dev is the CI toolchain image — never shipped to
# customers — so its CVE surface is tracked but does not gate publishes.
# The website surface (website/package-lock.json) is the same class: build
# tooling for the documentation site, shipped in no package, so it is scanned
# and reported (and its lockfile coverage is satisfied) without gating.
is_report_only() {
  case "$1" in
    pagespeed2-dev*|website) return 0 ;;
  esac
  return 1
}

# Every COMMITTED npm lockfile, as "<surface-name> <repo-relative-path>".
# assert_npm_coverage cross-checks this list against `git ls-files`, so this is
# the single place to register a new JS surface. Both lockfile dialects are
# handled (pnpm-lock.yaml and package-lock.json).
#   workbench       - the workbench SPA (tools/workbench)
#   website         - the documentation site (website/); report-only, see
#                     is_report_only
NPM_LOCKFILES="
workbench       tools/workbench/pnpm-lock.yaml
website         website/package-lock.json
"

note() { printf '\033[36m[dep-scan]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[33m[dep-scan] WARN:\033[0m %s\n' "$*" >&2; }

cleanup() { [ -n "$WORK" ] && rm -rf "$WORK"; }
trap cleanup EXIT

require_tool() {
  local tool="$1" pin="$2" have
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "::error::$tool not found on PATH (pin $pin)"; exit 3
  fi
  have="$("$tool" version 2>/dev/null | awk '/^Version:/{print $2; exit}')"
  if [ -n "$have" ] && [ "$have" != "$pin" ]; then
    if [ -n "$FAIL_ON" ]; then
      # Blocking runs gate on grype JSON fields (fix.state enum); an unpinned
      # tool could drift the schema and silently fail the gate OPEN. Hard stop.
      echo "::error::$tool $have != pinned $pin — refusing to run a BLOCKING scan on an unpinned tool" >&2
      exit 3
    fi
    warn "$tool $have != pinned $pin (results may differ)"
  fi
}

emit_row() { printf '| %s | %s | %s | %s |\n' "$1" "$2" "$3" "$4" >>"$SUMMARY_MD"; }

# assert_npm_coverage — every committed npm lockfile must be registered in
# NPM_LOCKFILES. This is the guard for the scope hole this scan closes: the
# surface list can drift behind the tree, leaving a committed lockfile
# unscanned while the summary table still looks complete. A new lockfile is a
# loud row (and a gate hit in blocking mode), never a silent hole. Cargo.lock
# is deliberately NOT in the glob — cargo is a separate surface.
assert_npm_coverage() {
  command -v git >/dev/null 2>&1 || { warn "git unavailable; skipping npm lockfile coverage check"; return; }
  local tracked registered missing=""
  tracked="$(git -C "$REPO_ROOT" ls-files -- '*package-lock.json' '*pnpm-lock.yaml' '*yarn.lock' 2>/dev/null)"
  [ -n "$tracked" ] || { warn "git ls-files returned no lockfiles; skipping coverage check"; return; }
  registered="$(printf '%s\n' "$NPM_LOCKFILES" | awk 'NF{print $2}')"
  local f
  for f in $tracked; do
    # Herestring, not a pipe: `grep -q` exits on the first match and a
    # `printf | grep -q` pipeline then fails under pipefail whenever printf
    # loses the race to write ("printf: write error: Broken pipe"), which
    # reported a REGISTERED lockfile as unscanned and failed the gate.
    grep -qxF -- "$f" <<<"$registered" || missing="${missing}${f} "
  done
  [ -z "$missing" ] && { note "npm lockfile coverage: all $(printf '%s\n' "$tracked" | grep -c .) committed lockfile(s) registered"; return; }
  warn "UNSCANNED committed npm lockfile(s) — add them to NPM_LOCKFILES: ${missing}"
  emit_row "npm-coverage" npm "**unscanned**" "$(printf '%s' "$missing" | tr ' ' '\n' | grep -c .) lockfile(s) missing"
  [ -n "$FAIL_ON" ] && GATE_HITS=$((GATE_HITS + 1))
  return 0
}

# run_scan <kind> <name> <syft-source>   (source carries its prefix: dir:/docker:)
#   syft -> SBOM, grype (+VEX) -> report, appends a severity row. Distinguishes
#   a FAILED scan (syft/grype error or non-JSON report) from a genuinely clean one.
run_scan() {
  local kind="$1" name="$2" src="$3"
  local sbom="${OUT_DIR}/${name}.spdx.json" rpt="${OUT_DIR}/${name}.grype.json"
  if ! syft "${src}" ${SYFT_ARGS[@]+"${SYFT_ARGS[@]}"} -o "spdx-json=${sbom}" -q 2>"${OUT_DIR}/${name}.syft.err"; then
    warn "syft FAILED for ${name} (${src}) — see ${name}.syft.err"
    if [ -n "$FAIL_ON" ] && ! is_report_only "$name"; then GATE_HITS=$((GATE_HITS + 1)); fi
    emit_row "$name" "$kind" "scan-failed" "**scan failed**"; return
  fi
  local pkgs; pkgs="$(jq '.packages | length - 1' "$sbom" 2>/dev/null || echo '?')"
  if ! grype "sbom:${sbom}" ${VEX_ARGS[@]+"${VEX_ARGS[@]}"} -o json >"$rpt" 2>"${OUT_DIR}/${name}.grype.err"; then
    warn "grype FAILED for ${name} — see ${name}.grype.err"
    if [ -n "$FAIL_ON" ] && ! is_report_only "$name"; then GATE_HITS=$((GATE_HITS + 1)); fi
    emit_row "$name" "$kind" "$pkgs" "**scan failed**"; return
  fi
  # A valid grype report has a .matches array; anything else = broken scan, not "clean".
  if ! jq -e 'has("matches")' "$rpt" >/dev/null 2>&1; then
    warn "grype produced no .matches for ${name} — treating as scan failure"
    if [ -n "$FAIL_ON" ] && ! is_report_only "$name"; then GATE_HITS=$((GATE_HITS + 1)); fi
    emit_row "$name" "$kind" "$pkgs" "**scan failed**"; return
  fi
  # An empty catalog is a scan failure, not a clean surface: every registered
  # lockfile pins at least one package, so 0 packages means the cataloger
  # skipped the file (wrong dialect, unsupported lockfileVersion, or the dev
  # -dependency switch regressing) and the 0/0/0/0 below would be a lie.
  if [ "$kind" != "image" ] && [ "$pkgs" = "0" ]; then
    warn "${name}: syft catalogued 0 packages — treating as scan failure, not clean"
    if [ -n "$FAIL_ON" ] && ! is_report_only "$name"; then GATE_HITS=$((GATE_HITS + 1)); fi
    emit_row "$name" "$kind" "0" "**scan failed (empty catalog)**"; return
  fi
  local c h m l
  c="$(jq '[.matches[]|select(.vulnerability.severity=="Critical")]|length' "$rpt")"
  h="$(jq '[.matches[]|select(.vulnerability.severity=="High")]|length'     "$rpt")"
  m="$(jq '[.matches[]|select(.vulnerability.severity=="Medium")]|length'   "$rpt")"
  l="$(jq '[.matches[]|select(.vulnerability.severity=="Low")]|length'      "$rpt")"
  note "${name} (${kind}): ${pkgs} pkgs — ${c}C/${h}H/${m}M/${l}L"

  # Blocking mode. Two gate shapes by surface kind (2026-06-11 review-refined):
  #
  #   npm / cargo : UNCONDITIONAL — any finding at/above --fail-on gates,
  #     exactly as before this change (a registry advisory with no published
  #     fix still forces a dep replacement or a justified VEX entry).
  #
  #   image : MEDIUM band gates only when ACTIONABLE (fix.state == "fixed" —
  #     a rebuild's dist-upgrade or a base bump resolves it), because the
  #     shipped images' OS population carries a permanent wont-fix/not-fixed
  #     medium tail (post-remediation 2026-06-11: 100% of the SHIPPED images'
  #     remaining mediums) and "suppressing" it with unprovable not_affected
  #     VEX claims would be dishonest (the analysis Chromium genuinely links
  #     the codec libs, and it runs --no-sandbox). HIGH/CRITICAL always gate,
  #     fixable or not — a Critical in a shipped image never rides silently;
  #     the out is an evidence-backed VEX statement or structural removal
  #     (like the 2026-06-11 nginx re-base), not a filter.
  #
  # The FULL population (gating or not) is always reported above, lands in
  # the daily tracking issue, and remains VEX-suppressible with evidence.
  if [ -n "$FAIL_ON" ]; then
    local want img over unfixable
    want="$(sev_rank "$FAIL_ON")"
    if [ "$kind" = "image" ]; then img=true; else img=false; fi
    # Gate filter: rank >= threshold AND (non-image surface OR fix available
    # OR rank >= high). Shared by the count and the explainer listing below.
    local gate_jq='def rank: {"low":1,"medium":2,"high":3,"critical":4}[.vulnerability.severity|ascii_downcase] // 0;
              .matches[]|select( (rank >= $w)
                and ( ($img|not) or .vulnerability.fix.state == "fixed" or rank >= 3 ) )'
    over="$(jq --argjson w "$want" --argjson img "$img" "[${gate_jq}]|length" "$rpt")"
    unfixable="$(jq --argjson w "$want" --argjson img "$img" '
              def rank: {"low":1,"medium":2,"high":3,"critical":4}[.vulnerability.severity|ascii_downcase] // 0;
              [.matches[]|select( $img and (rank >= $w) and rank < 3
                and .vulnerability.fix.state != "fixed" )]|length' "$rpt")"
    [ "$unfixable" -gt 0 ] && \
      warn "${name}: ${unfixable} medium-band finding(s) with NO fix available — reported + issue-tracked, not gating"
    if [ "$over" -gt 0 ] && is_report_only "$name"; then
      warn "${name}: ${over} gating-class finding(s) >= ${FAIL_ON} — report-only surface, does NOT gate (owner decision 2026-06-11)"
      emit_row "$name" "$kind" "$pkgs" "${c} / ${h} / ${m} / ${l} ⚠ report-only (${over} would gate)"
    elif [ "$over" -gt 0 ]; then
      GATE_HITS=$((GATE_HITS + over))
      warn "${name}: ${over} finding(s) >= ${FAIL_ON} — BLOCKING:"
      jq -r --argjson w "$want" --argjson img "$img" "${gate_jq}
              | \"    \(.artifact.name)@\(.artifact.version) \(.vulnerability.id) [\(.vulnerability.severity)] fix=\(.vulnerability.fix.versions|join(\",\")//\"none\")\"" "$rpt" >&2
      emit_row "$name" "$kind" "$pkgs" "${c} / ${h} / ${m} / ${l} ❌"
    else
      emit_row "$name" "$kind" "$pkgs" "${c} / ${h} / ${m} / ${l} ✅"
    fi
  else
    emit_row "$name" "$kind" "$pkgs" "${c} / ${h} / ${m} / ${l}"
  fi
}

# scan_lockfiles <kind> <name> <lockfile> [lockfile...]
#   Copies the given COMMITTED lockfiles into an isolated temp dir (so syft sees
#   ONLY them) and scans. A missing lockfile is a loud row, not a silent skip.
scan_lockfiles() {
  local kind="$1" name="$2"; shift 2
  local d="${WORK}/${name}" f any=0
  mkdir -p "$d"
  for f in "$@"; do
    if [ -f "$f" ]; then cp "$f" "$d/"; any=1; else warn "missing committed lockfile: $f"; fi
  done
  if [ "$any" != "1" ]; then
    warn "no lockfile for ${name}; NOT scanned"
    # In blocking mode a vanished lockfile must fail the gate, not pass it: a
    # registered surface that silently stops being scanned is the exact hole
    # this widening closed.
    if [ -n "$FAIL_ON" ] && ! is_report_only "$name"; then GATE_HITS=$((GATE_HITS + 1)); fi
    emit_row "$name" "$kind" "**no lockfile**" "—"; return
  fi
  run_scan "$kind" "$name" "dir:${d}"
}

main() {
  require_tool syft "$SYFT_PIN"
  require_tool grype "$GRYPE_PIN"
  command -v jq >/dev/null 2>&1 || { echo "::error::jq required"; exit 3; }
  [ -n "$OUT_DIR" ] || { echo "::error::OUT_DIR empty"; exit 3; }
  rm -rf "$OUT_DIR"; mkdir -p "$OUT_DIR"
  WORK="$(mktemp -d)"
  SUMMARY_MD="${OUT_DIR}/SUMMARY.md"

  # Apply the same VEX suppressions the blocking gate uses, so the report
  # reflects post-triage reality and the ratchet ("clean at medium+") is real.
  local vexfile
  for vexfile in "${REPO_ROOT}"/sbom/*.vex.json; do
    [ -f "$vexfile" ] && VEX_ARGS+=(--vex "$vexfile") && note "applying VEX: $(basename "$vexfile")"
  done

  local mode_line scope_line
  if [ -n "$FAIL_ON" ]; then
    mode_line="enforced \`${FAIL_ON}+\` · **BLOCKING** (npm/cargo: any over-threshold finding; images: fixable at threshold, High/Critical always)"
  else
    mode_line="reported \`${GRYPE_SEVERITY}+\` · **report-only** (always exits 0)"
  fi
  scope_line="${SURFACES:-all surfaces}"
  {
    echo "## Dependency scan"
    echo ""
    echo "syft \`${SYFT_PIN}\` · grype \`${GRYPE_PIN}\` · scope \`${scope_line}\` · ${mode_line}"
    echo ""
    echo "| Source | Ecosystem | Pkgs | Crit / High / Med / Low |"
    echo "|---|---|--:|---|"
  } >"$SUMMARY_MD"

  # --- npm (every committed lockfile; see NPM_LOCKFILES) ---
  if want_surface npm; then
    assert_npm_coverage
    local nname npath
    while read -r nname npath; do
      [ -n "$nname" ] || continue
      scan_lockfiles npm "$nname" "${REPO_ROOT}/${npath}"
    done <<EOF
$(printf '%s\n' "$NPM_LOCKFILES" | awk 'NF')
EOF
  fi

  # --- cargo (committed Cargo.lock; staticlib compiled into the worker) ---
  if want_surface cargo; then
    scan_lockfiles cargo vtracer-ffi   "${REPO_ROOT}/lib/image/vtracer_ffi/Cargo.lock"
  fi

  # --- built product images (local docker cache; the real shipped C/C++ surface) ---
  # Images gate only when explicitly opted into the blocking set
  # (--images --fail-on <sev> --surfaces ...,image) — the scheduled
  # dependency-scan run does exactly that (the CVE ratchet). Absent images
  # degrade to a "not
  # built" row (no gate hit), so a runner with a cold cache can't false-pass.
  # --- published images (ghcr.io; what customers pull) ---
  # Always the image gate shape; a pull or scan failure is a gate hit, never
  # a clean row.
  if [ -n "$PUBLISHED_VERSION" ] && want_surface image; then
    local pname pplat pslug
    for pname in $PUBLISHED_NAMES; do
      for pplat in $PUBLISHED_PLATFORMS; do
        pslug="${pname}-${PUBLISHED_VERSION}-$(printf '%s' "$pplat" | tr '/' '-')"
        SYFT_ARGS=(--platform "$pplat")
        run_scan image "$pslug" "registry:ghcr.io/we-amp/${pname}:${PUBLISHED_VERSION}"
        SYFT_ARGS=()
      done
    done
  fi

  if [ "$SCAN_IMAGES" = "1" ] && want_surface image; then
    if command -v docker >/dev/null 2>&1; then
      local img slug
      for img in $PRODUCT_IMAGES; do
        if docker image inspect "$img" >/dev/null 2>&1; then
          slug="$(printf '%s' "$img" | tr '/:' '--')"
          run_scan image "$slug" "docker:${img}"
        else
          warn "image not in local cache (build it first): ${img}"
          emit_row "$(printf '%s' "$img" | tr '/:' '--')" image "**not built**" "—"
        fi
      done
    else
      warn "docker not present; skipping image scans"
    fi
  elif [ -z "$PUBLISHED_VERSION" ]; then
    note "image scan skipped (pass --images; runs on the build machine where images exist)"
  fi

  {
    echo ""
    if [ -n "$FAIL_ON" ]; then
      echo "_Blocking gate (the CVE ratchet). npm/cargo: ANY finding at or above \`${FAIL_ON}\` fails."
      echo "Images: findings at or above \`${FAIL_ON}\` fail when a fix is available (fix.state=fixed —"
      echo "a rebuild or bump resolves them) and High/Critical fail unconditionally; no-fix mediums are"
      echo "reported + issue-tracked but do not gate. Suppress a genuinely not-affected finding with a"
      echo "justified \`sbom/*.vex.json\` entry, or bump the dep._"
    else
      echo "_Report-only. Triage findings into \`sbom/*.vex.json\` (applied when present);"
      echo "flip a surface to blocking once clean. .NET (built .nupkg) + transitive C/C++ are deferred — see tools/sbom/README.md._"
    fi
  } >>"$SUMMARY_MD"
  note "summary -> ${SUMMARY_MD}"
  cat "$SUMMARY_MD" >&2

  # Blocking mode: non-zero exit iff any scanned surface had an over-threshold
  # finding. Report-only mode always exits 0 (the default daily posture).
  if [ -n "$FAIL_ON" ] && [ "$GATE_HITS" -gt 0 ]; then
    echo "::error::dep-scan BLOCKING: ${GATE_HITS} FIXABLE finding(s) >= ${FAIL_ON} on scope '${scope_line}'. Rebuild/bump to pick up the fix, or add a justified sbom/*.vex.json entry." >&2
    exit 1
  fi
  exit 0
}

main "$@"
