# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Shared settings for the rules_foreign_cc cmake() rules in this repo."""

# Number of jobs a cmake() build may run at once.
#
# On Linux and macOS rules_foreign_cc 0.11.1 generates Unix Makefiles and
# runs `cmake --build .` without a job count, so make compiles one file at a
# time. For libaom that is several minutes of a single core while the rest of
# the build has long finished, whatever the machine size.
#
# Bazel schedules the whole cmake() action as one job, so these compiles run
# on top of Bazel's own --jobs. 8 is a deliberate cap rather than nproc: it
# removes most of the serial tail on a large machine and only briefly
# oversubscribes a small one. A bare `--parallel` (no number) is avoided on
# purpose: with Makefiles it becomes an unbounded `make -j`.
_CMAKE_BUILD_JOBS = "8"

def cmake_build_args():
    """build_args for cmake(): a bounded parallel build where it is serial.

    Windows is left alone. There rules_foreign_cc picks the Ninja generator,
    which already runs in parallel, and a fixed job count would only lower it.

    Returns:
      A select() over the build_args list.
    """
    return select({
        "@platforms//os:windows": [],
        "//conditions:default": ["--parallel", _CMAKE_BUILD_JOBS],
    })
