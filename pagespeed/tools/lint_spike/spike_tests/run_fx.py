#!/usr/bin/env python3
# usage: run_fx.py <lint binary> <fx dir>
import json, subprocess, sys
lint, fx = sys.argv[1], sys.argv[2]
ok = 0
rows = [l.split() for l in open(f"{fx}/expected.tsv") if l.strip()]
for n, rule, line in rows:
    ht = n in ("12", "13")
    cmd = [lint, "--root", fx] + (["--htaccess", f"bad/{n}.conf"] if ht else [f"bad/{n}.conf"])
    p = subprocess.run(cmd, capture_output=True, timeout=30)
    try:
        d = json.loads(p.stdout)
    except Exception:
        print(n, "FAIL no json rc", p.returncode, p.stderr[:200]); continue
    hit = [f for f in d["findings"] if f["rule"] == rule and f["line"] == int(line)]
    got = [(f["rule"], f["line"], f["file"]) for f in d["findings"]]
    status = "PASS" if hit else "FAIL"
    ok += bool(hit)
    print(n, status, "want", rule, line, "rc", p.returncode, "got", got)
print(f"{ok}/{len(rows)} seeded configs gave the expected rule and line")
