#!/bin/bash

# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# build-js-fuzz-direct.sh - direct clang++ build of the JS minifier fuzz
# harness (lib/js/js_minify_fuzz.cc) OUTSIDE bazel, for the js-fuzz CI lane
# (the JS-fuzz lane). Mirrors the css-fuzz lane's direct-clang
# shape (see the css-fuzz.yml header for why bazel is rejected for fuzzer
# builds: gcc rejects -fsanitize=fuzzer and the repo's bazel clang configs
# carry no -stdlib=libc++ linkopt).
#
# Unlike the CSS harness, the JS kernel needs RE2 (lib/js/compat/re2.h ->
# re2/re2.h) and modern RE2 needs abseil. No system RE2 can be assumed on
# the CI runners, so this script builds BOTH from pinned upstream
# source tarballs — the same versions MODULE.bazel pins for the bazel build
# (parsed out of MODULE.bazel below, so a dependency bump that forgets the
# lane fails loudly here instead of silently diverging), sha256-verified,
# compiled with the same $CXX and -std=c++23 the repo's .bazelrc uses, and
# archived into one static lib the harness links against.
#
# Outputs into the directory named by $1 (default ${RUNNER_TEMP:-/tmp}/
# js-fuzz-bin):  replay  (deterministic main(), ASan+UBSan)
#                fuzzer  (-DJS_MINIFY_LIBFUZZER, libFuzzer+ASan+UBSan)
# Both modes are built on every run so neither can rot (css-fuzz.yml
# precedent).
#
# Local use (macOS): works with homebrew llvm@20 (CXX=/opt/homebrew/opt/
# llvm@20/bin/clang++). NOTE: on macOS the ASan-instrumented binaries hang
# at sanitizer init with homebrew clang (known local quirk) — pass
# SANITIZERS="" for a functional local run; ASan evidence locally comes
# from the bazel --config=asan build.

set -euo pipefail

OUT_DIR="${1:-${RUNNER_TEMP:-/tmp}/js-fuzz-bin}"
CXX="${CXX:-clang++-20}"
# `SANITIZERS=` (explicitly empty) disables sanitizers for local macOS runs;
# unset gets the default.
SANITIZERS="${SANITIZERS--fsanitize=address,undefined}"
# CI_SLOT_CPUS: the runner's per-job CPU budget (tools/ci/slot-budget.sh).
JOBS="${JOBS:-${CI_SLOT_CPUS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
cd "$REPO_ROOT"

# --- Pins: parsed from MODULE.bazel so the lane cannot drift from bazel.
absl_ver="$(sed -n 's/.*bazel_dep(name = "abseil-cpp", version = "\([^"]*\)".*/\1/p' MODULE.bazel | head -1)"
re2_ver="$(sed -n 's/.*bazel_dep(name = "re2", version = "\([^"]*\)".*/\1/p' MODULE.bazel | head -1)"
re2_ver="${re2_ver%.bcr.*}"  # bzlmod suffix is not part of the upstream tag
[ -n "$absl_ver" ] && [ -n "$re2_ver" ] || {
  echo "ERROR: could not parse abseil-cpp/re2 versions from MODULE.bazel" >&2
  exit 1
}

# sha256 of the github source-archive tarballs for the pinned tags.
case "$absl_ver" in
  20260107.1) absl_sha=4314e2a7cbac89cac25a2f2322870f343d81579756ceff7f431803c2c9090195 ;;
  *) echo "ERROR: no pinned sha256 for abseil-cpp $absl_ver — update $0 alongside the MODULE.bazel bump" >&2; exit 1 ;;
esac
case "$re2_ver" in
  2025-11-05) re2_sha=87f6029d2f6de8aa023654240a03ada90e876ce9a4676e258dd01ea4c26ffd67 ;;
  *) echo "ERROR: no pinned sha256 for re2 $re2_ver — update $0 alongside the MODULE.bazel bump" >&2; exit 1 ;;
esac

fetch() {  # fetch <url> <sha256> <dest-dir>
  local url="$1" sha="$2" dest="$3"
  local tarball="${dest}.tar.gz"
  curl -fsSL --retry 3 -o "$tarball" "$url"
  if command -v sha256sum >/dev/null; then
    echo "$sha  $tarball" | sha256sum -c - >/dev/null
  else
    [ "$(shasum -a 256 "$tarball" | awk '{print $1}')" = "$sha" ]
  fi
  mkdir -p "$dest"
  tar xzf "$tarball" -C "$dest" --strip-components=1
}

WORK="${OUT_DIR}/work"
ABSL="${WORK}/absl"
RE2="${WORK}/re2"
OBJ="${WORK}/obj"
mkdir -p "$OUT_DIR" "$OBJ"

echo "== building pinned deps: abseil-cpp ${absl_ver}, re2 ${re2_ver} (CXX=${CXX}, -std=c++23, ${JOBS} jobs)"
fetch "https://github.com/abseil/abseil-cpp/archive/refs/tags/${absl_ver}.tar.gz" "$absl_sha" "$ABSL"
fetch "https://github.com/google/re2/archive/refs/tags/${re2_ver}.tar.gz" "$re2_sha" "$RE2"

export OBJ ABSL RE2 CXX
compile_tree() {  # compile_tree <root> <find exclusions...>
  local root="$1"; shift
  find "$root" -name "*.cc" "$@" -print0 |
    xargs -0 -n1 -P"$JOBS" sh -c '
      f="$1"
      obj="$OBJ/$(echo "$f" | tr "/." "__").o"
      exec "$CXX" -std=c++23 -O1 -pthread -c "$f" -I"$ABSL" -I"$RE2" -o "$obj"
    ' _
}

# Exclusions: tests/mocks/benchmarks/gmock matchers, Windows-only sources,
# and absl's codegen utilities that carry their own main() (print_hash_of,
# gaussian_distribution_gentables — an archived main() SHADOWS libFuzzer's
# at static-link time; found the hard way).
compile_tree "$ABSL/absl" \
  ! -name "*_test*" ! -name "test_*" ! -name "*benchmark*" ! -name "*mock*" \
  ! -name "*matchers*" ! -name "*_win.cc" \
  ! -name "print_hash_of.cc" ! -name "*gentables*"
compile_tree "$RE2/re2" ! -path "*testing*" ! -name "*_test*" ! -name "*benchmark*"
# Top-level util/ in the re2 tarball: rune.cc (chartorune) + strutil.cc are
# part of libre2 proper; pcre.cc is the optional PCRE-compat shim — skip it.
compile_tree "$RE2/util" ! -name "pcre.cc"

ARCHIVE="${WORK}/libabsl_re2.a"
ar rcs "$ARCHIVE" "$OBJ"/*.o
echo "== deps archive: $(find "$OBJ" -name '*.o' | wc -l | tr -d ' ') objects"

# absl's cctz local_time_zone() uses CoreFoundation on macOS only.
PLATFORM_LIBS=""
[ "$(uname -s)" = "Darwin" ] && PLATFORM_LIBS="-framework CoreFoundation"

KERNEL_SRCS="lib/js/js_minify.cc lib/js/js_tokenizer.cc lib/js/js_keywords.cc"
INCLUDES="-I. -I$ABSL -I$RE2"

echo "== building replay binary (deterministic main, sanitizers: ${SANITIZERS:-none})"
# shellcheck disable=SC2086
"$CXX" -std=c++23 -O1 -pthread $SANITIZERS $INCLUDES \
  lib/js/js_minify_fuzz.cc $KERNEL_SRCS "$ARCHIVE" $PLATFORM_LIBS \
  -o "${OUT_DIR}/replay"

FUZZ_SAN="-fsanitize=fuzzer"
[ -n "$SANITIZERS" ] && FUZZ_SAN="-fsanitize=fuzzer,address,undefined"
echo "== building libFuzzer binary (${FUZZ_SAN})"
# shellcheck disable=SC2086
"$CXX" -std=c++23 -O1 -pthread "$FUZZ_SAN" -DJS_MINIFY_LIBFUZZER $INCLUDES \
  lib/js/js_minify_fuzz.cc $KERNEL_SRCS "$ARCHIVE" $PLATFORM_LIBS \
  -o "${OUT_DIR}/fuzzer"

# Main-identity smoke: the exclusions above are name-based, so a future
# absl bump adding a NEW main-bearing utility under a different name would
# re-shadow libFuzzer's main() at static-link time (the print_hash_of
# incident) — and the lane would never notice, because it only runs the
# replay binary. Pin the fuzzer binary's identity: -runs=1 must exit 0 AND
# print the characteristic libFuzzer banner. A shadowed main (e.g. the
# absl hash tool: prints one number to stdout, no INFO banner) fails here.
echo "== main-identity smoke: fuzzer binary must be libFuzzer"
fuzz_smoke_log="${WORK}/fuzz-smoke.log"
if ! "${OUT_DIR}/fuzzer" -runs=1 -max_len=64 >"$fuzz_smoke_log" 2>&1; then
  echo "ERROR: fuzzer binary exited non-zero on -runs=1 — a foreign main()" >&2
  echo "       may have shadowed libFuzzer's at static-link time. Output:" >&2
  cat "$fuzz_smoke_log" >&2
  exit 1
fi
if ! grep -q "INFO: Running with entropic power schedule" "$fuzz_smoke_log"; then
  echo "ERROR: fuzzer binary prints no libFuzzer banner on -runs=1 — a" >&2
  echo "       foreign main() shadowed libFuzzer's at static-link time" >&2
  echo "       (a NEW main-bearing absl/re2 utility not covered by this" >&2
  echo "       script's name-based exclusions?). Output:" >&2
  cat "$fuzz_smoke_log" >&2
  exit 1
fi

echo "== built: ${OUT_DIR}/replay ${OUT_DIR}/fuzzer"
