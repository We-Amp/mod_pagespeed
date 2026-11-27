# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Content-Type correctness tests

# CSS Content-Type
GET {{host}}/etag-test.css
HTTP 200
[Asserts]
header "Content-Type" contains "text/css"

# JavaScript Content-Type
GET {{host}}/tiny.js
HTTP 200
[Asserts]
header "Content-Type" contains "javascript"

# HTML Content-Type
GET {{host}}/small.html
HTTP 200
[Asserts]
header "Content-Type" contains "text/html"

# JPEG Content-Type
GET {{host}}/1x1.jpg
HTTP 200
[Asserts]
header "Content-Type" contains "image"

# PNG Content-Type
GET {{host}}/1x1.png
HTTP 200
[Asserts]
header "Content-Type" contains "image"

# GIF Content-Type
GET {{host}}/1x1.gif
HTTP 200
[Asserts]
header "Content-Type" contains "image"
