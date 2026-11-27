#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Assert the stress harness speaks the notify wire version the daemon accepts.

The daemon checks the notification's version byte before it reads a single
field and accepts exactly one value, so a harness left behind on an older
version does not degrade gracefully -- every notification it sends is refused
whole. Tests that measure arrival by a counter then report
``Expected >= 500 notifications received, got 0``, which names neither the
version nor the harness, and the harness had been silently mute since the bump.

That is what a previous protocol bump did: it moved ``kIpcVersion`` and left
``tools/stress`` behind, and the periodic suite carried the failure until
someone read the wire format by hand.

This is the cheap guard against the next one: pure stdlib over two checked-in
text files, no build, no daemon, no container. It reads ``kIpcVersion`` from
the C++ header that defines the protocol and ``IPC_VERSION`` from the Python
harness that has to speak it, and requires them to be equal.

  check_ipc_version.py [--repo-root DIR]
  check_ipc_version.py --self-test

Exit status is 0 when they agree, 1 when they do not or when either constant
cannot be found -- a constant that has been renamed away is a guard that has
stopped guarding, so it fails rather than skipping.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

HEADER_REL = Path("src/proto/worker_ipc.h")
HARNESS_REL = Path("tools/stress/ipc_client.py")

# `inline constexpr uint8_t kIpcVersion = 5;` -- tolerant about the qualifiers
# and the integer suffix, strict about the name.
HEADER_RE = re.compile(
    r"\bconstexpr\b[^;=]*\bkIpcVersion\s*=\s*(\d+)\s*[uU]?\s*;"
)

# `IPC_VERSION = 5` at module scope.
HARNESS_RE = re.compile(r"^IPC_VERSION\s*=\s*(\d+)\s*$", re.MULTILINE)


class GuardError(Exception):
    """A constant could not be read, or the two disagree."""


def parse_header_version(text: str) -> int:
    matches = HEADER_RE.findall(text)
    if not matches:
        raise GuardError(
            f"no `constexpr ... kIpcVersion = N;` found in {HEADER_REL}. "
            "If the constant was renamed, update this guard in the same "
            "commit -- it is the only thing keeping the stress harness in "
            "step with the wire format."
        )
    if len(set(matches)) > 1:
        raise GuardError(
            f"{HEADER_REL} defines kIpcVersion more than once with differing "
            f"values: {sorted(set(matches))}."
        )
    return int(matches[0])


def parse_harness_version(text: str) -> int:
    matches = HARNESS_RE.findall(text)
    if not matches:
        raise GuardError(
            f"no module-level `IPC_VERSION = N` found in {HARNESS_REL}. "
            "The harness must name the version it emits in one place so this "
            "guard can check it against the header."
        )
    if len(set(matches)) > 1:
        raise GuardError(
            f"{HARNESS_REL} defines IPC_VERSION more than once with differing "
            f"values: {sorted(set(matches))}."
        )
    return int(matches[0])


def check(repo_root: Path) -> tuple[int, int]:
    """Return (header_version, harness_version) or raise GuardError."""
    header = repo_root / HEADER_REL
    harness = repo_root / HARNESS_REL
    for path in (header, harness):
        if not path.is_file():
            raise GuardError(f"missing file: {path}")

    header_version = parse_header_version(header.read_text(encoding="utf-8"))
    harness_version = parse_harness_version(harness.read_text(encoding="utf-8"))

    if header_version != harness_version:
        raise GuardError(
            f"notify wire-version skew: {HEADER_REL} says kIpcVersion="
            f"{header_version}, {HARNESS_REL} says IPC_VERSION="
            f"{harness_version}.\n"
            "The daemon refuses a notification whose version byte is not "
            "exactly kIpcVersion, so the stress harness is mute against this "
            "build: the notification-flood tests will report 0 received.\n"
            "Update the harness to emit the new version -- including any "
            "fields the bump added to the frame -- and re-run "
            "tools/stress."
        )
    return header_version, harness_version


def find_repo_root(start: Path) -> Path:
    for candidate in [start, *start.parents]:
        if (candidate / HEADER_REL).is_file():
            return candidate
    return start


# --------------------------------------------------------------------------
# Self-test: the guard's own parsers, against synthetic trees. Runs anywhere,
# needs nothing checked in, and fails if a regex stops matching the shape it
# is supposed to match.
# --------------------------------------------------------------------------

def _write_tree(root: Path, header_text: str, harness_text: str) -> None:
    (root / HEADER_REL.parent).mkdir(parents=True, exist_ok=True)
    (root / HARNESS_REL.parent).mkdir(parents=True, exist_ok=True)
    (root / HEADER_REL).write_text(header_text, encoding="utf-8")
    (root / HARNESS_REL).write_text(harness_text, encoding="utf-8")


def self_test() -> int:
    failures = []

    def expect(label: str, condition: bool) -> None:
        if condition:
            print(f"ok: {label}")
        else:
            print(f"FAIL: {label}", file=sys.stderr)
            failures.append(label)

    real_header = "inline constexpr uint8_t kIpcVersion = 5;\n"

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        _write_tree(root, real_header, "IPC_VERSION = 5\n")
        try:
            expect("agreeing versions pass", check(root) == (5, 5))
        except GuardError as exc:
            expect(f"agreeing versions pass ({exc})", False)

        _write_tree(root, real_header, "IPC_VERSION = 4\n")
        try:
            check(root)
            expect("skew is caught", False)
        except GuardError as exc:
            expect("skew is caught", "wire-version skew" in str(exc))

        # The pre-bump shape: a bare literal at the pack site and no constant.
        _write_tree(root, real_header, 'payload = struct.pack("B", 4)\n')
        try:
            check(root)
            expect("a missing harness constant is caught", False)
        except GuardError as exc:
            expect(
                "a missing harness constant is caught",
                "IPC_VERSION" in str(exc),
            )

        _write_tree(root, "constexpr uint8_t kSomethingElse = 5;\n",
                    "IPC_VERSION = 5\n")
        try:
            check(root)
            expect("a renamed header constant is caught", False)
        except GuardError as exc:
            expect("a renamed header constant is caught",
                   "kIpcVersion" in str(exc))

        # Qualifier and suffix tolerance: the header has said all of these.
        for variant in (
            "inline constexpr uint8_t kIpcVersion = 7;\n",
            "constexpr std::uint8_t kIpcVersion = 7;\n",
            "static constexpr uint8_t kIpcVersion = 7u;\n",
            "constexpr uint8_t kIpcVersion=7;\n",
        ):
            _write_tree(root, variant, "IPC_VERSION = 7\n")
            try:
                expect(f"parses {variant.strip()!r}", check(root) == (7, 7))
            except GuardError as exc:
                expect(f"parses {variant.strip()!r} ({exc})", False)

    if failures:
        print(f"self-test: {len(failures)} FAILURE(S)", file=sys.stderr)
        return 1
    print("self-test: all checks passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    repo_root = args.repo_root or find_repo_root(Path(__file__).resolve().parent)
    try:
        header_version, _ = check(repo_root)
    except GuardError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(
        f"ok: stress harness and {HEADER_REL} agree on notify wire version "
        f"{header_version}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
