#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Candidate seal of the module release lane (docs/ci/release.md).

A candidate run (a dry run of a release tag that does not exist yet) ends with
`seal`: it lists the artifacts of its own run through the Actions API, keeps
the ones on an explicit allowlist, downloads each by id, checks the zip against
the digest the API reports, hashes every file in it and writes
`candidate-seal.json`.

The seal records file hashes. Every value that says what the run is comes from
the run's own API record (display_title, head_sha, event, head_branch), and
the seal refuses to be written when those disagree with the values the
version job derived.

    release_adopt.py seal --repo OWNER/REPO --run-id N --tag module-vX.Y.Z \\
        --version X.Y.Z --sha <40 hex> --out candidate-seal.json [--prerelease]
    release_adopt.py --self-test

`seal` needs `gh` and a token with `actions: read` in GH_TOKEN.
"""

import argparse
import fnmatch
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

SCHEMA = 2

# Artifacts that are sealed: a candidate run must hold every one of them.
SEALED = (
    "asset-source",
    "asset-sbom",
    "asset-optimizer",
    "asset-linux-x64",
    "asset-linux-arm64",
    "asset-nginx-x64",
    "asset-nginx-arm64",
    "asset-nginx-el10",
    "asset-apache-el10",
    "asset-ea4-el8",
    "asset-ea4-el9",
    "asset-ea4-el10",
    "asset-sidecar-x64",
    "asset-sidecar-arm64",
    "unsigned-windows",
    "msi-old",
    "stock-el8",
    "testdata-oldpkgs-x64",
    "testdata-oldpkgs-arm64",
    "release-assets",
)

# The cPanel packages are not built for a pre-release version (the spec
# carries the release version only), so a release candidate has none.
SEALED_OPTIONAL_ON_PRERELEASE = (
    "asset-ea4-el8",
    "asset-ea4-el9",
    "asset-ea4-el10",
)

# Artifacts a candidate run may hold that are not sealed. Anything that is in
# neither list fails the seal, so a new upload is never silently left out.
NOT_SEALED_EXACT = (
    "tree",
    "tree-sidecar",
    "msi-unsigned",
    "asset-windows",
    "candidate-seal",
)
NOT_SEALED_GLOBS = (
    "nginx-x64-*",
    "nginx-arm64-*",
    "debug-*",
    "testdata-sidecar-results-*",
)

SHA_RE = re.compile(r"^[0-9a-f]{40}$")
DIGEST_RE = re.compile(r"^sha256:([0-9a-f]{64})$")


class SealError(Exception):
    pass


def expected_title(tag, sha):
    """The run-name release.yml gives a candidate dispatch."""
    return "Release %s candidate dry run @ %s" % (tag, sha)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def classify(names, prerelease):
    """Return (sealed names present, problems) for the artifact names of a run."""
    problems = []
    seen = set()
    for n in names:
        if n in seen:
            problems.append("artifact name '%s' appears more than once" % n)
        seen.add(n)
    required = [n for n in SEALED if not (prerelease and n in SEALED_OPTIONAL_ON_PRERELEASE)]
    for n in required:
        if n not in seen:
            problems.append("allowlisted artifact '%s' is missing" % n)
    for n in sorted(seen):
        if n in SEALED or n in NOT_SEALED_EXACT:
            continue
        if any(fnmatch.fnmatchcase(n, g) for g in NOT_SEALED_GLOBS):
            continue
        problems.append("unexpected artifact '%s' (neither sealed nor on the not-sealed list)" % n)
    return [n for n in SEALED if n in seen], problems


def check_run_record(run, tag, sha):
    """The run's own API record must say what the version job derived."""
    problems = []
    want = expected_title(tag, sha)
    if run.get("display_title") != want:
        problems.append("display_title is %r, expected %r" % (run.get("display_title"), want))
    if run.get("head_sha") != sha:
        problems.append("head_sha is %r, expected %r" % (run.get("head_sha"), sha))
    if run.get("event") != "workflow_dispatch":
        problems.append("event is %r, expected 'workflow_dispatch'" % (run.get("event"),))
    if run.get("head_branch") != "master":
        problems.append("head_branch is %r, expected 'master'" % (run.get("head_branch"),))
    return problems


def hash_zip(path):
    """[{path, size, sha256}] for every file in the zip, sorted by path."""
    files = []
    with zipfile.ZipFile(path) as zf:
        for info in zf.infolist():
            if info.is_dir():
                continue
            name = info.filename
            parts = name.split("/")
            if name.startswith("/") or "\\" in name or ".." in parts or "" in parts:
                raise SealError("unsafe path '%s' in an artifact" % name)
            h = hashlib.sha256()
            size = 0
            with zf.open(info) as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
                    size += len(chunk)
            files.append({"path": name, "size": size, "sha256": h.hexdigest()})
    names = [f["path"] for f in files]
    if len(set(names)) != len(names):
        raise SealError("duplicate file paths in an artifact")
    return sorted(files, key=lambda f: f["path"])


def seal(api, repo, run_id, tag, version, sha, prerelease, workdir):
    if not SHA_RE.match(sha):
        raise SealError("sha '%s' is not 40 lowercase hex digits" % sha)
    run = api.run(repo, run_id)
    problems = check_run_record(run, tag, sha)
    arts = api.artifacts(repo, run_id)
    sealed_names, more = classify([a["name"] for a in arts], prerelease)
    problems += more
    by_name = {a["name"]: a for a in arts}
    for n in sealed_names:
        a = by_name[n]
        if a.get("expired"):
            problems.append("artifact '%s' has expired" % n)
        wr = a.get("workflow_run") or {}
        if wr.get("id") not in (None, run_id):
            problems.append("artifact '%s' belongs to run %s, not %s" % (n, wr.get("id"), run_id))
        if not DIGEST_RE.match(a.get("digest") or ""):
            problems.append("artifact '%s' has no sha256 digest in the API record" % n)
    if problems:
        raise SealError("; ".join(problems))

    out = []
    sums_hash = None
    for n in sealed_names:
        a = by_name[n]
        dest = os.path.join(workdir, "%d.zip" % a["id"])
        api.download(repo, a["id"], dest)
        got = sha256_file(dest)
        want = DIGEST_RE.match(a["digest"]).group(1)
        if got != want:
            raise SealError("artifact '%s' (id %s): zip sha256 %s != API digest %s" % (n, a["id"], got, want))
        files = hash_zip(dest)
        os.unlink(dest)
        if n == "release-assets":
            sums = [f for f in files if f["path"] == "SHA256SUMS"]
            if len(sums) != 1:
                raise SealError("release-assets does not hold exactly one SHA256SUMS")
            sums_hash = sums[0]["sha256"]
        out.append({"name": n, "id": a["id"], "digest": a["digest"], "files": files})
    if sums_hash is None:
        raise SealError("release-assets is not among the sealed artifacts")
    return {
        "schema": SCHEMA,
        "candidate": True,
        "dry_run": True,
        "tag": tag,
        "version": version,
        "build_sha": sha,
        "artifacts": out,
        "release_assets_sha256sums_sha256": sums_hash,
    }


class GhApi:
    """The Actions API through `gh`, which follows the artifact redirect
    without sending the token to the storage host."""

    def _gh(self, *args):
        r = subprocess.run(["gh", "api", *args], check=False, capture_output=True, text=True)
        if r.returncode != 0:
            raise SealError("gh api %s failed: %s" % (" ".join(args[:1]), r.stderr.strip()))
        return r.stdout

    def run(self, repo, run_id):
        return json.loads(self._gh("repos/%s/actions/runs/%d" % (repo, run_id)))

    def artifacts(self, repo, run_id):
        out = self._gh("--paginate", "--jq", ".artifacts[]",
                       "repos/%s/actions/runs/%d/artifacts?per_page=100" % (repo, run_id))
        return [json.loads(line) for line in out.splitlines() if line.strip()]

    def download(self, repo, artifact_id, dest):
        with open(dest, "wb") as fh:
            r = subprocess.run(["gh", "api", "repos/%s/actions/artifacts/%d/zip" % (repo, artifact_id)],
                               check=False, stdout=fh, stderr=subprocess.PIPE)
        if r.returncode != 0:
            raise SealError("download of artifact %d failed: %s" % (artifact_id, r.stderr.decode(errors="replace").strip()))


# --------------------------------------------------------------------------
# Self-test


class FakeApi:
    def __init__(self, run, arts, zips):
        self._run, self._arts, self._zips = run, arts, zips

    def run(self, repo, run_id):
        return self._run

    def artifacts(self, repo, run_id):
        return self._arts

    def download(self, repo, artifact_id, dest):
        with open(dest, "wb") as fh:
            fh.write(self._zips[artifact_id])


def _zip_bytes(files):
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as zf:
        for name, data in sorted(files.items()):
            zf.writestr(name, data)
    return buf.getvalue()


def _fixture(prerelease=False, drop=(), extra=()):
    tag, sha, run_id = "module-v9.9.9", "a" * 40, 4242
    names = [n for n in SEALED if n not in drop and not (prerelease and n in SEALED_OPTIONAL_ON_PRERELEASE)]
    names += ["tree", "debug-linux-x64", "nginx-x64-el9", "nginx-arm64-el9", "testdata-sidecar-results-x64",
              "asset-windows"]
    names += list(extra)
    arts, zips = [], {}
    for i, n in enumerate(names, start=100):
        files = {"file-%s.txt" % n: ("payload of %s\n" % n).encode()}
        if n == "release-assets":
            files["SHA256SUMS"] = b"0123  file\n"
            files["manifest.json"] = b"{}\n"
        z = _zip_bytes(files)
        zips[i] = z
        arts.append({"id": i, "name": n, "expired": False, "digest": "sha256:" + hashlib.sha256(z).hexdigest(),
                     "workflow_run": {"id": run_id, "head_sha": sha}})
    run = {"display_title": expected_title(tag, sha), "head_sha": sha, "event": "workflow_dispatch",
           "head_branch": "master"}
    return dict(tag=tag, version="9.9.9", sha=sha, run_id=run_id, run=run, arts=arts, zips=zips)


def self_test():
    fails = 0

    def run_case(desc, want_ok, mutate=None, prerelease=False, drop=(), extra=(), want_in_error=""):
        nonlocal fails
        fx = _fixture(prerelease=prerelease, drop=drop, extra=extra)
        if mutate:
            mutate(fx)
        api = FakeApi(fx["run"], fx["arts"], fx["zips"])
        with tempfile.TemporaryDirectory() as wd:
            try:
                doc = seal(api, "o/r", fx["run_id"], fx["tag"], fx["version"], fx["sha"], prerelease, wd)
                ok, msg = True, ""
            except SealError as e:
                doc, ok, msg = None, False, str(e)
        good = ok == want_ok and want_in_error in msg
        print("%s %s%s" % ("ok  " if good else "FAIL", desc, "" if good else " (ok=%s, error=%r)" % (ok, msg)))
        if not good:
            fails += 1
        return doc

    doc = run_case("accept: every allowlisted artifact present", True)
    if doc is not None:
        want_keys = ["schema", "candidate", "dry_run", "tag", "version", "build_sha", "artifacts",
                     "release_assets_sha256sums_sha256"]
        shape = (list(doc) == want_keys and doc["schema"] == 2 and doc["candidate"] is True
                 and doc["dry_run"] is True and doc["build_sha"] == "a" * 40
                 and [a["name"] for a in doc["artifacts"]] == list(SEALED)
                 and doc["release_assets_sha256sums_sha256"] == hashlib.sha256(b"0123  file\n").hexdigest()
                 and all(set(a) == {"name", "id", "digest", "files"} for a in doc["artifacts"])
                 and all(set(f) == {"path", "size", "sha256"} for a in doc["artifacts"] for f in a["files"]))
        print("%s seal document has the schema-2 shape and leaves the not-sealed artifacts out"
              % ("ok  " if shape else "FAIL"))
        if not shape:
            fails += 1
    run_case("accept: a release candidate version has no cPanel packages", True, prerelease=True)
    run_case("refuse: allowlisted artifact missing", False, drop=("asset-linux-x64",),
             want_in_error="'asset-linux-x64' is missing")
    run_case("refuse: cPanel package missing on a final version", False, drop=("asset-ea4-el9",),
             want_in_error="'asset-ea4-el9' is missing")
    run_case("refuse: unexpected artifact", False, extra=("surprise-output",),
             want_in_error="unexpected artifact 'surprise-output'")
    run_case("refuse: a signing artifact in a candidate", False, extra=("signed-windows",),
             want_in_error="unexpected artifact 'signed-windows'")

    def bad_digest(fx):
        fx["arts"][0]["digest"] = "sha256:" + "0" * 64
    run_case("refuse: zip does not match the API digest", False, mutate=bad_digest, want_in_error="API digest")

    def no_digest(fx):
        fx["arts"][0]["digest"] = None
    run_case("refuse: no digest in the API record", False, mutate=no_digest, want_in_error="no sha256 digest")

    def bad_title(fx):
        fx["run"]["display_title"] = "Release module-v9.9.9 dry run @ " + fx["sha"]
    run_case("refuse: display_title is not the candidate title", False, mutate=bad_title, want_in_error="display_title")

    def bad_sha(fx):
        fx["run"]["head_sha"] = "b" * 40
    run_case("refuse: head_sha disagrees with the version job", False, mutate=bad_sha, want_in_error="head_sha")

    def bad_branch(fx):
        fx["run"]["head_branch"] = "feature"
    run_case("refuse: run is not on master", False, mutate=bad_branch, want_in_error="head_branch")

    def dup(fx):
        fx["arts"].append(dict(fx["arts"][0], id=999))
    run_case("refuse: duplicate artifact name", False, mutate=dup, want_in_error="more than once")

    def expired(fx):
        fx["arts"][0]["expired"] = True
    run_case("refuse: expired artifact", False, mutate=expired, want_in_error="expired")

    def other_run(fx):
        fx["arts"][0]["workflow_run"] = {"id": 1}
    run_case("refuse: artifact of another run", False, mutate=other_run, want_in_error="belongs to run")

    def evil_zip(fx):
        z = _zip_bytes({"../escape": b"x"})
        fx["zips"][fx["arts"][0]["id"]] = z
        fx["arts"][0]["digest"] = "sha256:" + hashlib.sha256(z).hexdigest()
    run_case("refuse: unsafe path inside an artifact", False, mutate=evil_zip, want_in_error="unsafe path")

    try:
        seal(FakeApi({}, [], {}), "o/r", 1, "module-v9.9.9", "9.9.9", "abc", False, ".")
        print("FAIL refuse: short sha"); fails += 1
    except SealError:
        print("ok   refuse: short sha")

    if fails:
        print("release_adopt self-test: %d failure(s)" % fails)
        return 1
    print("release_adopt self-test: all passed")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    sp = sub.add_parser("seal", help="write candidate-seal.json for this candidate run")
    sp.add_argument("--repo", required=True)
    sp.add_argument("--run-id", required=True, type=int)
    sp.add_argument("--tag", required=True)
    sp.add_argument("--version", required=True)
    sp.add_argument("--sha", required=True)
    sp.add_argument("--prerelease", action="store_true", help="the version is a release candidate (no cPanel packages)")
    sp.add_argument("--out", required=True)
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.cmd != "seal":
        ap.print_usage(sys.stderr)
        return 2
    try:
        with tempfile.TemporaryDirectory() as wd:
            doc = seal(GhApi(), args.repo, args.run_id, args.tag, args.version, args.sha, args.prerelease, wd)
    except SealError as e:
        print("::error::candidate seal refused: %s" % e)
        return 1
    with open(args.out, "w") as fh:
        json.dump(doc, fh, indent=2)
        fh.write("\n")
    print("sealed %d artifacts for %s at %s" % (len(doc["artifacts"]), args.tag, args.sha))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
