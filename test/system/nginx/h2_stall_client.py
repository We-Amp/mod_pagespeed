#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Minimal HTTP/2 client for the zero-copy stall smoke (TLS, or cleartext
with --plaintext).

Standard library only (the system test framework has no third-party
dependencies).  It opens one stream with a deliberately tiny flow-control
window and then behaves in one of two ways:

  stall  never sends WINDOW_UPDATE, so the server's send side is blocked by
         flow control for good.  The client stays connected and keeps the
         connection alive with PING frames, and reports how long after the
         last DATA frame the server ended the stream (RST_STREAM, GOAWAY or
         connection close).
  slow   grants a small WINDOW_UPDATE on a fixed interval that is shorter
         than the server's send_timeout, so the transfer progresses slowly
         but steadily.  The server must NOT end it.

The last line on stdout is machine readable:

  RESULT mode=<m> outcome=<complete|reset|goaway|eof|alive> \
bytes=<n> since_last_data=<seconds> elapsed=<seconds>

Exit status: 0 when a RESULT line was printed, 2 on a usage or I/O error.
"""

import argparse
import socket
import ssl
import struct
import sys
import time

SCHEME_HTTP = False  # set by --plaintext

PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

DATA, HEADERS, RST_STREAM, SETTINGS, PING, GOAWAY, WINDOW_UPDATE = (
    0x0, 0x1, 0x3, 0x4, 0x6, 0x7, 0x8)
END_STREAM, ACK = 0x1, 0x1
SETTINGS_INITIAL_WINDOW_SIZE = 0x4


def frame(ftype, flags, stream_id, payload=b""):
    return (struct.pack(">I", len(payload))[1:] +
            struct.pack(">BBI", ftype, flags, stream_id) + payload)


def hpack_string(value):
    # One length byte with a 7-bit prefix and no Huffman coding: only valid
    # below 127 bytes.
    if len(value) >= 127:
        raise ValueError("header string too long for this client "
                         "(%d bytes, limit 126)" % len(value))
    return bytes([len(value)]) + value


def literal(name, value):
    # HPACK literal header field without indexing, new name, no Huffman.
    return b"\x00" + hpack_string(name) + hpack_string(value)


def request_block(host, path):
    block = b"\x82"                              # :method GET (static idx 2)
    block += (b"\x86" if SCHEME_HTTP else b"\x87")  # :scheme http/https (idx 6/7)
    block += b"\x04" + hpack_string(path)  # :path, name idx 4
    block += b"\x01" + hpack_string(host)  # :authority, name idx 1
    block += literal(b"accept-encoding", b"identity")
    return block


class Reader:
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def read_frame(self):
        """Returns (type, flags, stream, payload); None on EOF; raises
        socket.timeout when nothing arrives within the socket timeout."""
        while len(self.buf) < 9:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            self.buf += chunk
        length = int.from_bytes(self.buf[:3], "big")
        ftype, flags, stream = struct.unpack(">BBI", self.buf[3:9])
        stream &= 0x7FFFFFFF
        while len(self.buf) < 9 + length:
            chunk = self.sock.recv(65536)
            if not chunk:
                return None
            self.buf += chunk
        payload = self.buf[9:9 + length]
        self.buf = self.buf[9 + length:]
        return ftype, flags, stream, payload


def run(args):
    global SCHEME_HTTP
    raw = socket.create_connection((args.host, args.port), timeout=10)
    if args.plaintext:
        # HTTP/2 over cleartext TCP with prior knowledge (h2c).
        SCHEME_HTTP = True
        sock = raw
    else:
        ctx = ssl.create_default_context()
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.set_alpn_protocols(["h2"])
        sock = ctx.wrap_socket(raw, server_hostname=args.host)
        if sock.selected_alpn_protocol() != "h2":
            print("ERROR: server did not negotiate h2", file=sys.stderr)
            return 2
    sock.settimeout(0.1)
    reader = Reader(sock)

    settings = struct.pack(">HI", SETTINGS_INITIAL_WINDOW_SIZE,
                           args.initial_window)
    sock.sendall(PREFACE + frame(SETTINGS, 0, 0, settings))
    sock.sendall(frame(HEADERS, 0x4 | END_STREAM, 1,
                       request_block(("%s:%d" % (args.host, args.port)).encode(),
                                     args.path.encode())))

    start = time.monotonic()
    last_data = start
    next_ping = start + 0.5
    next_grant = start + args.grant_interval
    received = 0
    outcome = "alive"
    ended_at = None
    deadline = start + args.max_wait

    while True:
        now = time.monotonic()
        if now >= deadline:
            break
        if now >= next_ping:
            sock.sendall(frame(PING, 0, 0, b"stallsmk"))
            next_ping = now + 0.5
        if args.mode == "slow" and now >= next_grant:
            inc = struct.pack(">I", args.grant_bytes)
            sock.sendall(frame(WINDOW_UPDATE, 0, 1, inc) +
                         frame(WINDOW_UPDATE, 0, 0, inc))
            next_grant = now + args.grant_interval
        try:
            fr = reader.read_frame()
        except socket.timeout:
            continue
        except (ConnectionError, ssl.SSLError):
            outcome, ended_at = "eof", time.monotonic()
            break
        if fr is None:
            outcome, ended_at = "eof", time.monotonic()
            break
        ftype, flags, stream, payload = fr
        if ftype == SETTINGS and not flags & ACK:
            sock.sendall(frame(SETTINGS, ACK, 0))
        elif ftype == PING and not flags & ACK:
            sock.sendall(frame(PING, ACK, 0, payload))
        elif ftype == DATA and stream == 1:
            received += len(payload)
            last_data = time.monotonic()
            if flags & END_STREAM:
                outcome, ended_at = "complete", last_data
                break
        elif ftype == RST_STREAM and stream == 1:
            outcome, ended_at = "reset", time.monotonic()
            break
        elif ftype == GOAWAY:
            outcome, ended_at = "goaway", time.monotonic()
            break

    end = ended_at if ended_at is not None else time.monotonic()
    print("RESULT mode=%s outcome=%s bytes=%d since_last_data=%.2f "
          "elapsed=%.2f" % (args.mode, outcome, received,
                            end - last_data, end - start))
    try:
        sock.close()
    except OSError:
        pass
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--host", default="localhost")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--path", required=True)
    p.add_argument("--mode", choices=("stall", "slow"), required=True)
    p.add_argument("--initial-window", type=int, default=1024)
    p.add_argument("--grant-bytes", type=int, default=32768)
    p.add_argument("--grant-interval", type=float, default=0.5)
    p.add_argument("--plaintext", action="store_true",
                   help="speak h2c (no TLS, prior knowledge)")
    p.add_argument("--max-wait", type=float, required=True,
                   help="give up (outcome=alive) after this many seconds")
    args = p.parse_args()
    try:
        return run(args)
    except (OSError, ssl.SSLError, ValueError) as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
