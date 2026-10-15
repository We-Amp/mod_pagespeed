#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Structural check that Windows files carry an Authenticode signature.

  release_signature_check.py pe  FILE...     each PE file has a certificate table
  release_signature_check.py msi FILE...     each MSI has a digital signature stream
  release_signature_check.py --self-test

This runs on a Linux runner, where the signature itself cannot be validated:
the signing job validates it on Windows (Get-AuthenticodeSignature). This
check only proves that the file that reaches the release carries a signature
at all, so a step that silently handed over an unsigned file is caught at the
job boundary. Exit status 1 names the files without one.
"""

import os
import struct
import sys
import tempfile

# "\x05DigitalSignature" as the UTF-16 name of the MSI signature stream.
MSI_SIG_NAME = "\x05DigitalSignature".encode("utf-16-le")


def pe_signed(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:2] != b"MZ" or len(data) < 0x40:
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x10B:
        dirs = opt + 96
    elif magic == 0x20B:
        dirs = opt + 112
    else:
        raise ValueError("unknown optional header")
    # Data directory 4 is the certificate table; its address is a file offset.
    va, size = struct.unpack_from("<II", data, dirs + 4 * 8)
    return va != 0 and size != 0 and va + size <= len(data)


def msi_signed(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != bytes.fromhex("D0CF11E0A1B11AE1"):
        raise ValueError("not an MSI file")
    return MSI_SIG_NAME in data


def check(kind, files):
    fn = pe_signed if kind == "pe" else msi_signed
    bad = []
    for f in files:
        try:
            ok = fn(f)
        except (OSError, ValueError) as e:
            print("::error::%s: %s" % (os.path.basename(f), e))
            bad.append(f)
            continue
        print("%s: %s" % (os.path.basename(f), "signature present" if ok else "NO SIGNATURE"))
        if not ok:
            bad.append(f)
    return 1 if bad else 0


def _fake_pe(signed, plus=True):
    hdr = bytearray(0x400)
    hdr[0:2] = b"MZ"
    struct.pack_into("<I", hdr, 0x3C, 0x80)
    hdr[0x80:0x84] = b"PE\0\0"
    opt = 0x80 + 24
    struct.pack_into("<H", hdr, opt, 0x20B if plus else 0x10B)
    dirs = opt + (112 if plus else 96)
    if signed:
        struct.pack_into("<II", hdr, dirs + 32, 0x300, 0x80)
    return bytes(hdr)


def self_test():
    fail = 0
    tmp = tempfile.mkdtemp(prefix="sigcheck.")

    def put(name, body):
        p = os.path.join(tmp, name)
        with open(p, "wb") as fh:
            fh.write(body)
        return p

    cases = [
        ("pe", put("s64.dll", _fake_pe(True)), 0),
        ("pe", put("s32.dll", _fake_pe(True, plus=False)), 0),
        ("pe", put("u64.dll", _fake_pe(False)), 1),
        ("pe", put("text.dll", b"hello"), 1),
        ("pe", put("trunc.dll", _fake_pe(True)[:0x340]), 1),
        ("msi", put("s.msi", bytes.fromhex("D0CF11E0A1B11AE1") + b"\0" * 64 + MSI_SIG_NAME), 0),
        ("msi", put("u.msi", bytes.fromhex("D0CF11E0A1B11AE1") + b"\0" * 64), 1),
        ("msi", put("t.msi", b"not an msi"), 1),
    ]
    for kind, path, want in cases:
        if check(kind, [path]) != want:
            print("FAIL: %s %s expected exit %d" % (kind, os.path.basename(path), want))
            fail = 1
    for name in os.listdir(tmp):
        os.remove(os.path.join(tmp, name))
    os.rmdir(tmp)
    if fail == 0:
        print("release_signature_check self-test: ok")
    return fail


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        return self_test()
    if len(argv) < 3 or argv[1] not in ("pe", "msi"):
        print(__doc__)
        return 2
    return check(argv[1], argv[2:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
