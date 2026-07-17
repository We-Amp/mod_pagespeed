# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

"""M1 merge-gate integration test for the IIS zero-copy aliased serve.

The zero-copy submit loop (IisModuleBaseFetch::DriveZeroCopyServe) carries an
in-code MERGE GATE (M1): the aliased chunk's async Flush is initiated from a
PSOL thread while the request is parked in RQ_BEGIN_REQUEST, relying on IIS
routing its completion to the module's OnAsyncCompletion, and the comment
requires -- before any merge -- that this be

    validated on IIS with an integration test: slow client + AppVerifier + a
    canary that rewrites the mapped region after each completion and asserts
    byte-identity-or-reset.

This module implements the slow-client + region-rewrite-canary halves; the
AppVerifier half is supplied by the harness that runs it
(tools/ci/Invoke-ZeroCopyGate.ps1, wired into the nightly IIS AppVerifier
workflow), which arms full page heap on pagespeed_iis.dll so any aliased
use-after-overwrite surfaces as an AppVerifier stop rather than as silent
corruption on the wire.

Mechanics (black-box, against the running Full-IIS rig):

  * A large `.pagespeed.ic.` cache hit is served via the aliased path (the
    serve points HTTP.sys straight at the Cyclone mmap region and streams it
    as HTTP/1.1 chunked).  The gate resource is deliberately large -- ~0.5 MB,
    several 128 KB aliased chunks -- so the serve runs the full repeated
    submit -> async-flush -> OnZeroCopyCompletion loop (the M1 hazard), not a
    single-chunk special case.  A raw socket with a tiny SO_RCVBUF drains it a
    few KB at a time with sleeps, so TCP flow control keeps each aliased chunk
    in flight longer than Cyclone's read-lease horizon (read_lease_duration
    ~5s; lease_wrap_ceiling ~60s per stripe episode).  One "ceiling" trial
    drains slowly enough to cross the 60s per-stripe ceiling, where the
    forced-wrap deadline ignores the lease and the planner must degrade to the
    copy path.

  * Concurrently, a flood of distinct cacheable inserts -- each carrying a
    unique CANARY token -- churns the (deliberately small, 64 MB) Cyclone
    volume so a wrap can reuse and overwrite the slot the slow client is still
    draining.  This is the "canary that rewrites the mapped region": if the
    strict-renew / copy-then-verify / fail-closed machinery ever let an
    overwritten byte reach the client, the delivered payload would diverge
    from the reference (and could carry the CANARY token).

  * The invariant asserted for EVERY trial is byte-identity-or-reset: the
    decoded bytes delivered must be an exact prefix of the reference payload.
    A correct serve delivers the whole body; a wrap that lapses the lease
    mid-flight is allowed to abort (a shorter prefix / truncated chunk stream
    / connection reset) -- but a torn or foreign byte at any position, and any
    appearance of the CANARY token, is a hard failure.  Every race must
    resolve to {correct bytes, clean abort}, never to torn bytes.

The test is opt-in (PAGESPEED_ZEROCOPY_GATE=1) because it needs the rig
configured with CycloneZeroCopy(+Serve) on, a small Cyclone volume, the large
noise source image and the canary.js flood resource; the gate harness
(setup_iis_full.ps1 under that env var) sets all that up.  Set
PAGESPEED_ZEROCOPY_GATE_FORCEFAIL=1 to corrupt the reference the invariant
compares against, or =2 to corrupt a DELIVERED byte from the first trial --
deliberately-broken runs proving respectively the comparator and the
delivered-byte plumbing have teeth.
"""

import http.client
import os
import re
import socket
import threading
import time

import pytest

pytestmark = pytest.mark.iis_only

GATE_ENV = "PAGESPEED_ZEROCOPY_GATE"
FORCEFAIL_ENV = "PAGESPEED_ZEROCOPY_GATE_FORCEFAIL"
# Unique token the flood inserts carry; must never surface in an aliased serve.
CANARY = b"ZZQX_CYCLONE_ZEROCOPY_CANARY_MARKER_ZZQX"
# The large multi-chunk gate resource, then the small single-chunk fallback.
TARGETS = [
    ("zerocopy_multichunk.html", r'[^"\']*zc_noise_src\.jpg\.pagespeed\.ic\.[^"\']+\.jpg'),
    ("zerocopy_big_image.html", r'[^"\']*Puzzle\.jpg\.pagespeed\.ic\.[^"\']+\.jpg'),
]


def _timeout_scale():
    try:
        return max(1.0, float(os.environ.get("PAGESPEED_TEST_TIMEOUT_MULTIPLIER", "1")))
    except ValueError:
        return 1.0


def _dechunk(raw):
    """Decode an HTTP/1.1 chunked stream, tolerating truncation.

    Returns (decoded_payload, complete).  `complete` is True only when the
    terminating zero-length chunk was seen.  A truncated stream returns the
    decoded prefix received so far -- exactly what byte-identity-or-reset
    needs to compare.
    """
    out = bytearray()
    i, n = 0, len(raw)
    complete = False
    while i < n:
        j = raw.find(b"\r\n", i)
        if j < 0:  # partial size line
            break
        size_token = raw[i:j].split(b";", 1)[0].strip()
        try:
            size = int(size_token, 16)
        except ValueError:
            break
        i = j + 2
        if size == 0:
            complete = True
            break
        if i + size > n:  # partial chunk data (raw payload bytes, no framing)
            out += raw[i:n]
            break
        out += raw[i:i + size]
        i += size
        if raw[i:i + 2] == b"\r\n":
            i += 2
        else:
            break
    return bytes(out), complete


def _drain(host, port, path, *, rcvbuf=None, delay=0.0, chunk=4096, budget=30.0):
    """Fetch `path` over a raw socket, optionally slowly; decode the payload.

    Returns (status, headers_text, payload_bytes, complete_bool, reset_bool).
    With rcvbuf small and delay>0, the server's send blocks on TCP flow
    control, holding an aliased chunk in flight across the read-lease horizon.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf is not None:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    sock.settimeout(20.0)
    try:
        sock.connect((host, port))
        req = (
            "GET {p} HTTP/1.1\r\nHost: {h}\r\n"
            "Accept-Encoding: identity\r\nConnection: close\r\n\r\n"
        ).format(p=path, h=host)
        sock.sendall(req.encode("latin-1"))
    except OSError:
        sock.close()
        raise

    buf = b""
    reset = False
    try:
        while b"\r\n\r\n" not in buf:
            d = sock.recv(256)
            if not d:
                break
            buf += d
    except (socket.timeout, ConnectionResetError, OSError):
        reset = True
    head, _, rest = buf.partition(b"\r\n\r\n")
    headers = head.decode("latin-1", "replace")
    status = 0
    m = re.match(r"HTTP/1\.[01]\s+(\d+)", headers)
    if m:
        status = int(m.group(1))
    is_chunked = bool(re.search(r"(?im)^Transfer-Encoding:\s*chunked", headers))
    clm = re.search(r"(?im)^Content-Length:\s*(\d+)\s*$", headers)
    content_length = int(clm.group(1)) if clm else None

    raw_body = bytearray(rest)
    start = time.time()
    while not reset:
        if time.time() - start > budget:
            break
        if not is_chunked and content_length is not None and len(raw_body) >= content_length:
            break
        if delay:
            time.sleep(delay)
        try:
            d = sock.recv(chunk)
        except (socket.timeout, ConnectionResetError, OSError):
            reset = True
            break
        if not d:  # FIN
            break
        raw_body += d
    try:
        sock.close()
    except OSError:
        pass

    if is_chunked:
        payload, complete = _dechunk(bytes(raw_body))
    elif content_length is not None:
        payload = bytes(raw_body[:content_length])
        complete = len(raw_body) >= content_length
    else:
        payload = bytes(raw_body)
        complete = not reset
    return status, headers, payload, complete, reset


def _resolve_ic_url(host, port, test_root):
    """Drive the fixture pages until a rewritten .pagespeed.ic URL appears.

    Prefers the large multi-chunk resource; falls back to the small one.
    """
    deadline = time.time() + 50.0 * _timeout_scale()
    while time.time() < deadline:
        for page, ic_re in TARGETS:
            status, _, body, _, _ = _drain(
                host, port, "{r}/{p}".format(r=test_root, p=page), budget=25.0
            )
            if status == 200:
                m = re.search(ic_re.encode("latin-1"), body)
                if m:
                    return m.group(0).decode("latin-1")
        time.sleep(1.0)
    return None


def _aliased_count(client, stats_path):
    return client.get_statistics(stats_path=stats_path).get("zerocopy_serve_aliased", 0)


class _Flood:
    """Background volume churn: distinct canary-bearing cacheable inserts."""

    def __init__(self, host, port, test_root, workers=6):
        self.host, self.port, self.test_root = host, port, test_root
        self.workers = workers
        self._stop = threading.Event()
        self._threads = []
        self.inserts = 0
        self._lock = threading.Lock()

    def _run(self):
        n = 0
        while not self._stop.is_set():
            n += 1
            # Distinct input URL each time -> distinct Cyclone HTTP-cache
            # insert -> volume churn.  canary.js is written into the web root
            # by the gate harness and is full of the CANARY token.
            q = "{r}/canary.js?PageSpeedFilters=rewrite_javascript&v={v}".format(
                r=self.test_root, v="%d_%d" % (threading.get_ident(), n)
            )
            try:
                conn = http.client.HTTPConnection(self.host, self.port, timeout=8)
                conn.request("GET", q, headers={"Accept-Encoding": "identity"})
                resp = conn.getresponse()
                resp.read()
                conn.close()
                with self._lock:
                    self.inserts += 1
            except OSError:
                pass

    def __enter__(self):
        for _ in range(self.workers):
            t = threading.Thread(target=self._run, daemon=True)
            t.start()
            self._threads.append(t)
        return self

    def __exit__(self, *exc):
        self._stop.set()
        for t in self._threads:
            t.join(timeout=5.0)


def _first_divergence(a, b):
    for k in range(len(a)):
        if k >= len(b) or a[k] != b[k]:
            return k
    return len(a)


@pytest.mark.slow
class TestZeroCopyMergeGate:
    """M1 gate: slow client + region-rewrite canary on the aliased serve."""

    def _require_gate(self, client, server_config):
        if os.environ.get(GATE_ENV) != "1":
            pytest.skip(
                "zero-copy M1 gate is opt-in; the gate harness sets "
                "PAGESPEED_ZEROCOPY_GATE=1 after configuring the rig with "
                "CycloneZeroCopy(+Serve) on, a small Cyclone volume and the "
                "gate fixtures"
            )
        host, port = server_config.host, server_config.port
        ic = _resolve_ic_url(host, port, server_config.test_root)
        if not ic:
            pytest.skip("no rewritten .pagespeed.ic URL appeared for the gate")
        # Warm the target into the Cyclone mmap volume and confirm the aliased
        # path engages.  A single hit right after cache setup can be a
        # RAM-cache/copied serve; retry so a transient copied-first serve is
        # not mistaken for a copy-only rig (aliased flat).
        before = _aliased_count(client, server_config.stats_path)
        engaged = False
        for _ in range(10):
            _drain(host, port, ic, budget=25.0)
            if _aliased_count(client, server_config.stats_path) > before:
                engaged = True
                break
            time.sleep(0.5)
        if not engaged:
            pytest.skip(
                "aliased serve never engaged over 10 warm hits "
                "(zerocopy_serve_aliased flat at {b}); CycloneZeroCopyServe "
                "off or Cyclone not memory-mapped on this rig".format(b=before)
            )
        return host, port, ic

    def _run_trial(self, host, port, ic, reference, *, delay, budget, label):
        status, _, body, complete, reset = _drain(
            host, port, ic, rcvbuf=2048, delay=delay, chunk=8192, budget=budget,
        )
        if os.environ.get(FORCEFAIL_ENV) == "2" and label == "trial 0" and body:
            # Deliberate break, mode 2: corrupt a DELIVERED byte.  Unlike
            # mode 1 (which corrupts the reference), this proves the harness
            # actually compares the bytes that came off the wire.
            mid = len(body) // 2
            body = body[:mid] + bytes([body[mid] ^ 0xFF]) + body[mid + 1:]
        # THE INVARIANT (byte-identity-or-reset): every delivered byte must
        # match the reference at its position.  A full serve delivers the
        # whole body; an abort delivers a strict prefix and stops.  A tear --
        # any byte that differs, at any position -- fails here.
        prefix_ok = body == reference[:len(body)]
        assert prefix_ok, (
            "{L}: delivered bytes diverge from the reference at offset {off} "
            "(torn/foreign serve; status={s}, got {n}/{ref} decoded bytes, "
            "complete={c}, reset={r})".format(
                L=label, off=_first_divergence(body, reference),
                s=status, n=len(body), ref=len(reference), c=complete, r=reset,
            )
        )
        assert CANARY not in body, (
            "{L}: aliased serve leaked the flood canary token (a foreign "
            "cache slot reached the client)".format(L=label)
        )
        # SILENT-TRUNCATION SIGNATURE (hard fail): the aliased serve is
        # HTTP/1.1 chunked with no Content-Length, so the ONLY way a client
        # or downstream cache can detect an abort is the absence of the
        # terminating 0-length chunk.  A short body that arrived with a clean
        # terminator is an abort masquerading as a complete response --
        # AbortZeroCopyServe must reset the socket (RST) instead; see the
        # ResetConnection contract in iis_module_base_fetch.cpp.
        full = len(body) == len(reference)
        assert not (complete and not full), (
            "{L}: SILENT TRUNCATION -- abort delivered a cleanly-terminated "
            "short body ({n}/{ref} bytes ending in the final 0-length "
            "chunk); a torn abort must surface as a reset/unterminated "
            "stream".format(L=label, n=len(body), ref=len(reference))
        )
        return full, len(body)

    def test_slow_client_region_rewrite_canary(self, client, server_config):
        host, port, ic = self._require_gate(client, server_config)

        # Reference: the full, correct payload, captured BEFORE any flood.
        # Under the small volume the resource can be transiently evicted (an
        # empty/short serve), so take the longest of several fast drains and
        # require two consecutive identical full-length reads for stability.
        reference = b""
        prev = None
        for _ in range(12):
            _, _, cand, _, _ = _drain(host, port, ic, budget=40.0)
            if len(cand) > len(reference):
                reference = cand
            # Stable only counts if it is also the longest seen: a transient
            # short/evicted serve repeating twice must not latch over a
            # longer (full) read captured earlier.
            if cand and cand == prev and len(cand) >= len(reference):
                reference = cand
                break
            prev = cand
            time.sleep(0.3)
        assert reference, "reference fetch never returned a non-empty body"
        assert CANARY not in reference, "reference already contains the canary"
        # The gate MUST run against the multi-chunk resource (several 128 KB
        # aliased chunks) -- that is what exercises the M1 repeated
        # submit -> async-flush -> completion loop.  If rig setup failed to
        # generate zc_noise_src.jpg, the resolver silently falls back to the
        # small single-chunk image; a weakened nightly is false confidence,
        # so fail loudly instead.
        assert len(reference) >= 160 * 1024, (
            "gate resource is not multi-chunk ({n} bytes < 160 KB): the rig "
            "is serving the single-chunk fallback -- check zc_noise_src.jpg "
            "generation in setup_iis_full.ps1".format(n=len(reference))
        )
        print("M1 gate: reference payload = {n} bytes (~{c} aliased chunks)".format(
            n=len(reference), c=max(1, (len(reference) - 16384 + 131071) // 131072)))
        if os.environ.get(FORCEFAIL_ENV) == "1":
            # Deliberate break: corrupt the middle of the reference so the
            # byte-identity invariant must fire on a correct (complete) serve.
            mid = len(reference) // 2
            reference = (
                reference[:mid]
                + bytes([reference[mid] ^ 0xFF])
                + reference[mid + 1:]
            )

        stats0 = client.get_statistics(stats_path=server_config.stats_path)
        scale = _timeout_scale()
        trials = int(os.environ.get("PAGESPEED_ZEROCOPY_GATE_TRIALS", "5"))
        ceiling = os.environ.get("PAGESPEED_ZEROCOPY_GATE_CEILING", "1") == "1"

        outcomes = {"complete": 0, "reset": 0}
        short_trials = []  # (label, delivered_bytes) for non-complete trials
        with _Flood(host, port, server_config.test_root) as flood:
            # Standard trials: each 128 KB chunk in flight > the ~5s read
            # lease, under heavy volume churn.
            for i in range(trials):
                label = "trial %d" % i
                done, delivered = self._run_trial(
                    host, port, ic, reference,
                    delay=0.35, budget=70.0 * scale, label=label,
                )
                outcomes["complete" if done else "reset"] += 1
                if not done:
                    short_trials.append((label, delivered))
            # Ceiling trial: drain slowly enough that the serve holds the
            # stripe past the ~60s lease_wrap_ceiling, where the forced-wrap
            # deadline ignores the lease and the planner must degrade to the
            # copy path (or abort on a torn epoch).
            if ceiling:
                done, delivered = self._run_trial(
                    host, port, ic, reference,
                    delay=1.3, budget=180.0 * scale, label="ceiling trial",
                )
                outcomes["complete" if done else "reset"] += 1
                if not done:
                    short_trials.append(("ceiling trial", delivered))

        # COMPLETION FLOOR: at least one trial must deliver the full,
        # byte-identical body.  Without this, an always-abort (or
        # headers-only) regression passes every trial vacuously --
        # prefix-of-reference is trivially true for the empty body.
        assert outcomes["complete"] >= 1, (
            "no trial delivered the complete byte-identical body "
            "({r} aborted/short of {t}); the aliased serve never finished "
            "once -- an always-abort regression, not a passing gate".format(
                r=outcomes["reset"], t=outcomes["complete"] + outcomes["reset"]
            )
        )
        # Aborted trials must still have made real progress before the
        # reset: a legitimate mid-serve abort has at least the first aliased
        # chunk on the wire, while a headers-only/instant-abort regression
        # delivers (near-)nothing.  10% of the reference is well under one
        # 128 KB chunk of the ~0.5 MB gate resource.
        for label, delivered in short_trials:
            assert delivered >= len(reference) // 10, (
                "{L}: aborted after only {n} of {ref} bytes (<10%); "
                "instant-abort/headers-only serve, not a mid-flight "
                "lease-lapse abort".format(
                    L=label, n=delivered, ref=len(reference)
                )
            )

        stats1 = client.get_statistics(stats_path=server_config.stats_path)

        def d(name):
            return stats1.get(name, 0) - stats0.get(name, 0)

        # Liveness (reported, not asserted -- races are probabilistic): the
        # slow-drain-under-flood should have kept the aliased machinery busy
        # and, on the ceiling trial, engaged the copy-out / reset path.
        print(
            "M1 gate: complete={c} reset={r} flood_inserts={f} | "
            "d(aliased)={a} d(copied_out)={co} d(renew_fail_reset)={rf} "
            "d(aborted)={ab}".format(
                c=outcomes["complete"], r=outcomes["reset"], f=flood.inserts,
                a=d("zerocopy_serve_aliased"),
                co=d("zerocopy_serve_copied_out"),
                rf=d("zerocopy_serve_renew_fail_reset"),
                ab=d("zerocopy_serve_aborted"),
            )
        )
        # The aliased machinery must have stayed engaged across the run.
        assert d("zerocopy_serve_aliased") >= 1, (
            "aliased serve stopped engaging mid-run"
        )
