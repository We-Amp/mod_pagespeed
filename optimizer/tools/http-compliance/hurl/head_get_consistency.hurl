# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# HEAD vs GET consistency tests
# HEAD should return same status and headers, empty body

# GET request
GET {{host}}/small.html
HTTP 200
[Captures]
get_status: status
get_content_type: header "Content-Type"

# HEAD should return same status
HEAD {{host}}/small.html
HTTP 200
[Asserts]
header "Content-Type" == {{get_content_type}}
body == ""

# HEAD on CSS
HEAD {{host}}/etag-test.css
HTTP 200
[Asserts]
body == ""

# HEAD on image
HEAD {{host}}/1x1.jpg
HTTP 200
[Asserts]
body == ""
