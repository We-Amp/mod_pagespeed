# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Raw HTTP/1.1 reads that keep chunk boundaries and arrival times.

Equivalent to the bash suite's check_flushing
(pagespeed/automatic/system_test_helpers.sh:786-831), which reads
`curl -N --raw` output chunk by chunk with `read -t` timeouts.
http.client hides chunk boundaries, so flush behaviour needs a socket-level
reader.
"""

import socket
import time
from dataclasses import dataclass
from typing import Dict, List, Optional


@dataclass
class Chunk:
    """One chunk: seconds from sending the request to its size line, and its data."""

    elapsed: float
    data: bytes


@dataclass
class ChunkedResponse:
    status: int
    headers: Dict[str, str]  # lower-case names; repeated headers joined with ", "
    chunks: List[Chunk]
    chunked: bool

    @property
    def body(self) -> bytes:
        return b"".join(chunk.data for chunk in self.chunks)

    def header(self, name: str, default: str = "") -> str:
        return self.headers.get(name.lower(), default)


def fetch_chunks(
    connect_host: str,
    connect_port: int,
    request_target: str,
    host_header: str,
    headers: Optional[Dict[str, str]] = None,
    timeout: float = 30.0,
) -> ChunkedResponse:
    """GET request_target from connect_host:connect_port, keeping chunk timing.

    request_target may be a path or, to go through the secondary port the way
    `curl --proxy $SECONDARY_HOSTNAME` did, an absolute URL
    (http://flush.example.com/...). timeout bounds every single read, like
    check_flushing's `read -t`: a chunk that takes longer raises
    socket.timeout.
    """
    lines = [
        f"GET {request_target} HTTP/1.1",
        f"Host: {host_header}",
        "Connection: close",
        "Accept-Encoding: identity",
    ]
    for name, value in (headers or {}).items():
        lines.append(f"{name}: {value}")
    sock = socket.create_connection((connect_host, connect_port), timeout=timeout)
    try:
        start = time.monotonic()
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        reader = sock.makefile("rb")
        status = int(reader.readline().decode("latin-1").split(" ", 2)[1])
        response_headers: Dict[str, str] = {}
        while True:
            line = reader.readline()
            if line in (b"\r\n", b"\n", b""):
                break
            name, _, value = line.decode("latin-1").partition(":")
            key, value = name.strip().lower(), value.strip()
            if key in response_headers:
                response_headers[key] = f"{response_headers[key]}, {value}"
            else:
                response_headers[key] = value

        if "chunked" in response_headers.get("transfer-encoding", "").lower():
            chunks: List[Chunk] = []
            while True:
                size_line = reader.readline()
                if not size_line:
                    raise ConnectionError(
                        "connection closed before the terminating zero-size chunk"
                    )
                arrived = time.monotonic() - start
                size = int(size_line.split(b";")[0].strip(), 16)
                if size == 0:
                    while reader.readline() not in (b"\r\n", b"\n", b""):
                        pass  # trailers
                    return ChunkedResponse(status, response_headers, chunks, True)
                data = reader.read(size)
                reader.readline()  # CRLF after the chunk data
                chunks.append(Chunk(arrived, data))

        length = response_headers.get("content-length")
        data = reader.read(int(length)) if length is not None else reader.read()
        return ChunkedResponse(
            status, response_headers, [Chunk(time.monotonic() - start, data)], False
        )
    finally:
        sock.close()
