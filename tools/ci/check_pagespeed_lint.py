#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Checks of a built `pagespeed lint` binary that a unit test cannot make.

  check_pagespeed_lint.py report  --binary B --testdata D --schema S
      Runs the binary over every case of the golden test's fixtures, checks
      each JSON report against the committed JSON Schema (the subset of
      draft 2020-12 the schema uses, checked by this script: no third-party
      module needed), checks the exit codes (0/1 for a report, 2 for a missing
      input, refused --show-values into a pipe) and that 20 runs give the same
      bytes.

  check_pagespeed_lint.py report ... --save DIR
      Also writes each report to DIR, for `offline --expect DIR`.

  check_pagespeed_lint.py offline --binary B --testdata D [--expect DIR]
      Runs the binary with networking off and requires the same output as a
      normal run (or as the reports saved in DIR). Networking is off when the
      process has no network interface but the loopback one (for example in
      a container started with --network none); otherwise the runs go
      through `unshare -rn` (a new user and network namespace, no privilege
      needed). Fails when neither is possible.

  check_pagespeed_lint.py symbols --binary B
      ELF (nm) or PE (objdump/llvm-objdump -p) import check: no socket or
      process-spawning import, no fetcher class linked in.

  check_pagespeed_lint.py --self-test
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

CASES = [
    # (config, htaccess files) as in test/pagespeed/tools/cli/lint_golden_test.cc
    ("good/pagespeed.conf", []),
    ("seeded/01-unknown.conf", []),
    ("seeded/09-directory.conf", []),
    ("seeded/19-include-case.conf", []),
    ("seeded/23-vhost-ignored.conf", []),
    ("seeded/25-glued-equals.conf", []),
    ("seeded/31-crlf.conf", []),
    ("", ["seeded/12-htaccess.htaccess"]),
]

# Imports a binary that reads files and prints a report never needs.
FORBIDDEN_ELF = {"socket", "connect", "bind", "listen", "accept", "getaddrinfo",
                 "gethostbyname", "popen", "system", "fork", "vfork",
                 "execve", "execv", "execvp", "posix_spawn"}
FORBIDDEN_PE_DLLS = {"ws2_32.dll", "wsock32.dll", "winhttp.dll", "wininet.dll"}
FETCHER = re.compile(r"(Wget|Serf|Curl)\w*Fetcher|UrlAsyncFetcher::Fetch")


def fail(msg):
    print("check_pagespeed_lint: " + msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------
# A JSON Schema subset: type, const, enum, pattern, minLength, minimum,
# required, properties, additionalProperties: false, items.

def _type_ok(value, t):
    if t == "object":
        return isinstance(value, dict)
    if t == "array":
        return isinstance(value, list)
    if t == "string":
        return isinstance(value, str)
    if t == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    raise ValueError("schema type not supported by this checker: " + t)


KNOWN_KEYWORDS = {"$schema", "$id", "title", "description", "type", "const", "enum",
                  "pattern", "minLength", "minimum", "required", "properties",
                  "additionalProperties", "items"}


def validate(value, schema, path="$"):
    """Returns a list of problems (empty when valid)."""
    unknown = set(schema) - KNOWN_KEYWORDS
    if unknown:
        raise ValueError("schema keyword not supported by this checker: %s" % sorted(unknown))
    out = []
    if "type" in schema and not _type_ok(value, schema["type"]):
        return ["%s: not of type %s" % (path, schema["type"])]
    if "const" in schema and value != schema["const"]:
        out.append("%s: is not %r" % (path, schema["const"]))
    if "enum" in schema and value not in schema["enum"]:
        out.append("%s: not one of %r" % (path, schema["enum"]))
    if isinstance(value, str):
        if "pattern" in schema and not re.search(schema["pattern"], value):
            out.append("%s: does not match /%s/" % (path, schema["pattern"]))
        if len(value) < schema.get("minLength", 0):
            out.append("%s: shorter than %d" % (path, schema["minLength"]))
    if isinstance(value, int) and "minimum" in schema and value < schema["minimum"]:
        out.append("%s: below %d" % (path, schema["minimum"]))
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                out.append("%s: missing %s" % (path, key))
        props = schema.get("properties", {})
        for key, sub in value.items():
            if key in props:
                out.extend(validate(sub, props[key], "%s.%s" % (path, key)))
            elif schema.get("additionalProperties", True) is False:
                out.append("%s: unexpected property %s" % (path, key))
    if isinstance(value, list) and "items" in schema:
        for i, sub in enumerate(value):
            out.extend(validate(sub, schema["items"], "%s[%d]" % (path, i)))
    return out


# --------------------------------------------------------------------------

def lint_args(binary, testdata, config, htaccess):
    args = [binary, "lint", "--json", "--root", testdata]
    for h in htaccess:
        args += ["--htaccess", h]
    if config:
        args.append(config)
    return args


def run(args, prefix=()):
    p = subprocess.run(list(prefix) + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       stdin=subprocess.DEVNULL, timeout=120)
    return p.returncode, p.stdout, p.stderr


def cmd_report(a):
    with open(a.schema, encoding="utf-8") as fh:
        schema = json.load(fh)
    if a.save:
        os.makedirs(a.save, exist_ok=True)
    for i, (config, htaccess) in enumerate(CASES):
        args = lint_args(a.binary, a.testdata, config, htaccess)
        rc, out, err = run(args)
        if rc not in (0, 1):
            fail("%s: exit %d: %s" % (config or htaccess, rc, err.decode(errors="replace")))
        try:
            report = json.loads(out)
        except ValueError as e:
            fail("%s: output is not JSON: %s" % (config or htaccess, e))
        problems = validate(report, schema)
        if problems:
            fail("%s: report does not match the schema:\n  %s" % (config or htaccess, "\n  ".join(problems)))
        if any(b < 0x20 and b != 0x0a or b >= 0x7f for b in out):
            fail("%s: output is not printable ASCII" % (config or htaccess))
        want_rc = 1 if report["summary"]["error"] > 0 else 0
        if rc != want_rc:
            fail("%s: exit %d with %d errors" % (config or htaccess, rc, report["summary"]["error"]))
        for _ in range(20):
            if run(args)[1] != out:
                fail("%s: output differs between runs" % (config or htaccess))
        if a.save:
            with open(os.path.join(a.save, "%d.json" % i), "wb") as fh:
                fh.write(out)
    rc, _out, _err = run(lint_args(a.binary, a.testdata, "no/such.conf", []))
    if rc != 2:
        fail("a missing input exits %d, not 2" % rc)
    rc, _out, _err = run([a.binary, "lint", "--show-values", "--root", a.testdata, CASES[0][0]])
    if rc != 2:
        fail("--show-values into a pipe exits %d, not 2" % rc)
    rc, out, _err = run([a.binary, "version", "--json"])
    version = json.loads(out)
    if rc != 0 or version.get("schema") != schema["properties"]["schema"]["const"] or version.get("build") != "release":
        fail("unexpected version output: %r" % out)
    print("check_pagespeed_lint: %d reports match the schema, exit codes and 20 runs agree" % len(CASES))


ISOLATED = "test \"$(tail -n +3 /proc/net/dev | grep -vc '^ *lo:')\" = 0"


def cmd_offline(a):
    # /proc/net/dev lists the interfaces of the reader's own namespace.
    if run(["sh", "-c", ISOLATED])[0] == 0:
        prefix = []
        how = "no network interface"
    elif shutil.which("unshare") and run(["sh", "-c", ISOLATED], ["unshare", "-rn", "--"])[0] == 0:
        prefix = ["unshare", "-rn", "--"]
        how = "unshare -rn"
    else:
        fail("cannot run with networking off: this process has a network interface and "
             "`unshare -rn` does not work here (run it in a container with --network none)")
    binary = os.path.abspath(a.binary)
    testdata = os.path.abspath(a.testdata)
    for i, (config, htaccess) in enumerate(CASES):
        args = lint_args(binary, testdata, config, htaccess)
        if a.expect:
            with open(os.path.join(a.expect, "%d.json" % i), "rb") as fh:
                saved = fh.read()
            want = (1 if json.loads(saved)["summary"]["error"] else 0, saved)
        else:
            normal = run(args)
            want = (normal[0], normal[1])
        offline = run(args, prefix)
        if want[0] != offline[0] or want[1] != offline[1]:
            fail("%s: the output with networking off differs" % (config or htaccess))
    print("check_pagespeed_lint: %d runs with networking off (%s) match" % (len(CASES), how))


def elf_undefined(binary):
    out = subprocess.run(["nm", "-D", "--undefined-only", binary], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, check=True).stdout.decode()
    return {line.split()[-1].split("@")[0] for line in out.splitlines() if line.strip()}


def pe_dlls(binary):
    for tool in ("llvm-objdump", "objdump"):
        if shutil.which(tool):
            out = subprocess.run([tool, "-p", binary], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, check=True).stdout.decode(errors="replace")
            return {m.lower() for m in re.findall(r"DLL Name:\s*(\S+)", out)}
    fail("no objdump to read the import table")


def cmd_symbols(a):
    with open(a.binary, "rb") as fh:
        magic = fh.read(4)
    if magic == b"\x7fELF":
        bad = sorted(elf_undefined(a.binary) & FORBIDDEN_ELF)
        if bad:
            fail("the binary imports %s" % ", ".join(bad))
        defined = subprocess.run(["nm", "-C", a.binary], stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE).stdout.decode(errors="replace")
        hits = sorted({m.group(0) for m in FETCHER.finditer(defined)})
        if hits:
            fail("a fetcher is linked in: %s" % ", ".join(hits[:5]))
        print("check_pagespeed_lint: ELF, no socket/spawn import, no fetcher linked")
    elif magic[:2] == b"MZ":
        dlls = pe_dlls(a.binary)
        bad = sorted(dlls & FORBIDDEN_PE_DLLS)
        if bad:
            fail("the binary imports %s" % ", ".join(bad))
        print("check_pagespeed_lint: PE imports %s" % ", ".join(sorted(dlls)))
    else:
        fail("neither an ELF nor a PE file: " + a.binary)


def self_test():
    schema = {"type": "object", "required": ["a"], "additionalProperties": False,
              "properties": {"a": {"type": "string", "pattern": "^x"},
                             "b": {"type": "array", "items": {"type": "integer", "minimum": 0}},
                             "c": {"enum": ["p", "q"]}, "d": {"const": "k"}}}
    assert validate({"a": "xy"}, schema) == []
    assert validate({"a": "y"}, schema)
    assert validate({}, schema)
    assert validate({"a": "x", "z": 1}, schema)
    assert validate({"a": "x", "b": [1, -1]}, schema)
    assert validate({"a": "x", "b": [True]}, schema)
    assert validate({"a": "x", "c": "r"}, schema)
    assert validate({"a": "x", "d": "j"}, schema)
    try:
        validate({}, {"oneOf": []})
        raise AssertionError("an unsupported keyword must not pass")
    except ValueError:
        pass
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "..", "pagespeed", "tools", "cli",
                           "pagespeed-lint.schema.json"), encoding="utf-8") as fh:
        lint_schema = json.load(fh)
    sample = {"schema": "pagespeed-lint/1", "tool_version": "1.0.0", "module_version": "2.9.2",
              "inputs": [{"path": "a.conf", "kind": "config", "sha256": "0" * 64}],
              "summary": {"error": 0, "warning": 1, "info": 0},
              "findings": [{"rule": "PS202", "severity": "warning", "file": "a.conf", "line": 2,
                            "scope": "virtualhost/directory+conditional", "directive": "ModPagespeedX",
                            "args": ["<masked>"], "message": "m"}]}
    assert validate(sample, lint_schema) == [], validate(sample, lint_schema)
    sample["findings"][0]["rule"] = "X1"
    assert validate(sample, lint_schema)
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "f")
        with open(p, "wb") as fh:
            fh.write(b"text")
        try:
            cmd_symbols(argparse.Namespace(binary=p))
            raise AssertionError("a non-binary must fail")
        except SystemExit as e:
            assert e.code == 1
    print("check_pagespeed_lint: self-test passed")


def main(argv):
    if argv == ["--self-test"]:
        self_test()
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["report", "offline", "symbols"])
    ap.add_argument("--binary", required=True)
    ap.add_argument("--testdata")
    ap.add_argument("--schema")
    ap.add_argument("--save")
    ap.add_argument("--expect")
    a = ap.parse_args(argv)
    if a.command in ("report", "offline") and not a.testdata:
        ap.error("--testdata is required")
    if a.command == "report" and not a.schema:
        ap.error("--schema is required")
    {"report": cmd_report, "offline": cmd_offline, "symbols": cmd_symbols}[a.command](a)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
