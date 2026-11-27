---
title: 'Keep C2PA content credentials through image optimization'
description: 'mod_pagespeed detects C2PA Content Credentials manifests and keeps image provenance instead of stripping it when optimizing. On by default in both parts.'
date: 2026-06-22
author: 'Otto van der Schaaf'
tags: ['images', 'c2pa', 'provenance', 'content-credentials']
lastUpdated: 2026-07-04
draft: false
---

> **Status: shipped in both parts of mod_pagespeed.** Provenance preservation is on by default in the module and in the optimizer worker. The PNG carry-through is experimental and off by default.

An image optimizer's job is to make a file smaller. The fastest way to do that is to throw away everything the browser does not need to paint pixels: color profiles it can default, EXIF blocks, XMP packets, and any other metadata riding along in the file. Strip the lot, recompress, ship fewer bytes.

That worked fine when metadata was camera trivia. It does not work fine anymore.

A growing share of images now carries a C2PA manifest, also branded Content Credentials. It is a cryptographically signed record of where the image came from: capture device, edit history, authorship, and whether a generative model touched it. Adobe Photoshop and Firefly add them. Some cameras write them at capture, and AI tools attach them to disclose synthetic origin. Platforms now read them, and regulators are starting to require them.

If your optimizer strips metadata, it silently destroys that manifest. The image still looks identical. The provenance is gone, and nothing in your pipeline tells you it happened.

## What a Content Credentials manifest is

A C2PA manifest is not a tag you can edit by hand. It is a signed assertion set embedded in the file. In a JPEG it lives in an APP11/JUMBF segment. In a PNG it lives in dedicated chunks (`caBX` and `iTXt`). The signature covers the image and the claims together, so a verifier can tell whether either was altered after signing.

That signature is also the hard part for an optimizer. A resize changes the signed pixels, so the original signature no longer verifies. Re-author the manifest and you are signing claims you did not make. Drop the manifest and you have stripped provenance. No transform preserves both a new image size and the original signed manifest.

So the honest options are narrow: carry the original manifest through untouched, or do not modify the image at all.

## How mod_pagespeed preserves C2PA provenance by default

Provenance preservation ships in both parts and is **on by default**. In the module it is the `PreserveImageProvenance` directive, scoped to the directory level; in the optimizer worker it is on unless you pass `--no-preserve-c2pa`. The mechanism is the same either way: detect-and-skip.

Before optimizing an image, the worker does a conservative, signature-only scan for a manifest, once per image. If it finds one, and if the planned optimization would modify the image in a way that drops the manifest (a resize, a format conversion, or a recompress that would not carry the manifest), it serves the **original** bytes instead. You get the original image and an intact, verifiable manifest, rather than a smaller image with the provenance silently removed.

One case is better than a skip. A JPEG carrying an APP11/JUMBF manifest that is **not** being resized can still be recompressed for byte savings, because the JPEG codec carries the manifest segment through the recompress. There you keep both: smaller file, valid manifest.

The detection is deliberately conservative. It looks for a signature, not for every shape a manifest might take, and when anything looks off it falls back to serving the original. That is the design point. The worker would rather hand back the untouched image than hand back something it modified in a way that breaks the manifest. We are not claiming it can never miss an exotic manifest layout. We are claiming the failure mode is "you keep your original image," not "your provenance is gone."

## The trade-off, stated plainly

A manifest-bearing image may skip some optimization under the default. It will not be converted to WebP or AVIF, because those outputs would not carry the manifest. A resized variant of a signed image gets served as the original instead of a smaller resized file.

That cost is real, and it is scoped. It only applies to images that actually carry a manifest, which is still a small slice of real-world traffic today. For most sites the measured impact is near zero, because almost none of their images are signed. The images that are signed are usually the ones where provenance is the point: press photos, AI-disclosure images, anything an editor signed on purpose.

If you do not care about provenance on your site, you can turn the behavior off. Set `PreserveImageProvenance` to off (1.15) or pass `--no-preserve-c2pa` (2.0) and the worker goes back to strip-and-optimize for every image, signed or not. It is a single switch.

## Opt-in carry-through for PNG

Carry-through ships in both parts but is **off by default**, and it is PNG-only: `ImageProvenanceCarry` in the module, `--c2pa-carry` in the optimizer worker. Think of it as Level A carry-through.

With it enabled, a non-resized PNG that carries a manifest gets recompressed for byte savings, and its original manifest chunks (`caBX` and `iTXt`) are spliced into the recompressed output unmodified. The worker never re-authors the manifest. It copies the original signed chunks verbatim into the new file. On any anomaly, such as a chunk layout it does not recognize or anything that would risk corrupting the manifest, it falls back to the same detect-and-skip behavior as the default and serves the original.

It is off by default out of caution, not for lack of capability. Chunk splicing touches the manifest's container, and we would rather operators opt into that explicitly than have it happen everywhere automatically. If you serve signed PNGs and want byte savings on the non-resized ones while keeping the original manifest chunks intact, with a fall-back to serving the original on any anomaly, this is the directive to turn on.

## How to confirm it on your own setup

Test it. Serve a JPEG or PNG that carries a C2PA manifest through the optimizer and request a resize. You get the original bytes back, not a stripped, resized image. Verify the manifest in any C2PA-aware tool; it should still validate.

Then turn on carry-through (`ImageProvenanceCarry` in the module, `--c2pa-carry` in the optimizer worker), serve a non-resized PNG with a manifest, and inspect the PNG chunk stream on the response. The output is recompressed and smaller, and the `caBX` and `iTXt` chunks are present and byte-identical to the input. The repository ships rewriter tests covering exactly these cases, so the behavior is pinned in CI rather than asserted in a blog post.

For the precise per-case verdicts (which optimizations are skipped, which carry through, what the worker does at each boundary) read the test cases rather than trusting a paraphrase. They are the source of truth, and they stay current with the code when this post does not.

## Where this sits

Provenance preservation is one piece of a larger shift: the origin server, not a third-party service, is where image decisions should be made. Keeping a signed manifest intact is part of keeping control of your own content. We cover the economics of doing image work on your own infrastructure in [the economics of image optimization](/blog/economics-of-image-optimization/). [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/) explains the resize logic that decides whether an image gets a new variant at all, the same decision point where provenance is preserved or carried. [The agentic web at the origin](/blog/agentic-web-at-the-origin/) makes the broader case for running this at the origin.

**Next step:** run a signed image through your staging instance and confirm you get the original back on resize; if you serve signed PNGs, enable carry-through (`ImageProvenanceCarry` in the module, `--c2pa-carry` in the optimizer worker) to also save bytes on non-resized ones. Start from [viewport-aware image optimization](/blog/viewport-aware-image-optimization/) to see how the resize decision interacts with provenance preservation.
