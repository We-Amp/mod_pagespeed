#!/bin/bash -eu
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
#
# OSS-Fuzz build of the fuzz harnesses in this tree.
#
# google/oss-fuzz's projects/mod_pagespeed/build.sh runs this script in the
# OSS-Fuzz builder image. The environment there names the compiler and the
# flags of the build ($CC, $CXX, $CFLAGS, $CXXFLAGS: one sanitizer, one
# fuzzing engine), the engine's main() ($LIB_FUZZING_ENGINE) and the output
# directory ($OUT). To run it against a local checkout from a google/oss-fuzz
# clone:
#
#     python3 infra/helper.py build_fuzzers mod_pagespeed /path/to/this/tree
#     python3 infra/helper.py check_build mod_pagespeed
#
# Each harness is one source file with two build modes (see its header): a
# replay main() by default, LLVMFuzzerTestOneInput behind -D<NAME>_LIBFUZZER.
# The define goes in through --per_file_copt, so one Bazel invocation builds
# every harness, each linked against $LIB_FUZZING_ENGINE. The CSS and JS
# harnesses carry their seed corpus in the source (--dump_corpus on the replay
# binary); the other three read theirs from tools/*-corpus/seeds.

cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.."

# The builder's Bazelisk predates MODULE.bazel-only workspaces: without a
# WORKSPACE file it does not find .bazelversion and would run the newest
# Bazel. Name the version explicitly.
USE_BAZEL_VERSION="$(cat .bazelversion)"
export USE_BAZEL_VERSION

# label | name in $OUT | source path (a --per_file_copt regex) | define | seeds
readonly HARNESSES=(
  "//pagespeed/kernel/html:html_fuzz|html_parse_fuzzer|pagespeed/kernel/html/html_fuzz[.]cc|HTML_PARSE_LIBFUZZER|dir:tools/html-parse-corpus/seeds"
  "//pagespeed/kernel/js:js_minify_fuzz|js_minify_fuzzer|pagespeed/kernel/js/js_minify_fuzz[.]cc|JS_MINIFY_LIBFUZZER|dump"
  "//pagespeed/kernel/packs:pack_loader_fuzz|pack_loader_fuzzer|pagespeed/kernel/packs/pack_loader_fuzz[.]cc|PACK_LOADER_LIBFUZZER|dir:tools/pack-loader-corpus/seeds"
  "//pagespeed/kernel/packs:rule_pack_filter_fuzz|rule_pack_filter_fuzzer|pagespeed/kernel/packs/rule_pack_filter_fuzz[.]cc|RULE_PACK_FILTER_LIBFUZZER|dir:tools/rule-pack-filter-corpus/seeds"
  "//third_party/css_parser:parser_fuzz|css_parser_fuzzer|third_party/css_parser/src/webutil/css/parser_fuzz[.]cc|CSS_PARSER_LIBFUZZER|dump"
)

# bazel-bin path of a label: //a/b:c -> bazel-bin/a/b/c
bin_of() { local l="${1#//}"; echo "bazel-bin/${l/:/\/}"; }

# The flags every build here shares. OSS-Fuzz's compiler flags go to C and
# C++ compiles as they are; the sanitizer and coverage flags go on the link
# line too, so the driver links their runtimes. -stdlib=libc++ stays off the
# link line: the oss-fuzz config links OSS-Fuzz's libc++ statically.
flags=(--config=oss-fuzz -c opt "--action_env=CC=${CC}" "--action_env=CXX=${CXX}")
for f in ${CFLAGS}; do flags+=("--conlyopt=${f}"); done
for f in ${CXXFLAGS}; do flags+=("--cxxopt=${f}"); done
for f in ${CXXFLAGS}; do
  case "${f}" in -stdlib=*) ;; *) flags+=("--linkopt=${f}") ;; esac
done
if [ "${SANITIZER:-}" = undefined ]; then
  # Bazel links with the C driver, which leaves out the C++ half of the UBSan
  # runtime (bazelbuild/bazel#8777); the vptr checks need it.
  flags+=("--linkopt=$("${CXX}" -print-file-name=libclang_rt.ubsan_standalone_cxx.a)")
fi

# 1. Replay binaries of the harnesses whose seed corpus is dumped from them.
replay=()
for h in "${HARNESSES[@]}"; do
  IFS='|' read -r label _ _ _ seeds <<<"${h}"
  [ "${seeds}" = dump ] && replay+=("${label}")
done
bazel build "${flags[@]}" -- "${replay[@]}"
for h in "${HARNESSES[@]}"; do
  IFS='|' read -r label name _ _ seeds <<<"${h}"
  [ "${seeds}" = dump ] || continue
  dir="$(mktemp -d)"
  if "$(bin_of "${label}")" "--dump_corpus=${dir}" && [ -n "$(ls -A "${dir}")" ]; then
    zip -j -q "${OUT}/${name}_seed_corpus.zip" "${dir}"/*
  else
    echo "WARNING: no seed corpus for ${name}: the replay binary did not dump one" >&2
  fi
  rm -rf "${dir}"
done

# 2. The fuzzers: every harness in one build, each source given its define,
#    all linked against the engine.
fuzz=("${flags[@]}" "--linkopt=${LIB_FUZZING_ENGINE}")
targets=()
for h in "${HARNESSES[@]}"; do
  IFS='|' read -r label _ src define _ <<<"${h}"
  fuzz+=("--per_file_copt=${src}@-D${define}")
  targets+=("${label}")
done
bazel build "${fuzz[@]}" -- "${targets[@]}"

# 3. Binaries, the file-based seed corpora and the dictionaries into $OUT.
for h in "${HARNESSES[@]}"; do
  IFS='|' read -r label name _ _ seeds <<<"${h}"
  cp -f "$(bin_of "${label}")" "${OUT}/${name}"
  case "${seeds}" in
    dir:*) zip -j -q "${OUT}/${name}_seed_corpus.zip" "${seeds#dir:}"/* ;;
  esac
  if [ -f ".oss-fuzz/dicts/${name}.dict" ]; then
    cp -f ".oss-fuzz/dicts/${name}.dict" "${OUT}/${name}.dict"
  fi
done

# 4. For the coverage build, the sources as the compiler saw them: Bazel
#    compiles from its execution root with workspace-relative paths, and the
#    report resolves them against $OUT/proc/self/cwd.
if [ "${SANITIZER:-}" = coverage ]; then
  mkdir -p "${OUT}/proc/self/cwd"
  rsync -aLk \
    --include '*.h' --include '*.hh' --include '*.hpp' --include '*.inc' \
    --include '*.c' --include '*.cc' --include '*.cpp' --include '*/' \
    --exclude '*' \
    "$(bazel info execution_root)/" "${OUT}/proc/self/cwd/"
fi

bazel shutdown
