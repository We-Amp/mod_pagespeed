#!/usr/bin/env bash
# SPDX-License-Identifier: BUSL-1.1
# Copyright (c) 2024-2026 We-Amp B.V.
# Comprehensive, report-only dependency CVE scan — the design record Phase 1 (1.1 port).
#
# Generates SBOMs from DETERMINISTIC, COMMITTED inputs and scans them with grype:
#   - npm    : the admin-console SPA's committed pnpm-lock.yaml
#              (pagespeed/system/console). This is the only shipped JS surface
#              in 1.1.
#   - images : (only with --images) the BUILT package-test images the release
#              pipeline produces — a real base OS (Ubuntu/Rocky + Apache/nginx)
#              with the actual shipped .deb / .rpm / ngx_pagespeed .so installed
#              on top (test/package-test/Dockerfile.*). Scanned from the local
#              docker cache. This is the OS / apt / dnf / built-module layer
#              grype matches reliably — the shipped C/C++ product surface, the
#              1.1 analogue of the 2.0 optimizer line's modpagespeed/worker + /nginx runtime images
#             . We deliberately do NOT scan the pagespeed1.1-dev build
#              image or the bare distro base images: a base/build image is false
#              confidence — it misses the package layer the product actually adds.
#
# Determinism: every source ecosystem is scanned from a COMMITTED lockfile,
# copied into an isolated temp dir so syft sees ONLY that file. We never
# `syft dir:.` a project tree (that pulls in whatever node_modules / obj / bin
# happen to be present; a .NET project dir was observed cataloging 744 npm
# pkgs) and we never generate a lockfile at scan time (a generated lockfile can
# resolve differently than what the product build pins — the exact drift this
# is meant to kill). Images are scanned by the build machine where they exist.
#
# NOT covered here (tracked in the design record / README):
#   - .NET (the aspnetcore middleware + WeAmpSite): its real surface is the
#     built .nupkg / publish output, not a dev-time `dotnet restore` — deferred
#     to a post-build scan, same as the 2.0 optimizer line.
#   - cargo: 1.1 ships NO Rust — there is no Cargo.toml / Cargo.lock anywhere in
#     the tree, so there is no cargo surface to scan (omitted, not stubbed).
#   - transitive vendored C/C++ Bazel deps (Envoy / libwebp / optipng / …):
#     covered by the SEPARATE cpp-cve-matcher work (Envoy-style per-dep
#     cpe+release_date+NVD model), NOT by this syft/grype scan.
#
# REPORT-ONLY: prints a per-source severity histogram, writes SBOMs + grype
# reports under $OUT_DIR, and ALWAYS exits 0. The blocking gate stays the
# medium+ curated one in the CI workflow (security-gates) / release.yml; surfaces flip to
# blocking per the design record's ratchet only after their findings are triaged into
# sbom/*.vex.json.
#
# syft + grype must be on PATH at the pinned versions below.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${OUT_DIR:-${REPO_ROOT}/sbom/scan}"
GRYPE_SEVERITY="${GRYPE_SEVERITY:-medium}"   # reported threshold (NOT enforced)
SYFT_PIN="1.44.0"
GRYPE_PIN="0.112.0"
SCAN_IMAGES=0
SUMMARY_MD=""
WORK=""
VEX_ARGS=()

for arg in "$@"; do
  case "$arg" in
    --images) SCAN_IMAGES=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Built package-test images (local docker tags). These bake the actual shipped
# .deb / .rpm / nginx .so into a production-representative base OS, so they carry
# the real product C/C++ + apt/dnf surface (test/package-test/Dockerfile.*):
#   - mps-smoke-nginx                : release.yml builds this tag directly
#                                      (Dockerfile.ubuntu-nginx + module .so).
#   - package-test-test-ubuntu-apache: default docker-compose tag for
#                                      Dockerfile.ubuntu-apache (.deb installed).
#   - package-test-test-rocky-apache : default docker-compose tag for
#                                      Dockerfile.rocky-apache (.rpm installed).
# Absent images skip cleanly with an explicit "not built" row — we never fall
# back to scanning a base/build image as a proxy.
PRODUCT_IMAGES="
mps-smoke-nginx:latest
package-test-test-ubuntu-apache:latest
package-test-test-rocky-apache:latest
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
  [ -z "$have" ] || [ "$have" = "$pin" ] || warn "$tool $have != pinned $pin (results may differ)"
}

emit_row() { printf '| %s | %s | %s | %s |\n' "$1" "$2" "$3" "$4" >>"$SUMMARY_MD"; }

# run_scan <kind> <name> <syft-source>   (source carries its prefix: dir:/docker:)
#   syft -> SBOM, grype (+VEX) -> report, appends a severity row. Distinguishes
#   a FAILED scan (syft/grype error or non-JSON report) from a genuinely clean one.
run_scan() {
  local kind="$1" name="$2" src="$3"
  local sbom="${OUT_DIR}/${name}.spdx.json" rpt="${OUT_DIR}/${name}.grype.json"
  if ! syft "${src}" -o "spdx-json=${sbom}" -q 2>"${OUT_DIR}/${name}.syft.err"; then
    warn "syft FAILED for ${name} (${src}) — see ${name}.syft.err"
    emit_row "$name" "$kind" "scan-failed" "**scan failed**"; return
  fi
  local pkgs; pkgs="$(jq '.packages | length - 1' "$sbom" 2>/dev/null || echo '?')"
  if ! grype "sbom:${sbom}" ${VEX_ARGS[@]+"${VEX_ARGS[@]}"} -o json >"$rpt" 2>"${OUT_DIR}/${name}.grype.err"; then
    warn "grype FAILED for ${name} — see ${name}.grype.err"
    emit_row "$name" "$kind" "$pkgs" "**scan failed**"; return
  fi
  # A valid grype report has a .matches array; anything else = broken scan, not "clean".
  if ! jq -e 'has("matches")' "$rpt" >/dev/null 2>&1; then
    warn "grype produced no .matches for ${name} — treating as scan failure"
    emit_row "$name" "$kind" "$pkgs" "**scan failed**"; return
  fi
  local c h m l
  c="$(jq '[.matches[]|select(.vulnerability.severity=="Critical")]|length' "$rpt")"
  h="$(jq '[.matches[]|select(.vulnerability.severity=="High")]|length'     "$rpt")"
  m="$(jq '[.matches[]|select(.vulnerability.severity=="Medium")]|length'   "$rpt")"
  l="$(jq '[.matches[]|select(.vulnerability.severity=="Low")]|length'      "$rpt")"
  note "${name} (${kind}): ${pkgs} pkgs — ${c}C/${h}H/${m}M/${l}L"
  emit_row "$name" "$kind" "$pkgs" "${c} / ${h} / ${m} / ${l}"
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
  [ "$any" = "1" ] || { warn "no lockfile for ${name}; NOT scanned"; emit_row "$name" "$kind" "**no lockfile**" "—"; return; }
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
  # (The 1.1 gate expects a single sbom/pagespeed-1.1.vex.json; the glob handles
  # zero, one, or several without tripping `set -u` if none exist yet.)
  local vexfile
  for vexfile in "${REPO_ROOT}"/sbom/*.vex.json; do
    [ -f "$vexfile" ] && VEX_ARGS+=(--vex "$vexfile") && note "applying VEX: $(basename "$vexfile")"
  done

  {
    echo "## Dependency scan (report-only — the design record Phase 1)"
    echo ""
    echo "syft \`${SYFT_PIN}\` · grype \`${GRYPE_PIN}\` (live DB) · reported \`${GRYPE_SEVERITY}+\` · **non-blocking**"
    echo ""
    echo "| Source | Ecosystem | Pkgs | Crit / High / Med / Low |"
    echo "|---|---|--:|---|"
  } >"$SUMMARY_MD"

  # --- npm (committed lockfile: the admin-console SPA) ---
  scan_lockfiles npm admin-console "${REPO_ROOT}/pagespeed/system/console/pnpm-lock.yaml"

  # --- built package-test images (local docker cache; the real shipped surface) ---
  if [ "$SCAN_IMAGES" = "1" ]; then
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
  else
    note "image scan skipped (pass --images; runs on the build machine where images exist)"
  fi

  {
    echo ""
    echo "_Report-only. Triage findings into \`sbom/*.vex.json\` (applied when present);"
    echo "flip a surface to blocking once clean. .NET (built .nupkg) + transitive vendored C/C++"
    echo "(separate cpp-cve-matcher) are out of scope here — see tools/sbom/README.md._"
  } >>"$SUMMARY_MD"
  note "summary -> ${SUMMARY_MD}"
  cat "$SUMMARY_MD" >&2
  exit 0   # report-only: never fail the build
}

main "$@"
