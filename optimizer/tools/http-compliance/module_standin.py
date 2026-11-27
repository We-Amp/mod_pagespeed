# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""A stand-in for mod_pagespeed's HTML store, over the public C API.

It does what a web-server module does when it stores an HTML page in its own
site keyspace and asks the optimizer for the page's Markdown copy, through
libpagespeed.so and nothing else:

  store    ps_html_store_classify, then the paired headers sidecar
           (ps_cache_write_headers_sidecar_site), then the durable original
           (ps_html_store_write_params + ps_cache_write_original_site),
           stamped with the SHA-256 of the body.
  notify   ps_notify_worker_ex with PS_NOTIFY_FLAG_* bits of the caller's
           choosing, so the tests can send both the accepted shape and the
           shapes the library itself refuses.
  facts    the exact-id reads a module's serve path makes (original, Markdown
           copy, withheld marker, sidecar), ps_html_stored_facts_from_reads,
           ps_html_markdown_etag and ps_html_serve_classify for an agent
           request, plus a selection read that shows whether any HTML variant
           exists for the page.

Run inside a container that has the library and the worker's volume:

    python3 module_standin.py '<json request>'

It prints one JSON object. Standard library only; the ctypes layouts are
checked against the ABI golden by test_daemon_html.py.
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import os
import sys
import time

LIB_PATH = os.environ.get("PS_LIB", "/usr/local/lib/libpagespeed.so")
CACHE_PATH = os.environ.get("PS_CACHE_PATH", "/shared/cache.vol")
SOCKET_PATH = os.environ.get("PS_SOCKET_PATH", "/shared/pagespeed.sock")

PS_OK = 0
ERROR_NAMES = {
    0: "ok",
    1: "not_found",
    2: "io",
    3: "corrupted",
    4: "no_space",
    5: "invalid_arg",
    6: "busy",
    7: "closed",
    8: "too_many_alternates",
    9: "exists",
    10: "not_owned",
    11: "version_mismatch",
    99: "internal",
}

PS_CONTENT_HTML = 0
PS_SENTINEL_ORIGINAL = 0x0C
PS_SENTINEL_MARKDOWN = 0x7C
PS_SENTINEL_MARKDOWN_WITHHELD = 0xCC
PS_FLAG_HEADERS_CLASSIFIED = 0x10
PS_HTML_CACHE_SERVE_VARIANTS = 2

NOTIFY_FLAGS = {"host_vouched": 0x1, "from_module": 0x2, "html_stored": 0x8}

STORE_ACTIONS = {
    0: "refuse",
    1: "store",
    2: "store_no_notify",
    3: "would_store",
    4: "would_store_no_notify",
}
SERVE_ACTIONS = {0: "fall_through", 1: "serve_html", 2: "serve_markdown", 3: "validate"}
FACT_BITS = {
    0x1: "present",
    0x2: "needs_validation",
    0x4: "explicit_shared",
    0x8: "headers_not_reproducible",
    0x10: "sidecar_required",
    0x20: "sidecar_present",
    0x40: "purged",
    0x80: "validated_this_request",
    0x100: "variant_present",
    0x200: "binding_live",
    0x400: "no_transform",
    0x800: "withheld",
}
WITHHELD_REASONS = {
    1: "below_floor",
    2: "too_large",
    3: "encoded",
    4: "hash_mismatch",
    5: "extract_failed",
}

c_size = ctypes.c_size_t
c_str = ctypes.c_char_p
c_u8p = ctypes.POINTER(ctypes.c_uint8)


class CacheConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("volume_path", c_str),
        ("volume_size", ctypes.c_uint64),
        ("enable_checksum", ctypes.c_int),
        ("ram_cache_size", c_size),
        ("max_metadata_size", c_size),
        # ps_cache_open reads every field the library defines; an
        # over-allocated buffer is the portable spelling (pagespeed.h).
        ("_room", ctypes.c_uint8 * 256),
    ]


class WriteParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("alternate_id", ctypes.c_uint8),
        ("content_length", ctypes.c_uint64),
        ("full_mask", ctypes.c_uint32),
        ("content_type", ctypes.c_int),
        ("flags", ctypes.c_uint8),
        ("origin_ct", c_str),
        ("origin_etag", c_str),
        ("cache_inserted_at", ctypes.c_uint32),
        ("origin_max_age", ctypes.c_uint32),
        ("origin_s_maxage", ctypes.c_uint32),
        ("origin_last_modified", ctypes.c_uint32),
        ("origin_cc_flags", ctypes.c_uint16),
        ("_reserved", ctypes.c_uint8 * 6),
        ("origin_html_hash", c_u8p),
    ]


class SidecarWriteParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("url", c_str),
        ("hostname", c_str),
        ("scheme", c_str),
        ("site", c_str),
        ("origin_html_hash", c_u8p),
    ]


class NotifyParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("url", c_str),
        ("hostname", c_str),
        ("scheme", c_str),
        ("content_type", ctypes.c_int),
        ("mask", ctypes.c_uint32),
        ("agent_request", ctypes.c_int),
        ("option_context", c_str),
        ("option_context_length", c_size),
        ("option_signature", c_str),
        ("site", c_str),
        ("notify_flags", ctypes.c_uint32),
    ]


class ServeRequest(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("mode", ctypes.c_int),
        ("method", c_str),
        ("accept", c_str),
        ("cache_control", c_str),
        ("pragma", c_str),
        ("if_none_match", c_str),
        ("request_flags", ctypes.c_uint32),
        ("now_seconds", ctypes.c_uint32),
    ]


class StoredFacts(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("flags", ctypes.c_uint32),
        ("stored_at", ctypes.c_uint32),
        ("age_at_store", ctypes.c_uint32),
        ("lifetime_s", ctypes.c_uint32),
        ("render_ttl_s", ctypes.c_uint32),
        ("withheld_reason", ctypes.c_int),
        ("binding_hash", ctypes.c_uint8 * 32),
        ("markdown_hash", ctypes.c_uint8 * 32),
    ]


class ServeVerdict(ctypes.Structure):
    _fields_ = [
        ("struct_size", c_size),
        ("action", ctypes.c_int),
        ("reason", ctypes.c_int),
        ("enforce", ctypes.c_int),
        ("wants_markdown", ctypes.c_int),
        ("age_s", ctypes.c_uint32),
        ("not_modified", ctypes.c_int),
        ("etag", ctypes.c_char * 32),
    ]


# The published structs this file mirrors, by their C names, for the layout
# check against lib/pagespeed/abi/abi-golden.json.
STRUCTS = {
    "ps_write_params_t": WriteParams,
    "ps_sidecar_write_params_t": SidecarWriteParams,
    "ps_notify_params_t": NotifyParams,
    "ps_html_serve_request_t": ServeRequest,
    "ps_html_stored_facts_t": StoredFacts,
    "ps_html_serve_verdict_t": ServeVerdict,
}
# CacheConfig carries extra room on purpose; only its prefix is published.
PREFIX_STRUCTS = {"ps_cache_config_t": CacheConfig}


def _load():
    lib = ctypes.CDLL(LIB_PATH)
    vp = ctypes.c_void_p
    sigs = {
        "ps_version_minor": (ctypes.c_int, []),
        "ps_last_error_message": (c_str, []),
        "ps_cache_config_init_sized": (None, [vp, c_size]),
        "ps_cache_open": (ctypes.c_int, [vp, ctypes.POINTER(vp)]),
        "ps_cache_close": (None, [vp]),
        "ps_read_shared_config_volume_size": (ctypes.c_uint64, [c_str]),
        "ps_read_shared_config_html_mode": (ctypes.c_int, [c_str]),
        "ps_read_shared_config_agent_entitled": (ctypes.c_int, [c_str]),
        "ps_html_store_classify": (
            ctypes.c_int,
            [
                ctypes.c_int,
                c_str,
                c_str,
                ctypes.c_uint32,
                ctypes.c_int,
                vp,
                vp,
                c_size,
                ctypes.c_uint64,
                ctypes.c_int,
                c_str,
                c_size,
                ctypes.c_uint64,
                ctypes.c_uint32,
                ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_uint32),
                ctypes.POINTER(ctypes.c_uint32),
                ctypes.POINTER(ctypes.c_uint16),
                ctypes.POINTER(ctypes.c_int),
                ctypes.c_char_p,
                c_size,
                ctypes.POINTER(c_size),
            ],
        ),
        "ps_html_store_reason_name": (c_str, [ctypes.c_int]),
        "ps_html_store_write_params": (
            ctypes.c_int,
            [
                ctypes.c_int,
                ctypes.c_uint32,
                ctypes.c_uint32,
                ctypes.c_uint16,
                ctypes.c_int,
                ctypes.c_int64,
                vp,
                c_size,
                ctypes.c_uint64,
                c_str,
                ctypes.POINTER(WriteParams),
            ],
        ),
        "ps_write_params_init_sized": (None, [vp, c_size]),
        "ps_cache_write_headers_sidecar_site": (
            ctypes.c_int,
            [
                vp,
                ctypes.POINTER(SidecarWriteParams),
                vp,
                vp,
                c_size,
                ctypes.POINTER(ctypes.c_int),
                ctypes.POINTER(ctypes.c_int),
            ],
        ),
        "ps_cache_write_original_site": (
            ctypes.c_int,
            [
                vp,
                c_str,
                c_str,
                c_str,
                c_str,
                ctypes.POINTER(WriteParams),
                ctypes.POINTER(vp),
            ],
        ),
        "ps_write_data": (ctypes.c_int, [vp, ctypes.c_char_p, c_size]),
        "ps_write_close": (ctypes.c_int, [vp]),
        "ps_write_abort": (None, [vp]),
        "ps_notify_params_init_sized": (None, [vp, c_size]),
        "ps_notify_worker_ex": (ctypes.c_int, [c_str, ctypes.POINTER(NotifyParams)]),
        "ps_cache_read_alternate_site": (
            ctypes.c_int,
            [vp, c_str, c_str, c_str, c_str, ctypes.c_uint8, ctypes.POINTER(vp)],
        ),
        "ps_cache_read_best_site": (
            ctypes.c_int,
            [vp, c_str, c_str, c_str, c_str, ctypes.c_uint32, ctypes.POINTER(vp)],
        ),
        "ps_read_content": (
            ctypes.c_int,
            [
                vp,
                ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)),
                ctypes.POINTER(c_size),
            ],
        ),
        "ps_read_mask": (ctypes.c_uint32, [vp]),
        "ps_read_flags": (ctypes.c_uint8, [vp]),
        "ps_read_origin_html_hash": (c_u8p, [vp]),
        "ps_read_free": (None, [vp]),
        "ps_headers_sidecar_read": (
            ctypes.c_int,
            [vp, c_str, c_str, c_str, c_str, ctypes.POINTER(vp)],
        ),
        "ps_header_block_pairing": (ctypes.c_int, [vp, c_u8p]),
        "ps_header_block_free": (None, [vp]),
        "ps_html_stored_facts_from_reads": (
            ctypes.c_int,
            [vp, vp, vp, ctypes.c_int, ctypes.POINTER(StoredFacts)],
        ),
        "ps_html_markdown_etag": (
            ctypes.c_int,
            [c_u8p, c_u8p, ctypes.c_char_p, c_size, ctypes.POINTER(c_size)],
        ),
        "ps_html_serve_classify": (
            ctypes.c_int,
            [
                ctypes.POINTER(ServeRequest),
                ctypes.POINTER(StoredFacts),
                ctypes.POINTER(ServeVerdict),
            ],
        ),
        "ps_html_serve_reason_name": (c_str, [ctypes.c_int]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = args
    return lib


def _err(code: int) -> str:
    return ERROR_NAMES.get(code, f"error_{code}")


def _b(s: str | None) -> bytes | None:
    return None if s is None else s.encode()


def _hash_ptr(digest: bytes):
    buf = (ctypes.c_uint8 * 32).from_buffer_copy(digest)
    return buf, ctypes.cast(buf, c_u8p)


def _header_arrays(headers):
    names = (ctypes.c_char_p * max(len(headers), 1))(*[n.encode() for n, _ in headers])
    values = (ctypes.c_char_p * max(len(headers), 1))(*[v.encode() for _, v in headers])
    return names, values


class Cache:
    def __init__(self, lib):
        self.lib = lib
        size = lib.ps_read_shared_config_volume_size(CACHE_PATH.encode())
        if size == 0:
            raise RuntimeError("the worker has not published its volume size")
        self.cfg = CacheConfig()
        lib.ps_cache_config_init_sized(ctypes.byref(self.cfg), ctypes.sizeof(self.cfg))
        self._path = CACHE_PATH.encode()
        self.cfg.volume_path = self._path
        self.cfg.volume_size = size
        self.handle = ctypes.c_void_p()
        rc = lib.ps_cache_open(ctypes.byref(self.cfg), ctypes.byref(self.handle))
        if rc != PS_OK:
            raise RuntimeError(
                f"ps_cache_open: {_err(rc)}: {lib.ps_last_error_message()!r}"
            )

    def close(self):
        self.lib.ps_cache_close(self.handle)

    def read(self, req, alternate_id):
        out = ctypes.c_void_p()
        rc = self.lib.ps_cache_read_alternate_site(
            self.handle,
            _b(req["url"]),
            _b(req["host"]),
            _b(req["scheme"]),
            _b(req["site"]),
            alternate_id,
            ctypes.byref(out),
        )
        return (out if rc == PS_OK else None), rc

    def content(self, result) -> bytes:
        data = ctypes.POINTER(ctypes.c_uint8)()
        length = c_size()
        rc = self.lib.ps_read_content(result, ctypes.byref(data), ctypes.byref(length))
        if rc != PS_OK:
            return b""
        return ctypes.string_at(data, length.value)


def _defaults(req):
    req.setdefault("scheme", "http")
    req.setdefault("site", "")
    return req


def cmd_store(lib, req):
    """Store a page the way the module's HTML store does."""
    body = req["body"].encode()
    headers = req.get("headers") or [
        ["Content-Type", "text/html; charset=utf-8"],
        ["Cache-Control", "public, max-age=600"],
    ]
    content_type = next(
        (v for n, v in headers if n.lower() == "content-type"), "text/html"
    )
    names, values = _header_arrays(headers)
    now = int(time.time())
    reason = ctypes.c_int()
    lifetime = ctypes.c_uint32()
    store_flags = ctypes.c_uint32()
    cc_flags = ctypes.c_uint16()
    sidecar_verdict = ctypes.c_int()
    detail = ctypes.create_string_buffer(256)
    detail_len = c_size()
    action = lib.ps_html_store_classify(
        PS_HTML_CACHE_SERVE_VARIANTS,
        b"GET",
        _b(req["url"]),
        0,
        200,
        ctypes.cast(names, ctypes.c_void_p),
        ctypes.cast(values, ctypes.c_void_p),
        len(headers),
        len(body),
        1,
        body,
        min(len(body), 65536),
        8 << 20,
        now,
        ctypes.byref(reason),
        ctypes.byref(lifetime),
        ctypes.byref(store_flags),
        ctypes.byref(cc_flags),
        ctypes.byref(sidecar_verdict),
        detail,
        ctypes.sizeof(detail),
        ctypes.byref(detail_len),
    )
    out = {
        "action": STORE_ACTIONS.get(action, str(action)),
        "reason": (lib.ps_html_store_reason_name(reason.value) or b"").decode(),
    }
    if action not in (1, 2):
        return out
    # The body hash the module stamps; "stamp" lets a test store bytes that do
    # not match their stamp, the way a torn or tampered write would.
    digest = hashlib.sha256(body).digest()
    if req.get("stamp"):
        digest = bytes.fromhex(req["stamp"])
    hash_buf, hash_ptr = _hash_ptr(digest)
    cache = Cache(lib)
    try:
        # Sidecar first, then the original (the documented order).
        sp = SidecarWriteParams()
        sp.struct_size = ctypes.sizeof(sp)
        sp.url, sp.hostname = _b(req["url"]), _b(req["host"])
        sp.scheme, sp.site = _b(req["scheme"]), _b(req["site"])
        sp.origin_html_hash = hash_ptr
        verdict, stored = ctypes.c_int(), ctypes.c_int()
        rc = lib.ps_cache_write_headers_sidecar_site(
            cache.handle,
            ctypes.byref(sp),
            ctypes.cast(names, ctypes.c_void_p),
            ctypes.cast(values, ctypes.c_void_p),
            len(headers),
            ctypes.byref(verdict),
            ctypes.byref(stored),
        )
        out["sidecar"] = _err(rc)
        out["sidecar_stored"] = bool(stored.value)

        wp = WriteParams()
        lib.ps_write_params_init_sized(ctypes.byref(wp), ctypes.sizeof(wp))
        ct = content_type.encode()
        if (
            lib.ps_html_store_write_params(
                action,
                lifetime.value,
                store_flags.value,
                cc_flags.value,
                int(bool(stored.value)),
                int(time.time() * 1000),
                None,
                0,
                len(body),
                ct,
                ctypes.byref(wp),
            )
            != 0
        ):
            out["original"] = "write_params_refused"
            return out
        wp.origin_html_hash = hash_ptr
        if req.get("unclassified"):
            # A writer that is not the HTML store: the flag that says so is
            # missing.
            wp.flags &= ~PS_FLAG_HEADERS_CLASSIFIED & 0xFF
        out["flags"] = wp.flags
        handle = ctypes.c_void_p()
        rc = lib.ps_cache_write_original_site(
            cache.handle,
            _b(req["url"]),
            _b(req["host"]),
            _b(req["scheme"]),
            _b(req["site"]),
            ctypes.byref(wp),
            ctypes.byref(handle),
        )
        if rc != PS_OK:
            out["original"] = _err(rc)
            return out
        rc = lib.ps_write_data(handle, body, len(body))
        if rc != PS_OK:
            lib.ps_write_abort(handle)
            out["original"] = _err(rc)
            return out
        out["original"] = _err(lib.ps_write_close(handle))
        out["hash"] = digest.hex()
        del hash_buf
        return out
    finally:
        cache.close()


def cmd_notify(lib, req):
    params = NotifyParams()
    lib.ps_notify_params_init_sized(ctypes.byref(params), ctypes.sizeof(params))
    params.url, params.hostname = _b(req["url"]), _b(req["host"])
    params.scheme = _b(req["scheme"])
    params.content_type = req.get("content_type", PS_CONTENT_HTML)
    params.mask = req.get("mask", 0)
    params.agent_request = 1 if req.get("agent") else 0
    params.site = _b(req["site"]) if req["site"] else None
    flags = 0
    for name in req.get("flags", []):
        flags |= NOTIFY_FLAGS[name]
    params.notify_flags = flags
    rc = lib.ps_notify_worker_ex(SOCKET_PATH.encode(), ctypes.byref(params))
    return {"rc": _err(rc)}


def cmd_facts(lib, req):
    """What a module's serve path learns about the page from the cache."""
    cache = Cache(lib)
    reads = []
    try:
        original, _ = cache.read(req, PS_SENTINEL_ORIGINAL)
        markdown, _ = cache.read(req, PS_SENTINEL_MARKDOWN)
        withheld, _ = cache.read(req, PS_SENTINEL_MARKDOWN_WITHHELD)
        reads = [r for r in (original, markdown, withheld) if r is not None]
        out = {
            "original": original is not None,
            "markdown": markdown is not None,
            "withheld": withheld is not None,
        }
        stamp = None
        if original is not None:
            p = lib.ps_read_origin_html_hash(original)
            stamp = ctypes.string_at(p, 32) if p else None
            out["stamp"] = stamp.hex() if stamp else None
            out["original_flags"] = lib.ps_read_flags(original)
        if markdown is not None:
            p = lib.ps_read_origin_html_hash(markdown)
            out["markdown_parent"] = ctypes.string_at(p, 32).hex() if p else None
            out["markdown_body"] = cache.content(markdown).decode("utf-8", "replace")

        block = ctypes.c_void_p()
        rc = lib.ps_headers_sidecar_read(
            cache.handle,
            _b(req["url"]),
            _b(req["host"]),
            _b(req["scheme"]),
            _b(req["site"]),
            ctypes.byref(block),
        )
        sidecar_present = rc == PS_OK
        out["sidecar"] = "absent"
        if sidecar_present:
            pairing = -1
            if stamp:
                _buf, ptr = _hash_ptr(stamp)
                pairing = lib.ps_header_block_pairing(block, ptr)
            out["sidecar"] = {1: "paired", 0: "unpaired"}.get(pairing, "unknown")
            lib.ps_header_block_free(block)

        facts = StoredFacts()
        facts.struct_size = ctypes.sizeof(facts)
        rc = lib.ps_html_stored_facts_from_reads(
            original, markdown, withheld, int(sidecar_present), ctypes.byref(facts)
        )
        out["facts_rc"] = rc
        out["facts"] = sorted(n for b, n in FACT_BITS.items() if facts.flags & b)
        out["withheld_reason"] = WITHHELD_REASONS.get(facts.withheld_reason)
        out["markdown_hash"] = bytes(facts.markdown_hash).hex()

        etag = ctypes.create_string_buffer(64)
        etag_len = c_size()
        rc = lib.ps_html_markdown_etag(
            ctypes.cast(facts.binding_hash, c_u8p),
            ctypes.cast(facts.markdown_hash, c_u8p),
            etag,
            ctypes.sizeof(etag),
            ctypes.byref(etag_len),
        )
        out["etag"] = etag.value.decode() if rc == 0 else None

        request = ServeRequest()
        request.struct_size = ctypes.sizeof(request)
        request.mode = PS_HTML_CACHE_SERVE_VARIANTS
        request.method = b"GET"
        request.accept = _b(req.get("accept", "text/markdown"))
        request.now_seconds = int(time.time())
        verdict = ServeVerdict()
        verdict.struct_size = ctypes.sizeof(verdict)
        rc = lib.ps_html_serve_classify(
            ctypes.byref(request), ctypes.byref(facts), ctypes.byref(verdict)
        )
        out["serve"] = {
            "rc": rc,
            "action": SERVE_ACTIONS.get(verdict.action, str(verdict.action)),
            "reason": (lib.ps_html_serve_reason_name(verdict.reason) or b"").decode(),
            "wants_markdown": bool(verdict.wants_markdown),
            "etag": verdict.etag.decode(),
        }

        # Whatever ordinary selection returns for the page in this site: an
        # optimized HTML variant would be a non-sentinel id here.
        best = ctypes.c_void_p()
        rc = lib.ps_cache_read_best_site(
            cache.handle,
            _b(req["url"]),
            _b(req["host"]),
            _b(req["scheme"]),
            _b(req["site"]),
            req.get("mask", 0),
            ctypes.byref(best),
        )
        if rc == PS_OK:
            out["best_alternate_id"] = lib.ps_read_mask(best) & 0xFF
            lib.ps_read_free(best)
        else:
            out["best_alternate_id"] = None
        return out
    finally:
        for r in reads:
            lib.ps_read_free(r)
        cache.close()


def cmd_shared_config(lib, _req):
    return {
        "version_minor": lib.ps_version_minor(),
        "html_mode": lib.ps_read_shared_config_html_mode(CACHE_PATH.encode()),
        "agent_entitled": lib.ps_read_shared_config_agent_entitled(CACHE_PATH.encode()),
    }


COMMANDS = {
    "store": cmd_store,
    "notify": cmd_notify,
    "facts": cmd_facts,
    "shared_config": cmd_shared_config,
}


def main(argv):
    req = _defaults(json.loads(argv[1]))
    lib = _load()
    print(json.dumps(COMMANDS[req["cmd"]](lib, req)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
