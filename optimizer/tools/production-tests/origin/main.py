# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Dynamic origin server for production readiness testing.

FastAPI server with programmable routes for every test scenario:
status codes, delays, chunked, disconnect, encoding, conditional
requests, range, cache-control directives, content types, headers.
"""

import asyncio
import gzip
import hashlib
import io
import json
import os
import struct

import brotli
from fastapi import FastAPI, Request, Response
from fastapi.responses import StreamingResponse

app = FastAPI()

# Global request counters
counters: dict[str, int] = {}

TESTDATA_DIR = os.path.join(os.path.dirname(__file__), "testdata")


# ---------------------------------------------------------------------------
# Health & utility
# ---------------------------------------------------------------------------


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/counter/{name}")
async def counter(name: str):
    """Increment and return request count for a named counter."""
    counters[name] = counters.get(name, 0) + 1
    return {"count": counters[name]}


@app.post("/reset-counters")
async def reset_counters():
    """Reset all request counters."""
    counters.clear()
    return {"status": "reset"}


@app.get("/echo")
async def echo(request: Request):
    """Echo request method, headers, and body."""
    body = await request.body()
    return {
        "method": request.method,
        "path": str(request.url.path),
        "query": str(request.url.query),
        "headers": dict(request.headers),
        "body_length": len(body),
        "body": body.decode("utf-8", errors="replace"),
    }


# ---------------------------------------------------------------------------
# Status code routes
# ---------------------------------------------------------------------------


@app.api_route("/status/{code}", methods=["GET", "HEAD", "POST", "OPTIONS"])
async def status_code(
    code: int,
    request: Request,
    body: str = "",
    cache: int = 0,
    headers: str = "",
    type: str = "text/css",
):
    """Return specified HTTP status code with optional body and headers."""
    resp_headers = {}
    if cache > 0:
        resp_headers["Cache-Control"] = f"public, max-age={cache}"
    if headers:
        for h in json.loads(headers):
            resp_headers[h["name"]] = h["value"]

    content = body.encode() if body else f"Status {code}".encode()
    if request.method == "HEAD":
        content = b""

    return Response(
        content=content,
        status_code=code,
        headers=resp_headers,
        media_type=type,
    )


# ---------------------------------------------------------------------------
# Cache-Control directive routes
# ---------------------------------------------------------------------------


@app.get("/cc/no-store")
async def cc_no_store():
    return Response(
        content=b"no-store content",
        headers={"Cache-Control": "no-store"},
        media_type="text/css",
    )


@app.get("/cc/no-cache")
async def cc_no_cache():
    content = b"no-cache content"
    etag = _weak_etag(content)
    return Response(
        content=content,
        headers={
            "Cache-Control": "no-cache",
            "ETag": etag,
        },
        media_type="text/css",
    )


@app.get("/cc/private")
async def cc_private():
    return Response(
        content=b"private content",
        headers={"Cache-Control": "private"},
        media_type="text/css",
    )


@app.get("/cc/must-revalidate")
async def cc_must_revalidate(max_age: int = 0):
    content = b"must-revalidate content"
    etag = _weak_etag(content)
    cc = f"must-revalidate, max-age={max_age}" if max_age else "must-revalidate"
    return Response(
        content=content,
        headers={"Cache-Control": cc, "ETag": etag},
        media_type="text/css",
    )


@app.get("/cc/s-maxage")
async def cc_s_maxage(s: int = 3600, m: int = 60):
    return Response(
        content=b"s-maxage content",
        headers={"Cache-Control": f"public, s-maxage={s}, max-age={m}"},
        media_type="text/css",
    )


@app.get("/cc/no-transform")
async def cc_no_transform():
    return Response(
        content=b"no-transform content",
        headers={"Cache-Control": "public, no-transform, max-age=3600"},
        media_type="text/css",
    )


@app.get("/cc/immutable")
async def cc_immutable(max_age: int = 31536000):
    return Response(
        content=b"immutable content",
        headers={"Cache-Control": f"public, immutable, max-age={max_age}"},
        media_type="text/css",
    )


@app.get("/cc/public")
async def cc_public(max_age: int = 3600):
    return Response(
        content=b"public content",
        headers={"Cache-Control": f"public, max-age={max_age}"},
        media_type="text/css",
    )


@app.get("/cc/max-age-zero")
async def cc_max_age_zero():
    content = b"max-age-zero content"
    etag = _weak_etag(content)
    return Response(
        content=content,
        headers={
            "Cache-Control": "max-age=0, must-revalidate",
            "ETag": etag,
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# Vary routes
# ---------------------------------------------------------------------------


@app.get("/vary/{header}")
async def vary_header(header: str):
    content = b"varied content"
    return Response(
        content=content,
        headers={
            "Vary": header,
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


@app.get("/vary-multi")
async def vary_multi(headers: str = "Accept,Accept-Encoding"):
    content = b"multi-varied content"
    return Response(
        content=content,
        headers={
            "Vary": headers,
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# ETag & conditional request routes
# ---------------------------------------------------------------------------


@app.get("/etag/{etag_type}/{resource_id}")
async def etag_route(etag_type: str, resource_id: str, request: Request):
    """Return resource with specified ETag type (weak/strong)."""
    content = f"Resource {resource_id} content".encode()
    if etag_type == "weak":
        etag = _weak_etag(content)
    else:
        etag = _strong_etag(content)

    # Conditional check
    if _check_if_none_match(request, etag):
        return Response(status_code=304, headers={"ETag": etag})

    return Response(
        content=content,
        headers={
            "ETag": etag,
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


@app.get("/conditional/{resource_id}")
async def conditional(resource_id: str, request: Request):
    """Support If-None-Match and If-Modified-Since."""
    content = f"Resource {resource_id} content".encode()
    etag = _weak_etag(content)
    last_modified = "Wed, 01 Jan 2025 00:00:00 GMT"

    # If-None-Match takes precedence (RFC 9110 Section 13.1.2)
    if _check_if_none_match(request, etag):
        return Response(
            status_code=304,
            headers={
                "ETag": etag,
                "Cache-Control": "public, max-age=3600",
                "Last-Modified": last_modified,
            },
        )

    # If-Modified-Since
    ims = request.headers.get("if-modified-since")
    if ims:
        return Response(
            status_code=304,
            headers={
                "Last-Modified": last_modified,
                "Cache-Control": "public, max-age=3600",
            },
        )

    return Response(
        content=content,
        headers={
            "ETag": etag,
            "Cache-Control": "public, max-age=3600",
            "Last-Modified": last_modified,
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# Range request routes
# ---------------------------------------------------------------------------


@app.get("/range/{resource_id}")
async def range_resource(resource_id: str, request: Request):
    """Support Range and If-Range requests."""
    # Generate deterministic CSS-like content for cacheability
    content = (f"/* Range content for {resource_id} */ ").encode() * 1000
    etag = _strong_etag(content)
    total = len(content)

    # If-None-Match takes precedence over Range
    if _check_if_none_match(request, etag):
        return Response(status_code=304, headers={"ETag": etag})

    range_header = request.headers.get("range")
    if not range_header:
        return Response(
            content=content,
            headers={
                "ETag": etag,
                "Accept-Ranges": "bytes",
                "Cache-Control": "public, max-age=3600",
            },
            media_type="text/css",
        )

    # If-Range check: only honor Range if If-Range matches
    if_range = request.headers.get("if-range")
    if if_range and if_range != etag:
        # If-Range doesn't match — send full response
        return Response(
            content=content,
            headers={
                "ETag": etag,
                "Accept-Ranges": "bytes",
                "Cache-Control": "public, max-age=3600",
            },
            media_type="text/css",
        )

    if not range_header.startswith("bytes="):
        return Response(content=content, status_code=200, media_type="text/css")

    range_spec = range_header[6:]

    # Multi-range
    if "," in range_spec:
        return _multipart_range(content, range_spec, etag)

    # Single range
    start, end = _parse_range(range_spec, total)
    if start is None:
        return Response(
            status_code=416,
            headers={"Content-Range": f"bytes */{total}"},
        )

    partial = content[start : end + 1]
    return Response(
        content=partial,
        status_code=206,
        headers={
            "Content-Range": f"bytes {start}-{end}/{total}",
            "Accept-Ranges": "bytes",
            "ETag": etag,
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# Chunked transfer routes
# ---------------------------------------------------------------------------


@app.get("/chunked/small")
async def chunked_small(size: int = 1024):
    """1-byte chunks."""

    async def generate():
        for i in range(size):
            yield bytes([ord("a") + (i % 26)])

    return StreamingResponse(generate(), media_type="text/css")


@app.get("/chunked/large")
async def chunked_large(chunks: int = 5):
    """1MB chunks."""

    async def generate():
        chunk = b"x" * (1024 * 1024)
        for _ in range(chunks):
            yield chunk

    return StreamingResponse(generate(), media_type="text/css")


@app.get("/chunked/slow")
async def chunked_slow(delay: float = 2.0, chunks: int = 10):
    """Chunks with configurable delay between them."""

    async def generate():
        for i in range(chunks):
            yield f"chunk-{i}\n".encode()
            if i < chunks - 1:
                await asyncio.sleep(delay)

    return StreamingResponse(generate(), media_type="text/css")


@app.get("/chunked/empty")
async def chunked_empty():
    """Zero-length chunked body."""

    async def generate():
        return
        yield  # Make it a generator

    return StreamingResponse(generate(), media_type="text/css")


@app.get("/chunked/rapid")
async def chunked_rapid(chunks: int = 100):
    """Many chunks sent instantly."""

    async def generate():
        for i in range(chunks):
            yield f"rapid-chunk-{i}\n".encode()

    return StreamingResponse(generate(), media_type="text/css")


@app.get("/chunked/trailers")
async def chunked_trailers():
    """Chunked with trailer headers (simulated)."""
    content = b"chunked content with trailers"
    return Response(
        content=content,
        headers={
            "Trailer": "X-Checksum",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# Sized response routes
# ---------------------------------------------------------------------------


@app.get("/size/{spec}")
async def sized_response(
    spec: str,
    type: str = "text/css",
    cache: int = 3600,
):
    """Return a response body of exact specified size."""
    size = _parse_size(spec)
    # Deterministic content based on size
    content = _generate_content(size)
    headers = {"Cache-Control": f"public, max-age={cache}"}
    return Response(content=content, headers=headers, media_type=type)


# ---------------------------------------------------------------------------
# Delay routes
# ---------------------------------------------------------------------------


@app.get("/delay/{seconds}")
async def delay_response(seconds: float, status: int = 200):
    """Delayed response."""
    await asyncio.sleep(seconds)
    return Response(
        content=f"Delayed {seconds}s".encode(),
        status_code=status,
        media_type="text/plain",
    )


# ---------------------------------------------------------------------------
# Disconnect routes
# ---------------------------------------------------------------------------


@app.get("/disconnect/mid")
async def disconnect_mid(request: Request, after: int = 1000):
    """Send `after` bytes then close connection."""

    async def generate():
        yield b"x" * after
        raise Exception("Intentional disconnect")

    return StreamingResponse(
        generate(),
        media_type="application/octet-stream",
        headers={"Content-Length": str(after * 10)},
    )


@app.get("/disconnect/headers-only")
async def disconnect_headers_only():
    """Send headers then close before body."""

    async def generate():
        raise Exception("Intentional disconnect after headers")
        yield

    return StreamingResponse(
        generate(),
        media_type="text/plain",
        headers={"Content-Length": "1000"},
    )


# ---------------------------------------------------------------------------
# Encoding routes
# ---------------------------------------------------------------------------


@app.get("/encoding/gzip")
async def encoding_gzip():
    """Pre-compressed gzip response."""
    content = b"This is gzip-compressed content. " * 100
    compressed = gzip.compress(content)
    return Response(
        content=compressed,
        headers={
            "Content-Encoding": "gzip",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


@app.get("/encoding/brotli")
async def encoding_brotli():
    """Pre-compressed brotli response."""
    content = b"This is brotli-compressed content. " * 100
    compressed = brotli.compress(content)
    return Response(
        content=compressed,
        headers={
            "Content-Encoding": "br",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


@app.get("/encoding/identity")
async def encoding_identity():
    """Explicit identity encoding."""
    return Response(
        content=b"identity encoded content",
        headers={
            "Content-Encoding": "identity",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


# ---------------------------------------------------------------------------
# Redirect routes
# ---------------------------------------------------------------------------


@app.api_route("/redirect/{code}", methods=["GET", "HEAD"])
async def redirect(code: int, to: str = "/status/200?body=redirected", cache: int = 0):
    """Redirect with specified status code."""
    headers = {"Location": to}
    if cache > 0:
        headers["Cache-Control"] = f"public, max-age={cache}"
    return Response(content=b"", status_code=code, headers=headers)


# ---------------------------------------------------------------------------
# Header routes
# ---------------------------------------------------------------------------


@app.get("/headers/set-cookie")
async def headers_set_cookie():
    return Response(
        content=b"has cookie",
        headers={
            "Set-Cookie": "session=abc123; Path=/; HttpOnly",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/html",
    )


@app.get("/headers/cors")
async def headers_cors():
    return Response(
        content=b"cors content",
        headers={
            "Access-Control-Allow-Origin": "https://example.com",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/css",
    )


@app.options("/headers/cors")
async def headers_cors_preflight():
    return Response(
        content=b"",
        status_code=204,
        headers={
            "Access-Control-Allow-Origin": "https://example.com",
            "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
            "Access-Control-Allow-Headers": "Content-Type",
            "Access-Control-Max-Age": "86400",
        },
    )


@app.get("/headers/security")
async def headers_security():
    return Response(
        content=b"secure content",
        headers={
            "Content-Security-Policy": "default-src 'self'",
            "Strict-Transport-Security": "max-age=31536000; includeSubDomains",
            "X-Frame-Options": "DENY",
            "X-Content-Type-Options": "nosniff",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/html",
    )


@app.get("/headers/custom")
async def headers_custom(h: str = "[]"):
    """Set custom response headers from JSON."""
    custom_headers = json.loads(h)
    resp_headers = {"Cache-Control": "public, max-age=3600"}
    for header in custom_headers:
        resp_headers[header["name"]] = header["value"]
    return Response(
        content=b"custom headers content",
        headers=resp_headers,
        media_type="text/css",
    )


@app.get("/headers/link-preload")
async def headers_link_preload():
    return Response(
        content=b"link preload content",
        headers={
            "Link": "</style.css>; rel=preload; as=style",
            "Cache-Control": "public, max-age=3600",
        },
        media_type="text/html",
    )


@app.get("/headers/content-disposition/{kind}")
async def headers_content_disposition(kind: str):
    headers = {"Cache-Control": "public, max-age=3600"}
    if kind == "attachment":
        headers["Content-Disposition"] = 'attachment; filename="test.txt"'
    elif kind == "inline":
        headers["Content-Disposition"] = "inline"
    return Response(
        content=b"disposition content",
        headers=headers,
        media_type="application/octet-stream",
    )


@app.get("/headers/allow")
async def headers_allow():
    return Response(
        content=b"",
        status_code=405,
        headers={"Allow": "GET, HEAD, OPTIONS"},
        media_type="text/plain",
    )


@app.get("/headers/retry-after")
async def headers_retry_after():
    return Response(
        content=b"",
        status_code=429,
        headers={"Retry-After": "120"},
        media_type="text/plain",
    )


# ---------------------------------------------------------------------------
# Content type routes
# ---------------------------------------------------------------------------


@app.get("/content/html")
async def content_html():
    html = (
        "<!DOCTYPE html><html><head>"
        '<link rel="stylesheet" href="/content/css">'
        "</head><body>"
        "<h1>Test Page</h1>"
        '<img src="/content/image" alt="test">'
        '<script src="/content/js"></script>'
        "</body></html>"
    )
    return Response(
        content=html.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/html; charset=utf-8",
    )


@app.get("/content/html-charset")
async def content_html_charset(c: str = "utf-8"):
    html = f'<!DOCTYPE html><html><head><meta charset="{c}"></head><body>charset test</body></html>'
    return Response(
        content=html.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type=f"text/html; charset={c}",
    )


@app.get("/content/html-bom")
async def content_html_bom():
    bom = b"\xef\xbb\xbf"
    html = b"<!DOCTYPE html><html><body>BOM test</body></html>"
    return Response(
        content=bom + html,
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/html; charset=utf-8",
    )


@app.get("/content/css")
async def content_css():
    css = "body { color: red; background: blue; font-size: 16px; }\n" * 50
    return Response(
        content=css.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/css",
    )


@app.get("/content/css-import-chain")
async def content_css_import_chain(depth: int = 3, level: int = 0):
    if level < depth:
        css = f'@import url("/content/css-import-chain?depth={depth}&level={level + 1}");\n'
        css += f"/* Level {level} styles */\n.level-{level} {{ color: red; }}\n"
    else:
        css = f"/* Leaf level {level} */\n.leaf {{ color: green; }}\n"
    return Response(
        content=css.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/css",
    )


@app.get("/content/css-xss")
async def content_css_xss():
    """CSS with embedded XSS attempt."""
    css = 'body { color: red; }\n</style><script>alert("xss")</script><style>\n'
    return Response(
        content=css.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/css",
    )


@app.get("/content/css-xss-url")
async def content_css_xss_url():
    """CSS with javascript: URL protocol."""
    css = 'body { background: url("javascript:alert(1)"); }\n'
    return Response(
        content=css.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="text/css",
    )


@app.get("/content/js")
async def content_js():
    js = 'console.log("test");\n' * 20
    return Response(
        content=js.encode(),
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="application/javascript",
    )


@app.get("/content/binary-nulls")
async def content_binary_nulls():
    """Binary content with embedded null bytes."""
    content = b"\x00\x01\x02\x00\x03\x04\x00" * 100
    return Response(
        content=content,
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="application/octet-stream",
    )


@app.get("/content/image")
async def content_image():
    """Serve a small JPEG test image (1x1 pixel)."""
    # Minimal valid JPEG (1x1 red pixel)
    jpeg = _minimal_jpeg()
    return Response(
        content=jpeg,
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="image/jpeg",
    )


@app.get("/content/image/large")
async def content_image_large():
    """Serve a larger JPEG test image for optimization testing."""
    # 100x100 JPEG
    jpeg = _generate_jpeg(100, 100)
    return Response(
        content=jpeg,
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="image/jpeg",
    )


@app.get("/content/animated-gif")
async def content_animated_gif():
    """Serve a minimal animated GIF (2 frames)."""
    gif = _minimal_animated_gif()
    return Response(
        content=gif,
        headers={"Cache-Control": "public, max-age=3600"},
        media_type="image/gif",
    )


@app.get("/content/sse")
async def content_sse():
    """Server-Sent Events stream."""

    async def generate():
        for i in range(5):
            yield f"data: event {i}\n\n".encode()
            await asyncio.sleep(0.1)

    return StreamingResponse(
        generate(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache"},
    )


@app.get("/content/{path:path}")
async def content_catchall(path: str):
    """Catch-all content route for unique cache keys.

    For CSS paths (css-*): returns substantial CSS (~500+ bytes) with the path
    embedded as a custom property (survives CSS minification, unlike comments).
    For other paths: returns text/html with the path in the body.
    """
    if path.startswith("css-") or path.startswith("gzfidelity"):
        # CSS content with path in a custom property (survives minification).
        css_lines = [
            f':root {{ --path: "{path}"; }}',
            "body { margin: 0; padding: 0; font-family: sans-serif; color: #333; }",
            "h1 { font-size: 2rem; line-height: 1.4; margin-bottom: 1rem; }",
            "h2 { font-size: 1.5rem; line-height: 1.3; margin-bottom: 0.75rem; }",
            "p { font-size: 1rem; line-height: 1.6; margin-bottom: 1rem; }",
            "a { color: #0066cc; text-decoration: none; transition: color 0.2s; }",
            "a:hover { color: #004499; text-decoration: underline; }",
            ".container { max-width: 1200px; margin: 0 auto; padding: 0 1rem; }",
            ".header { background: #f5f5f5; border-bottom: 1px solid #ddd; padding: 1rem 0; }",
            ".footer { background: #333; color: #fff; padding: 2rem 0; margin-top: 2rem; }",
        ]
        content = "\n".join(css_lines).encode()
        return Response(
            content=content,
            headers={"Cache-Control": "public, max-age=3600"},
            media_type="text/css",
        )
    else:
        # HTML content with path embedded in body for response mixing tests.
        body = f"<html><body><p>Content for {path}</p></body></html>"
        return Response(
            content=body.encode(),
            headers={"Cache-Control": "public, max-age=3600"},
            media_type="text/html",
        )


# ---------------------------------------------------------------------------
# Runtime configuration (for advanced test scenarios)
# ---------------------------------------------------------------------------


@app.post("/configure")
async def configure(request: Request):
    """Runtime route configuration (future use)."""
    config = await request.json()
    return {"status": "configured", "config": config}


# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------


def _weak_etag(content: bytes) -> str:
    h = hashlib.md5(content).hexdigest()[:8]
    return f'W/"{h}"'


def _strong_etag(content: bytes) -> str:
    h = hashlib.md5(content).hexdigest()[:16]
    return f'"{h}"'


def _check_if_none_match(request: Request, etag: str) -> bool:
    """Check If-None-Match header. Returns True if 304 should be sent."""
    inm = request.headers.get("if-none-match", "")
    if not inm:
        return False
    client_etags = [e.strip() for e in inm.split(",")]

    # Weak comparison (RFC 9110 Section 8.8.3.2)
    def normalize(e):
        return e[2:] if e.startswith("W/") else e

    return (
        normalize(etag) in [normalize(e) for e in client_etags] or "*" in client_etags
    )


def _parse_range(spec: str, total: int) -> tuple:
    """Parse a single byte range spec. Returns (start, end) or (None, None)."""
    parts = spec.strip().split("-")
    if len(parts) != 2:
        return None, None

    try:
        if parts[0] == "":
            # Suffix range: -N
            suffix_len = int(parts[1])
            start = max(0, total - suffix_len)
            end = total - 1
        elif parts[1] == "":
            # Open-ended: N-
            start = int(parts[0])
            end = total - 1
        else:
            start = int(parts[0])
            end = int(parts[1])
    except ValueError:
        return None, None

    if start > end or start >= total:
        return None, None

    end = min(end, total - 1)
    return start, end


def _multipart_range(content: bytes, range_spec: str, etag: str) -> Response:
    """Handle multipart byte range response."""
    total = len(content)
    boundary = "BOUNDARY_PRODUCTION_TEST"
    parts = []

    for spec in range_spec.split(","):
        start, end = _parse_range(spec, total)
        if start is None:
            return Response(
                status_code=416,
                headers={"Content-Range": f"bytes */{total}"},
            )
        part_data = content[start : end + 1]
        parts.append(
            f"--{boundary}\r\n"
            f"Content-Type: text/css\r\n"
            f"Content-Range: bytes {start}-{end}/{total}\r\n"
            f"\r\n"
        )
        parts.append(part_data)
        parts.append(b"\r\n")

    parts.append(f"--{boundary}--\r\n")

    body = b""
    for p in parts:
        body += p.encode() if isinstance(p, str) else p

    return Response(
        content=body,
        status_code=206,
        media_type=f"multipart/byteranges; boundary={boundary}",
        headers={"ETag": etag},
    )


def _parse_size(spec: str) -> int:
    """Parse size spec: '1024', '10mb', '10mb-plus-1', etc."""
    spec = spec.lower()
    if spec == "10mb":
        return 10 * 1024 * 1024
    if spec == "10mb-plus-1":
        return 10 * 1024 * 1024 + 1
    if spec.endswith("mb"):
        return int(spec[:-2]) * 1024 * 1024
    if spec.endswith("kb"):
        return int(spec[:-2]) * 1024
    return int(spec)


def _generate_content(size: int) -> bytes:
    """Generate deterministic content of exact size."""
    if size == 0:
        return b""
    # Repeating pattern for deterministic verification
    pattern = b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    repeats = (size // len(pattern)) + 1
    return (pattern * repeats)[:size]


def _minimal_jpeg() -> bytes:
    """Generate a minimal valid JPEG (1x1 red pixel)."""
    # Pre-computed minimal JPEG
    return bytes(
        [
            0xFF,
            0xD8,
            0xFF,
            0xE0,
            0x00,
            0x10,
            0x4A,
            0x46,
            0x49,
            0x46,
            0x00,
            0x01,
            0x01,
            0x00,
            0x00,
            0x01,
            0x00,
            0x01,
            0x00,
            0x00,
            0xFF,
            0xDB,
            0x00,
            0x43,
            0x00,
            0x08,
            0x06,
            0x06,
            0x07,
            0x06,
            0x05,
            0x08,
            0x07,
            0x07,
            0x07,
            0x09,
            0x09,
            0x08,
            0x0A,
            0x0C,
            0x14,
            0x0D,
            0x0C,
            0x0B,
            0x0B,
            0x0C,
            0x19,
            0x12,
            0x13,
            0x0F,
            0x14,
            0x1D,
            0x1A,
            0x1F,
            0x1E,
            0x1D,
            0x1A,
            0x1C,
            0x1C,
            0x20,
            0x24,
            0x2E,
            0x27,
            0x20,
            0x22,
            0x2C,
            0x23,
            0x1C,
            0x1C,
            0x28,
            0x37,
            0x29,
            0x2C,
            0x30,
            0x31,
            0x34,
            0x34,
            0x34,
            0x1F,
            0x27,
            0x39,
            0x3D,
            0x38,
            0x32,
            0x3C,
            0x2E,
            0x33,
            0x34,
            0x32,
            0xFF,
            0xC0,
            0x00,
            0x0B,
            0x08,
            0x00,
            0x01,
            0x00,
            0x01,
            0x01,
            0x01,
            0x11,
            0x00,
            0xFF,
            0xC4,
            0x00,
            0x1F,
            0x00,
            0x00,
            0x01,
            0x05,
            0x01,
            0x01,
            0x01,
            0x01,
            0x01,
            0x01,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x06,
            0x07,
            0x08,
            0x09,
            0x0A,
            0x0B,
            0xFF,
            0xC4,
            0x00,
            0xB5,
            0x10,
            0x00,
            0x02,
            0x01,
            0x03,
            0x03,
            0x02,
            0x04,
            0x03,
            0x05,
            0x05,
            0x04,
            0x04,
            0x00,
            0x00,
            0x01,
            0x7D,
            0x01,
            0x02,
            0x03,
            0x00,
            0x04,
            0x11,
            0x05,
            0x12,
            0x21,
            0x31,
            0x41,
            0x06,
            0x13,
            0x51,
            0x61,
            0x07,
            0x22,
            0x71,
            0x14,
            0x32,
            0x81,
            0x91,
            0xA1,
            0x08,
            0x23,
            0x42,
            0xB1,
            0xC1,
            0x15,
            0x52,
            0xD1,
            0xF0,
            0x24,
            0x33,
            0x62,
            0x72,
            0x82,
            0x09,
            0x0A,
            0x16,
            0x17,
            0x18,
            0x19,
            0x1A,
            0x25,
            0x26,
            0x27,
            0x28,
            0x29,
            0x2A,
            0x34,
            0x35,
            0x36,
            0x37,
            0x38,
            0x39,
            0x3A,
            0x43,
            0x44,
            0x45,
            0x46,
            0x47,
            0x48,
            0x49,
            0x4A,
            0x53,
            0x54,
            0x55,
            0x56,
            0x57,
            0x58,
            0x59,
            0x5A,
            0x63,
            0x64,
            0x65,
            0x66,
            0x67,
            0x68,
            0x69,
            0x6A,
            0x73,
            0x74,
            0x75,
            0x76,
            0x77,
            0x78,
            0x79,
            0x7A,
            0x83,
            0x84,
            0x85,
            0x86,
            0x87,
            0x88,
            0x89,
            0x8A,
            0x92,
            0x93,
            0x94,
            0x95,
            0x96,
            0x97,
            0x98,
            0x99,
            0x9A,
            0xA2,
            0xA3,
            0xA4,
            0xA5,
            0xA6,
            0xA7,
            0xA8,
            0xA9,
            0xAA,
            0xB2,
            0xB3,
            0xB4,
            0xB5,
            0xB6,
            0xB7,
            0xB8,
            0xB9,
            0xBA,
            0xC2,
            0xC3,
            0xC4,
            0xC5,
            0xC6,
            0xC7,
            0xC8,
            0xC9,
            0xCA,
            0xD2,
            0xD3,
            0xD4,
            0xD5,
            0xD6,
            0xD7,
            0xD8,
            0xD9,
            0xDA,
            0xE1,
            0xE2,
            0xE3,
            0xE4,
            0xE5,
            0xE6,
            0xE7,
            0xE8,
            0xE9,
            0xEA,
            0xF1,
            0xF2,
            0xF3,
            0xF4,
            0xF5,
            0xF6,
            0xF7,
            0xF8,
            0xF9,
            0xFA,
            0xFF,
            0xDA,
            0x00,
            0x08,
            0x01,
            0x01,
            0x00,
            0x00,
            0x3F,
            0x00,
            0x7B,
            0x94,
            0x11,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0xFF,
            0xD9,
        ]
    )


def _generate_jpeg(width: int, height: int) -> bytes:
    """Return a test JPEG suitable for image optimization testing.

    Uses a pre-built photo if available, otherwise generates a minimal
    valid JPEG in memory (avoids external file dependency).
    """
    photo_path = os.path.join(TESTDATA_DIR, "photo.jpg")
    if os.path.isfile(photo_path):
        with open(photo_path, "rb") as f:
            return f.read()
    # Generate a minimal valid JFIF JPEG (single-color grayscale).
    # This is enough for PageSpeed to detect as image and attempt optimization.
    return bytes(
        [0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10]  # SOI + APP0 marker (len=16)
        + list(b"JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")
        + [0xFF, 0xDB, 0x00, 0x43, 0x00]
        + [8] * 64  # DQT
        + [
            0xFF,
            0xC0,
            0x00,
            0x0B,
            0x08,  # SOF0 baseline
            (height >> 8) & 0xFF,
            height & 0xFF,
            (width >> 8) & 0xFF,
            width & 0xFF,
            0x01,
            0x01,
            0x11,
            0x00,
        ]  # 1 component, 1:1 sampling
        + [
            0xFF,
            0xC4,
            0x00,
            0x1F,
            0x00,  # DHT (minimal DC table)
            0x00,
            0x01,
            0x05,
            0x01,
            0x01,
            0x01,
            0x01,
            0x01,
            0x01,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x06,
            0x07,
            0x08,
        ]
        + [
            0xFF,
            0xDA,
            0x00,
            0x08,
            0x01,
            0x01,  # SOS
            0x00,
            0x00,
            0x3F,
            0x00,
            0x7F,
            0xFF,
            0xD9,
        ]  # scan data + EOI
    )


def _minimal_animated_gif() -> bytes:
    """Generate a minimal 2-frame animated GIF."""
    buf = io.BytesIO()
    # GIF89a header
    buf.write(b"GIF89a")
    # Logical screen descriptor: 2x2, no GCT
    buf.write(struct.pack("<HH", 2, 2))
    buf.write(bytes([0x00, 0x00, 0x00]))  # No GCT, bg=0, aspect=0
    # Netscape extension for looping
    buf.write(b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00")
    # Frame 1
    buf.write(b"\x21\xf9\x04\x04\x0a\x00\x00\x00")  # GCE: delay=10
    buf.write(b"\x2c")  # Image descriptor
    buf.write(struct.pack("<HHHH", 0, 0, 2, 2))
    buf.write(bytes([0x81, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF]))  # LCT
    buf.write(bytes([0x02, 0x02, 0x44, 0x01, 0x00]))  # LZW min=2 + data
    # Frame 2
    buf.write(b"\x21\xf9\x04\x04\x0a\x00\x00\x00")  # GCE: delay=10
    buf.write(b"\x2c")
    buf.write(struct.pack("<HHHH", 0, 0, 2, 2))
    buf.write(bytes([0x81, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF]))  # LCT
    buf.write(bytes([0x02, 0x02, 0x44, 0x01, 0x00]))  # LZW min=2 + data
    # Trailer
    buf.write(b"\x3b")
    return buf.getvalue()
