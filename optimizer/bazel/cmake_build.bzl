# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Starlark rule for cmake-based third_party builds with resource_set.

Replaces genrule + local=True for cmake libraries, gaining scheduler-aware
CPU/memory throttling so Bazel doesn't over-subscribe the host when cmake
spawns parallel compiler processes internally.

Requires on PATH: cmake, a C/C++ compiler, Perl (for libaom RTCD headers).
On Windows: MSVC (via vcvars64 or auto-detected), Ninja, MSYS2 coreutils.
"""

def _cmake_resource_set(_os, _inputs_size):
    """Declares the resource cost of a cmake build action.

    cmake spawns parallel compiler processes internally (via nproc), so a
    single Bazel action consumes most of the host's CPUs and significant
    memory.  Declaring high resource usage prevents Bazel from scheduling
    other heavy actions alongside the cmake build.
    """
    return {"cpu": 8, "memory": 8192}

def _cmake_build_impl(ctx):
    is_windows = ctx.target_platform_has_constraint(
        ctx.attr._windows_constraint[platform_common.ConstraintValueInfo],
    )

    out = ctx.actions.declare_file(ctx.attr.out)

    # Escape braces in cmake flags so Python .format() doesn't choke on
    # cmake generator expressions like -DCMAKE_CXX_FLAGS=-fsanitize={address}.
    cmake_flags = " ".join(ctx.attr.cmake_flags).replace("{", "{{").replace("}", "}}")

    if is_windows:
        script = _WINDOWS_SCRIPT.format(
            cmake_lists = ctx.file.cmake_lists.path,
            cmake_flags = cmake_flags,
            out = out.path,
        )
    else:
        script = _UNIX_SCRIPT.format(
            cmake_lists = ctx.file.cmake_lists.path,
            cmake_flags = cmake_flags,
            out = out.path,
        )

    ctx.actions.run_shell(
        inputs = depset(ctx.files.srcs + [ctx.file.cmake_lists]),
        outputs = [out],
        command = script,
        mnemonic = "CMakeBuild",
        progress_message = "Building %s with cmake" % ctx.label.name,
        use_default_shell_env = True,
        execution_requirements = {
            "local": "",
            "no-sandbox": "",
            "no-remote": "",
        },
        resource_set = _cmake_resource_set,
    )

    return [DefaultInfo(files = depset([out]))]

# Capped at 8 to match the resource_set above and the module's foreign_cc cap.
# A Bazel action cannot see the CI slot budget without putting it into every
# action's cache key, so the cap is a fixed number.
_NPROC = "$(n=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4); [ \"$n\" -gt 8 ] && n=8; echo $n)"

_UNIX_SCRIPT = """\
set -e
SRC_DIR="$(dirname '{cmake_lists}')"
BUILD_DIR="$(mktemp -d)"
trap 'rm -rf "$BUILD_DIR"' EXIT
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    {cmake_flags} \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
cmake --build "$BUILD_DIR" --config Release -j """ + _NPROC + """
cp "$BUILD_DIR/$(basename '{out}')" '{out}'
"""

_WINDOWS_SCRIPT = """\
set -e
SRC_DIR="$(dirname '{cmake_lists}')"
SRC_DIR="$(cygpath -m "$SRC_DIR" 2>/dev/null || echo "$SRC_DIR")"
BUILD_DIR="$(mktemp -d)"
BUILD_DIR_WIN="$(cygpath -m "$BUILD_DIR" 2>/dev/null || echo "$BUILD_DIR")"
OUT="$(cygpath -u '{out}' 2>/dev/null || echo '{out}')"
trap 'rm -rf "$BUILD_DIR"' EXIT

# Auto-detect MSVC environment if not already set (CI genrule sandbox).
if ! command -v cl.exe >/dev/null 2>&1; then
    MSVC_DIR="$(find "/c/Program Files"*/Microsoft*/2022/*/VC/Tools/MSVC -maxdepth 0 2>/dev/null | head -1)"
    SDK_DIR="/c/Program Files (x86)/Windows Kits/10"
    SDK_VER="$(ls "$SDK_DIR/Include/" 2>/dev/null | sort -V | tail -1)"
    if [ -n "$MSVC_DIR" ] && [ -n "$SDK_VER" ]; then
        MSVC_VER="$(ls "$MSVC_DIR" | sort -V | tail -1)"
        export PATH="$MSVC_DIR/$MSVC_VER/bin/Hostx64/x64:$SDK_DIR/bin/$SDK_VER/x64:$PATH"
        M="$(cygpath -m "$MSVC_DIR/$MSVC_VER" 2>/dev/null || echo "$MSVC_DIR/$MSVC_VER")"
        S="$(cygpath -m "$SDK_DIR" 2>/dev/null || echo "$SDK_DIR")"
        export INCLUDE="$M/include;$S/Include/$SDK_VER/ucrt;$S/Include/$SDK_VER/um;$S/Include/$SDK_VER/shared"
        export LIB="$M/lib/x64;$S/Lib/$SDK_VER/ucrt/x64;$S/Lib/$SDK_VER/um/x64"
    fi
fi

cmake -S "$SRC_DIR" -B "$BUILD_DIR_WIN" \
    -G Ninja \
    {cmake_flags}
cmake --build "$BUILD_DIR_WIN" --config Release
cp "$BUILD_DIR_WIN/$(basename '{out}')" "$OUT"
"""

cmake_build = rule(
    implementation = _cmake_build_impl,
    attrs = {
        "srcs": attr.label_list(
            allow_files = True,
            doc = "All source files for the cmake project.",
        ),
        "cmake_lists": attr.label(
            allow_single_file = ["CMakeLists.txt"],
            mandatory = True,
            doc = "The CMakeLists.txt at the root of the cmake project.",
        ),
        "cmake_flags": attr.string_list(
            doc = "List of -DKEY=VALUE cmake cache entries.",
        ),
        "out": attr.string(
            mandatory = True,
            doc = "Output library filename (e.g. libaom.a or aom.lib).",
        ),
        "_windows_constraint": attr.label(
            default = "@platforms//os:windows",
        ),
    },
    doc = "Builds a cmake project and produces a single static library.",
)
