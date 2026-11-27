#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""CI check: the container health probes wait as long as they claim to.

The Compose files and the Helm chart probe the optimizer's health socket with
socat. The optimizer writes one status line when the connection is accepted,
so the probe's deadline is however long socat waits for that line. Two things
decide that, and neither is the probe's `timeout`:

  * a probe that pipes an (empty) stdin into a two-way socat sees end of input
    at once, and socat then closes 0.5 s later (its `-t` default) -- a busy
    optimizer that answers after 0.6 s is reported unhealthy;
  * a one-way probe (`-u`) waits for the socket, bounded by `-T`.

Part 1 (always runs) reads every shipped probe and requires the one-way form
with an explicit `-T` that sits inside the probe's own timeout.

Part 2 (needs socat; skipped without it unless CHECK_HEALTH_PROBE_REQUIRED=1)
runs each probe command against a stand-in health socket that answers at once,
answers after 2 s, and never answers, and checks exit status and elapsed time.

Run:  python3 tools/ci/check_health_probe.py
"""

import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# File -> number of health probes it must carry. A probe that disappears (or a
# new one that is added) has to be acknowledged here.
PROBE_FILES = {
    "deploy/docker-compose.yml": 1,
    "deploy/helm/pagespeed/templates/deployment.yaml": 3,
    "website/public/docker-compose.yml": 1,
    "website/src/content/docs/deployment.mdx": 2,
    "website/src/content/blog/run-with-docker-compose.md": 1,
}

HEALTH_SOCKET = "/data/pagespeed.sock.health"
REPLY = (
    b"OK 0/128 notifs=0 variants=0 proactive=0 errors=0 cache_entries=0 inflight=0\n"
)
TIMEOUT_RE = re.compile(r"^\s*(?:timeout|timeoutSeconds):\s*(\d+)s?\s*$")
# The delay the old 0.5 s deadline could not survive but the documented one can.
SLOW_REPLY_SECONDS = 2.0


def find_probes(rel_path):
    """Returns [(line_no, command, timeout_seconds)] for one file."""
    with open(os.path.join(REPO_ROOT, rel_path), encoding="utf-8") as f:
        lines = f.read().splitlines()
    probes = []
    for i, line in enumerate(lines):
        if "socat" not in line or HEALTH_SOCKET not in line:
            continue
        stripped = line.strip()
        if stripped.startswith("#") or stripped.startswith("$"):
            continue  # a comment, or an interactive example in prose
        # The command is the last double-quoted string on the line.
        quoted = re.findall(r'"((?:[^"\\]|\\.)*)"', line)
        if not quoted:
            raise SystemExit(
                f"ERROR: {rel_path}:{i + 1}: cannot read the probe command "
                "(expected it in double quotes)"
            )
        # YAML double-quoted scalars escape a backslash as two.
        command = quoted[-1].replace("\\\\", "\\")
        timeout = None
        for later in lines[i + 1 : i + 12]:
            m = TIMEOUT_RE.match(later)
            if m:
                timeout = int(m.group(1))
                break
        probes.append((i + 1, command, timeout))
    return probes


def check_static(rel_path, line_no, command, timeout):
    """Returns a list of problems with one probe command."""
    where = f"{rel_path}:{line_no}"
    problems = []
    socat = command.split("|")[0].split()
    if not socat or socat[0] != "socat":
        problems.append(
            f"{where}: the probe must start with socat and feed it no input; "
            "piping an empty stdin into socat ends the wait 0.5 s after "
            "connecting"
        )
        return problems
    if "-u" not in socat:
        problems.append(
            f"{where}: socat must run one-way (-u) so the wait is bounded by "
            "-T, not by end of input"
        )
    if "-t" in socat:
        problems.append(f"{where}: use -T (the read deadline), not -t")
    deadline = None
    if "-T" in socat and socat.index("-T") + 1 < len(socat):
        try:
            deadline = float(socat[socat.index("-T") + 1])
        except ValueError:
            pass
    if deadline is None:
        problems.append(f"{where}: socat needs an explicit -T <seconds>")
    if timeout is None:
        problems.append(f"{where}: no timeout / timeoutSeconds found below the probe")
    if (
        deadline is not None
        and timeout is not None
        and not (SLOW_REPLY_SECONDS < deadline < timeout)
    ):
        problems.append(
            f"{where}: -T {deadline:g} must be above "
            f"{SLOW_REPLY_SECONDS:g} s and inside the probe timeout "
            f"({timeout} s)"
        )
    return problems


class StandIn:
    """A health socket that replies `delay` seconds after accepting."""

    def __init__(self, path, delay):
        self._delay = delay
        self._stop = threading.Event()
        self._server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._server.bind(path)
        self._server.listen(8)
        self._server.settimeout(0.1)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                conn, _ = self._server.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        with conn:
            if self._delay is None:
                self._stop.wait()  # never reply
                return
            if self._stop.wait(self._delay):
                return
            try:
                conn.sendall(REPLY)
            except OSError:
                pass

    def close(self):
        self._stop.set()
        self._thread.join()
        self._server.close()


def run_probe(command, sock_path, delay, limit):
    """Runs one probe as the deployments do (sh -c); returns (rc, seconds)."""
    stand_in = StandIn(sock_path, delay)
    try:
        start = time.monotonic()
        try:
            rc = subprocess.run(
                ["sh", "-c", command.replace(HEALTH_SOCKET, sock_path)],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=limit,
            ).returncode
        except subprocess.TimeoutExpired:
            rc = None
        return rc, time.monotonic() - start
    finally:
        stand_in.close()
        os.unlink(sock_path)


def check_behaviour(command, timeout):
    """Returns a list of problems seen when running one probe command."""
    problems = []
    tmp = tempfile.mkdtemp(prefix="hp-", dir="/tmp")
    sock_path = os.path.join(tmp, "h.sock")
    try:
        rc, took = run_probe(command, sock_path, 0.0, timeout)
        print(f"    immediate reply:   rc={rc} in {took:.2f}s")
        if rc != 0:
            problems.append(f"an immediate reply must pass (rc={rc})")

        rc, took = run_probe(command, sock_path, SLOW_REPLY_SECONDS, timeout)
        print(f"    reply after {SLOW_REPLY_SECONDS:g} s:   rc={rc} in {took:.2f}s")
        if rc != 0:
            problems.append(
                f"a reply after {SLOW_REPLY_SECONDS:g} s is inside the "
                f"documented {timeout} s and must pass (rc={rc} after "
                f"{took:.2f}s)"
            )

        rc, took = run_probe(command, sock_path, None, timeout)
        print(f"    no reply:          rc={rc} in {took:.2f}s")
        if rc is None:
            problems.append(
                f"with no reply the probe must fail by itself inside its "
                f"{timeout} s timeout; it was still running"
            )
        elif rc == 0:
            problems.append("with no reply the probe must fail")
        elif took <= SLOW_REPLY_SECONDS:
            problems.append(
                f"with no reply the probe gave up after {took:.2f}s, "
                "sooner than a slow reply it must accept"
            )
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return problems


def main():
    problems = []
    commands = {}  # command -> timeout
    print("=== Health probes: form and deadline ===")
    for rel_path, expected in PROBE_FILES.items():
        probes = find_probes(rel_path)
        if len(probes) != expected:
            problems.append(
                f"{rel_path}: expected {expected} health probe(s), found "
                f"{len(probes)} (update PROBE_FILES if this is intended)"
            )
        for line_no, command, timeout in probes:
            found = check_static(rel_path, line_no, command, timeout)
            problems.extend(found)
            if not found:
                print(f"OK: {rel_path}:{line_no}: {command}")
                commands.setdefault(command, timeout)
    if problems:
        for p in problems:
            print(f"ERROR: {p}")
        return 1

    print("")
    print("=== Health probes: behaviour against a stand-in socket ===")
    if shutil.which("socat") is None:
        if os.environ.get("CHECK_HEALTH_PROBE_REQUIRED", "0") == "1":
            print("ERROR: socat not installed and CHECK_HEALTH_PROBE_REQUIRED=1")
            return 1
        print("SKIP: socat not installed")
        return 0
    for command, timeout in commands.items():
        print(f"  {command}")
        for p in check_behaviour(command, timeout):
            problems.append(f"{command}: {p}")
    if problems:
        for p in problems:
            print(f"ERROR: {p}")
        return 1
    print("")
    print("All health probe checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
