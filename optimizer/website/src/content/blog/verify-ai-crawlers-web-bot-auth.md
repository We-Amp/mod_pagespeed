---
title: 'Web Bot Auth: verify AI crawlers at your origin'
description: 'mod_pagespeed ships an nginx Web Bot Auth verifier: it checks RFC 9421 signatures and labels requests in $x_verified_bot. Observe-only, off by default.'
date: 2026-06-22
author: 'Otto van der Schaaf'
tags: ['ai-agents', 'security', 'nginx']
draft: false
lastUpdated: 2026-10-22
tool: crawler-cost
---

> **Status: experimental (preview).** The Web Bot Auth verifier ships in mod_pagespeed 1.15 and, as of 2.0.34, in ModPageSpeed 2.0 — off by default and observe-only in both. The 2.0 configuration is documented at [Verify AI crawlers with Web Bot Auth](/docs/web-bot-auth/); the 1.15 directives below are still a preview.

## The user-agent string is not evidence

You want to know which AI crawlers hit your origin, so you read the `User-Agent` header. It says `Googlebot` or `ClaudeBot`. You log it and build a dashboard on top of it. That dashboard is fiction.

Any client can send any user-agent string. A scraper that wants your content sets `User-Agent: Googlebot/2.1` and your logs record a visit from Google. The string costs nothing to forge. Nothing about the header is signed or verified. It is a claim you have been treating as proof.

The traditional fix is reverse-DNS verification: look up the IP, confirm it resolves back into the crawler operator's domain, accept the request only if it does. That works only for the crawlers that publish stable IP ranges and run cooperative DNS, and it ties identity to network topology rather than to the agent.

There is a better primitive, and the mod_pagespeed module now speaks it.

## What Web Bot Auth checks

[RFC 9421](https://www.rfc-editor.org/rfc/rfc9421.html) defines HTTP Message Signatures: a way for a client to sign parts of its request with a private key. The Web Bot Auth scheme builds on it. A bot holds an Ed25519 private key and signs its request with it, then publishes the matching public key in a JWKS key directory at a known URL. The signature carries a key id that points at one of those published keys.

The verification is concrete. The request arrives with a signature. You hold the signer's public key. You check the signature against the request. Either the math holds and the request was signed by whoever owns that key, or it does not and you discard the claim. Identity now lives in cryptography, not in a string the client typed.

This is the same shape as the reverse-DNS check, but it binds to the agent's key instead of its IP. The bot can change IP or network and still prove who it is.

## How the module's verifier classifies a crawler {#how-the-115-verifier-classifies-a-crawler}

The module includes an nginx verifier for the Web Bot Auth scheme. It is observe-only. It checks the signature off the blocking path and writes its verdict into an nginx variable, `$x_verified_bot`, then hands control back to your config. It never blocks.

The verdict covers three cases. A signature that checks out against a known key produces the recognized bot name plus a verified marker. An unsigned request produces a `human`-style marker. A tampered signature, or one that points at a key you do not have, produces an `unknown`-style marker. The smoke test below has the exact strings.

What you do with the verdict is your decision. Log it, route on it, or add it to a response header for debugging. The verifier labels; it does not act.

The honest framing: the Web Bot Auth verifier is available to enable in mod_pagespeed 1.15 and ModPageSpeed 2.0 (2.0.34+), off by default, classification-only. The 1.15 directives below are still a preview; the 2.0 surface is [documented](/docs/web-bot-auth/). It does not block bad bots. Deciding what a verified agent may reach — allow the request, or return a 401/402 — is a separate, experimental layer: [pay-per-crawl at the origin](/blog/pay-per-crawl-at-the-origin/). That is not what this verifier does.

## The directives, with their real defaults

Three directives wire up the local-file path. A fourth adds aggregate telemetry. Every one is off or empty until you set it.

- `WebBotAuth` — default **off**. Set `pagespeed WebBotAuth on;` to enable observe-only classification. Nothing happens without this.
- `WebBotAuthKeyDirectoryFile` — default **empty**. Path to a local JWKS file holding the signer's published public keys.
- `WebBotAuthVerifiedBots` — default **empty**. Comma-separated `keyid=name` pairs that map each key id to a human-readable bot name. This is how a verified signature becomes `examplebot` instead of an opaque key id.
- `WebBotAuthTelemetry` — default **off**. Counts verified and signed-agent requests in a statistic when you want the aggregate rather than per-request labels.

That is the whole surface for the local-file mode. You supply the keys on disk, you name them, you turn it on.

### Resolving keys over the network

A second mode fetches the JWKS directory over HTTPS instead of reading a file. It is also default-off, gated behind empty values, and it needs the curl-based fetcher build. Without that build, mod_pagespeed falls back to local-file resolution.

- `WebBotAuthKeyDirectoryUrl` — the HTTPS JWKS URL. The fetch runs on a background thread, off the request path, so classification reads from cached key material rather than blocking on a live fetch.
- `WebBotAuthKeyDirectoryAllowlist` — an SSRF allowlist. Empty means warm-fetch is disabled and the verifier fails closed. You name the hosts it is allowed to reach, or it reaches nothing.
- `WebBotAuthDirectoryHost` — the directory host.
- `WebBotAuthKeyDirectoryRefreshSec` — refresh interval; empty defaults to 3600 seconds.

The design intent: pull key material out of band, off the request path, and fetch only from hosts you explicitly allow. The allowlist is the control lever. By design the verifier only reaches hosts you list, and an empty allowlist disables warm-fetch entirely, so the network mode is fail-closed.

## Verify a signed crawler on your own box

You do not have to trust the description. Wire it up and watch the verdict change.

Turn the verifier on, point it at a key directory file, and name the keys (replace `k1` and `examplebot` with your signer's real key id and name):

```nginx
pagespeed WebBotAuth on;
pagespeed WebBotAuthKeyDirectoryFile /etc/nginx/web-bot-auth/jwks.json;
pagespeed WebBotAuthVerifiedBots "k1=examplebot";
```

Then expose the verdict so you can read it from a client:

```nginx
add_header X-Verified-Bot $x_verified_bot always;
```

The repo ships a signing tool. Send a request signed with the key whose public half is in your JWKS file, and the header reads the bot name plus the verified marker. Send a plain unsigned request, and it reads the `human`-style marker. Tamper with the signature or sign with a key you have not published, and it reads the `unknown`-style marker.

Those three cases are not aspirational. The repo ships an nginx smoke test that exercises all three verdicts. For the exact strings and the precise per-case behavior rather than the paraphrase here, read the smoke test. It is the verdict's source of truth, and it runs against a real nginx build.

## Where this sits in the bigger picture

Verification is the first step, not the whole answer. Once you can tell a real agent from an impostor, you can decide what each one is allowed to do, and whether access to your content should carry a price. That is the agentic-web problem in full, and it is the subject of the pillar piece: [the agentic web at the origin](/blog/agentic-web-at-the-origin/).

The Web Bot Auth verifier gives you the identity primitive, available to enable in both parts of mod_pagespeed, off by default, observe-only. Enable it and read the verdict instead of trusting the user-agent string. The free [AI readability check](/ai-readability/) lists AI crawlers that your robots.txt blocks by name.

Next: enable the verifier with the snippet above, then see how identity becomes access control — allow, or a 401/402 status, and nothing more — in [pay-per-crawl at the origin](/blog/pay-per-crawl-at-the-origin/).
