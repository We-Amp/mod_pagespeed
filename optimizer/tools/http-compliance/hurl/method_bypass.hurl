# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

# Method bypass tests
# POST/PUT/DELETE should not be intercepted by pagespeed

# POST has no X-PageSpeed
POST {{host}}/echo
Content-Type: text/plain
```
test body
```
HTTP 200
[Asserts]
header "X-PageSpeed" not exists

# PUT has no X-PageSpeed
PUT {{host}}/echo
Content-Type: text/plain
```
update body
```
HTTP 200
[Asserts]
header "X-PageSpeed" not exists

# DELETE has no X-PageSpeed
DELETE {{host}}/echo
HTTP 200
[Asserts]
header "X-PageSpeed" not exists
