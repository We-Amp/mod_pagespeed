# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Read declared and resolved module versions without running Bazel.

Shared by tools/generate-sbom.py and tools/check-module-versions.py.

- Declared: the root MODULE.bazel `bazel_dep(name, version)` calls and the
  `single_version_override(module_name, version)` pins.
- Resolved: MODULE.bazel.lock. Its `registryFileHashes` records the registry's
  `source.json` only for the module versions the resolution selected (one per
  module; modules with a non-registry override such as local_path_override have
  none), so the version in that URL is the version Bazel builds.
"""

import json
import os
import re

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_BAZEL = os.path.join(REPO_ROOT, "MODULE.bazel")
MODULE_LOCK = os.path.join(REPO_ROOT, "MODULE.bazel.lock")

_SOURCE_JSON = re.compile(r"/modules/(?P<name>[^/]+)/(?P<version>[^/]+)/source\.json$")
_BCR_SUFFIX = re.compile(r"\.bcr\.\d+$")


class VersionError(Exception):
    pass


def upstream_version(version):
    """`2.3.3.bcr.2` -> `2.3.3`: a BCR patch release builds the upstream release."""
    return _BCR_SUFFIX.sub("", version)


def _calls(text, func):
    """Yield the argument text of every top-level `func(...)` call."""
    for m in re.finditer(r"(?m)^\s*" + re.escape(func) + r"\(", text):
        depth, i = 1, m.end()
        while depth and i < len(text):
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif c in "\"'":
                i = text.index(c, i + 1)
            i += 1
        yield text[m.end():i - 1]


def _kwarg(body, key):
    m = re.search(r"\b" + key + r'\s*=\s*"([^"]*)"', body)
    return m.group(1) if m else None


def parse_module_bazel(text):
    """Return ({dep name: declared version}, {module: single_version_override version})."""
    text = re.sub(r"(?m)#.*$", "", text)
    deps, overrides = {}, {}
    for body in _calls(text, "bazel_dep"):
        name = _kwarg(body, "name")
        if name:
            deps[name] = _kwarg(body, "version") or ""
    for body in _calls(text, "single_version_override"):
        name = _kwarg(body, "module_name")
        version = _kwarg(body, "version")
        if name and version:
            overrides[name] = version
    return deps, overrides


def resolved_from_lock(lock):
    """Return {module: resolved version} from a parsed MODULE.bazel.lock."""
    resolved = {}
    for url in lock.get("registryFileHashes", {}):
        m = _SOURCE_JSON.search(url)
        if not m:
            continue
        name, version = m.group("name"), m.group("version")
        if name in resolved and resolved[name] != version:
            raise VersionError(
                f"MODULE.bazel.lock selects two versions of {name}: "
                f"{resolved[name]} and {version}")
        resolved[name] = version
    return resolved


def load_declared(path=MODULE_BAZEL):
    with open(path) as f:
        return parse_module_bazel(f.read())


def load_resolved(path=MODULE_LOCK):
    with open(path) as f:
        return resolved_from_lock(json.load(f))
