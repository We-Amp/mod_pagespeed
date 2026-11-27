# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

licenses(["notice"])  # Apache 2

# Engine and live conformance cases for the HTML cache, the adversarial
# origin and the agent vectors. The engine runner and the live runner read the
# cases file; //test/pagespeed/system:
# daemon_html_conformance_cases_test checks its shape.
exports_files(
    [
        "agent_vectors.txt",
        "conformance_cases.json",
    ],
    visibility = ["//visibility:public"],
)

# Static pages the lanes serve from the web server itself (SSI and
# static-file rows).
filegroup(
    name = "static_pages",
    srcs = [
        "static/ssi-fragment.txt",
        "static/ssi-literal.shtml",
        "static/static.html",
    ],
    visibility = ["//visibility:public"],
)

# The adversarial origin the live lanes put behind each web server.
py_library(
    name = "adversarial_origin",
    srcs = ["adversarial_origin.py"],
    imports = ["."],
    visibility = ["//visibility:public"],
)

# Origin-level self-test of every adversarial origin endpoint.
py_test(
    name = "test_adversarial_origin",
    srcs = ["test_adversarial_origin.py"],
    data = [":static_pages"],
    python_version = "PY3",
    deps = [":adversarial_origin"],
)

# The shared agent vector file against a reference matcher.
py_test(
    name = "test_agent_vectors",
    srcs = ["test_agent_vectors.py"],
    data = ["agent_vectors.txt"],
    python_version = "PY3",
)
