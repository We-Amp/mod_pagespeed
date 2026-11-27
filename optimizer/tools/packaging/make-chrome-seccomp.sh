#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Regenerate deploy/chrome-seccomp.json.
#
#   make-chrome-seccomp.sh [--moby-ref vX.Y.Z] [--out PATH]
#
# The shipped profile is Docker's DEFAULT seccomp profile with exactly four
# calls unblocked, so that headless Chrome can build its own layer-1 sandbox
# inside a container.  It is generated rather than hand-written for one
# reason: a hand-maintained fork of a 30-entry upstream profile silently
# stops tracking upstream, and the whole value of "default plus four" is
# that the "default" half stays current.  Regenerate against a newer moby
# tag and commit the diff; the assertions in build-optimizer-*.sh --self-test
# and the rig check the four deltas, not the upstream body.
#
# The four deltas, and why each one (Chrome's namespace sandbox,
# sandbox/linux/services/{namespace_sandbox,credentials}.cc):
#
#   clone     Docker's default allows clone but ARGUMENT-FILTERS the
#             CLONE_NEW* namespace flags away unless the container holds
#             CAP_SYS_ADMIN.  Chrome's zygote clones with
#             CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET; the filter is what
#             makes that fail, which is why the usual answer is
#             --no-sandbox.  Here the argument filter is dropped.
#   clone3    Docker's default returns ENOSYS for clone3 without
#             CAP_SYS_ADMIN (deliberately, so glibc falls back to clone);
#             allowed here so a newer glibc/Chrome does not take a path the
#             fallback no longer covers.
#   unshare   MoveToNewUserNS() calls unshare(CLONE_NEWUSER).
#   chroot    DropFileSystemAccess() chroots the sandboxed child into an
#             empty directory.
#
# What this profile deliberately does NOT unblock: mount, umount2,
# pivot_root, setns, and everything else CAP_SYS_ADMIN gates in the upstream
# profile.  Chrome's namespace sandbox does not need them, and a profile
# that hands out the whole @mount family is not meaningfully tighter than
# --privileged.
#
# It is also NOT sufficient on its own: the container still needs
# unprivileged user namespaces available from the host kernel, and the
# daemon must not be running as uid 0 (Chrome refuses to sandbox itself as
# root; the daemon reports that as browser_sandbox=unavailable with the
# reason named).  See deploy/README.md.
set -euo pipefail

MOBY_REF="v27.3.1"
# The sha256 of the UPSTREAM profile this generator was last run against.
# Without it, "regenerate against a newer tag" silently accepts whatever the
# network returned -- including a profile someone else changed -- and the
# committed result would carry a provenance record it had not verified.
# Bump both together, deliberately, and read the diff.
MOBY_SHA256="9c1025c88ccaa517b648da571961838744ea2137f176bfe6a48b21294cae9c76"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="$REPO/deploy/chrome-seccomp.json"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --moby-ref) MOBY_REF="$2"; MOBY_SHA256=""; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "usage: make-chrome-seccomp.sh [--moby-ref REF] [--out PATH]" >&2; exit 2 ;;
  esac
done

URL="https://raw.githubusercontent.com/moby/moby/${MOBY_REF}/profiles/seccomp/default.json"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "fetching $URL"
curl -fsSL -o "$TMP/default.json" "$URL"
SRC_SHA="$(sha256sum "$TMP/default.json" | cut -d' ' -f1)"
if [[ -n "$MOBY_SHA256" ]]; then
  if [[ "$SRC_SHA" != "$MOBY_SHA256" ]]; then
    echo "error: upstream profile at $MOBY_REF hashes $SRC_SHA," >&2
    echo "       but this generator is pinned to $MOBY_SHA256." >&2
    echo "       Either the tag moved or the fetch was tampered with. Review" >&2
    echo "       the diff, then update MOBY_SHA256 in this script." >&2
    exit 1
  fi
  echo "upstream profile matches the pinned sha256"
else
  echo "notice: --moby-ref given, pin not checked; update MOBY_SHA256 after review" >&2
fi

MOBY_REF="$MOBY_REF" SRC_SHA="$SRC_SHA" OUT="$OUT" python3 - "$TMP/default.json" <<'PYEOF'
import json, os, sys

src = json.load(open(sys.argv[1]))
CHROME = ["clone", "clone3", "unshare", "chroot"]

out = []
for entry in src["syscalls"]:
    names = entry.get("names", [])
    # Drop every upstream rule that argument-filters or ENOSYS-es one of the
    # four: leaving them in front of the unconditional allow below would win
    # (libseccomp keeps the first matching rule for a syscall).
    if any(n in CHROME for n in names) and (
        "args" in entry or entry.get("action") != "SCMP_ACT_ALLOW"
    ):
        remaining = [n for n in names if n not in CHROME]
        if not remaining:
            continue
        entry = dict(entry, names=remaining)
    out.append(entry)

out.append({
    "names": CHROME,
    "action": "SCMP_ACT_ALLOW",
    "comment": (
        "Unconditionally allowed so headless Chrome can "
        "build its own layer-1 namespace sandbox. Everything else is "
        "Docker's default profile."
    ),
})

src["syscalls"] = out
src["_weamp"] = {
    "generated_by": "tools/packaging/make-chrome-seccomp.sh",
    "base": "moby/moby profiles/seccomp/default.json",
    "base_ref": os.environ["MOBY_REF"],
    "base_sha256": os.environ["SRC_SHA"],
    "delta": CHROME,
    "note": (
        "Docker's default seccomp profile plus four calls Chrome's namespace "
        "sandbox needs. Use with: docker run --security-opt "
        "seccomp=/path/to/chrome-seccomp.json ... Regenerate with the script "
        "above; do not hand-edit."
    ),
}
with open(os.environ["OUT"], "w") as f:
    json.dump(src, f, indent=2, sort_keys=False)
    f.write("\n")
print("wrote", os.environ["OUT"], "from moby", os.environ["MOBY_REF"], SRC := os.environ["SRC_SHA"][:12])
PYEOF

# Tripwire: the four must be unconditionally allowed, and nothing from the
# @mount family may have leaked in with them.
python3 - "$OUT" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
allowed = set()
for e in d["syscalls"]:
    if e.get("action") == "SCMP_ACT_ALLOW" and "args" not in e \
       and not e.get("includes") and not e.get("excludes"):
        allowed.update(e.get("names", []))
missing = [n for n in ("clone", "clone3", "unshare", "chroot") if n not in allowed]
leaked = [n for n in ("mount", "umount2", "pivot_root", "setns") if n in allowed]
# The property that actually makes this profile work, and the one a naive
# "is the name present?" check misses entirely: libseccomp keeps the FIRST
# matching rule per syscall, so an upstream entry that argument-filters
# `clone` and still sits ahead of our unconditional allow would win and the
# CLONE_NEW* flags would stay blocked -- with all four names present.
# A CONDITIONAL ALLOW (upstream's `includes: {caps: [...]}` entries) is not
# a hazard: it permits the call for containers that hold the capability and
# is simply redundant next to our unconditional allow. The hazard is a
# surviving rule that RESTRICTS -- an argument filter, or any non-allow
# action. Those are exactly what the generator strips above; this asserts it
# actually did.
shadowed = []
for e in d["syscalls"]:
    hit = set(e.get("names", [])) & {"clone", "clone3", "unshare", "chroot"}
    if not hit:
        continue
    if "args" in e or e.get("action") != "SCMP_ACT_ALLOW":
        shadowed.extend(sorted(hit))
if missing or leaked or shadowed:
    print("FAIL: missing=%s leaked=%s conditionally-restricted=%s"
          % (missing, leaked, sorted(set(shadowed))), file=sys.stderr)
    sys.exit(1)
print("ok: chrome-seccomp.json unconditionally allows %s and leaks none of "
      "the @mount family" % ", ".join(sorted(allowed & {"clone","clone3","unshare","chroot"})))
PYEOF
