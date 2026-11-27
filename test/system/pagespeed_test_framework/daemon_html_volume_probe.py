#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.

"""Reads one URL's HTML cache entries from the optimizer's volume.

The HTML cache conformance providers run this as the web server's own user,
the volume's owner (it is the optimizer worker's file), through the
optimizer's own client library: the entry the module stored for a URL, by
exact id, with its lifetime and stored header block, and whether its
Markdown copy is servable now. It writes nothing. One JSON object on stdout:

  {"present": bool, "lifetime_s": int, "stored_at": int, "sidecar": [[n, v]],
   "markdown_ready": bool, "coded": {"br": bool, "gzip": bool}}

"coded" says whether the volume holds the brotli and the gzip copy of the
Markdown copy with bytes (an entry without bytes is the optimizer's record
that the coding is not smaller).

With --ask-socket, a stored page whose Markdown copy is not servable yet is
also asked for once, through the worker's notification socket, exactly as
the module asks when a request for the copy finds none (an agent request for
a page this module stored for a host it vouched for). The module's counters
and the stored page are untouched by that.

Usage: daemon_html_volume_probe.py --library PATH --volume PATH --url PATH
       --hostname HOST --scheme SCHEME --site SITE [--ask-socket SOCKET]
"""

import argparse
import ctypes
import json
import sys
import time

PS_OK = 0
SENTINEL_ORIGINAL = 0x0C
SENTINEL_MARKDOWN = 0x7C
SENTINEL_WITHHELD = 0xCC
SENTINEL_MARKDOWN_BR = 0xDC
SENTINEL_MARKDOWN_GZIP = 0xEC
FACTS_PRESENT = 0x1
FACTS_NEEDS_VALIDATION = 0x2
FACTS_VARIANT_PRESENT = 0x100
FACTS_BINDING_LIVE = 0x200
FACTS_WITHHELD = 0x800


class CacheConfig(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("volume_path", ctypes.c_char_p),
        ("volume_size", ctypes.c_uint64),
        ("enable_checksum", ctypes.c_int),
        ("ram_cache_size", ctypes.c_size_t),
        ("max_metadata_size", ctypes.c_size_t),
        # Room for fields a newer library adds (ps_cache_open reads every
        # field it defines; see the header).
        ("_reserved", ctypes.c_uint8 * 256),
    ]


class NotifyParams(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("url", ctypes.c_char_p),
        ("hostname", ctypes.c_char_p),
        ("scheme", ctypes.c_char_p),
        ("content_type", ctypes.c_int),
        ("mask", ctypes.c_uint32),
        ("agent_request", ctypes.c_int),
        ("option_context", ctypes.c_char_p),
        ("option_context_length", ctypes.c_size_t),
        ("option_signature", ctypes.c_char_p),
        ("site", ctypes.c_char_p),
        ("notify_flags", ctypes.c_uint32),
    ]


NOTIFY_FROM_MODULE_VOUCHED_STORED = 0x1 | 0x2 | 0x8
CONTENT_HTML = 0


class StoredFacts(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_size_t),
        ("flags", ctypes.c_uint32),
        ("stored_at", ctypes.c_uint32),
        ("age_at_store", ctypes.c_uint32),
        ("lifetime_s", ctypes.c_uint32),
        ("render_ttl_s", ctypes.c_uint32),
        ("withheld_reason", ctypes.c_int),
        ("binding_hash", ctypes.c_uint8 * 32),
        ("markdown_hash", ctypes.c_uint8 * 32),
    ]


def _bind(lib):
    vp = ctypes.c_void_p
    sigs = {
        "ps_read_shared_config_volume_size": (ctypes.c_uint64, [ctypes.c_char_p]),
        "ps_cache_config_init_sized": (None, [ctypes.POINTER(CacheConfig), ctypes.c_size_t]),
        "ps_cache_open": (ctypes.c_int, [ctypes.POINTER(CacheConfig), ctypes.POINTER(vp)]),
        "ps_cache_close": (None, [vp]),
        "ps_cache_read_alternate_site": (
            ctypes.c_int,
            [vp, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
             ctypes.c_char_p, ctypes.c_uint8, ctypes.POINTER(vp)],
        ),
        "ps_read_free": (None, [vp]),
        "ps_read_content": (
            ctypes.c_int, [vp, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_size_t)]
        ),
        "ps_read_origin_max_age": (ctypes.c_uint32, [vp]),
        "ps_read_cache_inserted_at": (ctypes.c_uint32, [vp]),
        "ps_headers_sidecar_read": (
            ctypes.c_int,
            [vp, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
             ctypes.c_char_p, ctypes.POINTER(vp)],
        ),
        "ps_header_block_count": (ctypes.c_size_t, [vp]),
        "ps_header_block_get": (
            ctypes.c_int,
            [vp, ctypes.c_size_t, ctypes.POINTER(ctypes.c_char_p),
             ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_char_p),
             ctypes.POINTER(ctypes.c_size_t)],
        ),
        "ps_header_block_free": (None, [vp]),
        "ps_html_stored_facts_from_reads": (
            ctypes.c_int, [vp, vp, vp, ctypes.c_int, ctypes.POINTER(StoredFacts)]
        ),
        "ps_cache_read_site_purge_epoch": (
            ctypes.c_int, [vp, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)]
        ),
        "ps_notify_params_init_sized": (None, [ctypes.POINTER(NotifyParams), ctypes.c_size_t]),
        "ps_notify_worker_ex": (ctypes.c_int, [ctypes.c_char_p, ctypes.POINTER(NotifyParams)]),
    }
    for name, (res, args) in sigs.items():
        fn = getattr(lib, name)
        fn.restype = res
        fn.argtypes = args


def probe(args) -> dict:
    lib = ctypes.CDLL(args.library)
    _bind(lib)
    volume = args.volume.encode()
    config = CacheConfig()
    lib.ps_cache_config_init_sized(ctypes.byref(config), ctypes.sizeof(CacheConfig))
    config.volume_path = volume
    config.volume_size = lib.ps_read_shared_config_volume_size(volume)
    if config.volume_size == 0:
        raise RuntimeError("the optimizer publishes no volume size")
    cache = ctypes.c_void_p()
    rc = lib.ps_cache_open(ctypes.byref(config), ctypes.byref(cache))
    if rc != PS_OK:
        raise RuntimeError(f"ps_cache_open: {rc}")
    url, host = args.url.encode(), args.hostname.encode()
    scheme, site = args.scheme.encode(), args.site.encode()
    out = {"present": False, "markdown_ready": False, "coded": {"br": False, "gzip": False}}
    reads = {}
    try:
        for name, alt in (("br", SENTINEL_MARKDOWN_BR), ("gzip", SENTINEL_MARKDOWN_GZIP)):
            r = ctypes.c_void_p()
            if lib.ps_cache_read_alternate_site(
                    cache, url, host, scheme, site, alt, ctypes.byref(r)) == PS_OK:
                data, size = ctypes.c_void_p(), ctypes.c_size_t(0)
                out["coded"][name] = lib.ps_read_content(
                    r, ctypes.byref(data), ctypes.byref(size)) == PS_OK and size.value > 0
                lib.ps_read_free(r)
        for name, alt in (("original", SENTINEL_ORIGINAL),
                          ("markdown", SENTINEL_MARKDOWN),
                          ("withheld", SENTINEL_WITHHELD)):
            r = ctypes.c_void_p()
            if lib.ps_cache_read_alternate_site(
                    cache, url, host, scheme, site, alt, ctypes.byref(r)) == PS_OK:
                reads[name] = r
        original = reads.get("original")
        sidecar_present = 0
        if original is not None:
            out["present"] = True
            out["lifetime_s"] = lib.ps_read_origin_max_age(original)
            out["stored_at"] = lib.ps_read_cache_inserted_at(original)
            block = ctypes.c_void_p()
            if lib.ps_headers_sidecar_read(
                    cache, url, host, scheme, site, ctypes.byref(block)) == PS_OK:
                sidecar_present = 1
                lines = []
                for i in range(lib.ps_header_block_count(block)):
                    n, v = ctypes.c_char_p(), ctypes.c_char_p()
                    nl, vl = ctypes.c_size_t(), ctypes.c_size_t()
                    if lib.ps_header_block_get(block, i, ctypes.byref(n), ctypes.byref(nl),
                                               ctypes.byref(v), ctypes.byref(vl)) == PS_OK:
                        lines.append([n.value.decode("latin-1"), v.value.decode("latin-1")])
                lib.ps_header_block_free(block)
                out["sidecar"] = lines
            facts = StoredFacts()
            facts.struct_size = ctypes.sizeof(StoredFacts)
            if lib.ps_html_stored_facts_from_reads(
                    original, reads.get("markdown"), reads.get("withheld"),
                    sidecar_present, ctypes.byref(facts)) == 0:
                epoch = ctypes.c_uint64(0)
                epoch_ok = lib.ps_cache_read_site_purge_epoch(
                    cache, site, ctypes.byref(epoch)) == PS_OK
                fenced = (not epoch_ok) or facts.stored_at <= epoch.value
                fresh = time.time() - facts.stored_at < facts.lifetime_s
                out["markdown_ready"] = bool(
                    fresh
                    and not (facts.flags & FACTS_NEEDS_VALIDATION)
                    and (facts.flags & FACTS_PRESENT)
                    and (facts.flags & FACTS_VARIANT_PRESENT)
                    and (facts.flags & FACTS_BINDING_LIVE)
                    and not (facts.flags & FACTS_WITHHELD)
                    and not fenced
                )
        if args.ask_socket and out["present"] and not out["markdown_ready"]:
            n = NotifyParams()
            lib.ps_notify_params_init_sized(ctypes.byref(n), ctypes.sizeof(NotifyParams))
            n.url = f"{args.scheme}://{args.hostname}{args.url}".encode()
            n.hostname, n.scheme, n.site = host, scheme, site
            n.content_type = CONTENT_HTML
            n.agent_request = 1
            n.notify_flags = NOTIFY_FROM_MODULE_VOUCHED_STORED
            out["asked"] = lib.ps_notify_worker_ex(args.ask_socket.encode(), ctypes.byref(n))
    finally:
        for r in reads.values():
            lib.ps_read_free(r)
        lib.ps_cache_close(cache)
    return out


def main() -> int:
    p = argparse.ArgumentParser()
    for name in ("library", "volume", "url", "hostname", "scheme", "site"):
        p.add_argument(f"--{name}", required=True)
    p.add_argument("--ask-socket", default="")
    json.dump(probe(p.parse_args()), sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
