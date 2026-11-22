#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Keep pagespeed-doctor's option tables in step with the module source.

pagespeed-doctor is a shell script, so it carries its own copy of the option
names (for "did you mean" suggestions), the deprecated option names and the
deprecated filter names. This reads them from the C++ source and compares
them with the blocks between the BEGIN/END markers in the script.

  doctor_tables.py --check   exit 1 and print a diff when they drifted
  doctor_tables.py --write   rewrite the blocks in place
"""
import argparse
import pathlib
import re
import sys
import textwrap

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "install/common/pagespeed-doctor"
SOURCES = [
    "net/instaweb/rewriter/rewrite_options.cc",
    "pagespeed/system/system_rewrite_options.cc",
    "pagespeed/system/system_process_options.cc",
    "pagespeed/nginx/ngx_rewrite_options.cc",
    "pagespeed/apache/apache_config.cc",
    "pagespeed/apache/mod_instaweb.cc",
    "pagespeed/apache/apache_directive_table.h",
]
# Accepted with a warning by both servers; named through constants in the
# source, so they are listed here and checked to still exist there.
WARN_ONLY = [
    "AllowVaryOn", "PrivateNotVaryForIE", "AnalyticsID",
    "LogMobilizationSamples", "UseAnalyticsJs", "ContentExperimentID",
    "ContentExperimentVariantID", "IncreaseSpeedTracking",
    "UseExperimentalJsMinifier", "ExperimentVariable",
]
EXTRA_VALID = ["on", "off", "standby", "unplugged", "ProcessScriptVariables"]


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


SCOPE_RANK = {"kQueryScope": 0, "kDirectoryScope": 1, "kServerScope": 2,
              "kLegacyProcessScope": 3, "kProcessScopeStrict": 3}
SCOPE_RE = r"(kQueryScope|kDirectoryScope|kServerScope|kLegacyProcessScope|kProcessScopeStrict)"


def strip_prefix(v):
    if v.startswith("ModPagespeed") and len(v) > len("ModPagespeed"):
        return v[len("ModPagespeed"):]
    return v


def scopes():
    """Option name -> narrowest place nginx accepts it: 3 = http {} only,
    2 = http {} or server {}. Mirrors NgxRewriteOptions::GetOptionScope and
    SystemProcessOptions::Parse1/2 (kLegacyProcessScope
    counts as http {} only, as nginx enforces it)."""
    files = SOURCES + ["net/instaweb/rewriter/rewrite_options_properties.inc"]
    const = {}
    for rel in files:
        for m in re.finditer(
                r'(?:const char |inline constexpr char\s+)(?:[A-Za-z]+::)?(k[A-Za-z0-9]+)\[\]\s*=\s*"([A-Z][A-Za-z0-9]+)"',
                read(rel)):
            const.setdefault(m.group(1), m.group(2))
    rank = {}

    def put(name, r):
        name = strip_prefix(name)
        rank[name] = max(rank.get(name, 0), r)

    for rel in files:
        s = read(rel)
        # A name constant directly followed by its scope (X-macro table,
        # AddBaseProperty, add_ngx_option, the scoped AddSystemProperty).
        for m in re.finditer(r"\b(?:\w+::)?(k[A-Z]\w*),\s*(?:\w+::)?" + SCOPE_RE + r"\b", s):
            if m.group(1) in const:
                put(const[m.group(1)], SCOPE_RANK[m.group(2)])
        # AddSystemProperty without a scope argument means kServerScope.
        for m in re.finditer(r"AddSystemProperty\((.*?)\);", s, re.S):
            body = m.group(1)
            if re.search(SCOPE_RE, body):
                continue
            for k in re.findall(r"\b(?:\w+::)?(k[A-Z]\w*)\b", body):
                if k in const:
                    put(const[k], 2)
                    break
    ngx = read("pagespeed/nginx/ngx_rewrite_options.cc")
    for arr, r in (("server_only_options", 2), ("main_only_options", 3)):
        m = re.search(arr + r"\[\]\s*=\s*\{(.*?)\};", ngx, re.S)
        if not m:
            sys.exit(f"doctor_tables: {arr} not found in ngx_rewrite_options.cc")
        for name in re.findall(r'"([A-Za-z]+)"', m.group(1)):
            put(name, r)
    # The factory's own options: the strict ones (an error below process
    # scope) and CreateSharedMemoryMetadataCache.
    fac = read("pagespeed/system/system_process_options.cc")
    strict = re.findall(r"\{(k\w+),\s*SystemProcessOptions::Option::\w+,\s*true\}", fac)
    if not strict:
        sys.exit("doctor_tables: the factory's global-option table moved")
    for k in strict:
        if k in const:
            put(const[k], 3)
    m = re.search(r"SystemProcessOptions::Parse2\((.*?)\n}", fac, re.S)
    for k in re.findall(r"!StringCaseEqual\(option,\s*(k\w+)\)\)", m.group(1) if m else ""):
        if k in const:
            put(const[k], 3)
    put("ProcessScriptVariables", 3)
    return rank


def tables():
    valid, removed = set(EXTRA_VALID), set()
    filters = set()
    for rel in SOURCES:
        s = read(rel)
        for m in re.finditer(
                r'(?:const char |inline constexpr char\s+)(?:[A-Za-z]+::)?k[A-Za-z0-9]+\[\]\s*=\s*"([A-Z][A-Za-z0-9]+)"', s):
            valid.add(m.group(1))
        for m in re.finditer(r'IsDirective\(directive,\s*"([A-Za-z]+)"\)', s):
            valid.add(m.group(1))
        for m in re.finditer(r'AddDeprecatedProperty\(\s*"([A-Za-z]+)"', s):
            removed.add(m.group(1))
        for m in re.finditer(r"Filter '([a-z_]+)' is deprecated", s):
            filters.add(m.group(1))
        for m in re.finditer(r'"([a-z_]+) is deprecated and is now a no-op', s):
            filters.add(m.group(1))
    # Apache spells options ModPagespeedX; the doctor strips the prefix.
    valid = {v[len("ModPagespeed"):] if v.startswith("ModPagespeed") and
             len(v) > len("ModPagespeed") else v for v in valid}
    # Constants that are not option names.
    valid -= {"MPS", "PURGE", "Noop"}
    valid -= removed
    all_src = "".join(read(r) for r in SOURCES)
    for w in WARN_ONLY:
        if f'"{w}"' not in all_src:
            sys.exit(f"doctor_tables: {w} is no longer in the source; update WARN_ONLY")
    rank = scopes()
    return {
        "option-names": sorted(valid, key=str.lower),
        "process-scope-options": sorted((n for n, r in rank.items() if r == 3), key=str.lower),
        "server-scope-options": sorted((n for n, r in rank.items() if r == 2), key=str.lower),
        "removed-options": sorted(removed, key=str.lower),
        "deprecated-options": sorted(WARN_ONLY, key=str.lower),
        "deprecated-filters": sorted(filters),
    }


def render(name, words):
    body = textwrap.fill(" ".join(words), width=76,
                         break_long_words=False, break_on_hyphens=False)
    body = "\n".join("#: " + line for line in body.splitlines())
    return f"# BEGIN doctor-{name}\n{body}\n# END doctor-{name}"


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--write", action="store_true")
    args = ap.parse_args()
    text = SCRIPT.read_text(encoding="utf-8")
    new = text
    for name, words in tables().items():
        pat = re.compile(rf"# BEGIN doctor-{name}\n(?:.*?\n)?# END doctor-{name}", re.S)
        if not pat.search(new):
            sys.exit(f"doctor_tables: no doctor-{name} block in {SCRIPT}")
        new = pat.sub(lambda _m: render(name, words), new)
    if args.write:
        SCRIPT.write_text(new, encoding="utf-8")
        return 0
    if new != text:
        import difflib
        sys.stdout.writelines(difflib.unified_diff(
            text.splitlines(True), new.splitlines(True), "script", "source"))
        print("doctor_tables: the script's tables drifted from the source; "
              "run test/doctor/doctor_tables.py --write", file=sys.stderr)
        return 1
    print("doctor_tables: tables match the source")
    return 0


if __name__ == "__main__":
    sys.exit(main())
