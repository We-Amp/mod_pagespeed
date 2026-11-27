# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Error response passthrough tests
# Verifies 4xx/5xx from origin pass through the proxy

# 404
GET {{host}}/error/404
HTTP 404

# 403
GET {{host}}/error/403
HTTP 403

# 500
GET {{host}}/error/500
HTTP 500

# 502
GET {{host}}/error/502
HTTP 502

# 503
GET {{host}}/error/503
HTTP 503

# Nonexistent file
GET {{host}}/does-not-exist-at-all.html
HTTP 404
