# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.
# Fixture stand-in for tools/ci/release_adopt.py: prints the adoption schema.
import json
import sys

if sys.argv[1:] == ["schema", "--print"]:
    print(json.dumps({"adoptable": ["build"], "always_runs": ["version", "adopt", "sign", "validate"],
                      "environment": [], "candidate_only": [], "reuploaded": ["asset-a", "extra-x"]}))
