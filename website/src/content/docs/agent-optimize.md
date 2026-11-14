---
title: 'Serve Markdown to AI agents'
description: 'Serve AI agents a rendered Markdown copy of your pages at the same URL, and synthesize an /llms.txt — off by default, nothing leaves your server.'
order: 60
group: 'Agents'
lastUpdated: 2026-11-14
faq:
  - q: 'Does this change what browsers and search engines see?'
    a: 'No. Browsers and crawlers receive your normal HTML at the same URL. Only a request that explicitly negotiates the agent representation (Accept: text/markdown), on a deployment where you have enabled agent optimization, receives the rendered-Markdown copy. The response carries Vary: Accept so shared caches never mix the two.'
  - q: 'Is it on by default?'
    a: 'No. The capability ships in every build but stays off until you enable it with the `--agent-optimize` worker flag. There is no separate build.'
  - q: 'Does my content leave my server?'
    a: 'No. Rendering, cleaning, caching, and serving all happen on your own server. Serving a cached response performs zero outbound network I/O.'
  - q: 'Does this work with the module alone, or does it need the optimizer worker?'
    a: 'It needs the optimizer worker. The rendered-Markdown variant runs in the optimizer worker behind nginx, using headless Chrome. The module recognizes and safely declines the agent request (it always serves normal HTML); it does not produce the rendered variant.'
  - q: 'What is the synthesized /llms.txt, and is it your /llms.txt?'
    a: 'The optimizer worker can publish a synthesized /llms.txt for YOUR site — a Markdown index built from your own sitemap, off by default and enabled by its own worker flag. It is unrelated to the /llms.txt on modpagespeed.com, which describes our product. The index is built off the serving path and serving it performs zero outbound network I/O. It needs the worker: the module alone does not produce it.'
---

AI agents and LLM crawlers fetch your pages to read, summarize, and act on them —
but a JavaScript-rendered site often
[looks empty to a client that does not execute scripts](/blog/can-ai-read-your-website/).
The mod_pagespeed optimizer worker can serve those agents a **rendered,
readable copy** of each page, at the **same URL**, without changing anything a
browser or search engine sees.

:::caution[Experimental]
Agent optimization — the rendered-Markdown variant and the synthesized
`/llms.txt` — ships off by default. Its flags and behavior may change in a
future release, so evaluate it in staging before enabling it in production.
:::

## How it works

When an agent requests a page with `Accept: text/markdown` and you have enabled
agent optimization, the optimizer worker serves a Markdown
representation produced from the **fully-rendered DOM** — JavaScript executed,
layout settled. JS-heavy pages that are otherwise invisible to agents become
clean, structured text. Everyone else gets your normal optimized HTML at that
same URL.

The agent response is content-negotiated, with these guarantees:

- `Vary: Accept` — shared caches never serve the Markdown copy to a browser, or
  the HTML to an agent.
- `X-Robots-Tag: noindex` and `Cache-Control: private` — the agent copy stays
  out of search indexes and shared caches.
- The page **body and Content-Type for browsers are unchanged** — standard
  content negotiation on `Accept`, not cloaking.

The rendering runs in the optimizer worker process behind nginx, reusing
the same headless-Chrome subsystem as [browser analysis](/docs/browser-analysis/).

## Off by default

The capability is present in every build, but it does nothing until you enable
agent optimization on the worker (`--agent-optimize`, or
`PAGESPEED_AGENT_OPTIMIZE=true` in the Docker images). It also requires browser
analysis (`--enable-browser-analysis`), since the Markdown copy is produced from
the rendered DOM.

While the flag is off, an agent request **transparently receives the normal
HTML** of the same URL — never an error, never a half-rendered page. Turning the
feature off is always safe.

## Serve an `/llms.txt` site index

The optimizer worker can also publish a synthesized **`/llms.txt`** for your
site — a compact Markdown index that points AI agents at your important pages,
in the [llmstxt.org](https://llmstxt.org/) format. The worker builds it from your own
`sitemap.xml` (intersected with the paths you allow), with a one-line title and
summary per page, and nginx serves it at `/llms.txt` with `X-Robots-Tag: noindex`,
`Cache-Control: private`, and `Content-Type: text/markdown`.

It is off by default and enabled with its own flag (`--agent-optimize-llms-txt`,
which requires `--agent-optimize`). Per-page summaries come for free: from a page's
already-rendered agent copy when one exists, or from a cheap title/description
read otherwise. The file is **never** built by rendering pages on demand. The
index is generated and cached off the serving path and refreshed lazily when your
sitemap changes (or once a day at most).

Content is left out if your `robots.txt` tells AI crawlers to stay away, or if a
page opts out with `noai` / `X-Robots-Tag: noai`. A site that blocks AI crawlers
entirely gets no `/llms.txt` at all.

:::note[Not the same file as ours]
The `/llms.txt` on modpagespeed.com describes the *product*; this feature
publishes an `/llms.txt` for *your* site, from *your* content. They are
unrelated.
:::

Like the rendered-Markdown copy, `/llms.txt` needs the optimizer worker and its
headless-Chrome substrate; the module alone does not produce it.

## Nothing leaves your server

Rendering, cleaning, caching, and serving all happen on your own server. Serving a cached response — a page, the agent Markdown copy, or
`/llms.txt` — makes **no third-party network call**: no DNS, no telemetry, no
analytics, no beacon.

The only network activity happens off the serving path:

- Building the `/llms.txt` index fetches your own `sitemap.xml` and pages.
- Rendering may fetch a page's *own* resources.
- Both go through the same SSRF-guarded fetcher, IP-pinned to your origin.

This makes agent optimization suitable for sovereign and regulated environments
where content must not leave the premises.

## What the Markdown copy contains

The Markdown copy reproduces the page's own content. It is built from the page
as the headless browser renders it, after the page's scripts have run. Those
scripts run only inside that browser, as they would for any visitor; the
conversion step never executes them. The converter treats every piece of text
as data: it does not run it, follow instructions written in it, or fetch
anything it links to. Text and link targets are escaped, so the copy's
structure follows the page's markup. The copy has a fixed size limit.

Content a reader is not meant to see is left out:

- elements hidden in their own markup (`style="display:none"`,
  `style="visibility:hidden"`, the `hidden` attribute, or `aria-hidden="true"`),
  with everything inside them;
- `<noscript>` and `<template>` content;
- scripts, stylesheets, the document head, SVG graphics, and form drop-downs
  and labels;
- page-level navigation, header, and footer regions, unless the page's main
  content sits inside them.

Text hidden by CSS classes or an external stylesheet stays in the copy, because
the conversion works from the page's markup, not from computed styles. Treat the
copy as untrusted page content, not as instructions.

## How the page cache treats signed-in visitors

The page cache is shared between visitors, so it follows rules about requests
that carry credentials or cookies. The cookie rules also apply to the Markdown
copy and `/llms.txt`, which follow the page they were made from.

**Never stored.** A response is not stored when it sets a cookie
(`Set-Cookie`), or when it carries `Cache-Control: private` or
`Cache-Control: no-store`. A response is stored only when its `Vary` names
nothing beyond `Accept-Encoding`, `User-Agent`, `Accept` and `Save-Data`. Any
other field, including `Vary: Cookie`, and `Vary: *`, means it is not stored. For
a response that varies on `Accept`, `User-Agent` or `Save-Data`, those request
headers take part in choosing which stored copy answers.

**Requests with a cookie.** A request that carries a `Cookie` header is treated
by the kind of response:

- A page (every response except css, js and images) fetched for such a request
  is stored and shared only when the origin marks it `Cache-Control: public` or
  gives a single `s-maxage` greater than 0. A stored page is served to a request
  with a cookie only under the same mark. Otherwise the request goes to your
  origin.
- A static asset (css, js, images) fetched for such a request is stored only when
  the origin states a lifetime, with `max-age` or `s-maxage` above 0 (or
  `Expires`, when no `Cache-Control` is sent). Without one, nothing is stored.

**Requests with `Authorization`.** A request that carries an `Authorization`
header is served from a shared copy, and its response is stored, only under the
permit in RFC 9111 section 3.5: the response says `public`, `must-revalidate`
or `s-maxage`. Without one of those, the request goes to your origin and the
response is not stored. Once a copy is stale, it is served to such a request only
when the response said `public`. The Markdown copy is served to a request that
carries `Authorization` at most when the page itself was stored under that
permit. `/llms.txt` is never served to a request that carries `Authorization`.
When license enforcement (`rsl_cap_enforcement`) is on, a request whose
Authorization header carries valid credentials under that scheme is treated as an
agent, not as a signed-in visitor, and may be served the Markdown copy and
/llms.txt. See [RSL-CAP](/docs/rsl-cap/) for details.

### What to do on your origin

- Mark every personalised page `Cache-Control: private`, or send
  `Vary: Cookie`, which is never stored. `public` or `s-maxage` on a page that
  differs per visitor makes it shared.
- A response of any type that differs per visitor must carry
  `Cache-Control: private` or `no-store`; a response with an explicit lifetime
  and no `private` may be stored and shared.
- If your site sets a cookie on every request (analytics, consent), pages that
  are the same for every visitor are shared only when you mark them, for example
  `Cache-Control: public, max-age=600`. Give css, js and images an explicit
  `max-age`.

## Enabling it

Start the worker with `--agent-optimize` (and `--enable-browser-analysis`), then
add `--agent-optimize-llms-txt` if you also want the synthesized `/llms.txt`.
Evaluate it in staging first: the capability is experimental and its flags may
change.

See also: [Analyze pages with headless Chrome](/docs/browser-analysis/). For the
design and rationale,
read [Serve Markdown to AI agents](/blog/serve-markdown-to-ai-agents/) and the
deep-dive on
[serving Markdown and a synthesized /llms.txt to AI crawlers](/blog/serve-markdown-to-ai-crawlers-llms-txt/).
