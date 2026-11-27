#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Guards against two kinds of silent CI rot in .github/workflows.

1. Self-test scripts (tools/ci/test_*.sh, tools/ci/test-*.sh):
   - every step that runs one must let a non-zero exit fail the job: no
     `|| true`, no pipe after the command, no `if`/`while` around it, no
     `set +e` in the step, no `continue-on-error: true` on the step or job;
   - every such script must be invoked by some workflow (or be listed in
     ALLOW_UNREGISTERED below with a reason).
2. Compose stacks: a job that runs `docker compose` must set a run-unique
   COMPOSE_PROJECT_NAME (one that contains the run id) before its first use,
   so two runs on one Docker host never share a project name and remove each
   other's stack.

Usage: tools/ci/check_ci_rot.py [--root DIR]    (exit 1 on any violation)
"""
import glob
import os
import re
import sys

# script basename -> reason it is not run by any workflow
ALLOW_UNREGISTERED = {
    "test_deployment_smoke.sh": "quick-start smoke against published images; to be registered in a nightly or after-publish job",
}

TEST_REF = re.compile(r"(?:^|[\s;&|(\"'])(?:(?:bash|sh)\s+)?(?:\./)?tools/ci/(test[_-][\w.-]*\.sh)")
COMPOSE_USE = re.compile(r"\bdocker compose\b|\bdocker-compose\s+-")
PROJECT_SET = re.compile(r"COMPOSE_PROJECT_NAME[=:]")
RUN_ID = re.compile(r"GITHUB_RUN_ID|github\.run_id")
JOB_HEAD = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")


def indent(s):
    return len(s) - len(s.lstrip(" "))


def logical_lines(lines):
    """[(first line no (1-based), joined text)], backslash continuations joined,
    comment-only lines dropped."""
    out, i = [], 0
    while i < len(lines):
        if lines[i].lstrip().startswith("#"):
            i += 1
            continue
        n, text = i + 1, lines[i].rstrip("\n")
        while text.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            text = text.rstrip()[:-1] + " " + lines[i].strip()
        out.append((n, text))
        i += 1
    return out


def step_bounds(lines, idx):
    """(start, end) 0-based line indexes of the step containing lines[idx]."""
    start = idx
    if not re.match(r"^\s*-\s", lines[idx]):
        want = indent(lines[idx])
        start = None
        for j in range(idx - 1, -1, -1):
            if re.match(r"^\s*-\s", lines[j]) and indent(lines[j]) < want:
                start = j
                break
        if start is None:
            return idx, idx + 1
    base = indent(lines[start])
    end = len(lines)
    for j in range(start + 1, len(lines)):
        if lines[j].strip() and not lines[j].lstrip().startswith("#") and indent(lines[j]) <= base:
            end = j
            break
    return start, end


def job_bounds(lines):
    """[(job id, first idx, last idx exclusive)] for the jobs: section."""
    jobs, in_jobs, cur = [], False, None
    for i, l in enumerate(lines):
        if re.match(r"^jobs:\s*$", l):
            in_jobs = True
            continue
        if in_jobs and re.match(r"^\S", l) and l.strip() and not l.startswith("#"):
            in_jobs = False
        if in_jobs:
            m = JOB_HEAD.match(l)
            if m:
                if cur:
                    jobs.append((cur[0], cur[1], i))
                cur = (m.group(1), i)
    if cur:
        jobs.append((cur[0], cur[1], len(lines)))
    return jobs


def check_test_steps(path, lines, errs):
    jobs = job_bounds(lines)
    for n, text in logical_lines(lines):
        idx = n - 1
        for m in TEST_REF.finditer(text):
            name = m.group(1)
            head, tail = text[: m.start(1)], text[m.end():]
            where = "%s:%d (%s)" % (path, n, name)
            if re.search(r"\|\|(?!\s*(?:exit|return)(?:\s+[1-9][0-9]*)?\s*(?:$|[;&|)]))", tail):
                errs.append(where + ": exit status swallowed by || (only a bare or non-zero exit/return may follow)")
            if re.search(r"(?<![|&])\|(?!\|)", tail):
                errs.append(where + ": output piped, exit status masked")
            if re.search(r"&\s*$", tail):
                errs.append(where + ": run in the background")
            if re.search(r"(?:^|[\s;&|(])(if|while|until|!)\s+(?:(?:bash|sh)\s+)?[./\w-]*$", head):
                errs.append(where + ": used as a condition, exit status not propagated")
            s, e = step_bounds(lines, idx)
            region = "\n".join(l for l in lines[s:e] if not l.lstrip().startswith("#"))
            if re.search(r"continue-on-error:\s*(true|\$\{\{)", region):
                errs.append(where + ": step has continue-on-error")
            if re.search(r"^\s*set\s+\+e\b|;\s*set\s+\+e\b", region, re.M):
                errs.append(where + ": step runs set +e")
            for jid, a, b in jobs:
                if a <= idx < b:
                    hdr = []
                    for l in lines[a:b]:
                        if re.match(r"^    steps:\s*$", l):
                            break
                        hdr.append(l)
                    if re.search(r"^    continue-on-error:\s*(true|\$\{\{)", "\n".join(hdr), re.M):
                        errs.append(where + ": job " + jid + " has continue-on-error")


def check_compose(path, lines, errs):
    for jid, a, b in job_bounds(lines):
        first = None
        for i in range(a, b):
            if not lines[i].lstrip().startswith("#") and COMPOSE_USE.search(lines[i]):
                first = i
                break
        if first is None:
            continue
        ok = False
        for i in range(a, first):
            blk = " ".join(lines[i : i + 3])
            if not lines[i].lstrip().startswith("#") and PROJECT_SET.search(lines[i]) and RUN_ID.search(blk):
                ok = True
                break
        if not ok:
            errs.append("%s:%d: job %s runs docker compose without a run-unique "
                        "COMPOSE_PROJECT_NAME set before it" % (path, first + 1, jid))


def main():
    root = "."
    if len(sys.argv) >= 3 and sys.argv[1] == "--root":
        root = sys.argv[2]
    errs, referenced = [], set()
    wfs = sorted(glob.glob(os.path.join(root, ".github/workflows/*.yml")) +
                 glob.glob(os.path.join(root, ".github/actions/**/*.yml"), recursive=True))
    for wf in wfs:
        with open(wf, encoding="utf-8") as f:
            lines = f.read().split("\n")
        rel = os.path.relpath(wf, root)
        for _, text in logical_lines(lines):
            referenced.update(m.group(1) for m in TEST_REF.finditer(text))
        check_test_steps(rel, lines, errs)
        check_compose(rel, lines, errs)
    for f in sorted(glob.glob(os.path.join(root, "tools/ci/test[_-]*.sh"))):
        b = os.path.basename(f)
        if b not in referenced and b not in ALLOW_UNREGISTERED:
            errs.append("tools/ci/%s: not invoked by any workflow (register it or add it "
                        "to ALLOW_UNREGISTERED with a reason)" % b)
    for e in errs:
        print("::error::" + e if os.environ.get("GITHUB_ACTIONS") else "FAIL: " + e)
    if errs:
        return 1
    print("check_ci_rot: ok (%d workflows)" % len(wfs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
