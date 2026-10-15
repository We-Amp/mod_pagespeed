#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Assemble and verify the asset set of a module release (docs/ci/release.md).

  release_manifest.py build  --stage DIR --out DIR --tag TAG --version V
                             --pkg-version P --source-sha SHA --run-id ID
                             [--repository R] [--optimizer-tag T]
                             [--prerelease true|false] [--profile part-a|part-b|none]
  release_manifest.py verify DIR
  release_manifest.py --self-test

`build` reads the run artifacts that the build jobs uploaded. STAGE holds one
directory per artifact (the directory name is the artifact name, for example
`asset-nginx-x64`); every regular file below such a directory is one release
file. Directories whose name is not in ARTIFACTS are refused, so a new
artifact cannot slip into a release unclassified.

`build` writes to OUT: every file under its uploaded name (GitHub replaces a
'~' in an asset name with '.', so the file is renamed that way here, before
upload), `manifest.json` and `SHA256SUMS`. SHA256SUMS lists every file of OUT
except itself, manifest.json included, in C-locale order, with the uploaded
names, so that `sha256sum -c SHA256SUMS` works on the downloaded assets as
they are.

manifest.json (schema 1):
  {schema, tag, version, pkg_version, prerelease, source_sha, run_id,
   repository, optimizer_tag, files: [{asset, name, sha256, size, kind,
   component, channel, channel_also, distro, arch}]}
  asset         the uploaded name (what SHA256SUMS lists)
  name          the original name (what the packaging produced, '~' kept)
  kind          deb | rpm | tarball | so | zip | msi | json
  component     module | optimizer | source | sbom
  channel       routing hint for the consumer: apt | rpm-el9 | rpm-el10 |
                rpm-ea4 | none (a direct download only)
  channel_also  further channels the same file belongs to (the optimizer
                RPMs also belong in rpm-el10)
  distro        suite or release id: a deb suite (noble, bookworm, trixie,
                jammy), el8 | el9 | el10, or "any"
  arch          x64 | arm64 | any

`--profile part-b` (the default) also checks that the set is complete,
including the Windows installer; `part-a` is the same without the installer
(the set of a lane that builds none); `none` skips the check (self-test, local
use).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile

ARTIFACTS = {
    # artifact name: (component, default channel, distro, also-channels)
    "asset-source": ("source", "none", "any", ()),
    "asset-linux-x64": ("module", None, "el9", ()),
    "asset-linux-arm64": ("module", None, "el9", ()),
    "asset-nginx-x64": ("module", None, "any", ()),
    "asset-nginx-arm64": ("module", None, "any", ()),
    "asset-nginx-el10": ("module", "rpm-el10", "el10", ()),
    "asset-apache-el10": ("module", "rpm-el10", "el10", ()),
    "asset-ea4-el9": ("module", "rpm-ea4", "el9", ()),
    "asset-ea4-el8": ("module", "rpm-ea4", "el8", ()),
    "asset-ea4-el10": ("module", "rpm-ea4", "el10", ()),
    "asset-optimizer": ("optimizer", None, "any", ()),
    "asset-sbom": ("sbom", "none", "any", ()),
    "asset-windows": ("module", "none", "any", ()),
}

# The stock Apache RPM of el10 has no dist tag in its name, so it has the same
# name as the el9 one. Its uploaded name carries this prefix.
UNIQUE_PREFIX = {"asset-apache-el10": "el10-"}

SUITES = ("noble", "bookworm", "trixie", "jammy")
ASSET_CHARS = re.compile(r"^[A-Za-z0-9._+-]+$")


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def kind_of(name):
    if name.endswith(".deb"):
        return "deb"
    if name.endswith(".rpm"):
        return "rpm"
    if name.endswith(".tar.gz"):
        return "tarball"
    if name.endswith(".zip"):
        return "zip"
    if name.endswith(".msi"):
        return "msi"
    if name.endswith(".so"):
        return "so"
    if name.endswith(".json"):
        return "json"
    raise ValueError("unsupported file type: " + name)


def arch_of(name):
    if re.search(r"(amd64|x86_64|x64)", name):
        return "x64"
    if re.search(r"(arm64|aarch64)", name):
        return "arm64"
    return "any"


def distro_of(name, default):
    m = re.search(r"~([a-z]+)_[a-z0-9]+\.deb$", name)
    if m:
        return m.group(1)
    m = re.search(r"\.(el[0-9]+)[._]", name)
    if m:
        return m.group(1)
    if name.endswith(".deb"):
        return "any"
    return default


def channel_of(artifact, name, kind, default_channel):
    if kind == "deb":
        return "apt"
    if kind == "rpm":
        if default_channel:
            return default_channel
        return "rpm-el9"
    return default_channel or "none"


def collect(stage):
    """Return [(artifact, path, original_name)] for every file under stage."""
    items = []
    for artifact in sorted(os.listdir(stage)):
        base = os.path.join(stage, artifact)
        if not os.path.isdir(base):
            raise SystemExit("release_manifest: unexpected file in the stage: " + artifact)
        if artifact not in ARTIFACTS:
            raise SystemExit("release_manifest: unknown artifact directory: " + artifact)
        for root, _dirs, files in os.walk(base):
            for f in sorted(files):
                items.append((artifact, os.path.join(root, f), f))
    return items


def sanitize(name):
    new = name.replace("~", ".")
    if not ASSET_CHARS.match(new):
        raise SystemExit("release_manifest: file name outside [A-Za-z0-9._+-]: " + name)
    return new


def required(pkg_version, prerelease, msi=False):
    """(description, artifact, regex over the original name) for a complete set."""
    v = re.escape(pkg_version)
    req = []
    for arch, deb_arch, rpm_arch, tar_arch, so_arch, art in (
        ("x64", "amd64", "x86_64", "x86_64", "x86_64", "x64"),
        ("arm64", "arm64", "aarch64", "aarch64", "aarch64", "arm64"),
    ):
        req.append(("apache deb " + arch, "asset-linux-" + art,
                    r"^mod-pagespeed_%s-[^_]+_%s\.deb$" % (v, deb_arch)))
        req.append(("apache rpm " + arch, "asset-linux-" + art,
                    r"^mod-pagespeed-%s-[^.]+\.%s\.rpm$" % (v, rpm_arch)))
        req.append(("apache module " + arch, "asset-linux-" + art,
                    r"^libmod_pagespeed-linux-%s\.so$" % so_arch))
        req.append(("nginx tarball " + arch, "asset-linux-" + art,
                    r"^ngx_pagespeed-.*-linux-%s\.tar\.gz$" % tar_arch))
        for suite in SUITES:
            req.append(("nginx deb %s %s" % (suite, arch), "asset-nginx-" + art,
                        r"^nginx-module-pagespeed_%s-[^_]*~%s_%s\.deb$" % (v, suite, deb_arch)))
        req.append(("nginx rpm el9 " + arch, "asset-nginx-" + art,
                    r"^nginx-module-pagespeed-%s-[^.]+\.el9\.%s\.rpm$" % (v, rpm_arch)))
    req.append(("apache rpm el10", "asset-apache-el10",
                r"^mod-pagespeed-%s-[^.]+\.x86_64\.rpm$" % v))
    req.append(("nginx rpm el10", "asset-nginx-el10",
                r"^nginx-module-pagespeed-%s-[^.]+\.el10\.x86_64\.rpm$" % v))
    if not prerelease:
        # The cPanel packages are built against the spec's version; a
        # pre-release tag does not match it and builds none.
        for el in ("el9", "el8"):
            req.append(("ea4 rpm " + el, "asset-ea4-" + el,
                        r"^ea-apache24-mod_pagespeed-.*\.%s\..*\.rpm$" % el))
    req.append(("optimizer deb amd64", "asset-optimizer", r"^pagespeed-optimizer_.*_amd64\.deb$"))
    req.append(("optimizer deb arm64", "asset-optimizer", r"^pagespeed-optimizer_.*_arm64\.deb$"))
    req.append(("optimizer rpm x86_64", "asset-optimizer", r"^pagespeed-optimizer-.*\.x86_64\.rpm$"))
    req.append(("optimizer rpm aarch64", "asset-optimizer", r"^pagespeed-optimizer-.*\.aarch64\.rpm$"))
    req.append(("source tarball", "asset-source", r"^mod_pagespeed-.*\.tar\.gz$"))
    req.append(("sbom", "asset-sbom", r"\.spdx\.json$"))
    req.append(("vex", "asset-sbom", r"\.vex\.json$"))
    if msi:
        # The installer is named after the display version of the stamped
        # tree: the package version with a '-' where dpkg has a '~'.
        req.append(("windows installer", "asset-windows",
                    r"^pagespeed-iis-%s-win-x64\.msi$" % re.escape(pkg_version.replace("~", "-"))))
    return req


def optional(prerelease):
    """(description, artifact, regex) of files that may be present."""
    opt = [("optimizer windows zip", "asset-optimizer", r"^pagespeed-optimizer-.*-win-x64\.zip$")]
    if not prerelease:
        opt.append(("ea4 rpm el10", "asset-ea4-el10",
                    r"^ea-apache24-mod_pagespeed-.*\.el10\..*\.rpm$"))
    return opt


def build(args):
    stage = os.path.abspath(args.stage)
    out = os.path.abspath(args.out)
    prerelease = args.prerelease == "true"
    if os.path.exists(out) and os.listdir(out):
        raise SystemExit("release_manifest: output directory is not empty: " + out)
    os.makedirs(out, exist_ok=True)

    items = collect(stage)
    if not items:
        raise SystemExit("release_manifest: no files in the stage")

    files = []
    seen = {}
    for artifact, path, name in items:
        if os.path.getsize(path) == 0:
            raise SystemExit("release_manifest: empty file: %s/%s" % (artifact, name))
        component, default_channel, default_distro, _also = ARTIFACTS[artifact]
        if name == "SHA256SUMS" or name == "manifest.json" or name.endswith(".asc"):
            raise SystemExit("release_manifest: reserved or foreign file in the stage: %s/%s" % (artifact, name))
        kind = kind_of(name)
        asset = sanitize(UNIQUE_PREFIX.get(artifact, "") + name)
        if asset in seen:
            raise SystemExit("release_manifest: two files map to the asset name %s (%s and %s)"
                             % (asset, seen[asset], artifact))
        seen[asset] = artifact
        channel = channel_of(artifact, name, kind, default_channel)
        also = []
        if component == "optimizer" and kind == "rpm":
            also = ["rpm-el10"]
        shutil.copyfile(path, os.path.join(out, asset))
        files.append({
            "asset": asset,
            "name": name,
            "sha256": sha256_of(os.path.join(out, asset)),
            "size": os.path.getsize(os.path.join(out, asset)),
            "kind": kind,
            "component": component,
            "channel": channel,
            "channel_also": also,
            "distro": distro_of(name, default_distro),
            "arch": arch_of(name),
        })
    files.sort(key=lambda f: f["asset"].encode())

    if args.profile in ("part-a", "part-b"):
        msi = args.profile == "part-b"
        missing = []
        if prerelease:
            for a, _p, n in items:
                if a.startswith("asset-ea4-"):
                    missing.append("cPanel package on a pre-release: %s/%s" % (a, n))
        rules = required(args.pkg_version, prerelease, msi) + optional(prerelease)
        for a, _p, n in items:
            if not any(a == art and re.search(rx, n) for _d, art, rx in rules):
                missing.append("unexpected file claimed by no rule: %s/%s" % (a, n))
        for desc, artifact, rx in required(args.pkg_version, prerelease, msi):
            hits = [f for f in items if f[0] == artifact and re.search(rx, f[2])]
            if len(hits) == 0:
                missing.append("%s (%s, /%s/)" % (desc, artifact, rx))
            elif len(hits) > 1:
                missing.append("%s matches %d files (%s)" % (desc, len(hits), ", ".join(h[2] for h in hits)))
        if missing:
            for m in missing:
                print("::error::release_manifest: incomplete or ambiguous asset set: " + m, file=sys.stderr)
            raise SystemExit(1)

    manifest = {
        "schema": 1,
        "tag": args.tag,
        "version": args.version,
        "pkg_version": args.pkg_version,
        "prerelease": prerelease,
        "source_sha": args.source_sha,
        "run_id": args.run_id,
        "repository": args.repository,
        "optimizer_tag": args.optimizer_tag,
        "files": files,
    }
    with open(os.path.join(out, "manifest.json"), "w") as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write("\n")
    write_sums(out)
    verify(out)
    print("release_manifest: %d files + manifest.json + SHA256SUMS in %s" % (len(files), out))


def write_sums(out):
    names = sorted((n for n in os.listdir(out) if n != "SHA256SUMS"), key=lambda n: n.encode())
    lines = ["%s  %s\n" % (sha256_of(os.path.join(out, n)), n) for n in names]
    with open(os.path.join(out, "SHA256SUMS"), "w") as fh:
        fh.writelines(lines)


def verify(out):
    out = os.path.abspath(out)
    sums_path = os.path.join(out, "SHA256SUMS")
    if not os.path.isfile(sums_path):
        raise SystemExit("release_manifest: no SHA256SUMS in " + out)
    listed = {}
    with open(sums_path) as fh:
        for line in fh:
            m = re.match(r"^([0-9a-f]{64})  (.+)\n$", line)
            if not m:
                raise SystemExit("release_manifest: malformed SHA256SUMS line: " + line.rstrip())
            if m.group(2) in listed:
                raise SystemExit("release_manifest: duplicate SHA256SUMS row: " + m.group(2))
            listed[m.group(2)] = m.group(1)
    present = {n for n in os.listdir(out) if n != "SHA256SUMS"}
    if present != set(listed):
        raise SystemExit("release_manifest: SHA256SUMS and the directory disagree: only listed %s, only present %s"
                         % (sorted(set(listed) - present), sorted(present - set(listed))))
    for n, digest in listed.items():
        if sha256_of(os.path.join(out, n)) != digest:
            raise SystemExit("release_manifest: checksum mismatch: " + n)
    with open(os.path.join(out, "manifest.json")) as fh:
        manifest = json.load(fh)
    if manifest.get("schema") != 1:
        raise SystemExit("release_manifest: unknown manifest schema")
    assets = set()
    for f in manifest["files"]:
        if f["asset"] in assets:
            raise SystemExit("release_manifest: duplicate asset in the manifest: " + f["asset"])
        assets.add(f["asset"])
        if f["asset"] != sanitize(f["asset"]):
            raise SystemExit("release_manifest: asset name not in GitHub spelling: " + f["asset"])
        if listed.get(f["asset"]) != f["sha256"]:
            raise SystemExit("release_manifest: manifest digest differs from SHA256SUMS: " + f["asset"])
        if os.path.getsize(os.path.join(out, f["asset"])) != f["size"]:
            raise SystemExit("release_manifest: manifest size differs: " + f["asset"])
    if assets | {"manifest.json"} != present:
        raise SystemExit("release_manifest: the manifest does not list exactly the files present")


def self_test():
    fail = 0
    tmp = tempfile.mkdtemp(prefix="relmanifest.")

    def expect_exit(label, fn):
        nonlocal fail
        try:
            fn()
        except SystemExit as e:
            if e.code in (None, 0):
                print("FAIL: %s: exited 0" % label)
                fail = 1
            return
        print("FAIL: %s: expected a refusal" % label)
        fail = 1

    def mk(stage, artifact, name, body=b"x"):
        d = os.path.join(stage, artifact)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as fh:
            fh.write(body)

    def ns(stage, out, profile="none", pre="false", pkg="1.19.0"):
        return argparse.Namespace(stage=stage, out=out, tag="module-v1.19.0", version="1.19.0",
                                  pkg_version=pkg, source_sha="a" * 40, run_id="1", repository="o/r",
                                  optimizer_tag="optimizer-v1.19.0", prerelease=pre, profile=profile)

    try:
        # 1. mapping, collisions and the contract of the output directory
        st = os.path.join(tmp, "s1")
        mk(st, "asset-nginx-x64", "nginx-module-pagespeed_1.19.0-r1~bookworm_amd64.deb", b"deb")
        mk(st, "asset-linux-x64", "mod-pagespeed-1.19.0-1.x86_64.rpm", b"el9 stock")
        mk(st, "asset-apache-el10", "mod-pagespeed-1.19.0-1.x86_64.rpm", b"el10 stock")
        mk(st, "asset-optimizer", "pagespeed-optimizer-1.19.0-1.x86_64.rpm", b"opt")
        mk(st, "asset-sbom", "pagespeed-1.1-1.19.0.spdx.json", b"{}")
        out = os.path.join(tmp, "o1")
        build(ns(st, out))
        names = sorted(os.listdir(out))
        want = ["SHA256SUMS", "el10-mod-pagespeed-1.19.0-1.x86_64.rpm", "manifest.json",
                "mod-pagespeed-1.19.0-1.x86_64.rpm",
                "nginx-module-pagespeed_1.19.0-r1.bookworm_amd64.deb",
                "pagespeed-1.1-1.19.0.spdx.json", "pagespeed-optimizer-1.19.0-1.x86_64.rpm"]
        if names != want:
            print("FAIL: asset names: %r" % names)
            fail = 1
        with open(os.path.join(out, "manifest.json")) as fh:
            man = json.load(fh)
        by = {f["asset"]: f for f in man["files"]}
        deb = by["nginx-module-pagespeed_1.19.0-r1.bookworm_amd64.deb"]
        if deb["name"] != "nginx-module-pagespeed_1.19.0-r1~bookworm_amd64.deb" or deb["distro"] != "bookworm" \
                or deb["channel"] != "apt" or deb["kind"] != "deb" or deb["arch"] != "x64":
            print("FAIL: deb entry: %r" % deb)
            fail = 1
        e10 = by["el10-mod-pagespeed-1.19.0-1.x86_64.rpm"]
        if e10["channel"] != "rpm-el10" or e10["name"] != "mod-pagespeed-1.19.0-1.x86_64.rpm" or e10["distro"] != "el10":
            print("FAIL: el10 entry: %r" % e10)
            fail = 1
        if by["mod-pagespeed-1.19.0-1.x86_64.rpm"]["channel"] != "rpm-el9":
            print("FAIL: el9 channel")
            fail = 1
        opt = by["pagespeed-optimizer-1.19.0-1.x86_64.rpm"]
        if opt["component"] != "optimizer" or opt["channel_also"] != ["rpm-el10"]:
            print("FAIL: optimizer entry: %r" % opt)
            fail = 1
        sums = open(os.path.join(out, "SHA256SUMS")).read().splitlines()
        if len(sums) != len(want) - 1 or any("~" in s for s in sums):
            print("FAIL: SHA256SUMS rows: %r" % sums)
            fail = 1
        if not any(s.endswith("  manifest.json") for s in sums):
            print("FAIL: manifest.json is not in SHA256SUMS")
            fail = 1
        verify(out)

        # 2. tamper detection
        with open(os.path.join(out, "mod-pagespeed-1.19.0-1.x86_64.rpm"), "ab") as fh:
            fh.write(b"!")
        expect_exit("tampered asset", lambda: verify(out))

        # 3. a non-empty output directory is refused
        expect_exit("non-empty output", lambda: build(ns(st, out)))

        # 4. collisions: same name from two artifacts without a unique prefix
        st2 = os.path.join(tmp, "s2")
        mk(st2, "asset-linux-x64", "same_1.0_amd64.deb")
        mk(st2, "asset-linux-arm64", "same_1.0_amd64.deb")
        expect_exit("collision", lambda: build(ns(st2, os.path.join(tmp, "o2"))))

        # 5. names GitHub would rewrite are refused
        st3 = os.path.join(tmp, "s3")
        mk(st3, "asset-linux-x64", "bad name.deb")
        expect_exit("odd name", lambda: build(ns(st3, os.path.join(tmp, "o3"))))

        # 6. unknown artifact, empty file, signature files
        st4 = os.path.join(tmp, "s4")
        mk(st4, "module-mystery", "a.deb")
        expect_exit("unknown artifact", lambda: build(ns(st4, os.path.join(tmp, "o4"))))
        st5 = os.path.join(tmp, "s5")
        mk(st5, "asset-linux-x64", "a.deb", b"")
        expect_exit("empty file", lambda: build(ns(st5, os.path.join(tmp, "o5"))))
        st6 = os.path.join(tmp, "s6")
        mk(st6, "asset-linux-x64", "a.deb.asc")
        expect_exit("signature in the stage", lambda: build(ns(st6, os.path.join(tmp, "o6"))))

        # 7. completeness: the part-a profile refuses an incomplete set and
        # accepts a complete one
        expect_exit("incomplete set", lambda: build(ns(st, os.path.join(tmp, "o7"), profile="part-a")))
        st8 = os.path.join(tmp, "s8")
        v = "1.19.0"
        for art, deb_arch, rpm_arch, tar_arch in (("x64", "amd64", "x86_64", "x86_64"),
                                                  ("arm64", "arm64", "aarch64", "aarch64")):
            mk(st8, "asset-linux-" + art, "mod-pagespeed_%s-1_%s.deb" % (v, deb_arch))
            mk(st8, "asset-linux-" + art, "mod-pagespeed-%s-1.%s.rpm" % (v, rpm_arch))
            mk(st8, "asset-linux-" + art, "libmod_pagespeed-linux-%s.so" % tar_arch)
            mk(st8, "asset-linux-" + art, "ngx_pagespeed-%s-linux-%s.tar.gz" % (v, tar_arch))
            for suite in SUITES:
                mk(st8, "asset-nginx-" + art, "nginx-module-pagespeed_%s-r1~%s_%s.deb" % (v, suite, deb_arch))
            mk(st8, "asset-nginx-" + art, "nginx-module-pagespeed-%s-1.el9.%s.rpm" % (v, rpm_arch))
        mk(st8, "asset-apache-el10", "mod-pagespeed-%s-1.x86_64.rpm" % v)
        mk(st8, "asset-nginx-el10", "nginx-module-pagespeed-%s-1.el10.x86_64.rpm" % v)
        mk(st8, "asset-ea4-el9", "ea-apache24-mod_pagespeed-%s-1.el9.cpanel.x86_64.rpm" % v)
        mk(st8, "asset-ea4-el8", "ea-apache24-mod_pagespeed-%s-1.el8.cpanel.x86_64.rpm" % v)
        mk(st8, "asset-optimizer", "pagespeed-optimizer_%s_amd64.deb" % v)
        mk(st8, "asset-optimizer", "pagespeed-optimizer_%s_arm64.deb" % v)
        mk(st8, "asset-optimizer", "pagespeed-optimizer-%s-1.x86_64.rpm" % v)
        mk(st8, "asset-optimizer", "pagespeed-optimizer-%s-1.aarch64.rpm" % v)
        mk(st8, "asset-source", "mod_pagespeed-%s.tar.gz" % v)
        mk(st8, "asset-sbom", "pagespeed-1.1-%s.spdx.json" % v)
        mk(st8, "asset-sbom", "pagespeed-1.1-%s.vex.json" % v)
        out8 = os.path.join(tmp, "o8")
        build(ns(st8, out8, profile="part-a"))
        # a pre-release tag needs no cPanel packages
        shutil.rmtree(os.path.join(st8, "asset-ea4-el9"))
        shutil.rmtree(os.path.join(st8, "asset-ea4-el8"))
        build(ns(st8, os.path.join(tmp, "o9"), profile="part-a", pre="true"))
        expect_exit("missing cpanel package on a release",
                    lambda: build(ns(st8, os.path.join(tmp, "o10"), profile="part-a")))
        # an ambiguous match (two apache debs) is refused
        mk(st8, "asset-linux-x64", "mod-pagespeed_%s-2_amd64.deb" % v)
        expect_exit("ambiguous", lambda: build(ns(st8, os.path.join(tmp, "o11"), profile="part-a", pre="true")))
        os.remove(os.path.join(st8, "asset-linux-x64", "mod-pagespeed_%s-2_amd64.deb" % v))
        # optional files are accepted
        mk(st8, "asset-optimizer", "pagespeed-optimizer-%s-win-x64.zip" % v)
        build(ns(st8, os.path.join(tmp, "o12"), profile="part-a", pre="true"))
        # the part-b profile also needs the installer, named for the version
        expect_exit("missing installer", lambda: build(ns(st8, os.path.join(tmp, "o12b"), profile="part-b", pre="true")))
        mk(st8, "asset-windows", "pagespeed-iis-1.19.0-win-x64.msi", b"msi")
        out12c = os.path.join(tmp, "o12c")
        build(ns(st8, out12c, profile="part-b", pre="true"))
        with open(os.path.join(out12c, "manifest.json")) as fh:
            msi_entry = [f for f in json.load(fh)["files"] if f["kind"] == "msi"]
        if len(msi_entry) != 1 or msi_entry[0]["channel"] != "none" or msi_entry[0]["arch"] != "x64" \
                or msi_entry[0]["component"] != "module":
            print("FAIL: installer entry: %r" % msi_entry)
            fail = 1
        # the part-a profile does not know an installer
        expect_exit("installer under part-a", lambda: build(ns(st8, os.path.join(tmp, "o12d"), profile="part-a", pre="true")))
        # a wrongly named installer is refused
        os.rename(os.path.join(st8, "asset-windows", "pagespeed-iis-1.19.0-win-x64.msi"),
                  os.path.join(st8, "asset-windows", "pagespeed-iis-1.18.0-win-x64.msi"))
        expect_exit("installer of another version", lambda: build(ns(st8, os.path.join(tmp, "o12e"), profile="part-b", pre="true")))
        # a pre-release version is spelled with a hyphen in the installer name
        rc = [r for r in required("1.19.0~rc.1", True, True) if r[0] == "windows installer"]
        if len(rc) != 1 or not re.search(rc[0][2], "pagespeed-iis-1.19.0-rc.1-win-x64.msi") \
                or re.search(rc[0][2], "pagespeed-iis-1.19.0-win-x64.msi"):
            print("FAIL: installer rule of a pre-release: %r" % rc)
            fail = 1
        shutil.rmtree(os.path.join(st8, "asset-windows"))
        # an extra deb no rule claims
        mk(st8, "asset-linux-x64", "extra_1.0_amd64.deb")
        expect_exit("extra deb", lambda: build(ns(st8, os.path.join(tmp, "o13"), profile="part-a", pre="true")))
        os.remove(os.path.join(st8, "asset-linux-x64", "extra_1.0_amd64.deb"))
        # a stray rpm in the cPanel el9 artifact (release)
        mk(st8, "asset-ea4-el9", "ea-apache24-mod_pagespeed-%s-1.el9.cpanel.x86_64.rpm" % v)
        mk(st8, "asset-ea4-el8", "ea-apache24-mod_pagespeed-%s-1.el8.cpanel.x86_64.rpm" % v)
        build(ns(st8, os.path.join(tmp, "o14"), profile="part-a"))
        mk(st8, "asset-ea4-el9", "stray-1.0-1.x86_64.rpm")
        expect_exit("stray rpm in ea4-el9", lambda: build(ns(st8, os.path.join(tmp, "o15"), profile="part-a")))
        os.remove(os.path.join(st8, "asset-ea4-el9", "stray-1.0-1.x86_64.rpm"))
        # cPanel rpms on a pre-release
        expect_exit("cpanel rpms on a pre-release", lambda: build(ns(st8, os.path.join(tmp, "o16"), profile="part-a", pre="true")))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if fail == 0:
        print("release_manifest self-test: ok")
    return fail


def main(argv):
    if len(argv) >= 2 and argv[1] == "--self-test":
        return self_test()
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--stage", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--tag", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--pkg-version", required=True)
    b.add_argument("--source-sha", required=True)
    b.add_argument("--run-id", required=True)
    b.add_argument("--repository", default="")
    b.add_argument("--optimizer-tag", default="")
    b.add_argument("--prerelease", choices=("true", "false"), default="false")
    b.add_argument("--profile", choices=("part-a", "part-b", "none"), default="part-b")
    v = sub.add_parser("verify")
    v.add_argument("dir")
    args = p.parse_args(argv[1:])
    if args.cmd == "build":
        build(args)
    else:
        verify(args.dir)
        print("release_manifest: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
