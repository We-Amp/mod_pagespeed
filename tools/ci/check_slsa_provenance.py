#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Check the SLSA provenance of a release set before it is published.

The release lanes call the SLSA project's generic generator
(generator_generic_slsa3.yml), which uploads `multiple.intoto.jsonl`. From
v2 on that file is a Sigstore bundle (`mediaType`
application/vnd.dev.sigstore.bundle..., the statement in
`dsseEnvelope.payload`); older generators wrote a bare DSSE envelope
(`payload` at the top level). Both are read. The statement must be an
in-toto statement with an SLSA provenance predicate (the generic generator
writes https://slsa.dev/provenance/v0.2), and every subject must be a file
of DIR with that sha256; SHA256SUMS must be a subject.

With --exact the subjects must also be exactly the files that DIR/SHA256SUMS
lists plus SHA256SUMS itself (a lane whose provenance covers the whole set).
Without it the check is one way: the module release lane leaves out the
pinned optimizer packages, which the optimizer's own release attests.

The signature is not verified here (slsa-verifier does that, as a user
would); this is the fail-closed consistency check between the statement and
the bytes about to be released.

    check_slsa_provenance.py verify PROVENANCE DIR [--exact]
    check_slsa_provenance.py --self-test

Standard library only, like the other CI tools.
"""

import base64
import copy
import hashlib
import json
import os
import shutil
import sys
import tempfile

BUNDLE_MEDIA_PREFIX = "application/vnd.dev.sigstore.bundle"
INTOTO_PAYLOAD_TYPE = "application/vnd.in-toto+json"
STATEMENT_PREFIX = "https://in-toto.io/Statement/"
SLSA_PREFIX = "https://slsa.dev/provenance/"
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata", "slsa-provenance",
                       "generic-v2.1.0.intoto.jsonl")


class ProvenanceError(Exception):
    pass


def statement_of(text):
    """The in-toto statement (a dict) of a provenance file's text."""
    lines = [l for l in text.splitlines() if l.strip()]
    if len(lines) != 1:
        raise ProvenanceError("expected exactly one JSON document, found %d non-empty lines" % len(lines))
    try:
        doc = json.loads(lines[0])
    except ValueError as e:
        raise ProvenanceError("not JSON: %s" % e)
    if not isinstance(doc, dict):
        raise ProvenanceError("not a JSON object")
    if "dsseEnvelope" in doc:
        media = doc.get("mediaType")
        if not isinstance(media, str) or not media.startswith(BUNDLE_MEDIA_PREFIX):
            raise ProvenanceError("a bundle whose mediaType %r is not %s*" % (media, BUNDLE_MEDIA_PREFIX))
        env = doc["dsseEnvelope"]
    elif "payload" in doc:
        env = doc
    else:
        raise ProvenanceError("neither a Sigstore bundle (dsseEnvelope) nor a DSSE envelope (payload)")
    if not isinstance(env, dict) or not isinstance(env.get("payload"), str):
        raise ProvenanceError("the envelope has no payload")
    if env.get("payloadType") != INTOTO_PAYLOAD_TYPE:
        raise ProvenanceError("payloadType %r is not %s" % (env.get("payloadType"), INTOTO_PAYLOAD_TYPE))
    if not env.get("signatures"):
        raise ProvenanceError("the envelope has no signature")
    try:
        st = json.loads(base64.b64decode(env["payload"], validate=True))
    except (ValueError, TypeError) as e:
        raise ProvenanceError("the payload is not base64 JSON: %s" % e)
    if not isinstance(st, dict):
        raise ProvenanceError("the statement is not a JSON object")
    if not str(st.get("_type", "")).startswith(STATEMENT_PREFIX):
        raise ProvenanceError("_type %r is not an in-toto statement" % st.get("_type"))
    if not str(st.get("predicateType", "")).startswith(SLSA_PREFIX):
        raise ProvenanceError("predicateType %r is not an SLSA provenance" % st.get("predicateType"))
    return st


def subjects_of(st):
    """[(name, sha256)] of a statement, checked for shape."""
    subs = st.get("subject")
    if not isinstance(subs, list) or not subs:
        raise ProvenanceError("the statement has no subject")
    out, seen = [], set()
    for s in subs:
        name = s.get("name") if isinstance(s, dict) else None
        digest = (s.get("digest") or {}).get("sha256") if isinstance(s, dict) else None
        if not isinstance(name, str) or not name or "/" in name or "\\" in name or name.startswith("."):
            raise ProvenanceError("subject name %r is not a plain file name" % (name,))
        if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
            raise ProvenanceError("subject %r has no sha256 digest" % name)
        if name in seen:
            raise ProvenanceError("subject %r appears twice" % name)
        seen.add(name)
        out.append((name, digest))
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sums_names(path):
    names = []
    with open(path) as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split(None, 1)
            if len(parts) != 2:
                raise ProvenanceError("malformed SHA256SUMS row %r" % line[:80])
            names.append(parts[1].lstrip("*"))
    return names


def verify(prov_path, directory, exact=False):
    with open(prov_path) as fh:
        st = statement_of(fh.read())
    subs = subjects_of(st)
    for name, digest in subs:
        p = os.path.join(directory, name)
        if not os.path.isfile(p):
            raise ProvenanceError("subject %r is not a file of the set" % name)
        got = sha256_file(p)
        if got != digest:
            raise ProvenanceError("subject %r: sha256 %s, the provenance says %s" % (name, got, digest))
    names = set(n for n, _ in subs)
    if "SHA256SUMS" not in names:
        raise ProvenanceError("SHA256SUMS is not a subject of the provenance")
    if exact:
        want = set(sums_names(os.path.join(directory, "SHA256SUMS"))) | {"SHA256SUMS"}
        if names != want:
            raise ProvenanceError("the subjects are not the set: only in the provenance %s, only in SHA256SUMS %s"
                                  % (sorted(names - want), sorted(want - names)))
    return st, subs


# --------------------------------------------------------------------------
def _self_test():
    fails = []

    def check(desc, ok, detail=""):
        print("%s %s%s" % ("ok  " if ok else "FAIL", desc, "" if ok else "  <-  " + detail))
        if not ok:
            fails.append(desc)

    def refused(desc, fn, want):
        try:
            fn()
        except ProvenanceError as e:
            check(desc, want in str(e), "refused, but with %r" % str(e))
            return
        check(desc, False, "accepted")

    with open(FIXTURE) as fh:
        real_text = fh.read()
    real = json.loads(real_text)
    st = statement_of(real_text)
    subs = subjects_of(st)
    check("the real v2.1.0 generic bundle is read: a Sigstore bundle with the statement in dsseEnvelope",
          real["mediaType"].startswith(BUNDLE_MEDIA_PREFIX) and "payload" not in real and len(subs) == 1)
    check("the real bundle's predicate is SLSA provenance v0.2 and its subject is the fixture binary",
          st["predicateType"] == "https://slsa.dev/provenance/v0.2"
          and subs[0] == ("gha_generic-binary-linux-amd64-workflow_dispatch",
                          "376e7e01348585b6e6643bc6663146b9d525d9b41228bf180eaeb4d4a3706caa"), repr(subs))

    def bundle(statement, mutate=None):
        d = copy.deepcopy(real)
        d["dsseEnvelope"]["payload"] = base64.b64encode(json.dumps(statement).encode()).decode()
        if mutate:
            mutate(d)
        return json.dumps(d)

    def dsse(statement):
        env = copy.deepcopy(real["dsseEnvelope"])
        env["payload"] = base64.b64encode(json.dumps(statement).encode()).decode()
        return json.dumps(env)

    refused("refuse: a bundle with another mediaType",
            lambda: statement_of(bundle(st, lambda d: d.update(mediaType="application/json"))), "mediaType")
    refused("refuse: a bundle without signatures",
            lambda: statement_of(bundle(st, lambda d: d["dsseEnvelope"].update(signatures=[]))), "no signature")
    refused("refuse: another payloadType",
            lambda: statement_of(bundle(st, lambda d: d["dsseEnvelope"].update(payloadType="text/plain"))), "payloadType")
    refused("refuse: another predicate", lambda: statement_of(bundle(dict(st, predicateType="https://example.com/x"))),
            "predicateType")
    refused("refuse: two documents", lambda: statement_of(real_text + "\n" + real_text), "exactly one")
    refused("refuse: neither form", lambda: statement_of('{"x": 1}'), "neither")

    work = tempfile.mkdtemp()
    try:
        d = os.path.join(work, "set")
        os.mkdir(d)
        for n, body in (("a.deb", b"a"), ("b.rpm", b"bb"), ("manifest.json", b"{}")):
            with open(os.path.join(d, n), "wb") as fh:
                fh.write(body)
        with open(os.path.join(d, "SHA256SUMS"), "w") as fh:
            for n in ("a.deb", "b.rpm", "manifest.json"):
                fh.write("%s  %s\n" % (sha256_file(os.path.join(d, n)), n))

        def stmt(names):
            return dict(st, subject=[{"name": n, "digest": {"sha256": sha256_file(os.path.join(d, n))}} for n in names])

        def run(text, exact=False):
            p = os.path.join(work, "p.intoto.jsonl")
            with open(p, "w") as fh:
                fh.write(text)
            return verify(p, d, exact)

        allnames = ["a.deb", "b.rpm", "manifest.json", "SHA256SUMS"]
        try:
            run(bundle(stmt(allnames)), exact=True)
            check("accept: a bundle whose subjects are exactly the set (--exact)", True)
        except ProvenanceError as e:
            check("accept: a bundle whose subjects are exactly the set (--exact)", False, str(e))
        try:
            run(dsse(stmt(allnames)), exact=True)
            check("accept: a bare DSSE envelope (older generators)", True)
        except ProvenanceError as e:
            check("accept: a bare DSSE envelope (older generators)", False, str(e))
        try:
            run(bundle(stmt(["a.deb", "manifest.json", "SHA256SUMS"])))
            check("accept: one-way, a subset of the set", True)
        except ProvenanceError as e:
            check("accept: one-way, a subset of the set", False, str(e))
        refused("refuse: --exact with a file of SHA256SUMS that is no subject",
                lambda: run(bundle(stmt(["a.deb", "manifest.json", "SHA256SUMS"])), exact=True), "only in SHA256SUMS")
        refused("refuse: SHA256SUMS is no subject", lambda: run(bundle(stmt(["a.deb"]))), "SHA256SUMS is not a subject")
        bad = stmt(allnames)
        bad["subject"][0]["digest"]["sha256"] = "0" * 64
        refused("refuse: a subject whose digest differs", lambda: run(bundle(bad)), "the provenance says")
        missing = stmt(allnames)
        missing["subject"].append({"name": "gone.zip", "digest": {"sha256": "1" * 64}})
        refused("refuse: a subject that is not a file of the set", lambda: run(bundle(missing)), "not a file of the set")
        path = stmt(allnames)
        path["subject"][0]["name"] = "../a.deb"
        refused("refuse: a subject name with a path", lambda: run(bundle(path)), "plain file name")
    finally:
        shutil.rmtree(work)

    if fails:
        print("check_slsa_provenance self-test: %d failure(s)" % len(fails))
        return 1
    print("check_slsa_provenance self-test: all passed")
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return _self_test()
    args = [a for a in argv if a != "--exact"]
    if len(args) != 3 or args[0] != "verify":
        sys.stderr.write(__doc__)
        return 2
    try:
        _st, subs = verify(args[1], args[2], exact="--exact" in argv)
    except (ProvenanceError, OSError) as e:
        print("::error::SLSA provenance: %s" % e)
        return 1
    print("SLSA provenance: %d subject(s), each a file of the set with its digest%s"
          % (len(subs), " (exactly the set)" if "--exact" in argv else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
