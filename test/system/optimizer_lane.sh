#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# The optimizer worker and client library of the daemon lane.
#
# Sourced by setup_apache_test.sh and run_nginx_tests.sh (they provide
# PROJECT_ROOT, log_info and log_warn), or run on its own to build only:
#
#   test/system/optimizer_lane.sh build LOG_FILE RESULT_FILE
#
# which builds (or reuses) the pinned build and writes the directory holding
# it to RESULT_FILE; CI runs that in its own container, then hands the
# directory read-only to the test container.
#
# The lane builds the optimizer from its public source at one pinned commit,
# with the toolchain already in the dev container (the released packages need
# a newer C library than the container has). The pin is
# test/system/optimizer-test-pin.json, any optimizer main commit;
# PAGESPEED_OPTIMIZER_PIN names another pin file with an optimizer_commit
# field, such as the release pin install/debian/optimizer-pin.json.
#
# Environment:
#   PAGESPEED_DAEMON_ARTIFACT_DIR   where builds are kept, one directory per
#                                   pin and recipe (default
#                                   ~/.cache/pagespeed-daemon-lane)
#   PAGESPEED_DAEMON_ARTIFACT_PIN_DIR
#                                   use exactly this build directory, after
#                                   checking it; never build
#   PAGESPEED_DAEMON_ARTIFACT_OWNER uid a reused build must belong to
#                                   (default: the current uid)
#   PAGESPEED_OPTIMIZER_LANE_IMAGE  the dev image tag, part of the recipe key
#   PAGESPEED_OPTIMIZER_BAZEL_JOBS, PAGESPEED_OPTIMIZER_BAZEL_RAM_MB
#                                   resource limits for the build
#
# A build directory is named <commit12>-<recipe8>: the recipe hash covers
# this script, the cmake requirements, the cache-library commit this module
# links, the toolchain and the image tag, so a change to any of them builds
# again instead of reusing an older build. Each directory carries SHA256SUMS
# and optimizer-commit.txt, and is reused only when both check out and it
# and its files belong to the expected uid and are writable by no one else.
# A directory that fails the check is left alone; the build then goes to a
# fresh directory of its own.

OPTIMIZER_LANE_PIN_DEFAULT="$PROJECT_ROOT/test/system/optimizer-test-pin.json"
OPTIMIZER_LANE_REPO="https://github.com/We-Amp/pagespeed-optimizer.git"
OPTIMIZER_LANE_SCRIPT="$PROJECT_ROOT/test/system/optimizer_lane.sh"
OPTIMIZER_LANE_FILES="factory_worker libpagespeed.so optimizer-commit.txt"
# Minutes a lane waits for another build on the same host.
OPTIMIZER_LANE_LOCK_MINUTES=60

# Sets OPTIMIZER_LANE_COMMIT and OPTIMIZER_LANE_PIN from the pin file.
optimizer_lane_commit() {
    OPTIMIZER_LANE_PIN="${PAGESPEED_OPTIMIZER_PIN:-$OPTIMIZER_LANE_PIN_DEFAULT}"
    OPTIMIZER_LANE_COMMIT="$(python3 -I -c "import json,sys; print(json.load(open(sys.argv[1]))['optimizer_commit'])" "$OPTIMIZER_LANE_PIN" 2>/dev/null)" \
        || { log_warn "cannot read optimizer_commit from $OPTIMIZER_LANE_PIN"; return 1; }
    if ! [[ "$OPTIMIZER_LANE_COMMIT" =~ ^[0-9a-f]{40}$ ]]; then
        log_warn "optimizer pin $OPTIMIZER_LANE_PIN does not name a full commit hash"
        return 1
    fi
}

# The recipe hash: everything besides the commit that shapes the build.
optimizer_lane_recipe() {
    {
        sha256sum "$OPTIMIZER_LANE_SCRIPT" "$PROJECT_ROOT/tools/daemon-lane-requirements.txt" | cut -d' ' -f1
        grep -E '^\s*CYCLONE_COMMIT\s*=' "$PROJECT_ROOT/bazel/repositories.bzl"
        /usr/bin/clang++-20 --version 2>/dev/null | head -1
        ldd --version 2>/dev/null | head -1
        echo "image=${PAGESPEED_OPTIMIZER_LANE_IMAGE:-}"
    } | sha256sum | cut -c1-8
}

# optimizer_lane_verify DIR: the build in DIR is complete, unaltered, for the
# pinned commit, and belongs to the expected uid alone.
optimizer_lane_verify() {
    local dir="$1" owner="${PAGESPEED_DAEMON_ARTIFACT_OWNER:-$(id -u)}" f
    [ -d "$dir" ] && [ ! -L "$dir" ] || return 1
    [ "$(stat -c %u "$dir")" = "$owner" ] || return 1
    for f in $OPTIMIZER_LANE_FILES SHA256SUMS; do
        [ -f "$dir/$f" ] && [ ! -L "$dir/$f" ] || return 1
        [ "$(stat -c %u "$dir/$f")" = "$owner" ] || return 1
    done
    [ -z "$(find "$dir" -maxdepth 1 -perm /022 -print -quit)" ] || return 1
    [ "$(cat "$dir/optimizer-commit.txt")" = "$OPTIMIZER_LANE_COMMIT" ] || return 1
    (cd "$dir" && sha256sum --quiet -c SHA256SUMS) > /dev/null 2>&1 || return 1
    [ "$(cut -d' ' -f3 "$dir/SHA256SUMS" | sort | tr '\n' ' ')" = "factory_worker libpagespeed.so optimizer-commit.txt " ]
}

# optimizer_lane_build LOG_FILE
# Builds (or reuses) the pinned worker and client library. Sets
# DAEMON_ARTIFACT_DIR. Returns non-zero when they cannot be provided; the
# build log goes to LOG_FILE.
optimizer_lane_build() {
    local log="$1" parent key final
    optimizer_lane_commit || return 1
    log_info "Optimizer pin: $OPTIMIZER_LANE_COMMIT (from ${OPTIMIZER_LANE_PIN#"$PROJECT_ROOT"/})"
    if [ -n "${PAGESPEED_DAEMON_ARTIFACT_PIN_DIR:-}" ]; then
        DAEMON_ARTIFACT_DIR="$PAGESPEED_DAEMON_ARTIFACT_PIN_DIR"
        if ! optimizer_lane_verify "$DAEMON_ARTIFACT_DIR"; then
            log_warn "the optimizer build handed to this lane does not check out"
            return 1
        fi
        log_info "Optimizer worker for ${OPTIMIZER_LANE_COMMIT:0:12}: using the build handed to this lane"
        return 0
    fi
    parent="${PAGESPEED_DAEMON_ARTIFACT_DIR:-${HOME:-/tmp}/.cache/pagespeed-daemon-lane}"
    key="${OPTIMIZER_LANE_COMMIT:0:12}-$(optimizer_lane_recipe)"
    final="$parent/$key"
    DAEMON_ARTIFACT_DIR="$final"
    mkdir -p "$parent" "$(dirname "$log")" || return 1
    # Refresh first, so a prune cannot take a build between check and use.
    touch -c "$final" 2> /dev/null || true
    if optimizer_lane_verify "$final"; then
        log_info "Optimizer worker for $key already built"
        return 0
    fi

    # One build at a time per host: wait for a concurrent build, then reuse
    # what it published.
    local lock_fd rc=0
    exec {lock_fd}>> "$parent/.build.lock" || return 1
    if command -v flock > /dev/null 2>&1 \
        && ! flock -w $((OPTIMIZER_LANE_LOCK_MINUTES * 60)) "$lock_fd"; then
        log_warn "another optimizer build on this host held the lock for $OPTIMIZER_LANE_LOCK_MINUTES minutes"
        exec {lock_fd}>&-
        return 1
    fi
    _optimizer_lane_build_locked "$parent" "$key" "$log" || rc=$?
    exec {lock_fd}>&-
    return "$rc"
}

_optimizer_lane_build_locked() {
    local parent="$1" key="$2" log="$3" final="$1/$2" commit="$OPTIMIZER_LANE_COMMIT"
    if optimizer_lane_verify "$final"; then
        log_info "Optimizer worker for $key was built by a concurrent lane"
        return 0
    fi
    local target=""
    if [ -e "$final" ] || [ -L "$final" ]; then
        # Not ours or not sound: never reuse it, never remove it.
        log_warn "$final exists but does not check out; building into a fresh directory"
        target="$(mktemp -d "$parent/.fresh-$key-XXXXXX")" || return 1
    fi
    log_info "Building the optimizer worker at $key..."
    # Stable paths: Bazel action keys include PATH and the action
    # environment, so a per-run path would defeat the remote cache.
    # The same path for every uid (container root on a rootless daemon and
    # the runner's uid elsewhere must compute the same keys); a per-uid one
    # only where another uid already holds it on a shared /tmp.
    local work="${TMPDIR:-/tmp}/optimizer-lane"
    if [ -e "$work" ] && [ "$(stat -c %u "$work")" != "$(id -u)" ]; then
        work="$work-$(id -u)"
    fi
    local src="$work/src" pyuser="$work/py" broot="$work/bazel"
    local cfg="--config=docker" path_extra="" env_extra=""
    chmod -R u+w "$work" 2> /dev/null || true
    rm -rf "${src:?}" "${broot:?}"
    mkdir -p "$src"
    if ! { git -C "$src" init -q \
            && git -C "$src" fetch -q --depth 1 "$OPTIMIZER_LANE_REPO" "$commit" \
            && git -C "$src" checkout -q --detach FETCH_HEAD; }; then
        log_warn "cannot fetch the optimizer source at ${commit:0:12}"
        return 1
    fi
    if ! python3 -I "$PROJECT_ROOT/tools/ci/check_optimizer_pin_pair.py" "$OPTIMIZER_LANE_PIN" \
            "$src/MODULE.bazel" "$PROJECT_ROOT/bazel/repositories.bzl" > "$log" 2>&1; then
        log_warn "the pinned optimizer links another cache library than this module"
        cat "$log"
        return 1
    fi
    # Share the CI build cache where the host provides one.
    if getent hosts bazel-remote-cache > /dev/null 2>&1; then
        cfg="$cfg --config=remote-cache"
    fi
    [ -n "${PAGESPEED_OPTIMIZER_BAZEL_JOBS:-}" ] && cfg="$cfg --jobs=$PAGESPEED_OPTIMIZER_BAZEL_JOBS"
    [ -n "${PAGESPEED_OPTIMIZER_BAZEL_RAM_MB:-}" ] && cfg="$cfg --local_resources=memory=$PAGESPEED_OPTIMIZER_BAZEL_RAM_MB"
    # The bundled image codec is configured by CMake, which some dev images
    # do not carry; a hash-pinned user-level install keeps the image unchanged.
    if ! command -v cmake > /dev/null 2>&1; then
        if [ ! -x "$pyuser/bin/cmake" ]; then
            PYTHONUSERBASE="$pyuser" pip3 install --user -q --require-hashes \
                -r "$PROJECT_ROOT/tools/daemon-lane-requirements.txt" > /dev/null 2>&1 \
                || { log_warn "cannot install cmake"; return 1; }
        fi
        path_extra="$pyuser/bin:"
        # The sandbox does not pass HOME on, so say where it lives.
        env_extra="--action_env=PYTHONUSERBASE=$pyuser"
    fi
    local built=0
    (
        cd "$src"
        export PATH="$path_extra$PATH" CC=/usr/bin/clang-20 CXX=/usr/bin/clang++-20
        # No user rc: HOME may be a module checkout, whose .bazelrc is not
        # the optimizer's.
        # shellcheck disable=SC2086  # cfg and env_extra are lists of flags
        bazel --nohome_rc --output_user_root="$broot" build $cfg \
            --action_env=CC --action_env=CXX $env_extra \
            //src/worker:factory_worker //lib/pagespeed:libpagespeed.so \
            >> "$log" 2>&1
    ) && built=1
    local stage=""
    if [ "$built" = 1 ]; then
        grep -E '^INFO: [0-9]+ processes' "$log" | tail -1 | sed 's/^INFO: /  /' || true
        if [ -z "$target" ]; then
            stage="$(mktemp -d "$parent/.stage-$key-XXXXXX")" || built=0
        else
            stage="$target"
        fi
    else
        log_warn "optimizer worker build failed; see $log"
        tail -20 "$log" || true
    fi
    if [ "$built" = 1 ]; then
        { cp -L "$src/bazel-bin/src/worker/factory_worker" \
                "$src/bazel-bin/lib/pagespeed/libpagespeed.so" "$stage/" \
            && echo "$commit" > "$stage/optimizer-commit.txt" \
            && chmod 555 "$stage/factory_worker" "$stage/libpagespeed.so" \
            && chmod 444 "$stage/optimizer-commit.txt" \
            && (cd "$stage" && sha256sum factory_worker libpagespeed.so optimizer-commit.txt > SHA256SUMS) \
            && chmod 444 "$stage/SHA256SUMS" \
            && chmod 755 "$stage"; } || built=0
    fi
    # Stop the build's own Bazel server, then drop the tree it built in.
    (cd "$src" && bazel --nohome_rc --output_user_root="$broot" shutdown > /dev/null 2>&1) || true
    chmod -R u+w "$src" "$broot" 2> /dev/null || true
    rm -rf "${src:?}" "${broot:?}"
    if [ "$built" != 1 ]; then
        [ -n "$stage" ] && rm -rf "${stage:?}"
        return 1
    fi
    if [ -z "$target" ]; then
        if ! mv -T "$stage" "$final" 2> /dev/null; then
            rm -rf "${stage:?}"
            optimizer_lane_verify "$final" || return 1
        fi
        target="$final"
    fi
    DAEMON_ARTIFACT_DIR="$target"
    optimizer_lane_verify "$target" || { log_warn "the new optimizer build does not check out"; return 1; }
    # Drop this uid's builds no lane on this host used for two weeks, and
    # leftovers of interrupted or one-off builds; never another uid's.
    find "$parent" -mindepth 1 -maxdepth 1 -user "$(id -u)" -type d -mtime +14 \
        -regextype posix-extended -regex '.*/[0-9a-f]{12}-[0-9a-f]{8}' ! -name "$key" \
        -exec rm -rf {} + 2> /dev/null || true
    find "$parent" -mindepth 1 -maxdepth 1 -user "$(id -u)" \( -name '.stage-*' -o -name '.fresh-*' \) \
        -mtime +1 ! -path "$target" -exec rm -rf {} + 2> /dev/null || true
    return 0
}

# Logs the C API version the pinned client library reports, so a lane run
# shows which library it loaded.
optimizer_lane_report_abi() {
    local abi
    abi="$(python3 -I -c 'import ctypes,sys; l=ctypes.CDLL(sys.argv[1]); print("%d.%d" % (l.ps_version_major(), l.ps_version_minor()))' \
        "$DAEMON_ARTIFACT_DIR/libpagespeed.so" 2>/dev/null)" || abi=""
    if [ -z "$abi" ]; then
        log_warn "cannot read the C API version of $DAEMON_ARTIFACT_DIR/libpagespeed.so"
        return 1
    fi
    log_info "Optimizer client library at ${OPTIMIZER_LANE_COMMIT:0:12}: C API $abi (ps_version_minor ${abi#*.})"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -u
    if [ "${1:-}" != build ] || [ $# -ne 3 ]; then
        echo "usage: $0 build LOG_FILE RESULT_FILE" >&2
        exit 2
    fi
    PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    OPTIMIZER_LANE_PIN_DEFAULT="$PROJECT_ROOT/test/system/optimizer-test-pin.json"
    OPTIMIZER_LANE_SCRIPT="$PROJECT_ROOT/test/system/optimizer_lane.sh"
    log_info() { echo "[INFO] $1"; }
    log_warn() { echo "[WARN] $1"; }
    optimizer_lane_build "$2" || exit 1
    optimizer_lane_report_abi || exit 1
    echo "$DAEMON_ARTIFACT_DIR" > "$3"
fi
