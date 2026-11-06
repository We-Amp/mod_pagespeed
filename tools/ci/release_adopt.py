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
    release_adopt.py schema --print
    release_adopt.py --self-test

`seal` needs `gh` and a token with `actions: read` in GH_TOKEN.
"""

import argparse
import datetime
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

PIPELINE_FILE = ".github/workflows/pipeline-release-module.yml"

# Jobs an adopting run skips: the candidate built or checked these exact
# bytes, and the adopting run takes their verdict from the candidate's job
# list. Each fleet job and its hosted twin are listed. `release_adopt.py
# schema --print` gives this list to the shape rule of check-workflow-shape.sh.
ADOPTABLE = (
    "security-gates",
    "vendor",
    "vendor-hosted",
    "vendor-sidecar",
    "linux-release-build",
    "linux-release-build-hosted",
    "linux-arm64-release-build",
    "nginx-x64-plan",
    "nginx-distro-build",
    "nginx-distro-build-hosted",
    "nginx-ready",
    "nginx-arm64-plan",
    "nginx-distro-build-arm64",
    "nginx-arm64-join",
    "nginx-distro-build-el10",
    "nginx-distro-build-el10-hosted",
    "sidecar-pair",
    "sidecar-pair-arm64",
    "apache-el8-build",
    "apache-el8-build-hosted",
    "apache-el10-build",
    "apache-el10-build-hosted",
    "unit-tests",
    "unit-tests-hosted",
    "package-smoke-deb",
    "package-smoke-deb-hosted",
    "package-smoke-rpm",
    "package-smoke-rpm-hosted",
    "package-smoke-nginx",
    "package-smoke-nginx-hosted",
    "package-load-check-nginx",
    "package-load-check-nginx-hosted",
    "package-load-check-nginx-arm64",
    "package-smoke-nginx-packaged",
    "package-smoke-nginx-packaged-hosted",
    "ea4-build",
    "ea4-build-hosted",
    "ea4-el10-build",
    "ea4-el10-build-hosted",
    "windows-release-build",
    "windows-release-build-hosted",
)

# Jobs that run in an adopting run as they do in any other.
ALWAYS_RUNS = (
    "event-guard",
    "ref-guard",
    "version",
    "optimizer-fetch",
    "systest-gate",
    "ci-release-gate",
    "vendor-ready",
    "linux-ready",
    "el8-ready",
    "el10-ready",
    "windows-ready",
    "ea4-ready",
    "msi-build",
    "gate-iis-msi-upgrade",
    "gate-cpanel-smoke-el9",
    "gate-cpanel-smoke-el8",
    "validate",
)

# Jobs of the `release` environment: skipped in a candidate run.
ENVIRONMENT_JOBS = ("windows-sign", "msi-sign", "sign-checksums", "create-release")

# Jobs that only a candidate run has.
CANDIDATE_ONLY = ("candidate-seal",)

# Sealed artifacts that an adopting run copies into itself under the same
# name, because a job that still runs there reads them. The other sealed
# artifacts (stock-el8, testdata-oldpkgs-*, release-assets) are recorded only.
REUPLOADED = (
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
)

# Logins that may dispatch or re-run a candidate that another login tags.
ADMIN_ALLOWLIST = ("oschaaf",)

MAX_AGE_SECONDS = 3 * 24 * 3600


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


def check_plans(plans):
    """plans: {plan job id: [leg, ...]}, the matrices the plan jobs published."""
    for job, legs in plans.items():
        if (not isinstance(legs, list) or not legs
                or not all(isinstance(x, str) and re.match(r"^[A-Za-z0-9._-]+$", x) for x in legs)
                or len(set(legs)) != len(legs)):
            raise SealError("plan '%s' is not a non-empty list of distinct leg names" % job)


def seal(api, repo, run_id, tag, version, sha, prerelease, workdir, plans=None):
    if not SHA_RE.match(sha):
        raise SealError("sha '%s' is not 40 lowercase hex digits" % sha)
    if plans:
        check_plans(plans)
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
    doc = {
        "schema": SCHEMA,
        "candidate": True,
        "dry_run": True,
        "tag": tag,
        "version": version,
        "build_sha": sha,
        "artifacts": out,
        "release_assets_sha256sums_sha256": sums_hash,
    }
    if plans:
        # Additive: the legs of each matrix that a plan job published. Run
        # outputs are not in the API, and `verify` needs them to name the
        # jobs a candidate must have had. Readers of schema 2 ignore the key.
        doc["plans"] = {k: plans[k] for k in sorted(plans)}
    return doc


# --------------------------------------------------------------------------
# The expected job set: read from the pipeline file at the candidate's lane
# commit. The reader is line-oriented (jobs at two spaces, their keys at
# four, a matrix key at eight), like release_job_graph.py.

JOB_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
MATRIX_REF_RE = re.compile(r"\$\{\{\s*matrix\.([A-Za-z0-9_]+)\s*\}\}")
PLAN_REF_RE = re.compile(r"fromJSON\(\s*needs\.([A-Za-z0-9_-]+)\.outputs\.[A-Za-z0-9_]+")


class PipelineError(Exception):
    pass


def _unquote(v):
    v = v.strip()
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
        return v[1:-1]
    return v


def parse_pipeline(text):
    """{job id: {"name", "uses", "matrix"}} in file order. `matrix` is None or
    (key, values, plan job id): values is a list for a literal matrix and
    None for one taken from `fromJSON(needs.<plan>.outputs...)`."""
    jobs = {}
    in_jobs = False
    cur = None
    in_matrix = False
    for line in text.split("\n"):
        if not in_jobs:
            in_jobs = line == "jobs:"
            continue
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        m = JOB_RE.match(line)
        if m:
            cur = m.group(1)
            jobs[cur] = {"name": None, "uses": False, "matrix": None}
            in_matrix = False
            continue
        if cur is None:
            continue
        indent = len(line) - len(line.lstrip(" "))
        body = line.strip()
        if indent == 4:
            in_matrix = False
            if body.startswith("name:"):
                jobs[cur]["name"] = _unquote(body[5:])
            elif body.startswith("uses:"):
                jobs[cur]["uses"] = True
            continue
        if indent == 6 and body == "matrix:":
            in_matrix = True
            continue
        if indent == 6:
            in_matrix = False
            continue
        if in_matrix and indent == 8:
            key, _, val = body.partition(":")
            if jobs[cur]["matrix"] is not None or not key.isidentifier():
                raise PipelineError("job '%s' has a matrix this reader does not support" % cur)
            val = val.strip()
            if val.startswith("["):
                vals = [_unquote(x) for x in val.strip("[]").split(",") if x.strip()]
                jobs[cur]["matrix"] = (key, vals, None)
            else:
                pm = PLAN_REF_RE.search(val)
                if not pm:
                    raise PipelineError("job '%s' has a matrix this reader does not support" % cur)
                jobs[cur]["matrix"] = (key, None, pm.group(1))
    for j, d in jobs.items():
        if not d["name"]:
            raise PipelineError("job '%s' has no name" % j)
        stripped = MATRIX_REF_RE.sub("", d["name"])
        if "${{" in stripped:
            raise PipelineError("job '%s': name '%s' uses an expression this reader does not support" % (j, d["name"]))
        if MATRIX_REF_RE.search(d["name"]) and not d["matrix"]:
            raise PipelineError("job '%s' names a matrix it does not have" % j)
    return jobs


def schema_doc():
    return {"adoptable": list(ADOPTABLE), "always_runs": list(ALWAYS_RUNS),
            "environment": list(ENVIRONMENT_JOBS), "candidate_only": list(CANDIDATE_ONLY),
            "reuploaded": list(REUPLOADED)}


def twin_of(job_id, jobs):
    """The other leg of a fleet/hosted pair, or None."""
    if job_id.endswith("-hosted"):
        base = job_id[: -len("-hosted")]
        return base if base in jobs else None
    return job_id + "-hosted" if job_id + "-hosted" in jobs else None


def schema_problems(jobs):
    """Problems with the job schema against a parsed pipeline file."""
    problems = []
    groups = (("adoptable", ADOPTABLE), ("always-runs", ALWAYS_RUNS),
              ("environment", ENVIRONMENT_JOBS), ("candidate-only", CANDIDATE_ONLY))
    where = {}
    for label, group in groups:
        for j in group:
            if j in where:
                problems.append("job '%s' is in both %s and %s" % (j, where[j], label))
            where[j] = label
    for j in jobs:
        if j not in where:
            problems.append("job '%s' is in no group of the adoption schema" % j)
    for j in where:
        if j not in jobs:
            problems.append("schema names '%s', which the pipeline file does not have" % j)
    for j in ADOPTABLE:
        t = twin_of(j, jobs)
        if t is not None and t not in ADOPTABLE:
            problems.append("adoptable job '%s' has a twin '%s' that is not adoptable" % (j, t))
    return problems


# --------------------------------------------------------------------------
# verify: the checks of an adopting run on the candidate it names

RELEASE_PATH = ".github/workflows/release.yml"
PIN_FILE = "install/debian/optimizer-pin.json"
CALLER_PREFIX = "module / "
BINDING_SCHEMA = 1
SUMS_ROW_RE = re.compile(r"^([0-9a-f]{64}) [ *](\S+)$")


class VerifyError(Exception):
    pass


def parse_time(v):
    try:
        return datetime.datetime.strptime(v, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
    except (TypeError, ValueError):
        raise VerifyError("'%s' is not an ISO-8601 UTC time" % (v,))


def is_bot(user):
    login = (user or {}).get("login") or ""
    return login.endswith("[bot]") or (user or {}).get("type") == "Bot"


def split_ref(wf):
    """(file path, ref) of a referenced_workflows path 'owner/repo/.github/...yml@ref'."""
    path, _, ref = (wf.get("path") or "").partition("@")
    return path, ref or wf.get("ref") or ""


def check_run(run, repo, candidate_run, tag, tag_commit):
    """Checks 1 and 2 (the run record)."""
    problems = []
    if (run.get("repository") or {}).get("full_name") != repo:
        problems.append("check 1: the run belongs to repository %r, not %r"
                        % ((run.get("repository") or {}).get("full_name"), repo))
    path = (run.get("path") or "").partition("@")[0]
    if path != RELEASE_PATH:
        problems.append("check 1: the run's workflow path is %r, expected %r" % (run.get("path"), RELEASE_PATH))
    if run.get("event") != "workflow_dispatch":
        problems.append("check 1: the run's event is %r, expected 'workflow_dispatch'" % (run.get("event"),))
    if run.get("head_branch") != "master":
        problems.append("check 1: head_branch is %r, expected 'master'" % (run.get("head_branch"),))
    if run.get("status") != "completed":
        problems.append("check 1: the candidate run is %r, not completed" % (run.get("status"),))
    elif run.get("conclusion") != "success":
        problems.append("check 1: the candidate run concluded %r, not success" % (run.get("conclusion"),))
    want = expected_title(tag, tag_commit)
    if run.get("display_title") != want:
        problems.append("check 1: display_title is %r, expected %r" % (run.get("display_title"), want))
    if run.get("id") not in (None, candidate_run):
        problems.append("check 1: the record is of run %s, not %s" % (run.get("id"), candidate_run))
    if run.get("head_sha") != tag_commit:
        problems.append("check 2: the candidate built %r, the tag commit is %r" % (run.get("head_sha"), tag_commit))
    return problems


def check_lane(run, lane_sha):
    """Check 3: the same lane definition."""
    found = []
    for wf in run.get("referenced_workflows") or []:
        path, ref = split_ref(wf)
        if path.endswith("/" + PIPELINE_FILE):
            found.append((ref, wf.get("sha")))
    if len(found) != 1:
        return ["check 3: the candidate run references %d copies of pipeline-release-module.yml, expected 1"
                % len(found)]
    ref, sha = found[0]
    problems = []
    if ref != "refs/heads/master":
        problems.append("check 3: the candidate's lane ref is %r, expected 'refs/heads/master'" % ref)
    if sha != lane_sha:
        problems.append("check 3: lane definition differs: the candidate ran the lane at %s, this run at %s "
                        "(master moved since the candidate started)" % (sha, lane_sha))
    return problems


def check_actors(run, actor):
    """Check 4."""
    problems = []
    if is_bot({"login": actor}):
        problems.append("check 4: this run was triggered by a bot (%s)" % actor)
    for field in ("actor", "triggering_actor"):
        user = run.get(field) or {}
        login = user.get("login")
        if is_bot(user):
            problems.append("check 4: the candidate's %s is a bot (%s)" % (field, login))
        elif not login or (login != actor and login not in ADMIN_ALLOWLIST):
            problems.append("check 4: the candidate's %s %r is neither this run's actor %r nor on the admin allowlist"
                            % (field, login, actor))
    return problems


def check_age(run, started_at):
    """Check 7."""
    try:
        done = parse_time(run.get("updated_at"))
        start = parse_time(started_at)
    except VerifyError as e:
        return ["check 7: %s" % e]
    age = (start - done).total_seconds()
    if age < 0:
        return ["check 7: the candidate finished after this run started"]
    if age > MAX_AGE_SECONDS:
        return ["check 7: the candidate finished %.1f days before this run started (limit 3)" % (age / 86400.0)]
    return []


def norm_job_name(name, prefix=CALLER_PREFIX):
    return name[len(prefix):] if prefix and name.startswith(prefix) else name


def expand_expected(jobs, plans):
    """{job id: {"names": [names when it ran], "raw": name when skipped, "uses": bool,
    "need_plan": plan job id or None}}"""
    out = {}
    for j, d in jobs.items():
        info = {"names": [d["name"]], "raw": d["name"], "uses": d["uses"], "need_plan": None}
        if d["matrix"]:
            key, vals, plan = d["matrix"]
            if vals is None:
                vals = plans.get(plan)
                if vals is None:
                    info["names"], info["need_plan"] = None, plan
            if vals is not None:
                info["names"] = [MATRIX_REF_RE.sub(lambda m, v=v: v, d["name"]) for v in vals]
        out[j] = info
    return out


def job_states(jobs, plans, listing, prefix=CALLER_PREFIX):
    """Match the `filter=latest` listing against the pipeline file.
    Returns (states, sealed list, problems). states: job id -> 'success' | 'skipped'."""
    problems = []
    entries = {}
    for e in listing:
        n = norm_job_name(e.get("name") or "", prefix)
        if n in entries:
            problems.append("check 5: the listing has two jobs named %r" % n)
        entries[n] = e
    exp = expand_expected(jobs, plans)
    claimed = set()
    states = {}
    sealed = []

    def take(job, names):
        for n in names:
            claimed.add(n)
            e = entries[n]
            sealed.append({"job": job, "name": n, "conclusion": e.get("conclusion"),
                           "run_attempt": e.get("run_attempt")})
        return [entries[n].get("conclusion") for n in names]

    for j, info in exp.items():
        if info["uses"]:
            names = sorted(n for n in entries if n == info["raw"] or n.startswith(info["raw"] + " / "))
            if not names:
                problems.append("check 5: job '%s' (%r) is missing from the listing" % (j, info["raw"]))
                continue
            concl = take(j, names)
        elif info["raw"] in entries and (info["names"] is None or info["raw"] not in info["names"]):
            if info["names"] is not None and any(n in entries for n in info["names"]):
                problems.append("check 5: job '%s' is listed both skipped and expanded" % j)
            concl = take(j, [info["raw"]])
        elif info["names"] is None:
            problems.append("check 5: job '%s' needs the legs of plan '%s', and the seal records no such plan"
                            % (j, info["need_plan"]))
            continue
        else:
            missing = [n for n in info["names"] if n not in entries]
            if missing:
                problems.append("check 5: job '%s' is missing from the listing: %s" % (j, ", ".join(repr(m) for m in missing)))
                continue
            concl = take(j, info["names"])
        if any(c not in ("success", "skipped") for c in concl):
            bad = sorted(set(c for c in concl if c not in ("success", "skipped")))
            problems.append("check 5: job '%s' concluded %s" % (j, "/".join(str(b) for b in bad)))
        elif all(c == "skipped" for c in concl):
            states[j] = "skipped"
        elif info["uses"]:
            states[j] = "success"
        elif all(c == "success" for c in concl):
            states[j] = "success"
        else:
            problems.append("check 5: job '%s' has legs that are skipped and legs that are not" % j)
    for n in sorted(entries):
        if n not in claimed:
            problems.append("check 5: the listing has a job the pipeline file does not define: %r" % n)

    for j in jobs:
        st = states.get(j)
        if st is None:
            continue
        if j in ENVIRONMENT_JOBS:
            if st != "skipped":
                problems.append("check 5: job '%s' of the release environment concluded %s, expected skipped" % (j, st))
            continue
        t = twin_of(j, jobs)
        if t is not None:
            if t in states and st == states[t]:
                if j < t:
                    problems.append("check 5: of '%s' and its twin '%s' %s" % (
                        j, t, "both ran" if st == "success" else "neither ran"))
            continue
        if st != "success":
            problems.append("check 5: job '%s' concluded %s, expected success" % (j, st))
    return states, sealed, problems


def sums_rows(data):
    rows = {}
    for line in data.decode("utf-8", "replace").splitlines():
        m = SUMS_ROW_RE.match(line)
        if not m:
            raise VerifyError("malformed SHA256SUMS row %r" % line[:80])
        if m.group(2) in rows:
            raise VerifyError("duplicate SHA256SUMS row %r" % m.group(2))
        rows[m.group(2)] = m.group(1)
    return rows


def short(h):
    return h[:12]


def binding_string(run_id, seal_hash, sums_hash):
    return "run %s · seal %s · SHA256SUMS %s" % (run_id, short(seal_hash), short(sums_hash))


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def read_zip_member(path, name):
    with zipfile.ZipFile(path) as zf:
        try:
            return zf.read(name)
        except KeyError:
            return None


def verify(api, repo, candidate_run, tag, tag_commit, trailers, actor, lane_sha, started_at, workdir,
           read_only=False, adopting_run=None, tag_object=None, prefix=CALLER_PREFIX):
    """Run the nine checks. Returns (binding document, derived trailers).
    Raises VerifyError with every problem found by the cheap checks, or the first
    problem found while hashing."""
    if not SHA_RE.match(tag_commit or ""):
        raise VerifyError("the tag commit %r is not 40 lowercase hex digits" % (tag_commit,))
    if not SHA_RE.match(lane_sha or ""):
        raise VerifyError("the lane sha %r is not 40 lowercase hex digits" % (lane_sha,))
    if not re.match(r"^module-v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$", tag or ""):
        raise VerifyError("tag %r is not module-vX.Y.Z or module-vX.Y.Z-rc.N" % (tag,))
    if not read_only:
        if not adopting_run:
            raise VerifyError("this run's id is required")
        for k in ("run", "sums", "seal"):
            if not trailers.get(k):
                raise VerifyError("check 8: the tag has no %s trailer"
                                  % {"run": "Release-Candidate-Run", "sums": "Release-Candidate-SHA256SUMS",
                                     "seal": "Release-Candidate-Seal"}[k])
        if not tag_object or not tag_object.get("sha"):
            raise VerifyError("the tag object sha is required")
    for k in ("sums", "seal"):
        if trailers.get(k) and not re.match(r"^[0-9a-f]{64}$", trailers[k]):
            raise VerifyError("the %s trailer is not 64 lowercase hex digits" % k)
    if trailers.get("run") and str(trailers["run"]) != str(candidate_run):
        raise VerifyError("check 8: the Release-Candidate-Run trailer names run %s, not %s"
                          % (trailers["run"], candidate_run))

    run = api.run(repo, candidate_run)
    problems = check_run(run, repo, candidate_run, tag, tag_commit)
    problems += check_lane(run, lane_sha)
    problems += check_actors(run, actor)
    problems += check_age(run, started_at)

    # check 5: the jobs, against the pipeline file at the candidate's lane commit
    listing = api.jobs(repo, candidate_run)
    jobs = None
    try:
        jobs = parse_pipeline(api.contents(repo, PIPELINE_FILE, lane_sha))
    except (PipelineError, VerifyError) as e:
        problems.append("check 5: the pipeline file at %s: %s" % (lane_sha, e))
    seal_job = None
    sealed_jobs = []
    # The matrix legs come from the plans in the seal, which is read below:
    # the expected job set is matched once the seal is loaded.
    arts = api.artifacts(repo, candidate_run)

    # check 6, first part: exactly one candidate-seal artifact, from the passing seal job
    seal_arts = [a for a in arts if a.get("name") == "candidate-seal"]
    seal_ids = []
    if len(seal_arts) != 1:
        problems.append("check 6: the run has %d candidate-seal artifacts, expected 1" % len(seal_arts))
    sname = jobs["candidate-seal"]["name"] if jobs and "candidate-seal" in jobs else "Seal the candidate artifacts"
    entries = [e for e in listing if norm_job_name(e.get("name") or "", prefix) == sname]
    if len(entries) == 1 and entries[0].get("conclusion") == "success":
        seal_job = entries[0]
    elif len(entries) == 1:
        problems.append("check 5: the candidate-seal job concluded %r, expected success" % entries[0].get("conclusion"))
    else:
        problems.append("check 5: the listing has %d candidate-seal jobs, expected 1" % len(entries))
    if seal_job is not None and len(seal_arts) == 1:
        a = seal_arts[0]
        try:
            created = parse_time(a.get("created_at"))
            t0, t1 = parse_time(seal_job.get("started_at")), parse_time(seal_job.get("completed_at"))
            if not (t0 <= created <= t1):
                problems.append("check 6: the candidate-seal artifact (created %s) was not uploaded by the passing "
                                "candidate-seal job (attempt %s, %s to %s)"
                                % (a.get("created_at"), seal_job.get("run_attempt"), seal_job.get("started_at"),
                                   seal_job.get("completed_at")))
        except VerifyError as e:
            problems.append("check 6: %s" % e)
        if a.get("expired"):
            problems.append("check 6: the candidate-seal artifact has expired")
        wr = a.get("workflow_run") or {}
        if wr.get("id") != candidate_run or wr.get("head_sha") != tag_commit:
            problems.append("check 6: the candidate-seal artifact belongs to run %s at %s" % (wr.get("id"), wr.get("head_sha")))
        if not DIGEST_RE.match(a.get("digest") or ""):
            problems.append("check 6: the candidate-seal artifact has no sha256 digest in the API record")
        seal_ids = [a]
    if problems or not seal_ids:
        raise VerifyError("; ".join(problems))

    # check 8, first part: the seal itself
    sa = seal_ids[0]
    dest = os.path.join(workdir, "seal-%d.zip" % sa["id"])
    api.download(repo, sa["id"], dest)
    if sha256_file(dest) != DIGEST_RE.match(sa["digest"]).group(1):
        raise VerifyError("check 8: the candidate-seal zip does not match its API digest")
    raw = read_zip_member(dest, "candidate-seal.json")
    os.unlink(dest)
    if raw is None:
        raise VerifyError("check 8: the candidate-seal artifact holds no candidate-seal.json")
    seal_hash = sha256_bytes(raw)
    if trailers.get("seal") and seal_hash != trailers["seal"]:
        raise VerifyError("check 8: candidate-seal.json hashes to %s, the Release-Candidate-Seal trailer says %s"
                          % (seal_hash, trailers["seal"]))
    try:
        sd = json.loads(raw)
    except ValueError:
        raise VerifyError("check 8: candidate-seal.json is not JSON")
    problems = []
    if sd.get("schema") != SCHEMA:
        problems.append("check 8: the seal has schema %r, expected %d" % (sd.get("schema"), SCHEMA))
    if sd.get("candidate") is not True:
        problems.append("check 8: the seal does not say candidate: true")
    if sd.get("dry_run") is not True:
        problems.append("check 8: the seal does not say dry_run: true")
    if sd.get("tag") != tag:
        problems.append("check 8: the seal is of tag %r, not %r" % (sd.get("tag"), tag))
    if sd.get("version") != tag[len("module-v"):]:
        problems.append("check 8: the seal's version is %r, the tag's is %r" % (sd.get("version"), tag[len("module-v"):]))
    if sd.get("build_sha") != run.get("head_sha"):
        problems.append("check 8: the seal's build_sha is %r, the API's head_sha %r" % (sd.get("build_sha"), run.get("head_sha")))
    plans = sd.get("plans") or {}
    try:
        check_plans(plans)
    except SealError as e:
        problems.append("check 5: the seal's %s" % e)
    sealed_arts = sd.get("artifacts")
    if not isinstance(sealed_arts, list) or not sealed_arts:
        raise VerifyError("; ".join(problems + ["check 8: the seal lists no artifacts"]))

    # check 5, now that the plans are known
    if jobs is not None:
        _states, sealed_jobs, more = job_states(jobs, plans, listing, prefix)
        problems += more

    # check 6, second part: every sealed artifact is listed unchanged
    by_name = {}
    for a in arts:
        by_name.setdefault(a.get("name"), []).append(a)
    sealed_names = [x.get("name") for x in sealed_arts]
    prerelease = "-" in tag[len("module-v"):]
    _, cls = classify(sealed_names, prerelease)
    problems += ["check 6: the seal: %s" % c for c in cls]
    for x in sealed_arts:
        n = x.get("name")
        listed = by_name.get(n) or []
        if len(listed) != 1:
            problems.append("check 6: sealed artifact '%s' is listed %d times by the API" % (n, len(listed)))
            continue
        a = listed[0]
        if a.get("id") != x.get("id"):
            problems.append("check 6: artifact '%s' has id %s, the seal says %s" % (n, a.get("id"), x.get("id")))
        if a.get("digest") != x.get("digest"):
            problems.append("check 6: artifact '%s' has digest %s, the seal says %s" % (n, a.get("digest"), x.get("digest")))
        if a.get("expired"):
            problems.append("check 6: artifact '%s' has expired" % n)
        wr = a.get("workflow_run") or {}
        if wr.get("id") != candidate_run:
            problems.append("check 6: artifact '%s' belongs to run %s, not %s" % (n, wr.get("id"), candidate_run))
        if wr.get("head_sha") != tag_commit:
            problems.append("check 6: artifact '%s' was built at %s, not %s" % (n, wr.get("head_sha"), tag_commit))
        if not DIGEST_RE.match(a.get("digest") or ""):
            problems.append("check 6: artifact '%s' has no sha256 digest in the API record" % n)
    if problems:
        raise VerifyError("; ".join(problems))

    # check 8, second part: the bytes
    sums_data = manifest_data = None
    downloaded = {}
    for x in sealed_arts:
        n = x["name"]
        dest = os.path.join(workdir, "%d.zip" % x["id"])
        api.download(repo, x["id"], dest)
        got = sha256_file(dest)
        if got != DIGEST_RE.match(x["digest"]).group(1):
            raise VerifyError("check 8: artifact '%s' (id %s): zip sha256 %s != API digest %s" % (n, x["id"], got, x["digest"]))
        try:
            files = hash_zip(dest)
        except SealError as e:
            raise VerifyError("check 8: artifact '%s': %s" % (n, e))
        if n == "release-assets":
            sums_data = read_zip_member(dest, "SHA256SUMS")
            manifest_data = read_zip_member(dest, "manifest.json")
        os.unlink(dest)
        want = {f["path"]: (f["size"], f["sha256"]) for f in x.get("files") or []}
        have = {f["path"]: (f["size"], f["sha256"]) for f in files}
        if want != have:
            diff = sorted(set(want) ^ set(have)) or sorted(p for p in want if want[p] != have[p])
            raise VerifyError("check 8: artifact '%s' does not match the seal: %s" % (n, ", ".join(diff[:5])))
        downloaded[n] = files
    if sums_data is None:
        raise VerifyError("check 8: release-assets holds no SHA256SUMS")
    sums_hash = sha256_bytes(sums_data)
    if sums_hash != sd.get("release_assets_sha256sums_sha256"):
        raise VerifyError("check 8: SHA256SUMS hashes to %s, the seal says %s"
                          % (sums_hash, sd.get("release_assets_sha256sums_sha256")))
    if trailers.get("sums") and sums_hash != trailers["sums"]:
        raise VerifyError("check 8: SHA256SUMS hashes to %s, the Release-Candidate-SHA256SUMS trailer says %s"
                          % (sums_hash, trailers["sums"]))
    try:
        rows = sums_rows(sums_data)
    except VerifyError as e:
        raise VerifyError("check 8: %s" % e)
    for n, files in downloaded.items():
        if not n.startswith("asset-"):
            continue
        for f in files:
            base = f["path"].rsplit("/", 1)[-1].replace("~", ".")
            if not any(name.endswith(base) and h == f["sha256"] for name, h in rows.items()):
                raise VerifyError("check 8: file '%s' of '%s' has no row with its hash in SHA256SUMS" % (f["path"], n))

    # check 9: the optimizer pin at the tag is the pin the candidate built from
    try:
        pin = json.loads(api.contents(repo, PIN_FILE, tag_commit))
        manifest = json.loads(manifest_data) if manifest_data is not None else {}
    except ValueError:
        raise VerifyError("check 9: the pin file or manifest.json is not JSON")
    if not pin.get("tag") or pin.get("tag") != manifest.get("optimizer_tag"):
        raise VerifyError("check 9: %s names %r at the tag, the candidate's release-assets name %r"
                          % (PIN_FILE, pin.get("tag"), manifest.get("optimizer_tag")))

    derived = {"run": str(candidate_run), "sums": sums_hash, "seal": seal_hash}
    binding = {
        "schema": BINDING_SCHEMA,
        "repository": repo,
        "adopting_run": adopting_run,
        "candidate": {"run": candidate_run, "attempt": run.get("run_attempt"), "build_sha": tag_commit,
                      "actor": (run.get("actor") or {}).get("login"),
                      "triggering_actor": (run.get("triggering_actor") or {}).get("login"),
                      "completed_at": run.get("updated_at")},
        "tag": {"name": tag, "commit": tag_commit, "object": tag_object or {}},
        "lane_sha": lane_sha,
        "trailers": {"Release-Candidate-Run": derived["run"],
                     "Release-Candidate-SHA256SUMS": derived["sums"],
                     "Release-Candidate-Seal": derived["seal"]},
        "jobs": sealed_jobs,
        "artifacts": [{"name": x["name"], "reupload": x["name"] in REUPLOADED, "id": x["id"], "digest": x["digest"],
                       "new_id": None, "new_digest": None, "files": x["files"]} for x in sealed_arts],
        "binding": binding_string(candidate_run, seal_hash, sums_hash),
    }
    return binding, derived


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

    def jobs(self, repo, run_id):
        """The newest record of every job, across attempts."""
        out = self._gh("--paginate", "--jq", ".jobs[]",
                       "repos/%s/actions/runs/%d/jobs?filter=latest&per_page=100" % (repo, run_id))
        return [json.loads(line) for line in out.splitlines() if line.strip()]

    def contents(self, repo, path, ref):
        return self._gh("-H", "Accept: application/vnd.github.raw",
                        "repos/%s/contents/%s?ref=%s" % (repo, path, ref))

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



_FAKE_PIPELINE = """name: x
on: workflow_call
jobs:
  version:
    name: Derive versions
    runs-on: ubuntu-latest
  vendor:
    name: Vendor
  vendor-hosted:
    name: Vendor (hosted)
  nginx-x64-plan:
    name: Plan (x64)
  nginx-distro-build:
    name: Build (x64, ${{ matrix.distro }})
    strategy:
      fail-fast: false
      matrix:
        distro: ${{ fromJSON(needs.nginx-x64-plan.outputs.distros || '["none"]') }}
    steps:
      - name: not a job name
  nginx-distro-build-hosted:
    name: Build (x64, hosted, ${{ matrix.distro }})
    strategy:
      matrix:
        distro: ${{ fromJSON(needs.nginx-x64-plan.outputs.distros || '["none"]') }}
  ea4-build:
    name: EA4 (${{ matrix.os }})
    strategy:
      matrix:
        os: [el9, el8]
  ea4-build-hosted:
    name: EA4 (${{ matrix.os }}, hosted)
    strategy:
      matrix:
        os: [el9, el8]
  gate-iis-msi-upgrade:
    name: Gate, IIS installer
    uses: o/r/.github/workflows/gate.yml@master
  windows-sign:
    name: Sign the Windows binaries
    environment: release
  validate:
    name: Validate
  candidate-seal:
    name: Seal
"""

class FakeApi:
    def __init__(self, run, arts, zips, jobs=None, contents=None, arts_by_run=None):
        self._run, self._arts, self._zips = run, arts, zips
        self._jobs, self._contents, self._arts_by_run = jobs or [], contents or {}, arts_by_run or {}

    def run(self, repo, run_id):
        return self._run

    def jobs(self, repo, run_id):
        return self._jobs

    def contents(self, repo, path, ref):
        try:
            return self._contents[(path, ref)]
        except KeyError:
            raise VerifyError("no %s at %s" % (path, ref))

    def artifacts(self, repo, run_id):
        return self._arts_by_run.get(run_id, self._arts)

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
    contents = {}
    for n in names:
        contents[n] = {"file-%s.txt" % n: ("payload of %s\n" % n).encode()}
    rows = "".join("%s  %s\n" % (hashlib.sha256(d).hexdigest(), f)
                   for n in names if n.startswith("asset-") for f, d in sorted(contents[n].items()))
    sums = rows.encode()
    if "release-assets" in contents:
        contents["release-assets"]["SHA256SUMS"] = sums
        contents["release-assets"]["manifest.json"] = b'{"optimizer_tag": "optimizer-v9.9.9"}\n'
    arts, zips = [], {}
    for i, n in enumerate(names, start=100):
        z = _zip_bytes(contents[n])
        zips[i] = z
        arts.append({"id": i, "name": n, "expired": False, "digest": "sha256:" + hashlib.sha256(z).hexdigest(),
                     "workflow_run": {"id": run_id, "head_sha": sha}})
    run = {"display_title": expected_title(tag, sha), "head_sha": sha, "event": "workflow_dispatch",
           "head_branch": "master"}
    return dict(tag=tag, version="9.9.9", sha=sha, run_id=run_id, run=run, arts=arts, zips=zips, sums=sums)


_LANE = "c" * 40
_PUSHER = "pusher"
_T = "2026-11-05T"


def _job(name, concl="success", attempt=1, t0="11:00:00", t1="11:10:00", prefix=CALLER_PREFIX):
    return {"name": prefix + name, "conclusion": concl, "run_attempt": attempt,
            "started_at": _T + t0 + "Z", "completed_at": _T + t1 + "Z"}


def _listing():
    return [_job("Derive versions"), _job("Vendor"), _job("Vendor (hosted)", "skipped"), _job("Plan (x64)"),
            _job("Build (x64, el8)"), _job("Build (x64, el9)"),
            _job("Build (x64, hosted, ${{ matrix.distro }})", "skipped"),
            _job("EA4 (el9)"), _job("EA4 (el8)"), _job("EA4 (${{ matrix.os }}, hosted)", "skipped"),
            _job("Gate, IIS installer / inner"),
            _job("Sign the Windows binaries", "skipped"), _job("Validate"),
            _job("Seal", "success", 1, "11:50:00", "11:55:00")]


def _world(prerelease=False):
    fx = _fixture(prerelease=prerelease)
    tag = "module-v9.9.9-rc.1" if prerelease else fx["tag"]
    fx["tag"] = tag
    fx["run"]["display_title"] = expected_title(tag, fx["sha"])
    fx["run"].update({
        "id": fx["run_id"], "repository": {"full_name": "o/r"}, "path": RELEASE_PATH, "status": "completed",
        "conclusion": "success", "run_attempt": 1, "updated_at": _T + "12:00:00Z",
        "actor": {"login": _PUSHER, "type": "User"}, "triggering_actor": {"login": _PUSHER, "type": "User"},
        "referenced_workflows": [{"path": "o/r/" + PIPELINE_FILE + "@refs/heads/master", "sha": _LANE,
                                  "ref": "refs/heads/master"}],
    })
    plans = {"nginx-x64-plan": ["el8", "el9"]}
    with tempfile.TemporaryDirectory() as wd:
        sd = seal(FakeApi(fx["run"], fx["arts"], fx["zips"]), "o/r", fx["run_id"], tag, tag[len("module-v"):],
                  fx["sha"], prerelease, wd, plans=plans)
    w = {"fx": fx, "listing": _listing(), "pin": json.dumps({"tag": "optimizer-v9.9.9"}), "pipeline": _FAKE_PIPELINE,
         "extra_arts": {}}
    w["kw"] = dict(repo="o/r", candidate_run=fx["run_id"], tag=tag, tag_commit=fx["sha"],
                   trailers={}, actor=_PUSHER, lane_sha=_LANE, started_at="2026-11-06T09:00:00Z",
                   adopting_run=777, tag_object={"sha": "d" * 40, "tagger": _PUSHER})

    def set_seal(doc):
        raw = (json.dumps(doc, indent=2) + "\n").encode()
        z = _zip_bytes({"candidate-seal.json": raw})
        w["seal_zip"] = z
        w["seal_art"] = {"id": 900, "name": "candidate-seal", "expired": False, "created_at": _T + "11:54:00Z",
                         "digest": "sha256:" + hashlib.sha256(z).hexdigest(),
                         "workflow_run": {"id": fx["run_id"], "head_sha": fx["sha"]}}
        w["seal_doc"] = doc
        w["kw"]["trailers"]["seal"] = hashlib.sha256(raw).hexdigest()
    w["set_seal"] = set_seal
    set_seal(sd)
    w["kw"]["trailers"]["sums"] = hashlib.sha256(fx["sums"]).hexdigest()
    w["kw"]["trailers"]["run"] = str(fx["run_id"])
    return w


def _verify_world(w, read_only=False):
    fx = w["fx"]
    arts = list(fx["arts"]) + [w["seal_art"]] + list(w["extra_arts"].get("list", []))
    zips = dict(fx["zips"])
    zips[900] = w["seal_zip"]
    zips.update(w["extra_arts"].get("zips", {}))
    api = FakeApi(fx["run"], arts, zips, jobs=w["listing"], contents={
        (PIPELINE_FILE, _LANE): w["pipeline"], (PIN_FILE, fx["sha"]): w["pin"]})
    kw = dict(w["kw"])
    with tempfile.TemporaryDirectory() as wd:
        return verify(api, kw.pop("repo"), kw.pop("candidate_run"), kw.pop("tag"), kw.pop("tag_commit"),
                      kw.pop("trailers"), kw.pop("actor"), kw.pop("lane_sha"), kw.pop("started_at"), wd,
                      read_only=read_only, **kw)


def _verify_cases():
    out = []

    def case(desc, mutate=None, refused=None, prerelease=False, read_only=False, after=None):
        w = _world(prerelease)
        if mutate:
            mutate(w)
        try:
            binding, derived = _verify_world(w, read_only)
            ok, msg = True, ""
        except VerifyError as e:
            binding, ok, msg = None, False, str(e)
        if refused is None:
            good, why = ok, msg
            if good and after:
                good, why = after(w, binding)
        else:
            good, why = (not ok) and refused in msg, "ok=%s, error=%r" % (ok, msg)
        out.append((desc, good, why))

    def run(**kv):
        return lambda w: w["fx"]["run"].update(kv)

    def kw(**kv):
        return lambda w: w["kw"].update(kv)

    def shape(w, b):
        want = {"schema", "repository", "adopting_run", "candidate", "tag", "lane_sha", "trailers", "jobs",
                "artifacts", "binding"}
        sealed = [a["name"] for a in b["artifacts"]]
        ok = (set(b) == want and b["candidate"]["run"] == 4242 and b["lane_sha"] == _LANE
              and b["tag"]["object"]["sha"] == "d" * 40
              and b["binding"].startswith("run 4242 \u00b7 seal ") and b["binding"].endswith(
                  "SHA256SUMS " + w["kw"]["trailers"]["sums"][:12])
              and sealed == list(SEALED) and all(a["new_id"] is None and a["files"] for a in b["artifacts"])
              and {j["conclusion"] for j in b["jobs"]} == {"success", "skipped"}
              and any(j["name"] == "Build (x64, el9)" and j["job"] == "nginx-distro-build" for j in b["jobs"]))
        return ok, "binding: %r" % (b,)

    case("accept: every check passes and the binding is written in full", after=shape)
    case("accept: a release candidate version (no cPanel packages)", prerelease=True)
    case("accept: the hosted legs ran and the fleet legs were skipped",
         lambda w: w.update(listing=[j for j in w["listing"] if not j["name"].startswith(CALLER_PREFIX + "Build (x64")
                                     and j["name"] != CALLER_PREFIX + "Vendor" and not j["name"].startswith(CALLER_PREFIX + "Vendor (")]
                            + [_job("Build (x64, hosted, el8)"), _job("Build (x64, hosted, el9)"),
                               _job("Build (x64, ${{ matrix.distro }})", "skipped"),
                               _job("Vendor", "skipped"), _job("Vendor (hosted)")]))
    case("accept: the dispatcher is on the admin allowlist, the tag pusher is someone else",
         lambda w: (w["fx"]["run"].update(actor={"login": "oschaaf"}, triggering_actor={"login": "oschaaf"}),
                    w["kw"].update(actor="someone-else")))

    def attempt2(w):
        w["fx"]["run"]["run_attempt"] = 2
        for j in w["listing"]:
            if j["name"].endswith(("Validate", "Seal", "Gate, IIS installer / inner")):
                j["run_attempt"] = 2
        w["listing"][-1].update(started_at=_T + "13:30:00Z", completed_at=_T + "13:40:00Z")
        w["fx"]["run"]["updated_at"] = _T + "13:41:00Z"
        w["seal_art"]["created_at"] = _T + "13:39:00Z"
    case("accept: green only at attempt 2 after Re-run failed jobs (latest listing, seal from attempt 2)", attempt2,
         after=lambda w, b: (b["candidate"]["attempt"] == 2 and {j["run_attempt"] for j in b["jobs"]} == {1, 2}, str(b["jobs"])))

    def stale_seal(w):
        attempt2(w)
        w["seal_art"]["created_at"] = _T + "11:54:00Z"
    case("refuse: the seal artifact is from attempt 1, the passing seal job is attempt 2", stale_seal,
         refused="was not uploaded by the passing candidate-seal job")
    case("refuse: two candidate-seal artifacts",
         lambda w: w["extra_arts"].update(list=[dict(w["seal_art"], id=901)]), refused="2 candidate-seal artifacts")
    case("refuse: seal artifact created outside the seal job", lambda w: w["seal_art"].update(created_at=_T + "11:20:00Z"),
         refused="was not uploaded by the passing candidate-seal job")
    case("refuse: wrong repository", run(repository={"full_name": "x/y"}), refused="belongs to repository")
    case("refuse: wrong workflow path", run(path=".github/workflows/other.yml"), refused="workflow path")
    case("refuse: wrong event", run(event="push"), refused="event is 'push'")
    case("refuse: head_branch is not master", run(head_branch="topic"), refused="head_branch")
    case("refuse: wrong display_title", run(display_title="Release module-v9.9.9 dry run"), refused="display_title")
    case("refuse: candidate not completed", run(status="in_progress", conclusion=None), refused="not completed")
    case("refuse: candidate not successful", run(conclusion="failure"), refused="concluded 'failure'")
    case("refuse: the candidate built another commit than the tag's", kw(tag_commit="b" * 40), refused="check 2")
    case("refuse: lane definition differs (master moved)", kw(lane_sha="e" * 40), refused="lane definition differs")
    case("refuse: lane referenced from another ref",
         run(referenced_workflows=[{"path": "o/r/" + PIPELINE_FILE + "@refs/heads/topic", "sha": _LANE, "ref": "refs/heads/topic"}]),
         refused="lane ref")
    case("refuse: candidate dispatched by another login", run(actor={"login": "mallory", "type": "User"}),
         refused="candidate's actor 'mallory'")
    case("refuse: latest attempt started by a login that is not allowlisted (rerun)",
         run(triggering_actor={"login": "mallory", "type": "User"}), refused="triggering_actor 'mallory'")
    case("refuse: bot actor by login suffix",
         lambda w: (w["fx"]["run"].update(actor={"login": "dependabot[bot]", "type": "User"}), w["kw"].update(actor="dependabot[bot]")),
         refused="bot")
    case("refuse: bot actor by account type", run(triggering_actor={"login": _PUSHER, "type": "Bot"}), refused="is a bot")
    case("refuse: candidate older than three days", kw(started_at="2026-11-09T12:00:01Z"), refused="days before")
    case("refuse: the Release-Candidate-Run trailer names another run",
         lambda w: w["kw"]["trailers"].update(run="5"), refused="trailer names run")
    case("refuse: a signing job did not conclude skipped",
         lambda w: w["listing"][11].update(conclusion="success"), refused="release environment concluded success")
    case("refuse: a job of the pipeline file is missing from the listing",
         lambda w: w["listing"].pop(3), refused="missing from the listing")
    case("refuse: an extra job in the listing",
         lambda w: w["listing"].append(_job("Surprise")), refused="does not define: 'Surprise'")
    case("refuse: a matrix leg named by the plan is missing", lambda w: w["listing"].pop(5), refused="'Build (x64, el9)'")
    case("refuse: a job failed in the latest attempt", lambda w: w["listing"][1].update(conclusion="failure"),
         refused="concluded failure")
    case("refuse: both legs of a twin pair ran",
         lambda w: w["listing"][2].update(conclusion="success"), refused="both ran")
    case("refuse: neither leg of a twin pair ran",
         lambda w: w["listing"][1].update(conclusion="skipped"), refused="neither ran")
    case("refuse: the seal job did not succeed", lambda w: w["listing"][-1].update(conclusion="failure"),
         refused="candidate-seal job concluded")

    def no_plans(w):
        d = dict(w["seal_doc"])
        d.pop("plans")
        w["set_seal"](d)
    case("refuse: the seal records no plan for a matrix that ran", no_plans, refused="records no such plan")

    def drop_art(w):
        w["fx"]["arts"] = [a for a in w["fx"]["arts"] if a["name"] != "asset-linux-x64"]
    case("refuse: a sealed artifact is missing from the API listing", drop_art, refused="'asset-linux-x64' is listed 0 times")
    case("refuse: a sealed artifact expired", lambda w: w["fx"]["arts"][0].update(expired=True), refused="has expired")
    case("refuse: a sealed artifact has another id than the seal", lambda w: w["fx"]["arts"][0].update(id=555),
         refused="has id 555")
    case("refuse: a sealed artifact has another digest than the seal",
         lambda w: w["fx"]["arts"][0].update(digest="sha256:" + "1" * 64), refused="has digest")
    case("refuse: a sealed artifact of another run",
         lambda w: w["fx"]["arts"][0].update(workflow_run={"id": 9, "head_sha": w["fx"]["sha"]}), refused="belongs to run 9")
    case("refuse: a sealed artifact built at another commit",
         lambda w: w["fx"]["arts"][0].update(workflow_run={"id": 4242, "head_sha": "9" * 40}), refused="was built at")

    def tamper(w):
        fx = w["fx"]
        a = fx["arts"][0]
        z = _zip_bytes({"file-%s.txt" % a["name"]: b"tampered\n"})
        fx["zips"][a["id"]] = z
        a["digest"] = "sha256:" + hashlib.sha256(z).hexdigest()
        d = json.loads(json.dumps(w["seal_doc"]))
        d["artifacts"][0]["digest"] = a["digest"]
        w["set_seal"](d)
    case("refuse: a file differs from the seal although the API digest agrees", tamper, refused="does not match the seal")
    case("refuse: the zip bytes differ from the API digest",
         lambda w: w["fx"]["zips"].update({w["fx"]["arts"][0]["id"]: _zip_bytes({"x": b"y"})}), refused="API digest")
    case("refuse: SHA256SUMS differs from the trailer", lambda w: w["kw"]["trailers"].update(sums="2" * 64),
         refused="Release-Candidate-SHA256SUMS trailer")

    def seal_sums(w):
        d = json.loads(json.dumps(w["seal_doc"]))
        d["release_assets_sha256sums_sha256"] = "3" * 64
        w["set_seal"](d)
    case("refuse: SHA256SUMS differs from the seal's record", seal_sums, refused="the seal says")
    case("refuse: the seal hash differs from the trailer", lambda w: w["kw"]["trailers"].update(seal="4" * 64),
         refused="Release-Candidate-Seal trailer says")
    case("refuse: the tag has no Release-Candidate-Seal trailer", lambda w: w["kw"]["trailers"].pop("seal"),
         refused="no Release-Candidate-Seal trailer")
    case("refuse: the tag has no Release-Candidate-SHA256SUMS trailer", lambda w: w["kw"]["trailers"].pop("sums"),
         refused="no Release-Candidate-SHA256SUMS trailer")

    def sealed_field(field, value):
        def f(w):
            d = json.loads(json.dumps(w["seal_doc"]))
            d[field] = value
            w["set_seal"](d)
        return f
    case("refuse: the seal does not say candidate: true", sealed_field("candidate", False), refused="candidate: true")
    case("refuse: the seal does not say dry_run: true", sealed_field("dry_run", False), refused="dry_run: true")
    case("refuse: the seal is of another tag", sealed_field("tag", "module-v1.0.0"), refused="the seal is of tag")
    case("refuse: the seal's build_sha differs from the API", sealed_field("build_sha", "5" * 40), refused="build_sha")
    case("refuse: the seal has another version", sealed_field("version", "1.0.0"), refused="seal's version")
    case("refuse: the seal has another schema", sealed_field("schema", 3), refused="schema")
    case("refuse: the seal lists an artifact that is not allowlisted",
         lambda w: w["set_seal"](dict(w["seal_doc"], artifacts=w["seal_doc"]["artifacts"] + [
             {"name": "surprise", "id": 1, "digest": "sha256:" + "0" * 64, "files": []}])), refused="unexpected artifact 'surprise'")
    case("refuse: a file of an asset artifact has no row in SHA256SUMS", lambda w: _drop_row(w), refused="no row with its hash")
    case("refuse: the pin at the tag is not the pin the candidate built from",
         lambda w: w.update(pin=json.dumps({"tag": "optimizer-v1.0.0"})), refused="check 9")
    case("refuse: the pipeline file at the lane commit defines a job the candidate did not have",
         lambda w: w.update(pipeline=w["pipeline"] + "  new-job:\n    name: New\n"), refused="job 'new-job' is missing from the listing")

    def real_file(w):
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", PIPELINE_FILE)
        with open(path) as fh:
            text = fh.read()
        jobs = parse_pipeline(text)
        plans = {"nginx-x64-plan": ["el8", "el9"], "nginx-arm64-plan": ["el9", "noble"]}
        d = dict(w["seal_doc"], plans=plans)
        w["set_seal"](d)
        listing = []
        for j, info in expand_expected(jobs, plans).items():
            if j in ENVIRONMENT_JOBS or (twin_of(j, jobs) and j.endswith("-hosted")):
                listing.append(_job(info["raw"], "skipped"))
            elif info["uses"]:
                listing.append(_job(info["raw"] + " / inner"))
            elif j == "candidate-seal":
                listing.append(_job(info["raw"], "success", 1, "11:50:00", "11:55:00"))
            else:
                listing += [_job(n) for n in info["names"]]
        w.update(pipeline=text, listing=listing)
    case("accept: the real pipeline file, every job and matrix leg expanded, fleet legs ran and hosted twins skipped",
         real_file, after=lambda w, b: (len(b["jobs"]) > 60, str(len(b["jobs"]))))
    case("refuse: this run's id is missing", kw(adopting_run=None), refused="this run's id")

    def ro(w, b):
        return b is not None, ""
    case("accept: read-only needs neither this run's id, the tag object nor the trailers",
         lambda w: (w["kw"].update(adopting_run=None, tag_object=None), w["kw"]["trailers"].clear()), read_only=True,
         after=lambda w, b: (b["binding"].startswith("run 4242"), b["binding"]))
    case("refuse: read-only still applies every check", run(conclusion="failure"), read_only=True, refused="concluded 'failure'")
    return out


def _drop_row(w):
    fx = w["fx"]
    ra = [a for a in fx["arts"] if a["name"] == "release-assets"][0]
    rows = fx["sums"].decode().splitlines(True)[1:]
    sums = "".join(rows).encode()
    z = _zip_bytes({"SHA256SUMS": sums, "manifest.json": b'{"optimizer_tag": "optimizer-v9.9.9"}\n'})
    fx["zips"][ra["id"]] = z
    ra["digest"] = "sha256:" + hashlib.sha256(z).hexdigest()
    d = json.loads(json.dumps(w["seal_doc"]))
    for a in d["artifacts"]:
        if a["name"] == "release-assets":
            a["digest"] = ra["digest"]
            a["files"] = hash_zip_bytes(z)
    d["release_assets_sha256sums_sha256"] = hashlib.sha256(sums).hexdigest()
    w["kw"]["trailers"]["sums"] = d["release_assets_sha256sums_sha256"]
    w["set_seal"](d)


def hash_zip_bytes(data):
    with tempfile.TemporaryDirectory() as wd:
        p = os.path.join(wd, "z.zip")
        with open(p, "wb") as fh:
            fh.write(data)
        return hash_zip(p)


def self_test():
    fails = 0

    def check(desc, good, detail=""):
        nonlocal fails
        print("%s %s%s" % ("ok  " if good else "FAIL", desc, "" if good else " (%s)" % detail))
        if not good:
            fails += 1

    def run_case(desc, want_ok, mutate=None, prerelease=False, drop=(), extra=(), want_in_error="", plans=None):
        nonlocal fails
        fx = _fixture(prerelease=prerelease, drop=drop, extra=extra)
        if mutate:
            mutate(fx)
        api = FakeApi(fx["run"], fx["arts"], fx["zips"])
        with tempfile.TemporaryDirectory() as wd:
            try:
                doc = seal(api, "o/r", fx["run_id"], fx["tag"], fx["version"], fx["sha"], prerelease, wd,
                           plans=plans)
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
                 and doc["release_assets_sha256sums_sha256"] == hashlib.sha256(_fixture()["sums"]).hexdigest()
                 and all(set(a) == {"name", "id", "digest", "files"} for a in doc["artifacts"])
                 and all(set(f) == {"path", "size", "sha256"} for a in doc["artifacts"] for f in a["files"]))
        print("%s seal document has the schema-2 shape and leaves the not-sealed artifacts out"
              % ("ok  " if shape else "FAIL"))
        if not shape:
            fails += 1
    run_case("accept: a release candidate version has no cPanel packages", True, prerelease=True)
    pdoc = run_case("accept: plans are recorded as an additive field", True,
                    plans={"nginx-x64-plan": ["el8", "el9"]})
    check("seal with plans keeps the schema-2 keys and adds `plans`",
          pdoc is not None and pdoc["schema"] == 2 and pdoc["plans"] == {"nginx-x64-plan": ["el8", "el9"]})
    run_case("refuse: an empty plan", False, plans={"nginx-x64-plan": []}, want_in_error="non-empty list")
    run_case("refuse: a plan with a repeated leg", False, plans={"p": ["a", "a"]}, want_in_error="distinct")
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


    # ---- verify
    for desc, good, why in _verify_cases():
        check(desc, good, why)

    # ---- the adoption schema and the pipeline reader
    pipeline_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", PIPELINE_FILE)
    try:
        with open(pipeline_path) as fh:
            real_jobs = parse_pipeline(fh.read())
        probs = schema_problems(real_jobs)
    except (OSError, PipelineError) as e:
        real_jobs, probs = {}, [str(e)]
    check("every job of the real pipeline file is in exactly one schema group", not probs, "; ".join(probs))
    check("the schema prints as the four groups and the re-uploaded set",
          sorted(schema_doc()) == ["adoptable", "always_runs", "candidate_only", "environment", "reuploaded"]
          and set(REUPLOADED) <= set(SEALED))

    fake = parse_pipeline(_FAKE_PIPELINE)
    probs = schema_problems(fake)
    check("a job missing from the schema is named",
          any("in no group" in x and "'brand-new'" in x for x in schema_problems(dict(fake, **{"brand-new": fake["version"]}))))
    check("a schema job the file lacks is named", any("does not have" in x for x in probs))
    check("matrix: literal values and plan reference are read",
          fake["ea4-build"]["matrix"] == ("os", ["el9", "el8"], None)
          and fake["nginx-distro-build"]["matrix"] == ("distro", None, "nginx-x64-plan")
          and fake["gate-iis-msi-upgrade"]["uses"] is True and twin_of("vendor", fake) == "vendor-hosted"
          and twin_of("vendor-hosted", fake) == "vendor" and twin_of("version", fake) is None)
    try:
        parse_pipeline("jobs:\n  a:\n    name: A ${{ github.run_id }}\n")
        check("refuse: a job name with an expression other than a matrix key", False)
    except PipelineError:
        check("refuse: a job name with an expression other than a matrix key", True)
    if real_jobs:
        names = {}
        for j, d in real_jobs.items():
            names.setdefault(MATRIX_REF_RE.sub("*", d["name"]), []).append(j)
        dup = [v for v in names.values() if len(v) > 1]
        check("the real pipeline file has no two jobs with the same name pattern", not dup, str(dup))

    if fails:
        print("release_adopt self-test: %d failure(s)" % fails)
        return 1
    print("release_adopt self-test: all passed")
    return 0


# --------------------------------------------------------------------------
# upload-check: read back what `adopt` re-uploaded into this run


def upload_check(api, repo, run_id, binding, names, workdir):
    """Read each named artifact back from this run's own listing, check its zip
    against the API digest and its files against the binding, and record the
    new id and digest in the binding. Mutates and returns the binding."""
    by_name = {a["name"]: a for a in binding.get("artifacts") or []}
    if not names:
        names = [n for n in REUPLOADED if n in by_name]
    arts = api.artifacts(repo, run_id)
    problems = []
    for n in names:
        rec = by_name.get(n)
        if rec is None or n not in REUPLOADED:
            problems.append("'%s' is not a re-uploaded artifact of the binding" % n)
            continue
        listed = [a for a in arts if a.get("name") == n]
        if len(listed) != 1:
            problems.append("'%s' is listed %d times in this run" % (n, len(listed)))
            continue
        a = listed[0]
        wr = a.get("workflow_run") or {}
        if a.get("expired"):
            problems.append("'%s' has expired" % n)
        elif wr.get("id") != run_id:
            problems.append("'%s' belongs to run %s, not this run %s" % (n, wr.get("id"), run_id))
        elif not DIGEST_RE.match(a.get("digest") or ""):
            problems.append("'%s' has no sha256 digest in the API record" % n)
        else:
            dest = os.path.join(workdir, "up-%d.zip" % a["id"])
            api.download(repo, a["id"], dest)
            got = sha256_file(dest)
            if got != DIGEST_RE.match(a["digest"]).group(1):
                problems.append("'%s': the zip hashes to %s, the API digest says %s" % (n, got, a["digest"]))
                continue
            try:
                files = hash_zip(dest)
            except SealError as e:
                problems.append("'%s': %s" % (n, e))
                continue
            finally:
                if os.path.exists(dest):
                    os.unlink(dest)
            want = {f["path"]: (f["size"], f["sha256"]) for f in rec.get("files") or []}
            have = {f["path"]: (f["size"], f["sha256"]) for f in files}
            if want != have:
                diff = sorted(set(want) ^ set(have)) or sorted(p for p in want if want[p] != have[p])
                problems.append("'%s' differs from the candidate's files: %s" % (n, ", ".join(diff[:5])))
                continue
            rec["new_id"], rec["new_digest"] = a["id"], a["digest"]
    if problems:
        raise VerifyError("; ".join(problems))
    return binding


def cmd_upload_check(args):
    try:
        with open(args.binding, encoding="utf-8") as fh:
            binding = json.load(fh)
        with tempfile.TemporaryDirectory() as wd:
            upload_check(GhApi(), args.repo, args.run_id, binding, args.artifact, wd)
    except (VerifyError, SealError, OSError, ValueError) as e:
        print("::error::re-upload check failed: %s" % e)
        return 1
    write_binding(args.binding, binding)
    done = sum(1 for a in binding["artifacts"] if a.get("new_id"))
    print("re-upload check: %d of %d artifacts read back" % (done, sum(1 for a in binding["artifacts"] if a["reupload"])))
    return 0


def cmd_binding_hash(args):
    try:
        got = sha256_file(args.file)
    except OSError as e:
        print("::error::%s" % e)
        return 1
    if args.expect and got != args.expect:
        print("::error::adopt-binding.json hashes to %s, expected %s" % (got, args.expect))
        return 1
    print(got)
    return 0


def write_binding(path, doc):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=2, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


def cmd_verify(args):
    trailers = {"run": str(args.candidate_run), "sums": args.sums_trailer, "seal": args.seal_trailer}
    tag_object = None
    if args.tag_object_sha:
        tag_object = {"sha": args.tag_object_sha, "tagger": args.tagger}
    try:
        with tempfile.TemporaryDirectory() as wd:
            binding, derived = verify(GhApi(), args.repo, args.candidate_run, args.tag, args.tag_commit, trailers,
                                      args.actor, args.lane_sha, args.started_at, wd, read_only=args.read_only,
                                      adopting_run=args.run_id, tag_object=tag_object)
    except (VerifyError, SealError) as e:
        print("::error::adoption refused: %s" % e)
        return 1
    print(binding["binding"])
    if args.read_only:
        print("Release-Candidate-Run: %s" % derived["run"])
        print("Release-Candidate-SHA256SUMS: %s" % derived["sums"])
        print("Release-Candidate-Seal: %s" % derived["seal"])
        return 0
    write_binding(args.out, binding)
    if args.github_output:
        with open(args.out, "rb") as fh:
            digest = sha256_bytes(fh.read())
        with open(args.github_output, "a", encoding="utf-8") as fh:
            fh.write("adopted=true\ncandidate_run=%s\ncandidate_attempt=%s\nbinding=%s\nbinding_sha256=%s\n"
                     % (args.candidate_run, binding["candidate"]["attempt"], binding["binding"], digest))
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
    sp.add_argument("--plan", action="append", default=[], metavar="JOB=JSON",
                    help="a plan job's published legs, e.g. nginx-x64-plan='[\"el9\"]' (repeatable)")
    sp.add_argument("--out", required=True)
    vp = sub.add_parser("verify", help="check a candidate run for adoption and write adopt-binding.json")
    vp.add_argument("--repo", required=True)
    vp.add_argument("--run-id", type=int, help="this run's id (not needed with --read-only)")
    vp.add_argument("--candidate-run", required=True, type=int, help="the Release-Candidate-Run trailer")
    vp.add_argument("--tag", required=True)
    vp.add_argument("--tag-commit", required=True, help="the tag's commit (with --read-only: the intended sha)")
    vp.add_argument("--sums-trailer", default="", help="the Release-Candidate-SHA256SUMS trailer")
    vp.add_argument("--seal-trailer", default="", help="the Release-Candidate-Seal trailer")
    vp.add_argument("--actor", required=True, help="this run's triggering actor")
    vp.add_argument("--lane-sha", required=True, help="this run's sha of pipeline-release-module.yml")
    vp.add_argument("--started-at", required=True, help="this run's start time, YYYY-MM-DDTHH:MM:SSZ")
    vp.add_argument("--tag-object-sha", default="")
    vp.add_argument("--tagger", default="")
    vp.add_argument("--read-only", action="store_true", help="check only; write nothing, print the trailer values")
    vp.add_argument("--out", default="adopt-binding.json")
    vp.add_argument("--github-output", default="", help="append key=value outputs to this file")
    up = sub.add_parser("upload-check", help="read re-uploaded artifacts back from this run")
    up.add_argument("--repo", required=True)
    up.add_argument("--run-id", required=True, type=int)
    up.add_argument("--binding", required=True)
    up.add_argument("--artifact", action="append", default=[], help="one artifact name (default: all re-uploaded)")
    bp = sub.add_parser("binding-hash", help="print the sha256 of adopt-binding.json, optionally checking it")
    bp.add_argument("--file", required=True)
    bp.add_argument("--expect", default="")
    sc = sub.add_parser("schema", help="the adoption schema: which job ids are adopted, run, or neither")
    sc.add_argument("--print", action="store_true", dest="print_schema", required=True)
    args = ap.parse_args(argv)
    if args.self_test:
        return self_test()
    if args.cmd == "schema":
        print(json.dumps(schema_doc(), indent=2))
        return 0
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if args.cmd == "verify":
        return cmd_verify(args)
    if args.cmd == "upload-check":
        return cmd_upload_check(args)
    if args.cmd == "binding-hash":
        return cmd_binding_hash(args)
    if args.cmd != "seal":
        ap.print_usage(sys.stderr)
        return 2
    try:
        plans = {}
        for item in args.plan:
            job, _, raw = item.partition("=")
            try:
                plans[job] = json.loads(raw)
            except ValueError:
                raise SealError("--plan %s: not JSON" % job)
        with tempfile.TemporaryDirectory() as wd:
            doc = seal(GhApi(), args.repo, args.run_id, args.tag, args.version, args.sha, args.prerelease, wd,
                       plans=plans)
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
