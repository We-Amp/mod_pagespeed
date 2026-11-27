#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
#
# A loopback-only NuGet v3 push endpoint for the publish rehearsal
# (tools/ci/nupkg_checks.sh local-feed). It answers like nuget.org does for
# the cases a release meets: 201 for a new id/version, 409 for a duplicate,
# 400 for something that is not a package. Accepted packages are written to
# <dir> as <id>.<version>.nupkg, so the folder doubles as a restore source.
# It never forwards anything, binds 127.0.0.1 only and needs no key.
#
# Usage: nuget_mock_feed.py <dir> <port-file>     (port 0: pick a free one)
import http.server, io, os, re, sys, zipfile

FEED = sys.argv[1]
PORT_FILE = sys.argv[2]
os.makedirs(FEED, exist_ok=True)


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _send(self, code, body=b"", ctype="text/plain"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.rstrip("/").endswith("index.json"):
            url = "http://127.0.0.1:%d/api/v2/package" % self.server.server_port
            body = ('{"version":"3.0.0","resources":[{"@id":"%s","@type":"PackagePublish/2.0.0"}]}' % url).encode()
            return self._send(200, body, "application/json")
        self._send(404)

    def do_PUT(self):
        if self.headers.get("Expect", "").lower() == "100-continue":
            self.send_response_only(100)
            self.end_headers()
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            raw = b""
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                raw += self.rfile.read(size)
                self.rfile.readline()
        else:
            raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        start = raw.find(b"PK\x03\x04")
        end = raw.rfind(b"\r\n--")
        try:
            data = raw[start:end] if start >= 0 and end > start else raw
            z = zipfile.ZipFile(io.BytesIO(data))
            if z.testzip():
                raise ValueError("corrupt zip")
            spec = [x for x in z.namelist() if x.endswith(".nuspec") and "/" not in x]
            text = z.read(spec[0]).decode("utf-8-sig")
            pid = re.search(r"<id>([^<]+)</id>", text).group(1)
            ver = re.search(r"<version>([^<]+)</version>", text).group(1)
        except Exception as e:  # not a package
            return self._send(400, ("invalid package: %s" % e).encode())
        # Only plain ids and versions: an id like "x/../../escaped" must not
        # name a path outside the feed directory.
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", pid) or ".." in pid or not re.fullmatch(r"[0-9A-Za-z.+-]{1,64}", ver):
            return self._send(400, b"invalid package id or version")
        dest = os.path.join(FEED, "%s.%s.nupkg" % (pid, ver))
        if os.path.dirname(os.path.realpath(dest)) != os.path.realpath(FEED):
            return self._send(400, b"invalid package path")
        if os.path.exists(dest):
            return self._send(409, b"package already exists")
        with open(dest, "wb") as f:
            f.write(data)
        self._send(201)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
with open(PORT_FILE, "w") as f:
    f.write(str(srv.server_port))
srv.serve_forever()
