#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
"""Assert the module's image-selection Vary list is the optimizer's.

A response served from a selected image variant names, in `Vary`, every
request header the selection read. The module and the optimizer select image
variants by the same rule, and a downstream cache keys a response by that
list, so the two lists must be one: if the optimizer starts selecting by a
header the module does not name, a shared cache in front of the module can
hand one client's variant to another again.

The module mirrors the list as kPsImageSelectionVary in
pagespeed/system/daemon_abi.h. The optimizer defines it as kImageSelectionVary
in lib/classify/capability_mask.h; releases cut before that constant existed
carry the same list as the image row of lib/cache/vary_emission.cc, which is
read instead when the constant is absent. Usage:

    tools/ci/check_image_selection_vary.py <daemon_abi.h> \\
        <optimizer capability_mask.h> [<optimizer vary_emission.cc>]
    tools/ci/check_image_selection_vary.py --self-test
"""

import os
import re
import sys
import tempfile

MODULE_RE = re.compile(r'kPsImageSelectionVary\[\]\s*=\s*((?:"[^"]*"\s*)+);')
PEER_CONSTANT_RE = re.compile(
    r'kImageSelectionVary\s*=\s*((?:"[^"]*"\s*)+);')
PEER_ROW_RE = re.compile(
    r'case\s+ContentType::kImage:.*?return\s+((?:"[^"]*"\s*)+);', re.S)


class DriftError(Exception):
    pass


def _literal(concatenated):
    return "".join(re.findall(r'"([^"]*)"', concatenated))


def tokens(value):
    return [t.strip() for t in value.split(",") if t.strip()]


def module_list(daemon_abi_text):
    m = MODULE_RE.search(daemon_abi_text)
    if not m:
        raise DriftError("could not read kPsImageSelectionVary from daemon_abi.h")
    return _literal(m.group(1))


def peer_list(capability_mask_text, vary_emission_text=None):
    m = PEER_CONSTANT_RE.search(capability_mask_text)
    if m:
        return _literal(m.group(1))
    if vary_emission_text is not None:
        m = PEER_ROW_RE.search(vary_emission_text)
        if m:
            return _literal(m.group(1))
    raise DriftError(
        "could not find the optimizer's image-selection list (kImageSelectionVary "
        "in capability_mask.h, or the image row of vary_emission.cc); the "
        "optimizer moved it somewhere this gate cannot read. Fix the gate "
        "rather than skipping it.")


def check(daemon_abi_text, capability_mask_text, vary_emission_text=None):
    ours = module_list(daemon_abi_text)
    theirs = peer_list(capability_mask_text, vary_emission_text)
    if [t.lower() for t in tokens(ours)] != [t.lower() for t in tokens(theirs)]:
        raise DriftError(
            "the module names %r for a selected image variant but the optimizer "
            "selects by %r; update kPsImageSelectionVary (and the selection "
            "inputs it describes) together with the optimizer" % (ours, theirs))
    return ours


def _read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def self_test():
    abi = 'inline constexpr char kPsImageSelectionVary[] =\n    "Accept, Save-Data, User-Agent, Sec-CH-DPR";\n'
    constant = ('inline constexpr std::string_view kImageSelectionVary =\n'
                '    "Accept, Save-Data, User-Agent, Sec-CH-DPR";\n')
    row = ('    case ContentType::kImage:\n      // note\n'
           '      return "Accept, Save-Data, User-Agent, Sec-CH-DPR";\n'
           '    case ContentType::kHtml:\n      return "Accept-Encoding";\n')
    cases = 0
    # Equal through the constant, through the legacy row, and across a split
    # literal.
    assert check(abi, constant) == "Accept, Save-Data, User-Agent, Sec-CH-DPR"
    assert check(abi, "", row)
    split = constant.replace('"Accept, Save-Data, ', '"Accept, Save-Data, "\n    "')
    assert check(abi, split)
    cases += 3
    # The constant wins over the row when both are present.
    assert check(abi, constant, row.replace("Sec-CH-DPR", "X"))
    cases += 1
    # Drift in either direction, and an order change, fail.
    for peer in (constant.replace(", Sec-CH-DPR", ""),
                 constant.replace("Sec-CH-DPR", "Sec-CH-DPR, Viewport-Width"),
                 constant.replace("Save-Data, User-Agent", "User-Agent, Save-Data")):
        try:
            check(abi, peer)
        except DriftError:
            cases += 1
        else:
            raise AssertionError("drift not caught: %r" % peer)
    # Nothing to read on either side fails loudly.
    for ours, theirs in ((abi, "nothing here"), ("nothing here", constant)):
        try:
            check(ours, theirs)
        except DriftError:
            cases += 1
        else:
            raise AssertionError("unreadable input accepted")
    # And the file path entry point.
    with tempfile.TemporaryDirectory() as d:
        a, c = os.path.join(d, "a.h"), os.path.join(d, "c.h")
        with open(a, "w", encoding="utf-8") as f:
            f.write(abi)
        with open(c, "w", encoding="utf-8") as f:
            f.write(constant)
        assert main([a, c]) == 0
        cases += 1
    print("check_image_selection_vary self-test: %d cases passed" % cases)
    return 0


def main(argv):
    if argv == ["--self-test"]:
        return self_test()
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        value = check(_read(argv[0]), _read(argv[1]),
                      _read(argv[2]) if len(argv) == 3 else None)
    except DriftError as e:
        print("::error::%s" % e)
        return 1
    print("image-selection Vary matches the optimizer: %s" % value)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
