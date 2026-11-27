# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Conditional request: ETag capture → If-None-Match → 304
# Tests against the origin directly (port 8081)

# Step 1: GET to capture ETag
GET {{origin}}/etag-test.css
HTTP 200
[Captures]
etag: header "ETag"

# Step 2: Conditional request with captured ETag
GET {{origin}}/etag-test.css
If-None-Match: {{etag}}
HTTP 304

# Step 3: If-Modified-Since with future date → 304
GET {{origin}}/etag-test.css
If-Modified-Since: Sun, 01 Jan 2090 00:00:00 GMT
HTTP 304

# Step 4: If-Modified-Since with past date → 200
GET {{origin}}/etag-test.css
If-Modified-Since: Sun, 01 Jan 2000 00:00:00 GMT
HTTP 200
