#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""ABI stability gate for the lib/pagespeed C API (BLOCKING).

The library is shipped and loaded by consumers built somewhere else, at some
other time, against some other copy of pagespeed.h. Every failure this gate
catches has the same shape: the two sides stop agreeing about what the library
looks like, nothing goes red here, and the damage surfaces on a customer's
machine as an unresolved symbol or as a struct read at the wrong offsets.

Four checks, described in lib/pagespeed/ABI.md:

  1. Export-list consistency -- the PS_EXPORTed functions in pagespeed.h are
     exactly the symbols listed in symbols.lds / symbols.exp / symbols.def.
     A function that is declared but not listed does not exist at runtime on
     that platform.
  2. Golden agreement -- lib/pagespeed/abi/abi-golden.json records the current
     surface (symbols, version triple, struct sizes) and must match the header.
     Updating it is how an ABI change gets stated out loud in review.
  3. Layout pinning -- the struct sizes in the golden must agree with the
     static_asserts in test/lib/pagespeed/abi_layout_test.cc, which are what
     actually fail the build, on every platform CI compiles for.
  4. Compatibility -- against the golden from the merge base, a removal or a
     layout change demands a MAJOR bump and an addition demands a MINOR one.

Usage:
  tools/ci/check_abi.py                      # checks 1-3
  tools/ci/check_abi.py --compat-base FILE   # ... and 4, against a base golden
  tools/ci/check_abi.py --update             # rewrite the golden from source
  tools/ci/check_abi.py --self-test          # exercise the decision logic
"""

import argparse
import json
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

HEADER = os.path.join(REPO_ROOT, "lib", "pagespeed", "pagespeed.h")
LDS = os.path.join(REPO_ROOT, "lib", "pagespeed", "symbols.lds")
EXP = os.path.join(REPO_ROOT, "lib", "pagespeed", "symbols.exp")
DEF = os.path.join(REPO_ROOT, "lib", "pagespeed", "symbols.def")
GOLDEN = os.path.join(REPO_ROOT, "lib", "pagespeed", "abi", "abi-golden.json")
LAYOUT_TEST = os.path.join(REPO_ROOT, "test", "lib", "pagespeed", "abi_layout_test.cc")
LIB_BUILD = os.path.join(REPO_ROOT, "lib", "pagespeed", "BUILD")
IMPL = os.path.join(REPO_ROOT, "lib", "pagespeed", "pagespeed.cc")
DOTNET_STRUCTS = os.path.join(
    REPO_ROOT,
    "samples",
    "aspnetcore",
    "src",
    "WeAmp.PageSpeed",
    "Native",
    "NativeStructs.cs",
)

# The .NET binding hand-mirrors a prefix of three C structs and P/Invokes into
# them. It is not generated, nothing links it against the header, and the whole
# safety argument for ps_write_params_init's prefix-only behaviour rests on
# NativeWriteParams being exactly the 1.1 size. So the mapping is pinned here
# and checked, rather than left to a comment asking the next editor to keep two
# files in step.
DOTNET_MIRRORS = {
    "NativeWriteParams": {
        "native": "ps_write_params_t",
        # The full struct since #772: the .NET write path sets origin_cc_flags,
        # so the mirror carries every field this library defines. (It
        # deliberately stopped at the 1.1 prefix before that -- the size
        # ps_write_params_init clears.)
        "expect_size": "native",
        "fields": {
            "StructSize": "struct_size",
            "AlternateId": "alternate_id",
            "ContentLength": "content_length",
            "FullMask": "full_mask",
            "ContentType": "content_type",
            "Flags": "flags",
            "OriginCt": "origin_ct",
            "OriginCcFlags": "origin_cc_flags",
            "OriginHtmlHash": "origin_html_hash",
        },
    },
    "NativeCacheConfig": {
        "native": "ps_cache_config_t",
        "expect_size": "native",
        "fields": {
            "StructSize": "struct_size",
            "VolumePath": "volume_path",
            "VolumeSize": "volume_size",
            "EnableChecksum": "enable_checksum",
            "RamCacheSize": "ram_cache_size",
            "MaxMetadataSize": "max_metadata_size",
        },
    },
    "NativeCacheStats": {
        "native": "ps_cache_stats_t",
        "expect_size": "native",
        "fields": {
            "StructSize": "struct_size",
            "RamCacheHits": "ram_cache_hits",
            "RamCacheMisses": "ram_cache_misses",
            "DiskCacheHits": "disk_cache_hits",
            "DiskCacheMisses": "disk_cache_misses",
            "BytesRead": "bytes_read",
            "BytesWritten": "bytes_written",
            "Evictions": "evictions",
            "CurrentEntries": "current_entries",
            "CurrentSizeBytes": "current_size_bytes",
            "VolumeCapacityBytes": "volume_capacity_bytes",
            "RamCacheBytes": "ram_cache_bytes",
            "TotalHits": "total_hits",
            "TotalMisses": "total_misses",
        },
    },
}


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

# `PS_EXPORT [PS_NODISCARD] <return type...> ps_name(` -- the declaration may
# wrap across lines between the macro and the name, so the whole header is
# scanned as one string.
_DECL_RE = re.compile(r"PS_EXPORT\b(?:(?!;).)*?\b(ps_[A-Za-z0-9_]+)\s*\(", re.S)

_VERSION_RE = re.compile(r"#define\s+PS_API_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)")

# static_assert(sizeof(ps_foo_t) == 48, ...)
_SIZEOF_ASSERT_RE = re.compile(
    r"static_assert\s*\(\s*sizeof\s*\(\s*(ps_[A-Za-z0-9_]+)\s*\)\s*==\s*(\d+)"
)

# static_assert(offsetof(ps_foo_t, bar) == 8, ...)  -- possibly line-wrapped
_OFFSET_ASSERT_RE = re.compile(
    r"static_assert\s*\(\s*offsetof\s*\(\s*(ps_[A-Za-z0-9_]+)\s*,"
    r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*==\s*(\d+)",
    re.S,
)

# -Wl,-soname,libpagespeed.so
_SONAME_RE = re.compile(r"-Wl,-soname,([A-Za-z0-9_.+-]+)")

# constexpr size_t kWriteParamsSizeV1_1 = ...; static_assert(... == 48
_LEGACY_PREFIX_RE = re.compile(
    r"static_assert\s*\(\s*kWriteParamsSizeV1_1\s*==\s*(\d+)"
)

# [StructLayout(LayoutKind.Explicit, Size = 48)] / [FieldOffset(8)]  public T Name;
_CS_STRUCT_RE = re.compile(
    r"\[StructLayout\([^\]]*Size\s*=\s*(\d+)[^\]]*\)\]\s*"
    r"internal\s+struct\s+([A-Za-z0-9_]+)\s*\{(.*?)\n\}",
    re.S,
)
_CS_FIELD_RE = re.compile(
    r"\[FieldOffset\((\d+)\)\]\s*public\s+[^;]*?([A-Za-z0-9_]+)\s*;"
)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def header_symbols(text):
    """Exported function names, in declaration order, deduplicated."""
    seen = []
    for m in _DECL_RE.finditer(text):
        name = m.group(1)
        if name not in seen:
            seen.append(name)
    return seen


def header_version(text):
    parts = {m.group(1): int(m.group(2)) for m in _VERSION_RE.finditer(text)}
    missing = {"MAJOR", "MINOR", "PATCH"} - set(parts)
    if missing:
        raise SystemExit(
            "check_abi: pagespeed.h is missing PS_API_VERSION_{}".format(
                ", ".join(sorted(missing))
            )
        )
    return [parts["MAJOR"], parts["MINOR"], parts["PATCH"]]


def lds_symbols(text):
    """{symbol: version node} from the version script.

    Node identity is the point, not just membership: a symbol's exported name is
    `name@NODE`, so moving an existing symbol to another node renames it and
    unlinks every binary already built against it. Flattening the nodes into one
    list would make that invisible.
    """
    out = {}
    node = None
    in_global = False
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("/*"):
            continue
        m = re.match(r"([A-Za-z0-9_.]+)\s*\{", stripped)
        if m:
            node = m.group(1)
            in_global = False
            continue
        if stripped.startswith("global:"):
            in_global = True
            continue
        if stripped.startswith("local:"):
            in_global = False
            continue
        if stripped.startswith("}"):
            in_global = False
            continue
        if in_global:
            name = stripped.rstrip(";").strip()
            if name.startswith("ps_"):
                out[name] = node
    return out


def exp_symbols(text):
    out = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if stripped.startswith("_ps_"):
            out.append(stripped[1:])
    return out


def def_symbols(text):
    out = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith(";"):
            continue
        if stripped.startswith("ps_"):
            out.append(stripped)
    return out


def layout_records(text):
    """{struct: {"size": N, "fields": {name: offset}}} from the layout test.

    Sizes alone are not the ABI. Swapping two same-width fields leaves the size
    untouched and moves every consumer's reads onto the wrong member, so the
    offsets are recorded and compared too.
    """
    out = {}
    for m in _SIZEOF_ASSERT_RE.finditer(text):
        out.setdefault(m.group(1), {"size": None, "fields": {}})
        out[m.group(1)]["size"] = int(m.group(2))
    for m in _OFFSET_ASSERT_RE.finditer(text):
        name, field, off = m.group(1), m.group(2), int(m.group(3))
        out.setdefault(name, {"size": None, "fields": {}})
        out[name]["fields"][field] = off
    return out


def build_soname(text):
    """The DT_SONAME the shared library is actually linked with, or None."""
    m = _SONAME_RE.search(text)
    return m.group(1) if m else None


def legacy_init_prefix(text):
    """The byte count ps_write_params_init clears, per its own static_assert."""
    m = _LEGACY_PREFIX_RE.search(text)
    return int(m.group(1)) if m else None


def dotnet_mirrors(text):
    """{struct: {"size": N, "fields": {name: offset}}} from the .NET binding."""
    out = {}
    for m in _CS_STRUCT_RE.finditer(text):
        size, name, body = int(m.group(1)), m.group(2), m.group(3)
        fields = {f.group(2): int(f.group(1)) for f in _CS_FIELD_RE.finditer(body)}
        out[name] = {"size": size, "fields": fields}
    return out


# ---------------------------------------------------------------------------
# Checks (pure, so --self-test can drive every branch)
# ---------------------------------------------------------------------------


def check_export_lists(header_syms, lds, exp, dfn):
    """Every declared symbol is exported on every platform, and vice versa."""
    errors = []
    want = set(header_syms)
    for label, got in (
        ("symbols.lds", set(lds)),
        ("symbols.exp", set(exp)),
        ("symbols.def", set(dfn)),
    ):
        missing = sorted(want - got)
        extra = sorted(got - want)
        for name in missing:
            errors.append(
                f"{label}: {name} is declared PS_EXPORT in pagespeed.h but is not "
                "exported. It will fail to resolve at runtime on that "
                "platform."
            )
        for name in extra:
            errors.append(
                f"{label}: {name} is exported but not declared PS_EXPORT in "
                "pagespeed.h. Either the declaration was dropped (an ABI "
                "removal) or the export list is stale."
            )
    return errors


def check_golden(
    header_syms,
    lds_nodes,
    version,
    golden,
    layout,
    soname,
    mirrors,
    legacy_prefix,
    mirror_map=None,
):
    errors = []
    if golden.get("api_version") != version:
        errors.append(
            "abi-golden.json records api_version {} but pagespeed.h says {}. "
            "Run tools/ci/check_abi.py --update.".format(
                golden.get("api_version"), version
            )
        )
    g_syms = set(golden.get("symbols", {}))
    h_syms = set(header_syms)
    for name in sorted(h_syms - g_syms):
        errors.append(
            f"abi-golden.json does not record {name}. A new export is a MINOR "
            "bump: update PS_API_VERSION_MINOR and run --update."
        )
    for name in sorted(g_syms - h_syms):
        errors.append(
            f"abi-golden.json records {name} but pagespeed.h no longer declares "
            "it. Removing an export is a MAJOR break -- it unlinks every "
            "shipped consumer that calls it."
        )

    # Version-node membership. The node is half of a symbol's exported
    # identity, so it is recorded and compared like the name.
    g_nodes = golden.get("symbols", {})
    for name in sorted(set(lds_nodes) & set(g_nodes)):
        if g_nodes[name] != lds_nodes[name]:
            errors.append(
                f"symbol {name} is in version node {lds_nodes[name]} but abi-golden.json records "
                f"{g_nodes[name]}. A symbol's exported name is name@NODE, so moving it "
                "renames it and unlinks every binary already built against it."
            )

    # Struct size + every field offset.
    g_structs = golden.get("structs", {})
    for name, rec in sorted(layout.items()):
        if name not in g_structs:
            errors.append(
                "abi-golden.json does not record struct {} (the layout test "
                "pins it at {} bytes). Run --update.".format(name, rec["size"])
            )
            continue
        g = g_structs[name]
        if g.get("size") != rec["size"]:
            errors.append(
                "struct {}: abi-golden.json says {} bytes, the layout test "
                "pins {}. One of the two moved without the other.".format(
                    name, g.get("size"), rec["size"]
                )
            )
        g_fields = g.get("fields", {})
        for field, off in sorted(rec["fields"].items()):
            if field not in g_fields:
                errors.append(
                    f"abi-golden.json does not record {name}.{field} (pinned at offset "
                    f"{off}). Run --update."
                )
            elif g_fields[field] != off:
                errors.append(
                    f"{name}.{field}: abi-golden.json says offset {g_fields[field]}, the layout test "
                    f"pins {off}. A field moved."
                )
        for field in sorted(set(g_fields) - set(rec["fields"])):
            errors.append(
                f"abi-golden.json records {name}.{field} but abi_layout_test.cc no "
                "longer pins its offset. An unpinned field is a layout change "
                "waiting to ship unnoticed."
            )
    for name in sorted(set(g_structs) - set(layout)):
        errors.append(
            f"abi-golden.json records struct {name} but abi_layout_test.cc no "
            "longer pins it. A published struct without a compile-time pin is "
            "a layout change waiting to ship unnoticed."
        )

    # The soname is recorded, so it is checked. A recorded fact nothing can
    # falsify reads in review as "this is verified" while asserting nothing.
    if golden.get("soname") != soname:
        errors.append(
            "abi-golden.json records soname {!r} but lib/pagespeed/BUILD links "
            "with {!r}. DT_SONAME is what a linking consumer records as its "
            "dependency.".format(golden.get("soname"), soname)
        )

    if golden.get("legacy_init_prefix") != legacy_prefix:
        errors.append(
            "abi-golden.json records legacy_init_prefix {!r} but pagespeed.cc "
            "asserts {!r}. That number is how many bytes ps_write_params_init "
            "clears, and the .NET mirror is sized to match it.".format(
                golden.get("legacy_init_prefix"), legacy_prefix
            )
        )

    errors += check_dotnet_mirrors(golden, mirrors, legacy_prefix, mirror_map)
    return errors


def check_dotnet_mirrors(golden, mirrors, legacy_prefix, mirror_map=None):
    """The hand-written .NET layout mirrors still match the C structs.

    Nothing links these two files together: the mirror is offsets typed by hand
    into C#, and the argument for ps_write_params_init's prefix-only behaviour
    is precisely that NativeWriteParams is that prefix. Left to a comment, it
    holds until the first person who updates one side.
    """
    errors = []
    if mirror_map is None:
        mirror_map = DOTNET_MIRRORS
    g_structs = golden.get("structs", {})
    for cs_name, spec in sorted(mirror_map.items()):
        if cs_name not in mirrors:
            errors.append(
                "NativeStructs.cs no longer declares {}, which mirrors {}. If "
                "the mirror was renamed or dropped, update DOTNET_MIRRORS in "
                "this script so the pin follows it.".format(cs_name, spec["native"])
            )
            continue
        native = g_structs.get(spec["native"])
        if native is None:
            errors.append(
                "{} mirrors {}, which the golden does not record.".format(
                    cs_name, spec["native"]
                )
            )
            continue
        got = mirrors[cs_name]
        if spec["expect_size"] == "legacy_init_prefix":
            want_size = legacy_prefix
            why = (
                "the prefix ps_write_params_init clears -- if these diverge, "
                "that initializer either overruns the mirror or leaves part "
                "of it unset"
            )
        else:
            want_size = native.get("size")
            why = "the size of {}".format(spec["native"])
        if want_size is not None and got["size"] != want_size:
            errors.append(
                f"{cs_name} declares Size = {got['size']} but should be {want_size} ({why})."
            )
        for cs_field, c_field in sorted(spec["fields"].items()):
            if cs_field not in got["fields"]:
                errors.append(
                    "{} no longer declares {} (mirrors {}.{}).".format(
                        cs_name, cs_field, spec["native"], c_field
                    )
                )
                continue
            want_off = native.get("fields", {}).get(c_field)
            if want_off is None:
                errors.append(
                    "{}.{} mirrors {}.{}, whose offset the golden does not "
                    "record.".format(cs_name, cs_field, spec["native"], c_field)
                )
            elif got["fields"][cs_field] != want_off:
                errors.append(
                    f"{cs_name}.{cs_field} is at offset {got['fields'][cs_field]} but "
                    f"{spec['native']}.{c_field} is at {want_off}. The mirror is "
                    "read by P/Invoke at these offsets; a mismatch silently "
                    "reads the wrong member."
                )
    return errors


def check_compat(base, current):
    """Does the version move match the nature of the ABI change?"""
    errors = []
    b_ver = base.get("api_version", [0, 0, 0])
    c_ver = current.get("api_version", [0, 0, 0])
    b_syms = base.get("symbols", {})
    c_syms = current.get("symbols", {})
    b_structs = base.get("structs", {})
    c_structs = current.get("structs", {})

    def size_of(d, n):
        return (d.get(n) or {}).get("size")

    def fields_of(d, n):
        return (d.get(n) or {}).get("fields", {})

    removed = sorted(set(b_syms) - set(c_syms))
    added = sorted(set(c_syms) - set(b_syms))
    resized = sorted(
        n
        for n in set(b_structs) & set(c_structs)
        if size_of(b_structs, n) != size_of(c_structs, n)
    )
    dropped_structs = sorted(set(b_structs) - set(c_structs))

    breaking = []
    for name in removed:
        breaking.append(f"exported symbol {name} was removed")
    for name in dropped_structs:
        breaking.append(f"published struct {name} was removed")
    for name in sorted(set(b_syms) & set(c_syms)):
        if b_syms[name] != c_syms[name]:
            breaking.append(
                f"exported symbol {name} moved from version node {b_syms[name]} to {c_syms[name]}, which "
                "changes its name@NODE and unlinks existing binaries"
            )
    for name in sorted(set(b_structs) & set(c_structs)):
        bf, cf = fields_of(b_structs, name), fields_of(c_structs, name)
        for field in sorted(set(bf) & set(cf)):
            if bf[field] != cf[field]:
                breaking.append(
                    f"field {name}.{field} moved from offset {bf[field]} to {cf[field]}"
                )
        for field in sorted(set(bf) - set(cf)):
            breaking.append(f"field {name}.{field} was removed")
    for name in resized:
        # Growing a struct that carries struct_size is the sanctioned append;
        # only a SHRINK can strand a consumer's existing reads.
        if size_of(c_structs, name) < size_of(b_structs, name):
            breaking.append(
                f"published struct {name} shrank from {size_of(b_structs, name)} to {size_of(c_structs, name)} bytes"
            )
    if base.get("soname") != current.get("soname"):
        breaking.append(
            "DT_SONAME changed from {!r} to {!r}".format(
                base.get("soname"), current.get("soname")
            )
        )

    if breaking:
        if c_ver[0] <= b_ver[0]:
            errors.append(
                "ABI-BREAKING change without a MAJOR bump ({} -> {}):\n  - {}".format(
                    ".".join(map(str, b_ver)),
                    ".".join(map(str, c_ver)),
                    "\n  - ".join(breaking),
                )
            )
        return errors

    grew = [n for n in resized if size_of(c_structs, n) > size_of(b_structs, n)]
    new_structs = sorted(set(c_structs) - set(b_structs))
    if (added or grew or new_structs) and c_ver[0] == b_ver[0] and c_ver[1] <= b_ver[1]:
        what = []
        if added:
            what.append("new exports: {}".format(", ".join(added)))
        if grew:
            what.append("grown structs: {}".format(", ".join(grew)))
        if new_structs:
            what.append("new structs: {}".format(", ".join(new_structs)))
        errors.append(
            "the ABI surface grew without a MINOR bump ({} -> {}): {}".format(
                ".".join(map(str, b_ver)),
                ".".join(map(str, c_ver)),
                "; ".join(what),
            )
        )
    return errors


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------


def self_test():
    """Drive every decision branch. The count is COUNTED, never asserted from a
    literal -- a hardcoded coverage number is the thing most likely to rot into
    a comfortable lie about what was checked."""
    failures = []
    cases = [0]

    def expect(name, got, want_errors):
        cases[0] += 1
        if bool(got) != want_errors:
            failures.append(
                "{}: expected {}, got {!r}".format(
                    name, "errors" if want_errors else "no errors", got
                )
            )

    L = {"ps_a": "PAGESPEED_1.0", "ps_b": "PAGESPEED_1.0"}

    expect(
        "export lists in sync",
        check_export_lists(["ps_a", "ps_b"], L, ["ps_a", "ps_b"], ["ps_a", "ps_b"]),
        False,
    )
    expect(
        "declared but unexported",
        check_export_lists(
            ["ps_a", "ps_b"],
            {"ps_a": "PAGESPEED_1.0"},
            ["ps_a", "ps_b"],
            ["ps_a", "ps_b"],
        ),
        True,
    )
    expect(
        "exported but undeclared",
        check_export_lists(["ps_a"], L, ["ps_a"], ["ps_a"]),
        True,
    )

    layout = {"ps_t": {"size": 8, "fields": {"struct_size": 0}}}
    mirrors = {}
    golden = {
        "api_version": [1, 2, 0],
        "soname": "libpagespeed.so",
        "legacy_init_prefix": 48,
        "symbols": {"ps_a": "PAGESPEED_1.0"},
        "structs": {"ps_t": {"size": 8, "fields": {"struct_size": 0}}},
    }

    def golden_check(hdr, nodes, ver, g, lay, son="libpagespeed.so", mir=None, pref=48):
        # mirror_map={} keeps these cases about the golden itself; the mirror
        # branches get their own cases below.
        return check_golden(
            hdr,
            nodes,
            ver,
            g,
            lay,
            son,
            mir if mir is not None else mirrors,
            pref,
            mirror_map={},
        )

    N = {"ps_a": "PAGESPEED_1.0"}
    expect(
        "golden matches", golden_check(["ps_a"], N, [1, 2, 0], golden, layout), False
    )
    expect(
        "golden version stale",
        golden_check(["ps_a"], N, [1, 3, 0], golden, layout),
        True,
    )
    expect(
        "golden missing a symbol",
        golden_check(["ps_a", "ps_b"], N, [1, 2, 0], golden, layout),
        True,
    )
    expect(
        "golden has a dropped symbol",
        golden_check([], N, [1, 2, 0], golden, layout),
        True,
    )
    expect(
        "symbol changed version node",
        golden_check(["ps_a"], {"ps_a": "PAGESPEED_1.2"}, [1, 2, 0], golden, layout),
        True,
    )
    expect(
        "layout size disagrees",
        golden_check(
            ["ps_a"],
            N,
            [1, 2, 0],
            golden,
            {"ps_t": {"size": 16, "fields": {"struct_size": 0}}},
        ),
        True,
    )
    expect(
        "layout OFFSET disagrees (same size)",
        golden_check(
            ["ps_a"],
            N,
            [1, 2, 0],
            golden,
            {"ps_t": {"size": 8, "fields": {"struct_size": 4}}},
        ),
        True,
    )
    expect(
        "field lost its pin",
        golden_check(
            ["ps_a"], N, [1, 2, 0], golden, {"ps_t": {"size": 8, "fields": {}}}
        ),
        True,
    )
    expect(
        "struct lost its pin", golden_check(["ps_a"], N, [1, 2, 0], golden, {}), True
    )
    expect(
        "soname diverged from BUILD",
        golden_check(["ps_a"], N, [1, 2, 0], golden, layout, son="libpagespeed.so.9"),
        True,
    )
    expect(
        "legacy init prefix diverged",
        golden_check(["ps_a"], N, [1, 2, 0], golden, layout, pref=64),
        True,
    )

    # .NET mirror checks
    wp_golden = {
        "structs": {
            "ps_write_params_t": {
                "size": 80,
                "fields": {"struct_size": 0, "origin_ct": 40},
            }
        }
    }
    good_mirror = {
        "NativeWriteParams": {"size": 48, "fields": {"StructSize": 0, "OriginCt": 40}}
    }
    wp_map = {
        "NativeWriteParams": {
            "native": "ps_write_params_t",
            "expect_size": "legacy_init_prefix",
            "fields": {"StructSize": "struct_size", "OriginCt": "origin_ct"},
        }
    }
    expect(
        "mirror agrees", check_dotnet_mirrors(wp_golden, good_mirror, 48, wp_map), False
    )
    expect(
        "mirror size drifted",
        check_dotnet_mirrors(
            wp_golden,
            {
                "NativeWriteParams": {
                    "size": 80,
                    "fields": {"StructSize": 0, "OriginCt": 40},
                }
            },
            48,
            wp_map,
        ),
        True,
    )
    expect(
        "mirror field offset drifted",
        check_dotnet_mirrors(
            wp_golden,
            {
                "NativeWriteParams": {
                    "size": 48,
                    "fields": {"StructSize": 0, "OriginCt": 32},
                }
            },
            48,
            wp_map,
        ),
        True,
    )
    expect(
        "mirror struct vanished", check_dotnet_mirrors(wp_golden, {}, 48, wp_map), True
    )

    # NativeCacheStats mirror checks
    stats_golden = {
        "structs": {
            "ps_cache_stats_t": {
                "size": 112,
                "fields": {
                    "struct_size": 0,
                    "ram_cache_hits": 8,
                    "ram_cache_misses": 16,
                    "disk_cache_hits": 24,
                    "disk_cache_misses": 32,
                    "bytes_read": 40,
                    "bytes_written": 48,
                    "evictions": 56,
                    "current_entries": 64,
                    "current_size_bytes": 72,
                    "volume_capacity_bytes": 80,
                    "ram_cache_bytes": 88,
                    "total_hits": 96,
                    "total_misses": 104,
                },
            }
        }
    }
    good_stats_mirror = {
        "NativeCacheStats": {
            "size": 112,
            "fields": {
                "StructSize": 0,
                "RamCacheHits": 8,
                "RamCacheMisses": 16,
                "DiskCacheHits": 24,
                "DiskCacheMisses": 32,
                "BytesRead": 40,
                "BytesWritten": 48,
                "Evictions": 56,
                "CurrentEntries": 64,
                "CurrentSizeBytes": 72,
                "VolumeCapacityBytes": 80,
                "RamCacheBytes": 88,
                "TotalHits": 96,
                "TotalMisses": 104,
            },
        }
    }
    stats_map = {
        "NativeCacheStats": {
            "native": "ps_cache_stats_t",
            "expect_size": "native",
            "fields": {
                "StructSize": "struct_size",
                "RamCacheHits": "ram_cache_hits",
                "RamCacheMisses": "ram_cache_misses",
                "DiskCacheHits": "disk_cache_hits",
                "DiskCacheMisses": "disk_cache_misses",
                "BytesRead": "bytes_read",
                "BytesWritten": "bytes_written",
                "Evictions": "evictions",
                "CurrentEntries": "current_entries",
                "CurrentSizeBytes": "current_size_bytes",
                "VolumeCapacityBytes": "volume_capacity_bytes",
                "RamCacheBytes": "ram_cache_bytes",
                "TotalHits": "total_hits",
                "TotalMisses": "total_misses",
            },
        }
    }
    expect(
        "stats mirror agrees",
        check_dotnet_mirrors(stats_golden, good_stats_mirror, 48, stats_map),
        False,
    )
    expect(
        "stats mirror field offset drifted",
        check_dotnet_mirrors(
            stats_golden,
            {
                "NativeCacheStats": {
                    "size": 112,
                    "fields": {
                        "StructSize": 0,
                        "RamCacheHits": 8,
                        "RamCacheMisses": 16,
                        "DiskCacheHits": 24,
                        "DiskCacheMisses": 32,
                        "BytesRead": 40,
                        "BytesWritten": 48,
                        "Evictions": 56,
                        "CurrentEntries": 64,
                        "CurrentSizeBytes": 72,
                        "VolumeCapacityBytes": 80,
                        "RamCacheBytes": 88,
                        "TotalHits": 96,
                        "TotalMisses": 108,
                    },
                }
            },
            48,
            stats_map,
        ),
        True,
    )

    base = {
        "api_version": [1, 1, 0],
        "soname": "libpagespeed.so",
        "symbols": {"ps_a": "PAGESPEED_1.0", "ps_b": "PAGESPEED_1.0"},
        "structs": {"ps_t": {"size": 48, "fields": {"a": 0, "b": 8}}},
    }

    def cur(**kw):
        d = {
            "api_version": [1, 1, 0],
            "soname": "libpagespeed.so",
            "symbols": dict(base["symbols"]),
            "structs": {"ps_t": {"size": 48, "fields": {"a": 0, "b": 8}}},
        }
        d.update(kw)
        return d

    expect("no change, no bump needed", check_compat(base, base), False)
    expect(
        "addition with a MINOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                symbols={
                    "ps_a": "PAGESPEED_1.0",
                    "ps_b": "PAGESPEED_1.0",
                    "ps_c": "PAGESPEED_1.2",
                },
            ),
        ),
        False,
    )
    expect(
        "addition WITHOUT a bump",
        check_compat(
            base,
            cur(
                symbols={
                    "ps_a": "PAGESPEED_1.0",
                    "ps_b": "PAGESPEED_1.0",
                    "ps_c": "PAGESPEED_1.2",
                }
            ),
        ),
        True,
    )
    expect(
        "struct growth with a MINOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                structs={"ps_t": {"size": 80, "fields": {"a": 0, "b": 8}}},
            ),
        ),
        False,
    )
    expect(
        "struct growth WITHOUT a bump",
        check_compat(
            base, cur(structs={"ps_t": {"size": 80, "fields": {"a": 0, "b": 8}}})
        ),
        True,
    )
    expect(
        "removal WITHOUT a MAJOR bump",
        check_compat(
            base, cur(api_version=[1, 2, 0], symbols={"ps_a": "PAGESPEED_1.0"})
        ),
        True,
    )
    expect(
        "removal WITH a MAJOR bump",
        check_compat(
            base, cur(api_version=[2, 0, 0], symbols={"ps_a": "PAGESPEED_1.0"})
        ),
        False,
    )
    expect(
        "struct shrink WITHOUT a MAJOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                structs={"ps_t": {"size": 16, "fields": {"a": 0, "b": 8}}},
            ),
        ),
        True,
    )
    expect(
        "SAME-SIZE field reorder WITHOUT a MAJOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                structs={"ps_t": {"size": 48, "fields": {"a": 8, "b": 0}}},
            ),
        ),
        True,
    )
    expect(
        "field removed WITHOUT a MAJOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                structs={"ps_t": {"size": 48, "fields": {"a": 0}}},
            ),
        ),
        True,
    )
    expect(
        "symbol moved between nodes WITHOUT a MAJOR bump",
        check_compat(
            base,
            cur(
                api_version=[1, 2, 0],
                symbols={"ps_a": "PAGESPEED_1.2", "ps_b": "PAGESPEED_1.0"},
            ),
        ),
        True,
    )
    expect(
        "soname change WITHOUT a MAJOR bump",
        check_compat(base, cur(api_version=[1, 2, 0], soname="libpagespeed.so.2")),
        True,
    )
    expect(
        "soname change WITH a MAJOR bump",
        check_compat(base, cur(api_version=[2, 0, 0], soname="libpagespeed.so.2")),
        False,
    )

    if failures:
        print(f"check_abi self-test FAILED ({len(failures)} of {cases[0]} cases):")
        for f in failures:
            print(f"  - {f}")
        return 1
    print(f"check_abi self-test: {cases[0]} cases OK")
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--update",
        action="store_true",
        help="rewrite abi-golden.json from the current source",
    )
    ap.add_argument(
        "--compat-base",
        metavar="FILE",
        help="a golden from the merge base, for the semver check",
    )
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    header_text = read(HEADER)
    syms = header_symbols(header_text)
    version = header_version(header_text)
    layout = layout_records(read(LAYOUT_TEST))
    lds_nodes = lds_symbols(read(LDS))
    soname = build_soname(read(LIB_BUILD))
    legacy_prefix = legacy_init_prefix(read(IMPL))
    mirrors = dotnet_mirrors(read(DOTNET_STRUCTS))

    # Every parser is fail-closed. A gate that silently parses nothing reports
    # success on everything, which is worse than not having it.
    for label, value in (
        ("exported symbols from pagespeed.h", syms),
        ("struct pins from abi_layout_test.cc", layout),
        ("version nodes from symbols.lds", lds_nodes),
        (".NET mirrors from NativeStructs.cs", mirrors),
    ):
        if not value:
            print(
                f"check_abi: parsed ZERO {label}. The source shape changed and "
                "this gate is now blind; fix the parser rather than the "
                "thing it reads."
            )
            return 1
    if soname is None:
        print(
            "check_abi: no -Wl,-soname in lib/pagespeed/BUILD. Either the "
            "link options moved or the soname was dropped; the golden "
            "records one, so this cannot pass silently."
        )
        return 1
    if legacy_prefix is None:
        print(
            "check_abi: could not find the kWriteParamsSizeV1_1 assertion in "
            "pagespeed.cc. That constant is the size the .NET mirror is "
            "pinned against."
        )
        return 1
    missing_offsets = sorted(n for n, r in layout.items() if not r["fields"])
    if missing_offsets:
        print(
            "check_abi: these structs are pinned by size but have NO field "
            "offset assertions, so a same-size reorder would pass: {}".format(
                ", ".join(missing_offsets)
            )
        )
        return 1

    if args.update:
        golden = {
            "_comment": (
                "Generated by tools/ci/check_abi.py --update. This is the "
                "recorded shape of the shipped lib/pagespeed C ABI: changing "
                "it is changing what consumers built elsewhere will find. "
                "See lib/pagespeed/ABI.md."
            ),
            "api_version": version,
            "soname": soname,
            "legacy_init_prefix": legacy_prefix,
            "symbols": {n: lds_nodes.get(n) for n in sorted(syms)},
            "structs": {
                n: {
                    "size": layout[n]["size"],
                    "fields": dict(sorted(layout[n]["fields"].items())),
                }
                for n in sorted(layout)
            },
        }
        os.makedirs(os.path.dirname(GOLDEN), exist_ok=True)
        with open(GOLDEN, "w", encoding="utf-8") as f:
            json.dump(golden, f, indent=2)
            f.write("\n")
        pinned_fields = sum(len(r["fields"]) for r in layout.values())
        print(
            f"check_abi: wrote {os.path.relpath(GOLDEN, REPO_ROOT)} "
            f"({len(syms)} symbols, {len(layout)} structs, {pinned_fields} field offsets)"
        )
        return 0

    golden = json.loads(read(GOLDEN))

    errors = []
    errors += check_export_lists(
        syms, lds_nodes, exp_symbols(read(EXP)), def_symbols(read(DEF))
    )
    errors += check_golden(
        syms, lds_nodes, version, golden, layout, soname, mirrors, legacy_prefix
    )

    if args.compat_base:
        errors += check_compat(json.loads(read(args.compat_base)), golden)

    if errors:
        print(
            f"ABI gate FAILED ({len(errors)} problem{'s' if len(errors) != 1 else ''}):"
        )
        for e in errors:
            print(f"  - {e}")
        print("")
        print(
            "See lib/pagespeed/ABI.md for what each rule protects and how "
            "to land the change correctly."
        )
        return 1

    pinned_fields = sum(len(r["fields"]) for r in layout.values())
    print(
        f"ABI gate OK: {len(syms)} exported symbols in {len(set(lds_nodes.values()))} version node(s), "
        f"{len(layout)} structs / {pinned_fields} field offsets, {len(DOTNET_MIRRORS)} .NET mirrors, "
        f"soname {soname}, api {'.'.join(map(str, version))}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
