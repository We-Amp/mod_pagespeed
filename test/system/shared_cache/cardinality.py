# SPDX-License-Identifier: Apache-2.0
"""Shared-cache key cardinality for one image URL under a weighted traffic mix.

Classifies every User-Agent with the shipped classifier (ps_classify from the
optimizer's libpagespeed.so) and counts, for N requests sampled by weight, how
many copies a cache that keys on `User-Agent` (as `Vary` tells it to) holds
per image and Accept/Save-Data value, against how many device classes
(viewport x density) the selection actually distinguishes.

    python3 -I cardinality.py user-agents.json /usr/lib/x86_64-linux-gnu/libpagespeed.so

user-agents.json is a list of {"userAgent", "weight"} records, for example
dist/user-agents.json of the `user-agents` npm package (a weighted sample of
real browser traffic).
"""
import ctypes
import json
import random
import sys

data = json.load(open(sys.argv[1]))
lib = ctypes.CDLL(sys.argv[2])
lib.ps_classify.restype = ctypes.c_uint32
lib.ps_classify.argtypes = [ctypes.c_char_p] * 4


def device_class(ua):
    mask = lib.ps_classify(b"image/jpeg", ua.encode(), None, None)
    return (mask >> 2) & 3, (mask >> 4) & 1  # viewport, density


uas = [d["userAgent"] for d in data]
weights = [d["weight"] for d in data]
classes = {ua: device_class(ua) for ua in set(uas)}
names = {0: "mobile", 1: "tablet", 2: "desktop"}
print(f"dataset: {len(data)} UA records, {len(set(uas))} distinct strings")
print("classes in dataset:", sorted({(names.get(v, v), "2x" if d else "1x") for v, d in classes.values()}))
rng = random.Random(1)
for n in (1000, 10000, 100000):
    sample = rng.choices(uas, weights=weights, k=n)
    distinct_ua = len(set(sample))
    distinct_cls = len({classes[u] for u in sample})
    print(f"requests={n:>6}: Vary: User-Agent keys={distinct_ua:>5}  device-class keys={distinct_cls}  "
          f"ratio={distinct_ua / distinct_cls:.0f}x")
