# Web Bot Auth / RSL-CAP — a note on the word "license"

This directory implements two request-classification surfaces: an observe-only
RFC 9421 Web Bot Auth verifier, and an experimental, default-off RSL-CAP
capability-token check. The RSL-CAP half validates an
`Authorization: License <token>` header — a signed statement, presented by the
*requesting client*, about which content licenses and scopes that client holds
for the *content being served*. It maps the verdict to an HTTP status (allow,
401, 402) and nothing else: there is no settlement, metering, or payment code
here. This "license" is content licensing between a site and its visitors —
it is unrelated to software licensing of mod_pagespeed itself. mod_pagespeed
2.1 is Apache-2.0 open source in full; the daemon carries no license state,
no activation, and no entitlement check, and the install-entitlement
signer/verifier that once lived in this directory was removed before GA. A
grep for "license" that lands here has found the RSL-CAP content-licensing
subsystem, not a product-activation remnant.
