# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Cache-Control directive passthrough tests
# Tests through the pagespeed proxy (port 8080)

# no-store forwarded
GET {{host}}/cc/no-store
HTTP 200
[Asserts]
header "Cache-Control" contains "no-store"

# no-cache forwarded
GET {{host}}/cc/no-cache
HTTP 200
[Asserts]
header "Cache-Control" contains "no-cache"

# private forwarded
GET {{host}}/cc/private
HTTP 200
[Asserts]
header "Cache-Control" contains "private"

# max-age forwarded
GET {{host}}/cc/max-age
HTTP 200
[Asserts]
header "Cache-Control" contains "max-age"

# s-maxage forwarded
GET {{host}}/cc/s-maxage
HTTP 200
[Asserts]
header "Cache-Control" contains "s-maxage"
