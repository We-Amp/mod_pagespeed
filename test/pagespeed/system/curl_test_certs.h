/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// GENERATED TEST FIXTURE -- DO NOT EDIT BY HAND.
//
// Self-contained PEM material for CurlUrlAsyncFetcherTest's hermetic local
// HTTPS server. Regenerate with the openssl recipe documented in
// test/pagespeed/system/README-test-certs.md. Validity is 100 years from
// 2026-05-24 so these fixtures do not expire in CI.
//
//   kTestCaCertPem        -- Test root CA certificate (PEM). The test injects
//                            this as libcurl's CAINFO so the fetcher trusts the
//                            server leaf in the "succeeds" cases.
//   kTestServerCertPem    -- Server leaf certificate, signed by the test CA,
//                            SAN = IP:127.0.0.1, DNS:localhost.
//   kTestServerKeyPem     -- Private key for the server leaf.
//   kTestSelfSignedCertPem-- A self-signed certificate (its own root, NOT in
//                            the trusted CA bundle), SAN = IP:127.0.0.1. Used
//                            for the "fails for self-signed" cases.
//   kTestSelfSignedKeyPem -- Private key for the self-signed certificate.
//   kTestHostNameCertPem  -- A self-signed certificate for the host name only,
//                            SAN = DNS:pagespeed.test (no IP). A test that
//                            trusts it passes it as the CA file. Added on
//                            2026-10-05, valid 100 years from then.
//   kTestHostNameKeyPem   -- Private key for the host-name certificate.

#ifndef TEST_PAGESPEED_SYSTEM_CURL_TEST_CERTS_H_
#define TEST_PAGESPEED_SYSTEM_CURL_TEST_CERTS_H_

namespace net_instaweb {

inline const char kTestCaCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDbTCCAlWgAwIBAgIURqZQ9ZUEmqQE33AeyvGP5OiLtn4wDQYJKoZIhvcNAQEL
BQAwRTEeMBwGA1UECgwVbW9kX3BhZ2VzcGVlZCB0ZXN0IENBMSMwIQYDVQQDDBpt
b2RfcGFnZXNwZWVkIFRlc3QgUm9vdCBDQTAgFw0yNjA1MjQxNzA1NDhaGA8yMTI2
MDQzMDE3MDU0OFowRTEeMBwGA1UECgwVbW9kX3BhZ2VzcGVlZCB0ZXN0IENBMSMw
IQYDVQQDDBptb2RfcGFnZXNwZWVkIFRlc3QgUm9vdCBDQTCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAJx3vt9pKlaIjSP0c0Wud3NBB7ZRaybVg0tavrx7
J0xMyfiyUw4LsAmJgdUbFc7TKxTx+BI4ZrX3qD7HmVeg9M1MTHzcwJyIC0b4PpCL
H/DRFoEciMb46aHl4nG15XHaIPhr0259OKp3fVePCxYoyxgJ7nNcKFeyskMpFvBv
T7WD2YdNsJako3UjxM6rdO4KR0OQZ+i/P+1hGfIp7mdrNKec4shK9bXGdr/4uRT4
XCBa7QAZhz594FlAC3jAuLSmGpKWhwjrBNRk/QRgUYlFa4UT6kq11/hpvNhDS71f
uY20NOlvP1L06x4VIM/YXznU5b4nLLUSoZYQo9V7ZF7o+zkCAwEAAaNTMFEwHQYD
VR0OBBYEFG4aO5Ior07BJ81RmM8n8ShmxZRoMB8GA1UdIwQYMBaAFG4aO5Ior07B
J81RmM8n8ShmxZRoMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEB
ADiAW5EdGGHAHqpYY9ODXaQNK65OLcq6u9VLCz+Welx6rn9kC6p4QPNi4pI8SJpl
zRgFdXTvb+c9Pem7WpXTXtqYIQgz0ivy9NZRU34iefkYvWqdI4jxGkzTThO74Gb5
MCHl5Onuq1EIzDMxzxrgw/3XOTxFtHjODNP5AT9Go+E4vJyPXhqoU83askyOF9j6
4ExO5N72PEND2FdVtveabJ4bLdBt+efDbovj8jZuJMkh4zWh0obOarRE++h575lP
8RONYePCJGOkTp0QCCSOZVvcarge7pwt/loZw6jXct6r2ZYyZaLrBE+O6TrZfgHH
anqy7BgA9+5hpv/SyWZaGl8=
-----END CERTIFICATE-----
)PEM";

inline const char kTestServerCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDkzCCAnugAwIBAgIUeow3BvE2HP04U6GQ2aJxzWoHrl8wDQYJKoZIhvcNAQEL
BQAwRTEeMBwGA1UECgwVbW9kX3BhZ2VzcGVlZCB0ZXN0IENBMSMwIQYDVQQDDBpt
b2RfcGFnZXNwZWVkIFRlc3QgUm9vdCBDQTAgFw0yNjA1MjQxNzA1NDhaGA8yMTI2
MDQzMDE3MDU0OFowMTEbMBkGA1UECgwSbW9kX3BhZ2VzcGVlZCB0ZXN0MRIwEAYD
VQQDDAkxMjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQC7
hiPa4ckYCaYod3xRksZewWu+9KnZIa7yuiBw2xpvgcTu5XXnho/khCtY/+N9oLni
XbT85TMy6CBwfHUX6xD70CDMRlcM045EBSJmh4YbOR/cMIUsqwB2j1/Ga7tl3Ytz
vtGhlmGU/pq+O/NCH22YImzw24np2l2NEtBRUlUfoyE1MhgECVi4yBwRpf+/q4eP
eudpaQ0HzktCgPwmF7MkUMHOtCbi/zaXSFNQv6KyQ5RIGIV/IaWjSo1aQimr19Hp
pN6c0yL0iTkXB5DnmdPhEjAD/3qFGBhAwjUTB6ZZbAuwcOKX1vilxb2LhpDqDmQ/
3n3TtrXJSWMzuvOvxt1nAgMBAAGjgYwwgYkwCQYDVR0TBAIwADALBgNVHQ8EBAMC
BaAwEwYDVR0lBAwwCgYIKwYBBQUHAwEwGgYDVR0RBBMwEYcEfwAAAYIJbG9jYWxo
b3N0MB0GA1UdDgQWBBQSHuUJij/riOWra4l0zUfX1N7o7zAfBgNVHSMEGDAWgBRu
GjuSKK9OwSfNUZjPJ/EoZsWUaDANBgkqhkiG9w0BAQsFAAOCAQEAV32XNtWFhbdy
rvBBIa/UJHgLe3k2o/n2AvA75fnHHbthpeFIOtsIF/3p7YJf8GCvCJ0i363VIyNt
H6IB15AV8L8NC0rbJBKFk6/KuYAY4UlmibmFr7GQQbXeS4NpJf+yusWGS7zPC9tI
59EpKOpN2/k7StU/MSINZSV1Mbqas8VB9VBKK9OVneCyEvJGsrtZwfKl8ORa9zqf
m8IzO2wzySWMHSrp+JPgwfxLrrJD4enXj9Gsigwb1FUEM+gQ2eD5FmWFQhucgg4g
mzjbi3YBHOKyfIhDmqlwK4rfHDMwgWjsLdox4HfS4qQtnycPRGkCy/HYqtKkaJGI
Wpn1z8Y2BA==
-----END CERTIFICATE-----
)PEM";

inline const char kTestServerKeyPem[] = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQC7hiPa4ckYCaYo
d3xRksZewWu+9KnZIa7yuiBw2xpvgcTu5XXnho/khCtY/+N9oLniXbT85TMy6CBw
fHUX6xD70CDMRlcM045EBSJmh4YbOR/cMIUsqwB2j1/Ga7tl3YtzvtGhlmGU/pq+
O/NCH22YImzw24np2l2NEtBRUlUfoyE1MhgECVi4yBwRpf+/q4ePeudpaQ0HzktC
gPwmF7MkUMHOtCbi/zaXSFNQv6KyQ5RIGIV/IaWjSo1aQimr19HppN6c0yL0iTkX
B5DnmdPhEjAD/3qFGBhAwjUTB6ZZbAuwcOKX1vilxb2LhpDqDmQ/3n3TtrXJSWMz
uvOvxt1nAgMBAAECggEABGkIjtf16CAnTZcB0ClTi7Gg3GzZ/HFTsVcPTELa7E8b
rUKtnd8G+KjjHBoTOkZ9bgV7gYx3wnVoeJIfbuCTFa5fbG57Evd8G71t1wYjm/Wh
T4cUmr2q1R7/caXhp75TeUq+Q2lLV5BlXfQAVJ3Im2YJNIuf7WVlWIazajY1f6Oc
OwD+orSZ2t8uS3E4XmA7lbINd+ee6BBjbk0chMSYpF37yn3W3xVe6XKBsi1ZtVkz
6SdiiPhR3t5fbLulYNlElvP7gkYCl1ZG7JAOGBadEbnkQcpXLROz5WV1XlX5d4Ul
/T4/kXFi2sEl3fA6hLC+xfnkWzgv3bGCKplw1SpHgQKBgQDeYzIMw7EZuz43WQsD
fTwSeZRqIq5xh+6gCb3CcMSmNN/Rio0t6m3dKUnYInAAnEdyTrTCqrDOFwtBCZiJ
U/tfudwTmX/E7dnm9rDkdorx2jt7V0oMCIHB6dR/vo8YxpJhri3te/1D8r1+9Ct6
ro2IPyZyTgWf75WADm2cMv2ixQKBgQDX3foDjN91/5TFuZJhTy4GQpVa2O29Nnfk
IaQyT0VOxLUesUeFcMbUwQZf1B3TPj+wMtzymtTo9LuYbA5CJLVz6+yz1CARZqbA
YZ2KUGMQebldskGsJb4JiMCD+uQ2S1RR6obPEfKuQPu9ag2AmUzoLHTHWfmnCGwo
4rW9lmaSOwKBgQCnPwSZ8uYSyu2cZFvTEPEHl+XU+CYm/aqpMwpB29sYgDU++ir4
uBBNvkppwGRpIR7eSXDJ4eK76zqse9H6nW0z7awkFVGwwYwZlbcs6jXOc2g+d0QZ
zp//PKJyO9aUNGpMCdlCe/fZjQmUG39DcVjBeXSpjCHQyTauqqsmSHbVXQKBgQCO
IbLvSX8Mw5aiRZhzB95m8spCQdjvH4D7LYdeNMGOpogWyGUuIF5aUSmwSQrGPxNS
IXtHJdkP9avbJTKSLHdo9ysoEIB41JzwyJUhL+K6Q1tgrPD+tu8Uef1AKR6//QNs
2D2g89FVGKZoRf7T8JwptrPBWqW9bBdDKFq/lNq+ZwKBgQCxnkWRWfqAzVtcv0+3
G7rltk0ZTk94/OWNeIxzq31yTpVLHyIdOkvoN5Ro+WTyewRuqEzdtlFaICLKe5i0
opUEl2TekQZI/HixtLjU7KemDtos1TWDRfOjzAiYaxu0DNWDMNLJxWhBwKIlW5sg
mhgPgijfEnVUePa/sAbRioSfYg==
-----END PRIVATE KEY-----
)PEM";

inline const char kTestSelfSignedCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDczCCAlugAwIBAgIUdI2JHhelKkxihoKsR9zBJJ10/gMwDQYJKoZIhvcNAQEL
BQAwPTEnMCUGA1UECgwebW9kX3BhZ2VzcGVlZCB0ZXN0IHNlbGYtc2lnbmVkMRIw
EAYDVQQDDAkxMjcuMC4wLjEwIBcNMjYwNTI0MTcwNTQ4WhgPMjEyNjA0MzAxNzA1
NDhaMD0xJzAlBgNVBAoMHm1vZF9wYWdlc3BlZWQgdGVzdCBzZWxmLXNpZ25lZDES
MBAGA1UEAwwJMTI3LjAuMC4xMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEAos7yNgwtR+52DAwps9zYUlEuXMZX7NlLY9UQqcSssKIas8iujS11x9yhBd/c
ACLZM8JcGwAEhGQSaqsoDKcNvwhvY3Bw6//xO8xL9Azfj/NiPC5/9aNdBj6HlY2i
NzwokCH85iZMggEQ/wQRt6H1dggAuCuOs7BL/NqpHwWct+Aw+lH7yzkk+gJhrh0E
YmwaBEJZUD5TdbgX2NA46BzW4HeDOD9/iNHs9E8qT4Mv40ZS03IsvEdPCAz/PRXa
4VfePguNSB8vdyDqpDSxkaPekafLl1vnACMgihczrGE8uvyhSAENggtn8mUKXtKL
3zW4y8j27EoiT6gfAEtbAXEqmQIDAQABo2kwZzAdBgNVHQ4EFgQUDcf/QM6ClVZh
OeYdy2wEEBzu9uMwHwYDVR0jBBgwFoAUDcf/QM6ClVZhOeYdy2wEEBzu9uMwGgYD
VR0RBBMwEYcEfwAAAYIJbG9jYWxob3N0MAkGA1UdEwQCMAAwDQYJKoZIhvcNAQEL
BQADggEBAIi//vTGaVM6S3rdpLAFqJ/aw02Tk+LXGtutPl+8yYjNibWtP0RO1rSP
BIGilWjOLqMb905dABEEOy5oxN1dpYFFD+pV655YV+ci6GX3DUSGUwcXMjo9gZlP
HiLQGwFAXOK84/3QRWVvzhGD52sVcOaZIV6MtkxSUmpPElCOrVkY+jieHCuGINgZ
jx7GHsgaxdqa6lqBAoV+0pswl6Av2uX0TyjbywDOUcli/A9DQboDGy0hG0ZbdrW6
r1KgByhJvGabXJdZPtWet9XKdRkpR+2HkNAdMsGJYxth/qoDBeibXpF0Yg5pbRQf
JqdC+G1cnF8lGyRRzQRuQ9TRwm9r7uU=
-----END CERTIFICATE-----
)PEM";

inline const char kTestSelfSignedKeyPem[] = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCizvI2DC1H7nYM
DCmz3NhSUS5cxlfs2Utj1RCpxKywohqzyK6NLXXH3KEF39wAItkzwlwbAASEZBJq
qygMpw2/CG9jcHDr//E7zEv0DN+P82I8Ln/1o10GPoeVjaI3PCiQIfzmJkyCARD/
BBG3ofV2CAC4K46zsEv82qkfBZy34DD6UfvLOST6AmGuHQRibBoEQllQPlN1uBfY
0DjoHNbgd4M4P3+I0ez0TypPgy/jRlLTciy8R08IDP89FdrhV94+C41IHy93IOqk
NLGRo96Rp8uXW+cAIyCKFzOsYTy6/KFIAQ2CC2fyZQpe0ovfNbjLyPbsSiJPqB8A
S1sBcSqZAgMBAAECggEAKPBP0I5kTnukfGgMjnlAfgaC2XpYfqqvPeXEQGQ/plTE
0SNr3P0q6jxM0zzUxcX9hvnfDj0FZcMCLIdoVTImjzxQQhlyk61ym/5FtRMNnlVt
hMOOkpZnHFxZ3J3WRfxfGMsrHDZSM7iP7Qg5aksT/X+KqsTtJ7rziJv1PeM74rJa
S+ohtLXsyvZwHa4UfYgdlSfM1H4EwEFWjw+3jPyW7zG6mEFnVosaPF6BIye+rDiF
IZRCoVKIbW30r2GqX8KjzgOIvA8THzkecetWQDCvCoFUCXTPtvnfI1TEVsE4clcb
4rQD1J2/+spDlK0lgvVrPDHWHbZdCYEMIpxxg64T7wKBgQDlrFw0pbNIZzvfR/vS
56wBmNSWO/K1OsxBd+OXqiz007V8w4PVEEpiIe+WtsFf6q7mnszptpR0JghrXgkB
sDfcql/I6F+ZRLVm3x/liM+mxG0XBONgP5+gWUNKHVAUGeJOSG+dJ5Pjmr5l7N0J
FPu0FOJvUtPqbBOCzVtBZ1xXewKBgQC1eHihaHdyEiWGTdSq/P1Ulo/ePpccmz+9
69JEIg7sDLN1CclB0Ofn6ZP9XEx/tn1Jy5hzTVNSb8WFgSOUQEtQsQCkupjJXt45
7R3tunFyQz0ixuLrT1ASebuBTI4rSOW4lrpHRzvk7V5C96cbCKEuANXrJLp9BPR4
mWza8Raf+wKBgEqXyTacHndEeBCTi3k7HwVBwsGsZK5xk0csDfIDJii53bbQtS9s
5AutI+haIMHrMbTbHIhHcT2r4I4mc1xmBC6Z8xQITIw14YiwrOZaob5zC08vmj13
THvCofUfQhPVOEfehMmQwhpo9q+Z10wM0ZbyNXycdREs2sVftuSuEjKjAoGAJm+q
1T0kN4QCcKzhg4nsOlNdi4wkQ4naeWaOdaHlGTgjdoGpIAiYZfWCQ+KdzVsgtFWs
J5fUMxy7cGiG2aq4iRHEeh+Ppu8yEIDZmvWne7UkKM6JV5/H7PHdtig54I8jIPLD
5779v6JLGdIMkdxFD1Jb/N8dpMc85KJSfRkdWqsCgYEAiz0u5IWCAz3ZBcjmqm7/
bCygDvfA3G3xFkDeTXHgcEPyFNpInrlBV9SjtMJNeN5zHXZG2kttQ3kh+pUMXf58
SiyVzNMKTOFGNJzqpvvp8dyNTHkKEQASiLrjtu7vwqri+FZ/7KGi7G7sV4bQ0aS9
7gokVO7kikSmAAYFawc6Xnk=
-----END PRIVATE KEY-----
)PEM";

inline const char kTestHostNameCertPem[] = R"PEM(
-----BEGIN CERTIFICATE-----
MIIDfjCCAmagAwIBAgIUKczvaOv640bAhLaWGCyZ8W17HFYwDQYJKoZIhvcNAQEL
BQAwQDElMCMGA1UECgwcbW9kX3BhZ2VzcGVlZCB0ZXN0IGhvc3QgbmFtZTEXMBUG
A1UEAwwOcGFnZXNwZWVkLnRlc3QwIBcNMjYxMDA1MDgyMTE3WhgPMjEyNjA5MTEw
ODIxMTdaMEAxJTAjBgNVBAoMHG1vZF9wYWdlc3BlZWQgdGVzdCBob3N0IG5hbWUx
FzAVBgNVBAMMDnBhZ2VzcGVlZC50ZXN0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8A
MIIBCgKCAQEAy4TQsX6467hhVCunyiCQWgFFb11cHxJacDfmPElvvMHslDAPcdAH
Xdif8+ZlbVR2kBawsIGgUHVhELJyeyxJLs1C3MScCpKdeGHvpbSyYdtyZFv0BP82
r6mTL/MbBkY7sQdfCxg174dh3jPPk7EVnBWhTV8yUIZcxqyokHz/qah80tkl9UAC
udIOjQm40WBcyE5OKkQqFjr8hvEXVvwh3fCYUors8oO4Jr9poETaf9syNGx8+7G8
qifN28AQEU4arDr26KRa4cEpaad3LEybk6mY+vm3UvZP5zoY3hnZlsM/xRJHaPbj
vQ3lKH0ZHnSg9kdpBdJexMJq7W8NnAVnBQIDAQABo24wbDAdBgNVHQ4EFgQUw7JO
Royb0Yts9lCeIUHSx8fN+2MwHwYDVR0jBBgwFoAUw7JORoyb0Yts9lCeIUHSx8fN
+2MwDwYDVR0TAQH/BAUwAwEB/zAZBgNVHREEEjAQgg5wYWdlc3BlZWQudGVzdDAN
BgkqhkiG9w0BAQsFAAOCAQEAxdPRxHS9rc8aPG8xIvB6ExXEiZCevOvFUEXrON7Q
VH5lSHT2AzmZIpzZxGet1382Ag6NOznzcaWdV3rPnxwfn+b++3mYOmcSAqU3feFj
g3kbyBbuTVAAwvg8eLBopjfffBTZ25ppR9MaBrYB/C7PLRWxRlsvqnJFlM96wXcT
Dj/Ta9R8EwGb+vPqH6ZLbpDZjyVPH3Xg0haXSb3nAn3b6hDBcYVQlzYV2E+iG6sX
D2Vo4TN52u44NGlmB7hHu9Ke7vWGmeNRW7M0Bu70BAhWGoqngl2p6irt4sITVKid
JPxcLh4U30Y98oTrTvGw1kVJSzukeloKDMjmebT6Oc8PZQ==
-----END CERTIFICATE-----
)PEM";

inline const char kTestHostNameKeyPem[] = R"PEM(
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDLhNCxfrjruGFU
K6fKIJBaAUVvXVwfElpwN+Y8SW+8weyUMA9x0Add2J/z5mVtVHaQFrCwgaBQdWEQ
snJ7LEkuzULcxJwKkp14Ye+ltLJh23JkW/QE/zavqZMv8xsGRjuxB18LGDXvh2He
M8+TsRWcFaFNXzJQhlzGrKiQfP+pqHzS2SX1QAK50g6NCbjRYFzITk4qRCoWOvyG
8RdW/CHd8JhSiuzyg7gmv2mgRNp/2zI0bHz7sbyqJ83bwBARThqsOvbopFrhwSlp
p3csTJuTqZj6+bdS9k/nOhjeGdmWwz/FEkdo9uO9DeUofRkedKD2R2kF0l7Ewmrt
bw2cBWcFAgMBAAECgf9NeRcEw2989q060TK/DkTY8ikIo1bVuiC/ftEsRn8kGK5F
uvmsOyk9ZaDTetsVQ2EbzgFQO9djjIhIOoFNw7iqZ4T6F602ieKYuh0Yd1VwcRMU
mxEGK0WTvbaxqJaCieSEhXR+E/ArE9IPK7YcM5Ak1JD6VudcjzcXXgQiUe0BA1Yl
ASKpGqG87AoAOd2JQPiy6pIaDjCoKZvi9QuNH6plTwWgvuO5NcrBqfDbrltGWWYg
cgvJr0a4TrQ1BdEGMbZCu+/gC2ejsF46V9HAPa/R8yzPHpFo2XpQf+blKBxWbTcL
SEzj2NIRIhsvzF6qfrDt5Xayj8iTdmMAmXzV0xECgYEA7C7SIuYOW8De49YTOqPF
2kEd90svGbRQ234ruA2ph4+oozWebzvSP1F1KUlJ0sP7DLNY8PDovm4fYNajJrll
ee9cJKe10cDuCyrcSA2rDTlScTrPVX/ke7QDm6rmL63fhpm63Sqp8w7vGiomR2p1
O6SZVilVSmjrUgVMG0dQfJkCgYEA3Jhft5IpUPL94o08hS66o20m3+yi1thzaCUe
Pd1c6oBCm2a2d9pVLR2FdtJwPXOYU0nRu5boOQcmRcP4Yp8ezDNZRFzgKkNqlZ0O
/BXnZUmYJzIJ3lbx+ewYViKZKQSP1hR02E8NFy82CwNX6/DlwIW9DZxwDq5BRdg5
LTuOdU0CgYEAmn7OKtH+lnqF0tfYYEV9SRXLelaOCuxu8AWKp60YNnr8vzZ12yFw
mMa6MzWjuuFEObyf84xDUz5J/FirBMvF24t95ORoBTuE+30f92opeE8E5Nipg65T
UbAhpBSY2puMWkn5eG9OGJauRZlIzPKQmWFEhJ4pErGrLU4VyMpPlhECgYEAtIGu
2SytyEJ9w3Nk0WpSaMY5aERfX0SQ8uhknaTUkkLPvg/HQb2Tsy4T9w3tsCs1uvmN
uF2cuoWCKNZaGoEwF1HxOsJw38R4o+J3z4kw5q3Y5kHI13n32VT6+QiooWrNNBVb
I2FUSeNk6pAa0sM1jVsiPtAC1dB7MubiqoOwZ4UCgYEAxPYYyrfsLrssx20bzbwe
JrlsSj4apYXT8F3REgZ2yAkf0dGZymd0Alvqtk78PTN2bCc9MKHY/ioaSUuEJTFj
kC3cnsYvWmFXVzCf44N/STznf9kClfnmJy63ZgSwNmJAATbIbszvjpLibazlMawK
VPlLjiiBlCMQvliaIVvtb2k=
-----END PRIVATE KEY-----
)PEM";

}  // namespace net_instaweb

#endif  // TEST_PAGESPEED_SYSTEM_CURL_TEST_CERTS_H_
