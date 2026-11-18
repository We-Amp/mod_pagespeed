---
title: 'Validate RSL license capability tokens'
description: 'Validate Authorization: License capability tokens at your origin and return 401/402/pass — experimental operator access control, off by default.'
order: 62
group: 'Agents'
lastUpdated: 2026-10-18
faq:
  - q: 'Does it handle payment or billing?'
    a: 'No. The enforcement layer emits HTTP status codes only: it maps a capability-token verdict to allow, 401, or 402. It never settles, meters, escrows, or handles money, and never modifies a response beyond the status code.'
  - q: 'Is it on by default?'
    a: 'No. Enforcement ships off by default. When disabled the request path is zero-cost — the handler does one flag check and passes the request through untouched, with no token parsing and no allocation. It only acts once you set PAGESPEED_RSL_CAP_ENFORCEMENT=true and configure the required license/scope and a key directory.'
  - q: 'What is the difference from the Web Bot Auth verifier?'
    a: 'The Web Bot Auth verifier is observe-only — it labels and counts RFC 9421 request signatures and never blocks. RSL-CAP is an operator-gated access-control layer for a distinct token type (Authorization: License capability tokens): it validates the token and returns 401/402/pass. The two use separate, isolated key trust domains.'
  - q: 'Where do the issuer keys come from?'
    a: 'From HTTPS JWKS key directories you configure. The worker warm-fetches and refreshes them in the background into a dedicated trust realm, so validation on the request path is a single in-memory Ed25519 check and never waits on the network. v1 validates against key directories only — there is no live token-introspection endpoint or remote authorization call.'
  - q: 'Does the module validate these tokens too?'
    a: 'Yes. The module ships the same RSL-CAP validator core for nginx, configured with pagespeed directives. The optimizer worker configures it via environment variables.'
---

An `Authorization: License <token>` capability token is a compact, Ed25519-signed
statement of a licensed identity and the licenses/scopes it has been granted.
RSL-CAP validates those tokens at your origin and turns the verdict into an HTTP
status: a valid, authorized token passes through; anything else is answered with
`401` or `402`.

:::caution[Experimental]
RSL-CAP enforcement is an **experimental** feature. It ships off by default and
is an operator access-control capability — it **only emits status codes** and
never settles, meters, escrows, or handles payment. Its configuration surface
and behavior may change between releases, and there is no general-availability
commitment for this feature yet.
:::

## What it does

When enabled, every request is checked against the token in its `Authorization:
License` header before content is served. The validator resolves the token's
issuer key, verifies the Ed25519 signature over the exact bytes received, checks
expiry, and confirms the token grants the license id and scope you require. The
verdict maps to a status:

| Verdict | Status | Meaning |
|---|---|---|
| Authorized | *(request passes through)* | Valid token granting the required license and scope |
| No token | `401` | No `Authorization: License` header |
| Malformed | `401` | Not a well-formed token (bad shape/encoding, wrong type, non-Ed25519 alg) |
| Unknown issuer | `401` | The token's key id does not resolve in any configured key directory |
| Bad signature | `401` | The Ed25519 signature does not verify |
| Expired | `401` | The token's expiry is in the past |
| Unlicensed | `402` | Cryptographically valid identity, but the required license/scope is not granted |

The signature is always verified **before** expiry and authorization, so a
tampered expiry or grant list can never influence those checks. `401` means "no
valid licensed identity"; `402` means "the identity is cryptographically valid,
but this license/scope is not granted" — `401` responses carry a
`WWW-Authenticate: License` challenge. The validator only emits the status — it
never settles, meters, or handles money.

Aggregate counts are exported at `/v1/metrics`:

```text
pagespeed_rslcap_requests_total{verdict="authorized"}
pagespeed_rslcap_requests_total{verdict="denied_401"}
pagespeed_rslcap_requests_total{verdict="denied_402"}
```

## Enabling it

Environment variables on the worker container (Docker and Helm deploys alike).
Changing them requires a worker restart. RSL-CAP enforcement is available in
releases after 2.0.35.

```yaml
services:
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:2.9.0
    environment:
      - PAGESPEED_RSL_CAP_ENFORCEMENT=true
      # Comma-separated HTTPS JWKS key-directory URLs for the token issuers (no spaces).
      - PAGESPEED_RSL_CAP_KEY_DIRECTORIES=https://issuer.example/.well-known/http-message-signatures-directory
      # The license id and scope a token must grant to be authorized.
      - PAGESPEED_RSL_CAP_REQUESTED_LICENSE=premium
      - PAGESPEED_RSL_CAP_REQUESTED_SCOPE=render
      # Optional: only accept tokens from this issuer (reject others as unknown issuer).
      - PAGESPEED_RSL_CAP_ISSUER=issuer.example
```

The matching worker command-line flags are `--rsl-cap-enforcement`,
`--rsl-cap-key-directory <url>` (repeatable), `--rsl-cap-requested-license`,
`--rsl-cap-requested-scope`, and `--rsl-cap-issuer`.

- **Default-off is zero-cost.** With enforcement disabled the handler does a
  single flag check and passes every request through — no token parsing, no
  allocation.
- **Key directories** must be HTTPS; the worker rejects plain-HTTP URLs at
  startup. The worker fetches and refreshes each directory periodically in the
  background into a dedicated trust realm — kept separate from the Web Bot Auth
  verifier's keys — so key rotations are picked up without a restart, and
  validation itself never waits on the network. This first version validates
  against the configured key directories only — there is no live
  token-introspection endpoint or remote authorization call.
- **Required license and scope** are fail-closed: if either is empty, every
  token is denied with `402`. Set both to the values your issuer grants.
- **Issuer pin** is optional. When set, an otherwise-authorized token whose
  issuer does not match is rejected as an unknown issuer (`401`).

## Reading the results

Scrape `/v1/metrics` (see the [HTTP API](/docs/http-api/) for authentication)
for the three `pagespeed_rslcap_requests_total` counters. The split between
`authorized`, `denied_401` (no valid licensed identity), and `denied_402`
(valid identity, license/scope not granted) tells you how requests to your
enforced surface are being adjudicated.

## Related

The observe-only [Web Bot Auth verifier](/docs/web-bot-auth/) is the labelling
counterpart to this layer: it classifies RFC 9421 request signatures and never
blocks, whereas RSL-CAP is the operator-gated access-control layer that returns
`401`/`402`/pass. The two use separate, isolated key trust domains.

For the design rationale and the module's nginx directives that configure the
same validator, see [Pay-per-crawl at the
origin](/blog/pay-per-crawl-at-the-origin/). For where token-gated access sits
alongside crawler verification, rendered-markdown, and provenance preservation,
see the pillar overview, [The agentic web at the
origin](/blog/agentic-web-at-the-origin/).
