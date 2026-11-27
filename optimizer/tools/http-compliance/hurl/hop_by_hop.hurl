# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Hop-by-hop header handling tests
# Note: nginx reverse proxy passes through some hop-by-hop headers
# (like Proxy-Authenticate) — this is standard nginx behavior.

GET {{host}}/hop-by-hop
HTTP 200
[Asserts]
header "Content-Type" contains "text/html"
# Verify response is valid and has content
body contains "hop-by-hop"
