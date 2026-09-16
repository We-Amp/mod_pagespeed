# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""pytest entry point that propagates pytest's exit status.

pytest.main() only *returns* its exit code; it does not exit the process.
A py_test wrapper whose __main__ block calls pytest.main(...) and drops
the return value exits 0 no matter how many cases failed, and bazel
reports the target PASS on a red suite -- a silently green lane.
Routing the __main__ block through run_pytest() makes the
process exit code equal pytest's, so a failing case turns the bazel
target red.

The module is also directly executable:

    python3 pytest_main.py path/to/test_x.py -v
"""

import sys

import pytest


def run_pytest(args):
    """Run pytest with args and exit the process with pytest's status."""
    sys.exit(pytest.main(args))


if __name__ == "__main__":
    run_pytest(sys.argv[1:])
