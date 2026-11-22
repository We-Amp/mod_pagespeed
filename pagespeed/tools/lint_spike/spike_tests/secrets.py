#!/usr/bin/env python3
# Rule x secret-option test: every directive whose value is not on the
# allowlist is seeded with a unique marker in every context (server, vhost,
# directory, htaccess, conditional), with the right arity, one argument too
# many, one too few, and with values that trip the parser (bad URL, bad
# number).  The marker must never appear in stdout or stderr.
# usage: secrets.py <lint> <workdir>
import os, subprocess, sys
lint, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
rows = []
for line in subprocess.run([lint, "--list-directives"], capture_output=True,
                           text=True, check=True).stdout.splitlines():
    if line.startswith("#"):
        continue
    name, n, safe, ov = line.split("\t")
    rows.append((name, int(n), safe == "1"))
unsafe = [r for r in rows if not r[2]]
# Unknown and misspelled names are masked too.
unsafe += [("ModPagespeedUrlSigningKeyy", 1, False), ("ModPagespeedBogus", 2, False)]
print(f"{len(rows)} directives, {len(unsafe)} with masked values")

def marker(i, j):
    return f"SEKRIT{i:03d}x{j}Q"

def vals(i, n, k):
    # k selects the value shape: plain marker, URL with marker, number+marker
    shapes = [lambda j: marker(i, j),
              lambda j: f"http://u:{marker(i, j)}@%40%40/{marker(i, j)}",
              lambda j: f"9{marker(i, j)}"]
    return [shapes[k](j) for j in range(n)]

server, vhost, direc, cond, ht = [], [], [], [], []
markers = []
for i, (name, n, _) in enumerate(unsafe):
    for k in range(3):
        for nargs in (n, n + 1, max(n - 1, 0)):
            v = vals(i, nargs, k)
            markers.extend(v)
            ln = name + " " + " ".join('"%s"' % x for x in v)
            if name.startswith("<"):
                continue  # <ModPagespeedIf> is a section
            server.append(ln)
            vhost.append("  " + ln)
            direc.append("    " + ln)
            cond.append("  " + ln)
            ht.append(ln)
cfg = (["ModPagespeed on"] + server +
       ["<VirtualHost *:80>"] + vhost +
       ["  <Directory /var/www>"] + direc + ["  </Directory>", "</VirtualHost>"] +
       ["<ModPagespeedIf spdy>"] + cond + ["</ModPagespeedIf>"])
open(os.path.join(work, "secrets.conf"), "w").write("\n".join(cfg) + "\n")
open(os.path.join(work, "secrets.htaccess"), "w").write("\n".join(ht) + "\n")
markers = sorted(set(markers))
leaks = 0
total_findings = 0
for args in (["secrets.conf"], ["--htaccess", "secrets.htaccess"]):
    p = subprocess.run([lint, "--root", work] + args, capture_output=True)
    out = p.stdout + p.stderr
    total_findings += out.count(b'"rule"')
    for m in markers:
        if m.encode() in out or b"SEKRIT" in out:
            leaks += 1
            print("LEAK", m, args)
    print(args, "rc", p.returncode, "bytes", len(out), "findings", out.count(b'"rule"'))
# Every marker prefix must also be absent ("SEKRIT" itself).
print(f"{len(markers)} distinct seeded values, {total_findings} findings, "
      f"{leaks} leaks")
sys.exit(1 if leaks else 0)
