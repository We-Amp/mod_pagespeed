---
title: '55 of 150 AI crawler operators publish Web Bot Auth keys'
description: 'We asked 150 AI crawler operators for their Web Bot Auth key directory on 19 October 2026. 55 served one. The script, the data, and what to do with it.'
date: 2026-10-19
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['benchmark', 'ai-crawlers', 'ai-agents', 'security']
draft: false
product: '2.0'
---

In August someone ran mass vulnerability scans under ClaudeBot's user-agent, and the first reply in the [Hacker News thread](https://news.ycombinator.com/item?id=49272569) gave the usual advice: user-agents are faked, so look up who owns the IP. Web Bot Auth offers a check that does not depend on the IP, but it only works for crawlers that publish their public keys, so on 19 October 2026 I asked 150 AI crawler operators for their key directory.

55 of them served one. Many of those directories belong to agents that act for a user, such as ChatGPT agent and Google-Agent. The large training and search crawlers mostly have none tied to them in their operators' docs or in Cloudflare Radar.

## Where a key directory lives

Web Bot Auth applies HTTP Message Signatures ([RFC 9421](https://www.rfc-editor.org/rfc/rfc9421.html)) to bots. The crawler signs each request with a private key and publishes the public half as a JSON Web Key Set. The working-group draft, [draft-ietf-webbotauth-httpsig-protocol-00](https://datatracker.ietf.org/doc/draft-ietf-webbotauth-httpsig-protocol/00/) (1 September 2026), gives a signer three ways to point at its key set. The default, and the only one you can find without a signed request in hand, is a fixed path on the signer's origin:

```http
GET /.well-known/http-message-signatures-directory HTTP/1.1
Accept: application/http-message-signatures-directory+json
```

The draft also fixes what counts as a directory: it is served with status 200 and that media type, and a verifier "MUST NOT automatically follow HTTP redirects." The origin to ask comes from the signed request itself, in a `Signature-Agent` header. [An earlier post](/blog/verify-ai-crawlers-web-bot-auth/) covers verification; this one counts who has published keys to verify against.

## The population and the probe

I did not pick the operators. The population is the union of two public lists:

- Every entry in the [ai.robots.txt](https://github.com/ai-robots-txt/ai.robots.txt) `robots.json` at commit `6abe630` (18 October 2026). Its 188 user-agents resolve to 94 operators. I took the operator from each entry's operator or description field, or from the brand in the user-agent name (every mapping is in the published builder). I set aside 18 entries: they name no operator I could tie to a domain, or they are open-source clients anyone can run.
- Every bot in the AI crawler, AI assistant and AI search categories of [Cloudflare Radar's bots directory](https://radar.cloudflare.com/bots/directory): 121 bots from 89 operators. Two bots with no usable operator domain are left out.

Together that is 150 operators. For each one I requested the well-known path at the host its own docs or Radar name for any of its bots, at its apex and `www` hosts, and at product domains such as `claude.ai` and `chatgpt.com`: 396 URLs, from one vantage point in the Netherlands. Each was fetched without following redirects, as the draft requires. A 3xx answer was fetched again with redirects followed, to see what a lenient client gets.

The probe is 51 lines of Python around curl; it, the operator list and every timestamped row are published with this post.

The probe script, the target builder, the excluded entries and the raw results are published under [/data/web-bot-auth-2026-10-19/](/data/web-bot-auth-2026-10-19/).

## The count

| Population | Operators | Served a directory |
| --- | --- | --- |
| ai.robots.txt | 94 | 15 |
| Radar AI categories | 89 | 50 |
| Union | 150 | 55 |

The two lists disagree. Radar's records have a field for a Signature Agent URL, so an operator that signs has a reason to be listed there, and its AI categories lean that way: 50 of 89. On ai.robots.txt, a community list of AI-related crawlers "of all types, regardless of purpose", 15 of 94 did. Read 55 of 150 as a count over these two lists, one per operator whatever its size, not as a share of all AI crawlers.

One of the 55 is counted from the bare well-known path: Radar lists its URL with a trailing slash, which answers 308, while the well-known path itself returns 200. Every directory held Ed25519 keys; one operator's three directories also held a P-256 key.

Apex and `www` hosts added nothing. 57 operators had a documented host: at 55 a directory came back by the draft's rules (counting the trailing-slash case), one returned 404, and one host did not resolve.

## The names in your logs

| Operator | Directory host | 19 Oct 2026 | Fallback the operator documents |
| --- | --- | --- | --- |
| Google | `agent.bot.goog` ([Google's docs](https://developers.google.com/crawling/docs/crawlers-fetchers/web-bot-auth)) | 200, 5 keys | [IP range files and reverse DNS](https://developers.google.com/crawling/docs/crawlers-fetchers/verify-google-requests) |
| OpenAI | `chatgpt.com` (Radar, for ChatGPT agent) | 200, 1 key | [IP list per bot](https://developers.openai.com/api/docs/bots) |
| Meta | `www.meta.com` (Radar, for Meta-ExternalTest) | 200, 5 keys | [IP allow-listing advised](https://developers.facebook.com/docs/sharing/webmasters/web-crawlers/) |
| Amazon | 9 regional `on.aws` hosts (Radar, for AgentCore Browser) | 200, 1 key each | [IP list per bot](https://developer.amazon.com/amazonbot) |
| DuckDuckGo | `assistbot.duckduckgo.com` (Radar) | 200, 1 key | [IP list](https://duckduckgo.com/duckduckgo-help-pages/results/duckassistbot/) |
| Anthropic | none documented | no directory at anthropic.com, claude.ai, claude.com or their `www` hosts | [IP list](https://support.claude.com/en/articles/8896518-does-anthropic-crawl-data-from-the-web-and-how-can-site-owners-block-the-crawler) |
| Perplexity | none documented | no directory at perplexity.ai or `www` | [IP list per bot](https://docs.perplexity.ai/guides/bots) |
| Apple | none documented | no directory at apple.com or `www` | [reverse DNS and IP CIDRs](https://support.apple.com/en-us/119829) |
| Common Crawl | none documented | no directory at commoncrawl.org or `www` | [IP ranges with reverse DNS](https://commoncrawl.org/faq) |
| Mistral AI | none documented | no directory at mistral.ai or `www` | [IP list per bot](https://docs.mistral.ai/robots/) |

Radar's entry for xAI's Grok Bot names a directory on `cursorusercontent.com`, which served valid keys. That host is not on x.ai, the domain Radar's own record gives for xAI, or on grok.com, and its root redirects to cursor.com. xAI's bot page returned 403 to my client, so I could not check xAI's side. I count it as Radar records it and flag it.

"None documented" means exactly that: no directory where the operator or Radar says one lives, and none at the apex. A signer can also use the draft's other two types, a direct JWKS URL or a client metadata document. That URL then appears only in its signed requests, and this probe cannot see it. So the row does not mean the operator never signs, and it says nothing about how its crawler behaves. If yours publishes a directory somewhere I did not look, send me the URL.

## What a directory proves

A directory proves an operator holds keys. It does not prove that a given request was signed, or that every bot the operator runs signs. Google's [Web Bot Auth page](https://developers.google.com/crawling/docs/crawlers-fetchers/web-bot-auth) says so: "We don't sign every request of a particular agent." It adds: "A subset of requests made by the Google-Agent are signed with Web Bot Auth." Radar ties OpenAI's directory to ChatGPT agent, not to GPTBot. Amazon's directories belong to AgentCore Browser, a hosted browser that AWS customers' agents drive, not to Amazonbot.

An unsigned request claiming to be Google-Agent is not fake for that alone; it falls through to the next check.

## Index keys by thumbprint, not `kid`

Many directories carry no `kid` at all (Meta's, Klaviyo's and You.com's among them), and Google's five keys carry short labels instead of the JWK thumbprint the September draft asks for when `kid` is present. Index each key by the [RFC 7638](https://www.rfc-editor.org/rfc/rfc7638.html) thumbprint you compute yourself, and by its `kid` when it has one.

## What an origin can do today

Use the signals as a ladder, strongest first:

1. **A signature that verifies against a directory you pinned.** Pin the directory URLs you trust instead of fetching whatever host a `Signature-Agent` header names; that header is client input, and fetching it on demand lets strangers make your server issue requests. Honour `Cache-Control` (Google's directory sends `max-age=3600`) and drop keys that disappear.
2. **The operator's published IP ranges or reverse-DNS names.** Every operator in the table without a directory documents one of these.
3. **A user-agent with neither.** That is a claim. Log it as one.

Then decide what each verified identity may do: allow it, rate-limit it, or ask for a license. [Pay-per-crawl at the origin](/blog/pay-per-crawl-at-the-origin/) covers the 401 and 402 side. The free [AI readability check](/ai-readability/) lists AI crawlers that your robots.txt blocks by name.

If you run mod_pagespeed, its verifier takes this kind of list directly; it has shipped in every build since 2.0.34. `PAGESPEED_WEB_BOT_AUTH_KEY_DIRECTORIES` is a comma-separated set of HTTPS directory URLs, fetched in the background. The verdict lands in an `x-verified-bot` response header. It is experimental, off by default and observe-only; the [docs page](/docs/web-bot-auth/) has the setup.

## Rerun it

These numbers will move; rerun the script before you rely on any row here. To check one operator by hand:

```bash
curl -s -o /dev/null -w '%{http_code} %{content_type}\n' \
  -H 'Accept: application/http-message-signatures-directory+json' \
  https://agent.bot.goog/.well-known/http-message-signatures-directory
# 200 application/http-message-signatures-directory+json
```

Today a published directory exists for ChatGPT agent, for Google-Agent (which signs some of its requests), for Meta's test agent and for a long tail of newer agent companies. For the big training and search crawlers, the published IP list is still the check that works.
