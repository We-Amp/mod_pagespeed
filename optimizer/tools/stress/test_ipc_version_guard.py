# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""The stress harness must speak the notify version the daemon accepts.

This is the fast guard in front of the rest of this suite. The daemon checks
the version byte before it reads any field and accepts exactly one value, so a
harness left behind on an older version is not degraded -- it is mute. What
that looks like from here is ``Expected >= 500 notifications received, got 0``
after two minutes of timeouts, in tests that name neither the version nor the
harness.

Running the check as a test means a future protocol bump reports itself in a
second, by name, instead of being diagnosed by hand from the wire format.

No worker, no container, no build: it reads two checked-in text files.
"""

import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "ci" / "check_ipc_version.py"


def _run(*args):
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        capture_output=True,
        text=True,
        cwd=str(REPO_ROOT),
    )


def test_checker_self_test_passes():
    """The guard's own parsers still match the shapes they are meant to."""
    result = _run("--self-test")
    assert result.returncode == 0, (
        f"check_ipc_version.py --self-test failed:\n"
        f"{result.stdout}\n{result.stderr}"
    )


def test_harness_ipc_version_matches_header():
    """IPC_VERSION in ipc_client.py == kIpcVersion in src/proto/worker_ipc.h."""
    result = _run("--repo-root", str(REPO_ROOT))
    assert result.returncode == 0, (
        f"{result.stdout}\n{result.stderr}"
    )


def test_serialized_frame_carries_the_declared_version():
    """The constant is not just declared -- it is what goes on the wire.

    A constant that agrees with the header while the serializer still packs a
    literal would pass the text check and fail against a real daemon, so this
    reads the version byte back out of an actual frame.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import ipc_client

    frame = ipc_client.IpcClient.serialize_notification(
        "https://example.com/x", ipc_client.ContentType.HTML,
        hostname="example.com"
    )
    # [4B total_length][1B version]...
    assert frame[4] == ipc_client.IPC_VERSION, (
        f"serialized frame carries version {frame[4]}, but IPC_VERSION is "
        f"{ipc_client.IPC_VERSION}"
    )
