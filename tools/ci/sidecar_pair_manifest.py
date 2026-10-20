#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""The manifest of the sidecar native pair of a module release (docs/ci/release.md).

  sidecar_pair_manifest.py build  --dir DIR --tag TAG --version V --source-sha SHA --out FILE
  sidecar_pair_manifest.py verify --manifest FILE --dir DIR [--tag TAG] [--version V]
                                  [--source-sha SHA]
  sidecar_pair_manifest.py --self-test

The module release carries the native pair of the ASP.NET Core sidecar, one
tarball per runtime identifier (tools/ci/release_sidecar.sh package):

  pagespeed-sidecar-native-<version>-linux-x64.tar.gz
  pagespeed-sidecar-native-<version>-linux-arm64.tar.gz

`build` reads both tarballs from DIR, checks each against the build-info.json
it carries and against the other, and writes sidecar-pair.json, which the NuGet
lane pins and verifies before it packs the sidecar packages:

  {schema, tag, version, source_sha, nginx_version, glibc_floor, build_image,
   rids: {<rid>: {asset, sha256, size, files: {<name>: <sha256>}}}}

`files` names every regular file of the tarball (by its name inside the
tarball's single top directory) with its sha256. `verify` checks a manifest
against the tarballs in DIR, and against the release identity when given.
Both refuse anything unexpected: another file, a link, a missing binary, a
binary that is not executable, a version, commit or nginx that differs between
the two architectures or from build-info.json.
"""

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import tempfile

RIDS = ("linux-x64", "linux-arm64")
BINARIES = ("nginx", "ngx_pagespeed_module.so", "pagespeed-nginx-launch")
EXECUTABLES = ("nginx", "pagespeed-nginx-launch")
OTHERS = ("build-info.json", "LICENSE", "NOTICE", "THIRD-PARTY-NOTICES")
EXPECTED_FILES = set(BINARIES) | set(OTHERS)
INFO_SHA_FIELDS = {"nginx": "nginx_sha256", "ngx_pagespeed_module.so": "module_sha256",
                   "pagespeed-nginx-launch": "launch_sha256"}
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$")


class Refused(Exception):
    pass


def asset_name(version, rid):
    return "pagespeed-sidecar-native-%s-%s.tar.gz" % (version, rid)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_tarball(path, version, rid):
    """Return ({name: sha256}, build_info) of one tarball, refusing anything unexpected."""
    top = "pagespeed-sidecar-native-%s-%s" % (version, rid)
    files = {}
    info = None
    with tarfile.open(path, "r:gz") as tar:
        for m in tar.getmembers():
            name = m.name.rstrip("/")
            if name == top and m.isdir():
                continue
            if not name.startswith(top + "/") or "/" in name[len(top) + 1:]:
                raise Refused("%s: unexpected path %s" % (os.path.basename(path), m.name))
            base = name[len(top) + 1:]
            if not m.isfile():
                raise Refused("%s: %s is not a regular file" % (os.path.basename(path), base))
            if base not in EXPECTED_FILES:
                raise Refused("%s: unexpected file %s" % (os.path.basename(path), base))
            if base in files:
                raise Refused("%s: %s appears twice" % (os.path.basename(path), base))
            if base in EXECUTABLES and not (m.mode & 0o111):
                raise Refused("%s: %s is not executable" % (os.path.basename(path), base))
            data = tar.extractfile(m).read()
            if not data:
                raise Refused("%s: %s is empty" % (os.path.basename(path), base))
            files[base] = sha256_bytes(data)
            if base == "build-info.json":
                try:
                    info = json.loads(data.decode("utf-8"))
                except ValueError as e:
                    raise Refused("%s: build-info.json is not JSON: %s" % (os.path.basename(path), e))
    missing = EXPECTED_FILES - set(files)
    if missing:
        raise Refused("%s: missing %s" % (os.path.basename(path), ", ".join(sorted(missing))))
    return files, info


def check_info(info, files, rid, version, source_sha):
    where = "build-info.json of " + rid
    if info.get("rid") != rid:
        raise Refused("%s names rid %r" % (where, info.get("rid")))
    for key in ("package_version", "mod_pagespeed_version"):
        if info.get(key) != version:
            raise Refused("%s: %s is %r, not %s" % (where, key, info.get(key), version))
    if source_sha is not None and info.get("source_sha") != source_sha:
        raise Refused("%s: source_sha is %r, not %s" % (where, info.get("source_sha"), source_sha))
    for name, field in INFO_SHA_FIELDS.items():
        if info.get(field) != files[name]:
            raise Refused("%s: %s does not match the %s in the tarball" % (where, field, name))
    for key in ("nginx_version", "glibc_floor", "build_image"):
        if not isinstance(info.get(key), str) or not info.get(key):
            raise Refused("%s: no %s" % (where, key))


def build_manifest(directory, tag, version, source_sha):
    if not VERSION_RE.match(version):
        raise Refused("bad version %r" % version)
    if not re.match(r"^[0-9a-f]{40}$", source_sha):
        raise Refused("bad source sha %r" % source_sha)
    rids = {}
    common = None
    for rid in RIDS:
        name = asset_name(version, rid)
        path = os.path.join(directory, name)
        if not os.path.isfile(path):
            raise Refused("missing " + name)
        files, info = read_tarball(path, version, rid)
        check_info(info, files, rid, version, source_sha)
        mine = {k: info[k] for k in ("nginx_version", "glibc_floor", "build_image")}
        if common is None:
            common = mine
        elif mine != common:
            raise Refused("the architectures disagree: %r vs %r" % (common, mine))
        rids[rid] = {
            "asset": name,
            "sha256": sha256_file(path),
            "size": os.path.getsize(path),
            "files": dict(sorted(files.items())),
        }
    manifest = {"schema": 1, "tag": tag, "version": version, "source_sha": source_sha}
    manifest.update(common)
    manifest["rids"] = rids
    return manifest


def verify_manifest(manifest, directory, tag=None, version=None, source_sha=None):
    if manifest.get("schema") != 1:
        raise Refused("unknown manifest schema")
    for key, want in (("tag", tag), ("version", version), ("source_sha", source_sha)):
        if want is not None and manifest.get(key) != want:
            raise Refused("the manifest names %s %r, not %r" % (key, manifest.get(key), want))
    if set(manifest.get("rids", {})) != set(RIDS):
        raise Refused("the manifest does not name exactly %s" % ", ".join(RIDS))
    rebuilt = build_manifest(directory, manifest.get("tag"), manifest.get("version", ""),
                             manifest.get("source_sha", ""))
    if rebuilt != manifest:
        raise Refused("the manifest does not describe the tarballs in " + directory)


def write_json(obj, path):
    with open(path, "w") as fh:
        json.dump(obj, fh, indent=2, sort_keys=True)
        fh.write("\n")


def self_test():
    fail = 0
    tmp = tempfile.mkdtemp(prefix="sidecarmanifest.")
    sha = "a" * 40
    image = "ubuntu:20.04@sha256:" + "b" * 64

    def make(directory, version, rid, mutate=None, info_mutate=None):
        top = "pagespeed-sidecar-native-%s-%s" % (version, rid)
        content = {n: ("%s %s %s" % (n, rid, version)).encode() for n in BINARIES}
        content.update({"LICENSE": b"license", "NOTICE": b"notice", "THIRD-PARTY-NOTICES": b"third"})
        info = {"schema": 1, "package_version": version, "mod_pagespeed_version": version,
                "rid": rid, "nginx_version": "1.30.5", "glibc_floor": "2.31", "build_image": image,
                "source_sha": sha}
        for n, f in INFO_SHA_FIELDS.items():
            info[f] = sha256_bytes(content[n])
        if info_mutate:
            info_mutate(info)
        content["build-info.json"] = json.dumps(info).encode()
        if mutate:
            mutate(content)
        path = os.path.join(directory, asset_name(version, rid))
        with tarfile.open(path, "w:gz") as tar:
            d = tarfile.TarInfo(top)
            d.type = tarfile.DIRTYPE
            d.mode = 0o755
            tar.addfile(d)
            for n, data in sorted(content.items()):
                ti = tarfile.TarInfo(top + "/" + n)
                ti.size = len(data)
                ti.mode = 0o755 if n in EXECUTABLES else 0o644
                tar.addfile(ti, io.BytesIO(data))
        return path

    def expect_refused(label, fn):
        nonlocal fail
        try:
            fn()
        except Refused:
            return
        print("FAIL: %s: expected a refusal" % label)
        fail = 1

    try:
        good = os.path.join(tmp, "good")
        os.makedirs(good)
        for rid in RIDS:
            make(good, "2.5.1", rid)
        m = build_manifest(good, "module-v2.5.1", "2.5.1", sha)
        if m["nginx_version"] != "1.30.5" or m["glibc_floor"] != "2.31" or m["build_image"] != image:
            print("FAIL: common fields: %r" % m)
            fail = 1
        x64 = m["rids"]["linux-x64"]
        if x64["asset"] != "pagespeed-sidecar-native-2.5.1-linux-x64.tar.gz" \
                or x64["sha256"] != sha256_file(os.path.join(good, x64["asset"])) \
                or set(x64["files"]) != EXPECTED_FILES:
            print("FAIL: x64 entry: %r" % x64)
            fail = 1
        verify_manifest(m, good, tag="module-v2.5.1", version="2.5.1", source_sha=sha)
        out = os.path.join(tmp, "m.json")
        write_json(m, out)
        with open(out) as fh:
            verify_manifest(json.load(fh), good)

        expect_refused("other tag", lambda: verify_manifest(m, good, tag="module-v2.5.2"))
        expect_refused("other commit", lambda: verify_manifest(m, good, source_sha="c" * 40))
        tampered = json.loads(json.dumps(m))
        tampered["rids"]["linux-arm64"]["sha256"] = "0" * 64
        expect_refused("tampered digest", lambda: verify_manifest(tampered, good))
        dropped = json.loads(json.dumps(m))
        del dropped["rids"]["linux-arm64"]
        expect_refused("one rid", lambda: verify_manifest(dropped, good))

        def case(label, version="2.5.1", mutate=None, info_mutate=None, rid="linux-arm64"):
            d = os.path.join(tmp, label.replace(" ", "-"))
            os.makedirs(d)
            for r in RIDS:
                if r == rid:
                    make(d, version, r, mutate, info_mutate)
                else:
                    make(d, version, r)
            expect_refused(label, lambda: build_manifest(d, "module-v" + version, version, sha))

        case("extra file", mutate=lambda c: c.update({"extra": b"x"}))
        case("missing binary", mutate=lambda c: c.pop("pagespeed-nginx-launch"))
        case("empty binary", mutate=lambda c: c.update({"nginx": b""}))
        case("binary differs from build-info", mutate=lambda c: c.update({"nginx": b"other"}))
        case("other nginx on one arch", info_mutate=lambda i: i.update({"nginx_version": "1.30.4"}))
        case("other version in build-info", info_mutate=lambda i: i.update({"package_version": "2.5.0"}))
        case("other commit in build-info", info_mutate=lambda i: i.update({"source_sha": "d" * 40}))
        case("wrong rid in build-info", info_mutate=lambda i: i.update({"rid": "linux-x64"}))

        # a link, and a binary that is not executable
        d = os.path.join(tmp, "link")
        os.makedirs(d)
        make(d, "2.5.1", "linux-x64")
        p = make(d, "2.5.1", "linux-arm64")
        with tarfile.open(p, "r:gz") as src:
            members = [(m_, src.extractfile(m_).read() if m_.isfile() else None) for m_ in src.getmembers()]
        with tarfile.open(p, "w:gz") as tar:
            for m_, data in members:
                tar.addfile(m_, io.BytesIO(data) if data is not None else None)
            ln = tarfile.TarInfo("pagespeed-sidecar-native-2.5.1-linux-arm64/LICENSE2")
            ln.type = tarfile.SYMTYPE
            ln.linkname = "LICENSE"
            tar.addfile(ln)
        expect_refused("symlink", lambda: build_manifest(d, "module-v2.5.1", "2.5.1", sha))
        with tarfile.open(p, "w:gz") as tar:
            for m_, data in members:
                if m_.name.endswith("/nginx"):
                    m_.mode = 0o644
                tar.addfile(m_, io.BytesIO(data) if data is not None else None)
        expect_refused("not executable", lambda: build_manifest(d, "module-v2.5.1", "2.5.1", sha))

        expect_refused("bad version", lambda: build_manifest(good, "module-v2.5", "2.5", sha))
        expect_refused("bad sha", lambda: build_manifest(good, "module-v2.5.1", "2.5.1", "xyz"))
        empty = os.path.join(tmp, "empty")
        os.makedirs(empty)
        expect_refused("no tarballs", lambda: build_manifest(empty, "module-v2.5.1", "2.5.1", sha))
        # a release candidate
        rc = os.path.join(tmp, "rc")
        os.makedirs(rc)
        for rid in RIDS:
            make(rc, "2.6.0-rc.1", rid)
        build_manifest(rc, "module-v2.6.0-rc.1", "2.6.0-rc.1", sha)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if fail == 0:
        print("sidecar_pair_manifest self-test: ok")
    return fail


def main(argv):
    if len(argv) >= 2 and argv[1] == "--self-test":
        return self_test()
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--dir", required=True)
    b.add_argument("--tag", required=True)
    b.add_argument("--version", required=True)
    b.add_argument("--source-sha", required=True)
    b.add_argument("--out", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--dir", required=True)
    v.add_argument("--tag")
    v.add_argument("--version")
    v.add_argument("--source-sha")
    args = p.parse_args(argv[1:])
    try:
        if args.cmd == "build":
            manifest = build_manifest(args.dir, args.tag, args.version, args.source_sha)
            write_json(manifest, args.out)
            print("sidecar_pair_manifest: wrote %s" % args.out)
            for rid in RIDS:
                e = manifest["rids"][rid]
                print("  %s  %s" % (e["sha256"], e["asset"]))
        else:
            with open(args.manifest) as fh:
                manifest = json.load(fh)
            verify_manifest(manifest, args.dir, args.tag, args.version, args.source_sha)
            print("sidecar_pair_manifest: ok")
    except Refused as e:
        print("::error::sidecar_pair_manifest: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
