#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Report-only CI jobs carry an expiry, and an expired one fails CI.

A flaky job may be made report-only (job-level `continue-on-error`) while it
is being fixed, so it stops turning the default branch and the merge queue
red. That state must not become permanent. Every job-level
`continue-on-error` (any value but `false`) in .github/workflows must have a
line in tools/ci/quarantine.txt:

    <workflow file>|<job id>|<job name>|<issue number>|<expires YYYY-MM-DD>

and this check (run by ci.yml's `CI tooling self-tests`, so on every pull
request, merge group and push) refuses:

  * a report-only job without a line;
  * a line whose job is missing, has another name, or is not report-only
    (remove the line when the job is fixed);
  * a malformed line, or an expiry more than 28 days after today;
  * a line whose expiry has passed (today > expires, UTC): CI goes red on
    every run until the job is fixed and the line removed, or the expiry is
    moved, with the reason written in the tracking issue.

Three days or less before the expiry it prints a ::warning::.

The release lane's gate (tools/ci/ci_release_gate.sh) does not read this
file: a quarantined job that is a release requirement still has to pass at
the release commit.

Usage: tools/ci/check_quarantine.py [--root DIR]
Env:   QUARANTINE_TODAY=YYYY-MM-DD overrides today (tests).
Exit:  0 ok, 1 a violation (one ::error:: line each), 2 usage.
"""
import datetime
import glob
import os
import re
import sys

REGISTRY = "tools/ci/quarantine.txt"
MAX_DAYS = 28
WARN_DAYS = 3
JOB_HEAD = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(?:#.*)?$")
JOB_NAME = re.compile(r"^    name:\s*(.*?)\s*$")
JOB_COE = re.compile(r"^    continue-on-error:\s*(.*?)\s*(?:#.*)?$")


def unquote(v):
    if len(v) >= 2 and v[0] == v[-1] and v[0] in "'\"":
        return v[1:-1]
    return v


def jobs_of(path):
    """{job id: (name, continue-on-error value or None)} of one workflow."""
    jobs, cur, in_jobs = {}, None, False
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if re.match(r"^jobs:\s*$", line):
                in_jobs = True
                continue
            if in_jobs and re.match(r"^[^\s#]", line):
                in_jobs = False
            if not in_jobs:
                continue
            m = JOB_HEAD.match(line)
            if m:
                cur = m.group(1)
                jobs[cur] = [None, None]
                continue
            if cur is None:
                continue
            m = JOB_NAME.match(line)
            if m and jobs[cur][0] is None:
                jobs[cur][0] = unquote(m.group(1))
                continue
            m = JOB_COE.match(line)
            if m:
                jobs[cur][1] = unquote(m.group(1)).strip()
    return {k: tuple(v) for k, v in jobs.items()}


def report_only(coe):
    return coe is not None and coe.lower() != "false"


def today():
    v = os.environ.get("QUARANTINE_TODAY")
    if v:
        return datetime.date.fromisoformat(v)
    return datetime.datetime.now(datetime.timezone.utc).date()


def main(argv):
    root = "."
    if argv[:1] == ["--root"] and len(argv) == 2:
        root = argv[1]
    elif argv:
        sys.stderr.write(__doc__)
        return 2
    errs, notes = [], []
    now = today()

    workflows = {}
    for p in sorted(glob.glob(os.path.join(root, ".github", "workflows", "*.yml"))):
        workflows[os.path.basename(p)] = jobs_of(p)

    entries = {}
    reg = os.path.join(root, REGISTRY)
    lines = []
    if os.path.exists(reg):
        with open(reg, encoding="utf-8") as f:
            lines = f.read().splitlines()
    for n, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        where = "%s:%d" % (REGISTRY, n)
        parts = line.split("|")
        if len(parts) != 5:
            errs.append("%s: want <workflow>|<job id>|<job name>|<issue>|<expires>, got %r" % (where, line))
            continue
        wf, jid, name, issue, exp = (p.strip() for p in parts)
        if not issue.isdigit():
            errs.append("%s: issue %r is not a number" % (where, issue))
            continue
        try:
            expires = datetime.date.fromisoformat(exp)
        except ValueError:
            errs.append("%s: expiry %r is not a YYYY-MM-DD date" % (where, exp))
            continue
        jobs = workflows.get(wf)
        if jobs is None:
            errs.append("%s: workflow %s not found" % (where, wf))
            continue
        if jid not in jobs:
            errs.append("%s: job %s not found in %s" % (where, jid, wf))
            continue
        real_name, coe = jobs[jid]
        if real_name != name:
            errs.append("%s: job %s is named '%s', not '%s'" % (where, jid, real_name, name))
            continue
        if not report_only(coe):
            errs.append("%s: job %s (%s) is not report-only any more; remove its line and close issue #%s"
                        % (where, jid, name, issue))
            continue
        entries[(wf, jid)] = True
        left = (expires - now).days
        if left < 0:
            errs.append("%s: the report-only state of %s expired on %s (issue #%s): fix the job and remove "
                        "its continue-on-error and this line, or move the expiry with the reason in the issue"
                        % (where, name, exp, issue))
        elif left > MAX_DAYS:
            errs.append("%s: %s expires %s, more than %d days from %s; pick a nearer date"
                        % (where, name, exp, MAX_DAYS, now.isoformat()))
        else:
            notes.append("%s is report-only until %s (issue #%s)" % (name, exp, issue))
            if left <= WARN_DAYS:
                print("::warning::%s: the report-only state of %s expires on %s (issue #%s)"
                      % (where, name, exp, issue))

    for wf, jobs in workflows.items():
        for jid, (name, coe) in jobs.items():
            if report_only(coe) and (wf, jid) not in entries:
                errs.append(".github/workflows/%s: job %s (%s) has continue-on-error: %s but is not in %s"
                            % (wf, jid, name, coe, REGISTRY))

    for n in notes:
        print("quarantine: " + n)
    for e in errs:
        print("::error::quarantine: " + e)
    if errs:
        return 1
    if not notes:
        print("quarantine: no report-only jobs")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
