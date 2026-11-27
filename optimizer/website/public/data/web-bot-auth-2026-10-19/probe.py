#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 We-Amp B.V.
"""Probe Web Bot Auth key directories. Usage: probe.py targets.csv > results.tsv
targets.csv columns: operator,sources,url,basis. Needs curl on PATH."""
import csv, datetime, json, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

MT = "application/http-message-signatures-directory+json"
UA = "wba-directory-probe/1.0"
W = "%{http_code}\t%{content_type}\t%{redirect_url}\t%{url_effective}"

def curl(url, follow):
    with tempfile.NamedTemporaryFile() as f:
        args = ["curl", "-s", "-m", "20", "-A", UA, "-H", "Accept: " + MT, "-o", f.name, "-w", W]
        p = subprocess.run(args + (["-L"] if follow else []) + [url], capture_output=True, text=True)
        code, ctype, redir, final = (p.stdout.split("\t") + ["", "", "", ""])[:4]
        return code or "000", ctype, redir, final, open(f.name, "rb").read()

def jwks(body):
    try:
        keys = json.loads(body).get("keys")
    except Exception:
        return None
    if not isinstance(keys, list) or not keys:
        return None
    algs = sorted({"/".join(str(k.get(x)) for x in ("kty", "crv", "alg") if k.get(x)) for k in keys if isinstance(k, dict)})
    return len(keys), ",".join(algs)

def probe(row):
    code, ctype, redir, _, body = curl(row["url"], False)
    fcode, fctype, _, final, fbody = curl(row["url"], True) if code.startswith("3") else (code, ctype, "", row["url"], body)
    strict = jwks(body) if code == "200" else None
    followed = jwks(fbody) if fcode == "200" else None
    if strict:
        verdict = "directory" if ctype.split(";")[0].strip().lower() == MT else "directory (other media type)"
    elif followed:
        verdict = "directory only after redirect"
    else:
        verdict = "no directory at this URL"
    n, algs = strict or followed or ("", "")
    return [row["operator"], row["sources"], row["basis"], row["url"], code, ctype, redir,
            fcode, final, fctype, verdict, n, algs]

if __name__ == "__main__":
    rows = list(csv.DictReader(open(sys.argv[1])))
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    out = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
    out.writerow(["probed_utc", "operator", "sources", "basis", "url", "status", "content_type", "redirect_to",
                  "status_after_redirects", "final_url", "final_content_type", "verdict", "keys", "kty/crv/alg"])
    with ThreadPoolExecutor(8) as ex:
        for r in ex.map(probe, rows):
            out.writerow([stamp] + r)
