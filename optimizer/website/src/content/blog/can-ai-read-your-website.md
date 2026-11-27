---
title: 'Can AI read your website? We tested the top 1,000 sites'
description: '40% of the top 1,000 sites serve almost nothing to non-JavaScript AI crawlers: ChatGPT, Perplexity and Claude see an empty page. The data, and the fix.'
date: 2026-05-28
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['ai-readability', 'ai-crawlers', 'llm-seo', 'client-side-rendering', 'research']
draft: false
---

We rendered the top 1,000 websites and measured what an AI agent can actually
read from each one. Of the 711 sites we could read at all, **403 — 57% — show a
non-rendering agent a nearly empty page.** The content is there for your browser.
It is invisible to most AI crawlers. (That is 40% of the full top 1,000.)

To measure it, we built a scanner — [RenderPeek](/ai-readability/) — and pointed
it at the top 1,000 sites on the web (Majestic's referring-domains ranking — the
most-_linked_ sites, not the most-visited). For each one it does what a careful
AI tool does: fetches the raw HTML, renders the page in a real browser, then
extracts the readable content both ways and compares.

The short version: a large share of the most-linked sites on the web are nearly
empty until JavaScript runs — and many AI tools never run it.

## 403 of the top 1,000 are invisible without JavaScript

Those 403 sites returned almost no readable content from their raw HTML but
plenty after we rendered their JavaScript. That is the **client-side rendering
gap**: the content is there for a browser, but invisible to any agent that reads
HTML without executing scripts — which is how most non-Google crawlers still work
today.

One caveat before that number travels too far. We set aside another 289
sites that returned little to nothing _even after_ rendering — almost all of them
were blocking automated browsers (bot challenges, login walls). That is a real
problem, but a different one, so those sites are out of the figure above. Of the
**711 sites we could read at all, 403 — 57% — revealed their content only after
JavaScript ran.**

The full graded list is here: [ai-readability-top-1000-2026-05-28.csv](/data/ai-readability-top-1000-2026-05-28.csv).

## The names you know came back nearly blank

The pages that returned almost no content in raw HTML are names everyone knows.
As measured on 28 May 2026: Instagram, YouTube, Facebook, X, TikTok, Pinterest,
Reddit, Amazon, Spotify, Shopify, PayPal, Dropbox, and Booking.com all build
their content in the browser, so their homepages arrive as an application shell
with little readable text in the HTML itself.

For a person with a browser, that is fine. For an AI agent that reads HTML, the
page is close to blank. This is often a deliberate product choice — these are app
front ends, not articles — and it does not mean the rest of the site is
unreadable. But it does mean the public face of some of the biggest sites on the
web is invisible to an agent that does not render.

See which side your site is on: [scan it free](/ai-readability/) — about ten
seconds, no signup.

## Same publisher, opposite outcome

The cleanest illustration is the BBC. `bbc.co.uk` came back as an A — its content
is right there in the HTML. `bbc.com`, a different front end, fell into the
client-side rendering gap: same organization, same journalism, opposite outcome
for a machine reader. The only difference is how the page is delivered.

## JavaScript-heavy does not mean unreadable

Plenty of large, JavaScript-heavy sites serve their content cleanly. As measured
on 28 May 2026: Apple and Discord both scored 92/100 — real content in the HTML,
read cleanly by an agent. Stripe, Zoom, Harvard, and the BBC's UK site all earned
an A. Wikipedia, GitHub, Microsoft, Mozilla, LinkedIn, Stack Overflow, and Figma
earned a B. None of these are static brochures; they are big, dynamic properties
that still put their content in the initial HTML.

## How the top 1,000 graded out

Each site gets an A–F grade from a five-part rubric: content fidelity, token
efficiency, structured data, crawler access, and citation readiness.

| Grade | Sites | Share |
| ----- | ----: | ----: |
| A     |    94 |    9% |
| B     |   150 |   15% |
| C     |   102 |   10% |
| D     |   227 |   23% |
| F     |   427 |   43% |

Two-thirds of the top 1,000 landed at D or F.

## Why a blank page costs you

Search crawlers like Googlebot render JavaScript before indexing. Most of the
newer LLM crawlers do not — they fetch raw HTML and move on, because rendering
every page in a browser is slow and expensive at their scale. If your content
only exists after JavaScript runs, those tools see an empty page. And an empty
page does not get summarized, cited, or recommended.

## What to do about it

The fix is the same one that helps search engines and slow connections: get your
content into the HTML your server sends.

- **Server-side render or pre-render** your main content so it is present before
  JavaScript runs.
- **Cut markup bloat** so the signal is not buried — many pages ship more wrapper
  markup than content. (We wrote up one way to do this at the server layer in
  [server-side critical CSS for nginx](/blog/server-side-critical-css-nginx/).)
- **Add structured data and clear headings** so machines can tell what the page
  is about.

This is the kind of work [mod_pagespeed](/) has done for over a decade: it
optimizes pages and cuts page weight on your own server, which makes the markup
lighter for people and easier for machines to parse. The mod_pagespeed optimizer worker goes a step
further with an experimental, off-by-default option to
[serve a clean, fully-rendered Markdown copy to AI agents](/blog/serve-markdown-to-ai-agents/)
at the same URL — the content an agent needs, without changing what a browser
sees. If that would be useful to you, [tell us](/contact/).

## Check your own site

You can run the same AI readability check RenderPeek ran on the top 1,000.
[RenderPeek](/ai-readability/) renders one page, grades it A–F, shows you exactly
what an agent reads from your raw HTML versus after JavaScript, and gives you a
shareable result link. Free, no signup.

## Update — Google is now grading this too

Since we published this, Google shipped an experimental "agentic browsing" audit
in Lighthouse, and Chrome began previewing **WebMCP** — a way for a page to expose
actions an agent can invoke, like a search box or a form, instead of only text to
read. It validates the premise of this study: whether an AI agent can use your
site is now something the standard web tooling measures, not a fringe concern.

One thing that audit cannot do, though, is see the gap measured above. Lighthouse
runs inside your browser and audits the page _after_ JavaScript has run, so it
always holds the fully-rendered DOM. It cannot show you what a non-rendering agent
gets from your raw HTML. Measuring that difference is exactly why RenderPeek
renders the page _and_ fetches the static HTML, then compares the two.

WebMCP is about a different capability — an agent acting on your page, calling that
search box or submitting that form — but one catch ties it back to everything
above. The common way to register WebMCP tools runs in JavaScript. So on a page
whose content is already JavaScript-only, a non-rendering agent gets neither your
content nor your tools. The fix is the same on both sides: put it in the HTML your
server sends. (RenderPeek now flags WebMCP readiness as a bonus check on top of
the five read-side checks in the rubric.)

## Methodology

The method matters, so a few caveats:

- We scanned each site's **homepage**, not deep article or product pages. A
  homepage that is a JavaScript shell does not mean the whole site is unreadable.
- The scanner is **off-the-shelf parts** — headless Chromium plus Mozilla's
  Readability extraction — not our optimization software. It approximates what a
  competent AI crawler does; a given agent may do better or worse.
- Grades come from a rubric we **calibrated by hand**. They are directional, not
  an official score.
- This is a **snapshot** of one ranking on one day, not a census of the web.

The full dataset is published
[as a CSV](/data/ai-readability-top-1000-2026-05-28.csv) so you can check any of
the numbers above. The point is not the exact grade for any single site — it is
the pattern: a lot of the web is being built in a way that machines increasingly
cannot read.

---

**Related:** [the agentic web at the origin](/blog/agentic-web-at-the-origin/)
· [serve rendered Markdown to AI agents](/blog/serve-markdown-to-ai-agents/)
· [Agent Optimize (docs)](/docs/agent-optimize/)
· [mod_pagespeed alternatives in 2026](/blog/mod-pagespeed-alternatives/)
· [server-side critical CSS for nginx](/blog/server-side-critical-css-nginx/)
· [mod_pagespeed for nginx](/alternatives/mod-pagespeed/)
