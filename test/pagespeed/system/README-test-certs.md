<!--
Licensed to the Apache Software Foundation (ASF) under one or more contributor
license agreements. See the NOTICE file distributed with this work for
additional information regarding copyright ownership. The ASF licenses this
file to you under the Apache License, Version 2.0 (the "License"); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
-->

# Regenerating `curl_test_certs.h`

`curl_test_certs.h` embeds the PEM material used by `CurlUrlAsyncFetcherTest`'s
hermetic local HTTPS server (`curl_test_server.{h,cc}`). The fixtures let the
test run with **zero public-internet dependency**: it no longer touches
httpbin.org, self-signed.badssl.com, or www.google.com.

There are three identities:

- **Test root CA** (`kTestCaCertPem`) — the test injects this as libcurl's
  `CAINFO` so the fetcher trusts the server leaf in the "succeeds" cases.
- **Server leaf** (`kTestServerCertPem` / `kTestServerKeyPem`) — signed by the
  test CA, with `subjectAltName = IP:127.0.0.1, DNS:localhost` so curl's
  `VERIFYHOST=2` check passes when connecting to `127.0.0.1`.
- **Self-signed leaf** (`kTestSelfSignedCertPem` / `kTestSelfSignedKeyPem`) —
  its own root, NOT in the trusted bundle, used for the "fails for self-signed"
  cases.

Validity is 100 years so the fixtures do not expire in CI.

To regenerate (requires `openssl`):

```sh
DAYS=36500

# Test CA
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days $DAYS \
  -subj "/O=mod_pagespeed test CA/CN=mod_pagespeed Test Root CA"

# Server leaf signed by the test CA, SAN = IP:127.0.0.1 + localhost
cat > leaf.cnf <<'EOF'
[req]
distinguished_name = dn
req_extensions = v3_req
prompt = no
[dn]
O = mod_pagespeed test
CN = 127.0.0.1
[v3_req]
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt
[alt]
IP.1 = 127.0.0.1
DNS.1 = localhost
EOF
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -config leaf.cnf
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days $DAYS -extfile leaf.cnf -extensions v3_req

# Self-signed leaf (own root), SAN = IP:127.0.0.1 + localhost
openssl req -x509 -newkey rsa:2048 -nodes -keyout selfsigned.key \
  -out selfsigned.crt -days $DAYS \
  -subj "/O=mod_pagespeed test self-signed/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
  -addext "basicConstraints=CA:FALSE"
```

Then paste each `*.crt` / `*.key` into the corresponding `R"PEM(...)PEM"`
literal in `curl_test_certs.h`. Sanity-check the chain:

```sh
openssl verify -CAfile ca.crt server.crt        # must print: server.crt: OK
openssl verify -CAfile ca.crt selfsigned.crt    # must FAIL (self-signed)
```
