---
title: 'Verify AI crawlers with Web Bot Auth'
description: 'Check RFC 9421 signatures from AI crawlers at your origin and label each request with a verified bot identity — observe-only, off by default.'
order: 61
group: 'Agents'
lastUpdated: 2026-10-22
faq:
  - q: 'Does it ever block a request?'
    a: 'No. The verifier is observe-only: it labels signature-bearing requests with an x-verified-bot response header and counts verdicts in /v1/metrics. It never changes how a request is served — no blocking, no cache variation, no redirect.'
  - q: 'Is it on by default?'
    a: 'No. The verifier ships in every 2.0.34+ build but stays off until you enable it with PAGESPEED_WEB_BOT_AUTH=true and configure at least one key directory. When off, requests are handled exactly as before.'
  - q: 'Does verifying signatures slow requests down?'
    a: 'No measurable cost. Requests without signature headers short-circuit before any work. Signature-bearing requests get at most one Ed25519 verification against keys already warmed in memory — verification never fetches anything over the network on the request path.'
  - q: 'Which bots can it verify?'
    a: 'Any client that signs requests with RFC 9421 HTTP Message Signatures under the Web Bot Auth convention (tag="web-bot-auth", Ed25519) and publishes its public keys in an HTTPS JWKS key directory you configure. That covers Web Bot Auth adopters among AI crawlers and any signer you run yourself.'
  - q: 'Does the module verify Web Bot Auth signatures too?'
    a: 'Yes. The module ships the same verifier core for nginx, configured with pagespeed directives and exposing the verdict as the $x_verified_bot nginx variable. The optimizer worker configures it via environment variables and emits the verdict as a response header.'
  - q: 'What does the nginx WebBotAuthBotDetection directive do?'
    a: "A Web Bot Auth signature can inform whether a request is treated as an automated client, behind the opt-in WebBotAuthBotDetection directive (server configuration, default off). With it on, a request carrying a cryptographically valid Web Bot Auth signature (RFC 9421) is treated as an automated client whatever identifier it presents — so an agent that identifies honestly is classified correctly even when it sends a browser's user-agent string, which no identifier list can detect. Only a signature that verifies counts; an absent or failed signature changes nothing. Requires WebBotAuth, the existing directive that turns signature verification on. Off by default, so Web Bot Auth stays observe-only for every existing deployment: with the new directive off, a verification result still only labels the request — it populates the $x_verified_bot nginx variable, which you can log or pass to your own configuration, and the opt-in verified-request statistics."
  - q: 'What does "invalid" include?'
    a: 'Everything that carried signature material but fail-closed: tampered or expired Web Bot Auth signatures, signatures referencing keys you have not configured, and unparseable signature headers from other ecosystems (for example bare draft-cavage Signature headers used by some webhook and fediverse deliveries). Parseable RFC 9421 material that is simply not Web Bot Auth is counted separately as "other" and treated as unsigned.'
---

Any client can claim to be any crawler — a `User-Agent` string is unverified
text. [Web Bot Auth](https://datatracker.ietf.org/wg/webbotauth/about/) fixes
that with cryptography: a bot signs its requests with an Ed25519 key
([RFC 9421](https://www.rfc-editor.org/rfc/rfc9421.html) HTTP Message
Signatures) and publishes the public half in a JWKS key directory at a
well-known HTTPS URL. mod_pagespeed can verify those signatures at your
origin and tell you, per request, **which bot actually sent it**.

:::caution[Experimental]
The Web Bot Auth verifier is an **experimental** feature. It ships in 2.0.34+,
off by default and **observe-only**: it labels and counts, and never changes
how a request is served. Its configuration surface and behavior may change
between releases, and there is no general-availability commitment for this
feature yet.
:::

## What you get

For every request that carries Web Bot Auth signature material, the response
gains an `x-verified-bot` header:

| Request                                             | `x-verified-bot`               |
| --------------------------------------------------- | ------------------------------ |
| Valid signature, key id in your verified-bots map   | `<bot-name>, ed25519-verified` |
| Valid signature, key id not in the map              | `signed-agent`                 |
| Tampered/expired signature, or an unknown key       | `unknown`                      |
| No signature material (browsers, ordinary crawlers) | _no header_                    |

Requests without signature headers are untouched — the label never inflates
ordinary traffic. Aggregate counts are exported at `/v1/metrics`:

```text
pagespeed_webbotauth_signed_requests_total{result="verified"}
pagespeed_webbotauth_signed_requests_total{result="invalid"}
pagespeed_webbotauth_signed_requests_total{result="other"}
```

`verified` covers both mapped and unmapped valid signatures; `invalid` is
signature material that fail-closed; `other` is parseable RFC 9421 material
that is not Web Bot Auth (for example a CDN signing scheme), which is treated
as unsigned.

## Enabling it

Three environment variables on the worker container (Docker and Helm deploys
alike). Changing them requires a worker restart.

```yaml
services:
  worker:
    image: ghcr.io/we-amp/pagespeed-worker:2.9.2
    environment:
      - PAGESPEED_WEB_BOT_AUTH=true
      # Comma-separated HTTPS JWKS key-directory URLs (no spaces).
      - PAGESPEED_WEB_BOT_AUTH_KEY_DIRECTORIES=https://example-bot.com/.well-known/http-message-signatures-directory
      # Optional: promote key ids to readable bot names ("keyid=name,...").
      - PAGESPEED_WEB_BOT_AUTH_VERIFIED_BOTS=NFcWBUXB-example-key-id=examplebot
```

The matching worker command-line flags are `--web-bot-auth`,
`--web-bot-auth-key-directory <url>` (repeatable) and
`--web-bot-auth-verified-bots "keyid=name,..."`.

- **Key directories** must be HTTPS; the worker rejects plain-HTTP URLs at
  startup. The worker fetches and refreshes each directory periodically in the
  background, so key rotations by the bot operator are picked up without a
  restart — and verification itself never waits on the network.
- **The verified-bots map** is your allowlist of identities worth naming. A
  key id you have mapped renders as `<name>, ed25519-verified`; a valid
  signature from an unmapped key still verifies, but renders as the generic
  `signed-agent`.

## Reading the results

Spot-check the header from your logs or by inspecting responses to a signed
crawler you operate. For trend lines, scrape `/v1/metrics` (see the
[HTTP API](/docs/http-api/) for authentication) — the three counters make it
easy to answer "how much of my AI-crawler traffic is actually verifiable?"

Unsigned bot traffic — the majority today — simply shows no header and no
counter increment. The interesting signal is the split between `verified`
(cryptographically attributable crawls) and `invalid` (something sent
signature material that did not check out).

## Opt-in verified-crawl counter

:::caution[Experimental]
The opt-in counter is an **experimental** apparatus built alongside the
verifier. It is **off by default** and has **no general-availability
commitment**. Its shape and behavior may change between releases.
:::

Separately from the first-party `/v1/metrics` counters above, you can _opt in_
to publishing a small, machine-readable summary of your verified AI-crawl
volume at a well-known endpoint. It is off unless you enable it, and it never
changes how requests are served.

Enabling any non-`off` mode **publishes a discoverable marker** at:

```text
/.well-known/webbotauth-counter
```

Two configuration knobs — one non-secret mode, one secret token:

```yaml
services:
  worker:
    environment:
      # off (default) | private | public
      - PAGESPEED_WEB_BOT_AUTH_PUBLIC_COUNTER=public
  # The bearer token gating the exact document is read by the front-end from
  # its own environment (kept out of any shared, world-readable config):
  frontend:
    environment:
      - PAGESPEED_WEB_BOT_AUTH_COUNTER_TOKEN=<a long random secret>
```

The matching worker flag is `--web-bot-auth-public-counter <off|private|public>`.

Behavior by mode:

| Mode            | No / invalid token                 | Valid bearer token |
| --------------- | ---------------------------------- | ------------------ |
| `off` (default) | endpoint returns `404` (invisible) | `404`              |
| `private`       | `404` (existence hidden)           | **exact** document |
| `public`        | **coarse** document (bucketed)     | **exact** document |

- The **exact** document reports the counting-start date, an instance
  identifier (`boot_id`), cumulative verified / invalid / other totals, and a
  per-signer breakdown (signer name where you have mapped the key id, otherwise
  a reproducible key-id hash — see below). It is served `Cache-Control:
no-store`.
- The **coarse** document is deliberately narrower to reduce fingerprinting: it
  replaces every count with a fixed size tier (for example `1k-10k`), omits the
  counting-start date and the instance identifier, and lists **only your
  registered signers** as `name` → tier (unregistered/hash-only signers and the
  exact distinct-signer count are not disclosed). It is public and cacheable
  (`max-age=300`, `Vary: Authorization`).
- Present the token as `Authorization: Bearer <token>`. Only `GET` and `HEAD`
  are served; the document reflects no request data, carries no version or
  build identifiers, and is served with `X-Content-Type-Options: nosniff`.

Verify latency is **not** included in either document — it stays a first-party
signal on `/v1/metrics` only.

### About the identifiers

- **`boot_id`** is an _instance_ identifier, not a per-restart value. It is
  minted once when the counting file is first created and stays the same across
  ordinary worker restarts (which reset the counters but keep the file); it
  changes only when the file is recreated. So distinct `boot_id`s across
  responses indicate distinct instances (for example behind a load balancer),
  and a counter **reset** is detectable as a _decrease_ in a cumulative total —
  not as a change in `boot_id`.
- **The key-id hash** (used for a verified signer that is not in your
  verified-bots map) is a plain, unsalted
  [FNV-1a-64](https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function)
  of the raw key-id bytes — 64-bit offset basis `0xcbf29ce484222325`, prime
  `0x100000001b3`, rendered as 16 lowercase hex digits and prefixed `hash:`. The
  single input that would hash to `0` is remapped to a fixed non-zero value (`0`
  is the empty-slot marker). It is intentionally reproducible: anyone can hash a
  known key id and match it against the document, which is what lets a signer
  recognise its own crawls at an origin that has not named its key.

## Related

This page covers the optimizer worker's configuration; the module's nginx
directives are in
[Verify AI crawlers with Web Bot Auth](/blog/verify-ai-crawlers-web-bot-auth/).

The verifier described here is observe-only — it labels and counts, and never
blocks. If you need to gate a surface on a signed capability token, see the
experimental [RSL-CAP token validation](/docs/rsl-cap/) layer, which validates
`Authorization: License` tokens and returns `401`/`402`/pass. It uses a separate,
isolated key trust domain from this verifier.
