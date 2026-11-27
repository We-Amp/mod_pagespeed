---
title: 'JPEG XL in the Accept header, and your CDN image cache'
description: 'Chrome 155 and Firefox 158 now send image/jxl on desktop and Android. What that does to a cache keyed on Accept, and four curl commands to check yours.'
date: 2026-10-14
author: 'Otto van der Schaaf'
tags: ['deep-dive', 'jpeg-xl', 'caching', 'cdn', 'content-negotiation', 'image-optimization']
draft: false
product: '2.0'
---

[Chrome 155 shipped](https://developer.chrome.com/blog/jpeg-xl-in-chrome) on 6 October with `image/jxl` at the front of the `Accept` header on every image request on desktop and Android, and Firefox 158 followed on 13 October. On iOS, Chrome and Firefox run on WebKit, not Blink or Gecko ([Chromium](https://github.com/chromium/chromium/blob/main/ios/web/README.md), [Firefox for iOS](https://github.com/mozilla-mobile/firefox-ios/blob/5e40e7cd2bcf17b1f5150e95f0d95364ebc74965/BrowserKit/Sources/WebEngine/WKWebview/WKWebViewProvider.swift)), so this post makes no claim about what they send. Before you weigh JPEG XL against AVIF, check what your CDN does with a request header whose bytes just changed.

A third image format is a cache-key problem before it is a codec problem. The codec question has good answers already: Jake Archibald's [JPEG XL vs AVIF comparison](https://jakearchibald.com/2026/jpeg-xl-vs-avif/) defaults to AVIF 4:4:4 for lossy web images and finds that JPEG XL "can make existing JPEGs 9–20% smaller, without further loss." The negotiation rule itself is in [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/). mod_pagespeed, which I build, does not serve JPEG XL. A request that names only `image/jxl` gets the original format ([worker configuration docs](/docs/worker-configuration/#svg-auto-vectorization)).

## What the browsers send now

The strings come from each browser's source, plus a headless Chrome 155.0.8059.82 capture.

| Browser | Image `Accept` | Source |
| --- | --- | --- |
| Chrome 154 | `image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8` | Chromium `third_party/blink/common/loader/network_utils.cc` |
| Chrome 155 | `image/jxl,image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8` | [`network_utils.cc` at tag 155.0.8059.91](https://github.com/chromium/chromium/blob/155.0.8059.91/third_party/blink/common/loader/network_utils.cc); live capture |
| Firefox 158 | `image/avif,image/jxl,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5` | Gecko [`nsHttpHandler.cpp` at tag `FIREFOX_158_0_RELEASE`](https://github.com/mozilla-firefox/firefox/blob/FIREFOX_158_0_RELEASE/netwerk/protocol/http/nsHttpHandler.cpp); `image.jxl.enabled` defaults to true from 158 |

Safari has decoded JPEG XL since [Safari 17](https://webkit.org/blog/14445/webkit-features-in-safari-17-0/) (2023), and [WebKit's builder](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/loader/cache/CachedResourceRequest.cpp) puts `image/jxl` after `image/webp,image/avif` where the platform has a decoder. So a raw-keyed cache already stores a Safari copy. In October, the Chrome and Firefox strings changed in place.

Neither Chrome nor Firefox puts a weight on `image/jxl` or `image/avif`, so both carry the default weight of 1. The header says "either is fine," not "JPEG XL first." [The Chromium change that turned this on](https://github.com/chromium/chromium/commit/3f500995689490b24355bddb6efdcf76a90ccfb8) keeps the feature flag "as a kill switch," so the string can also change back.

## Three things a cache can do with `Vary: Accept`

A negotiating origin sends `Vary: Accept`. A cache in front has three options, and I ran all three today on stock nginx 1.30 with a test origin that returns AVIF, WebP or PNG. Each cache got four requests, twice: the old Chrome, Chrome 155 and Firefox 158 strings, and `image/webp,*/*` for a client without AVIF.

| Cache setup | Entries stored | First round | What the WebP-only client got |
| --- | --- | --- | --- |
| Honours `Vary` on the raw header (nginx default) | 4 | 4 misses | WebP |
| Keys on the chosen format | 2 | 2 misses | WebP |
| Ignores `Accept` | 1 | 1 miss | AVIF |

The raw-keyed cache stored the same AVIF three times (two Chrome strings, one Firefox), so it treats Chrome 155 as a new client. While the update rolls out "over the coming days/weeks" ([Chrome Releases](https://chromereleases.googleblog.com/2026/10/stable-channel-update-for-desktop_086471744.html)), each image is stored twice for Chrome, and its first Chrome 155 request at each cache node goes to the origin. It is a one-off refill, not a permanent doubling, and it recurs with every release that edits the string. The cache that ignores `Accept` stored one copy and gave AVIF to a client that never asked for it.

## What four CDNs document

These are documented defaults, not bugs. Each option lands in a row above.

| CDN | Documented default | What the documented option does |
| --- | --- | --- |
| Akamai | "Akamai edge servers don't cache responses that include the Vary header, even if the content is cacheable by definition," apart from `Accept-Encoding` ([Remove Vary Header](https://techdocs.akamai.com/property-mgr/docs/rm-vary-header)). On Adaptive Media Delivery, Download Delivery and Object Delivery, the behaviour that removes `Vary` is on by default. | Removing `Vary` makes the response cacheable, and the first response cached is what every client gets. |
| CloudFront | `Accept`: "CloudFront removes the header" before forwarding, and "caches only one version" unless the cache policy names the header ([request headers](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/RequestAndResponseBehaviorCustomOrigin.html), [header caching](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/header-caching.html)). | Adding `Accept` to the cache policy keys on the value as sent, and letter case in the value counts. |
| Cloudflare | "If the vary object is omitted, this Cache Rules Vary setting is turned off" ([Cache Rules settings](https://developers.cloudflare.com/cache/how-to/cache-rules/settings/#vary)); all plans since 2 July ([changelog](https://developers.cloudflare.com/changelog/post/2026-07-02-vary-for-cache-rules/)). [`passthrough`](https://developers.cloudflare.com/cache/concepts/vary/#passthrough) keys on the raw value. For `Accept`, [`normalize`](https://developers.cloudflare.com/cache/concepts/vary/#accept) lowercases the types, strips whitespace, sorts by quality value (alphabetically within a value), strips parameters, then drops the quality values, keeping `q=0`. | A `media_types` list removes every type not on it, so `image/jxl` drops out if you don't list it. Vary for Images (Pro, Business and Enterprise, set through the API) parses `Accept` to pick a format, caches each variant separately, and needs the formats your origin serves listed per file extension ([Vary for images](https://developers.cloudflare.com/cache/advanced-configuration/vary-for-images/)). It is closest to the "keys on the chosen format" row. |
| Fastly | "Fastly does not normalize `Accept` by default," and with `Vary: Accept` "cache performance may noticeably decline" ([Accept](https://www.fastly.com/documentation/reference/http/http-headers/Accept/)). | `accept.media_lookup` in VCL picks one type from a list you supply. |

## Four curl commands against your own CDN

Pick an image URL behind your CDN that nothing else requests, or add an unused query string if your CDN keys on query strings. Pin one edge so all eight requests reach the same cache, then run the block twice.

```bash
URL='https://cdn.example.com/img/hero.jpg?jxl-check=1'
PIN="cdn.example.com:443:$(dig +short cdn.example.com | tail -1)"   # one edge IP
H='^(content-type|vary|age|x-cache|cf-cache-status|cache-status):'
c() { curl -s --resolve "$PIN" -o /dev/null -D - -H "Accept: $1" "$URL" | grep -iE "$H"; echo; }

c 'image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8'
c 'image/jxl,image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8'
c 'image/avif,image/jxl,image/webp,image/png,image/svg+xml,image/*;q=0.8,*/*;q=0.5'
c 'image/webp,*/*'
```

One IP can front several cache servers, so if misses don't line up, read `age` too. How to read the second run:

- **All four hit and all four have the same `content-type`.** The cache ignores `Accept`, or strips it before the origin sees it (default CloudFront).
- **All four hit, the first three say `image/avif` and the fourth says `image/webp`.** The cache collapses `Accept` to a format. Check that the first run missed only twice.
- **The first run missed four times.** The cache keys on raw bytes, and every new browser string costs a refill.
- **Nothing ever hits.** The CDN is not caching responses that vary.

## Key the cache on the format decision

Decide the format once, then key on the decision, not the header. A setup that serves AVIF, WebP and the original has three slots per image URL, whatever the browsers send. JPEG XL would be a fourth slot, worth adding when you have a reason to serve it, and a browser that started asking is not one.

For an nginx cache in front of a negotiating origin, this is the tested config with one line changed:

```nginx
map $http_accept $img_fmt {
    ~*image/avif  avif;
    ~*image/webp  webp;
    default       orig;
}
map $img_fmt $canon_accept {
    avif    "image/avif,image/webp,*/*";
    webp    "image/webp,*/*";
    default "image/jpeg,image/png,image/gif";   # no wildcard
}
# in the image location:
proxy_set_header Accept $canon_accept;   # the origin sees one Accept per slot
proxy_cache_key "$scheme$proxy_host$request_uri|$img_fmt";
proxy_ignore_headers Vary;               # stop nginx keying on the raw header too
```

The `proxy_set_header` line matters most. An origin that sees the raw header can pick a format the cache key doesn't know. That is also why the `orig` slot's header has no wildcard: `*/*` or `image/*` leaves the format to the origin, and whatever it picks lands in `orig`. List the original types the location serves, since [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html#section-12.4.1) lets an origin answer 406 when nothing listed is acceptable.

The snippet doesn't parse q-values, so `image/avif;q=0` still counts as AVIF, and each slot's header must be one your origin answers with that slot's format. The rig ran with `"*/*"` as the `default`; its origin answers PNG to anything without `image/avif` or `image/webp`, so the table holds.

## What mod_pagespeed does with these strings

mod_pagespeed reads `Accept` once per request and sets one image-format slot (AVIF, WebP or the original), preferring AVIF or WebP when the client advertises it ([how classification works](/docs/worker-configuration/#how-classification-works)). Its [CDN integration docs](/docs/cdn-integration/#vary-user-agent) list the image `Vary` header as `Accept, Save-Data, User-Agent, Sec-CH-DPR`.

Measured today on modpagespeed.com with the four strings, the first round got the source PNG; the docs say a HIT "may contain original content if the worker hasn't processed it yet" ([response headers](/docs/worker-configuration/#response-headers)). From the second round on, the old Chrome, Chrome 155 and Firefox 158 strings all got the same 32,112-byte AVIF, and a request for `image/jxl` alone got a PNG.

There, the Chrome change added no variant in mod_pagespeed's own cache. A CDN in front still sees the raw headers, so run the four commands there too.
