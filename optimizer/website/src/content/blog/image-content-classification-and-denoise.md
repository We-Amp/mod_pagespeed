---
title: 'Classify, denoise, then encode: content-aware image encoding'
description: 'Content-aware image optimization classifies each image and denoises noisy sources before encoding, so photos, screenshots and logos get the right codec.'
date: 2026-06-13
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['images', 'image-optimization', 'deep-dive', 'performance']
draft: false
product: '2.0'
---

## A screenshot is not a photo, and the encoder doesn't know that

Take a user-generated content site: product photos shot in a studio, dashboard screenshots someone pasted into a support thread, a flat-color logo, and a phone snap taken in a dim room. Run the whole set through one quality setting and one format policy, and three of those four images get the wrong treatment. The screenshot, which is mostly sharp text and solid fills, gets lossy-compressed until the type fuzzes. The logo, which has fewer than a hundred colors, gets re-encoded as a lossy photo and ends up bigger than a clean PNG-8 would be. The dim-room snap is full of sensor noise, which is high-frequency random signal, and the codec dutifully spends bits trying to preserve every grain of it.

The decoder hands you a pixel buffer. By default the encoder treats every buffer the same way. The buffer itself tells you what kind of image it is and how to encode it, if you look before you encode. That look is what content-aware image optimization does: the classify-and-prepare stage that runs after decode, before any codec touches the pixels.

This is a different stage from the two we've written about elsewhere. [Learned quality prediction](/blog/learned-quality-prediction/) is about *which quality number* to pick once you've decided to encode. [AVIF vs WebP](/blog/avif-vs-webp-2026/) is about *which format wins* on a given image. Content classification and denoising sit upstream of both: they decide what *kind* of image you're holding and clean it up so the encoder isn't fighting the input.

## How content-aware image optimization classifies the buffer

After the worker decodes an image to pixels, it runs a cheap analysis pass over the buffer that's already in memory and assigns the image to a content class. In the 2.0 worker this is the `AnalyzeContent()` step, which returns a content class and per-format quality factors, slotted into the single-decode pipeline after decode and before encoding. It's on by default (`--no-content-analysis` turns it off). No new dependencies, no model file, pure C++ on the pixel buffer.

The signals are the boring, reliable ones from classical image processing:

- **Unique color count.** Sample the buffer and count distinct colors. At or under 64 unique colors the image is classified as an illustration outright; under 256, it's a weak illustration candidate that gets confirmed by a low photo metric. Either way you're almost certainly looking at a logo, an icon, or flat art, not a photograph, so it gets a quality bump (the illustration preset raises the per-format quality factor to 1.10) rather than being treated like a lossy photo.
- **Edge density via a Sobel filter, plus a histogram-based photo metric.** A screenshot or text-heavy image has high edge density (lots of sharp transitions) but a low photo metric (its histogram doesn't spread like a natural photo's). That combination is the fingerprint of UI and text. Text is exactly where lossy artifacts are most visible, so the screenshot class gets *higher* quality (factor 1.15), not lower.
- **Noise via block-variance MAD.** The analyzer estimates noise as the median absolute deviation of per-block (8x8) luminance variances. A high photo metric paired with high noise flags the image as noisy; that class gets a *lower* quality factor (0.85) because, after the noise is cleaned up, it compresses more aggressively. That noise estimate is also the hand-off to the denoise step below.

The output is a routing decision: a content class and a per-format quality factor for the formats the pipeline is about to emit. The classes are `kPhoto`, `kScreenshot`, `kIllustration`, `kNoisy`, and `kUnknown` for anything that doesn't match. Misclassification is a soft failure by design. Pick the wrong class and you still produce a valid, viewable image; it's just sized as if it were a different kind of image. That property is what makes a heuristic classifier safe to run by default. A lightweight ML classifier (MobileNetV3-Small or SqueezeNet, sub-2ms on CPU, trained on photo/illustration/screenshot/text/animation categories) is a possible later refinement for edge cases the heuristics miss, but the heuristic path captures most of the value with zero dependencies. The heuristic path is what ships today; the ML classifier is not built.

## Denoise noisy sources so the codec stops paying for grain

Sensor noise is the encoder's worst customer. It's random, high-frequency, and incompressible, three properties that mean a lossy codec burns bitrate trying to reproduce something a human can't distinguish from a slightly smoother version. Remove the noise before encoding and the same perceptual quality fits in fewer bytes. Research shows roughly 10 to 28 percent average bitrate savings from preprocessing filters on noisy images, with minimal effect on clean studio photos, because there's nothing to remove.

The shipped denoise step is a bilateral filter, applied after resize and before encode, and only when the image was classified `kNoisy` and its noise level exceeds the `denoise_threshold` (default 0.3; set it to 0 to disable). There's also a width gate: it only runs when the image is at least 640px wide, where noise survives downscaling. Clean images skip it entirely and pay nothing. The filter takes two sigma parameters, spatial (kernel size, default 3.0) and range (edge sensitivity, default 25.0); the range sigma is scaled by the estimated noise level, so noisier images get more aggressive smoothing. Heavier options behind the bilateral filter (FFDNet via ONNX Runtime, NLNet) are designed for higher-noise inputs but not built; only the bilateral filter ships.

A bilateral filter is the right default because it smooths flat regions while preserving edges, so denoising the dim-room snap doesn't also blur the text in the screenshot, and the threshold means it never touches the studio shot in the first place. The caveat is real: denoising removes wanted texture if the threshold is too low. Film grain, fabric weave, and skin texture are noise to a variance estimator but signal to a human. That's why the step is threshold-gated, and why pairing it with a perceptual metric matters. The worker verifies encoded output against an [SSIMULACRA2 target](/blog/ssimulacra2-image-quality-verification/) (default 70, also on by default), which catches a denoise pass that smoothed away something it shouldn't have before the result ever reaches cache.

The two steps compound. Classification tells you the dim-room snap is a noisy photo; denoising cleans the grain; then the encoder, fed a smoother buffer and the noisy-class quality factor, produces a smaller file at a perceptual score the SSIMULACRA2 check keeps inside its tolerance band. Each step is cheap relative to what comes after. The worker can spend anywhere from 500 milliseconds to 30 seconds encoding AVIF for a single image, so a content-analysis pass and a conditional bilateral filter are rounding error against that.

## Where this sits in the pipeline, and what's shipped versus designed

The 2.0 worker already does the hard architectural work this builds on: a single decode pass that resizes for viewport and encodes to all requested formats, per-format quality with Save-Data overrides, and proactive variant generation. Content analysis, bilateral denoising, and SSIMULACRA2 verification slot into that flow and all three ship today, on by default. After `DecodeToPixels()`, `AnalyzeContent()` runs on the original pixels (so the class is consistent across viewport sizes); the bilateral filter runs after any viewport resize, gated on the noisy class and the noise threshold; then each format is encoded and verified against the SSIMULACRA2 target, with a bounded re-encode loop if the score falls outside the tolerance band. Each piece has a worker flag: `--no-content-analysis`, `--denoise-threshold`, `--no-quality-verify`, `--target-ssimulacra2`.

Two designed features are *not* built. The heavier ML denoisers (FFDNet, NLNet) behind the bilateral filter are not implemented. Neither is the optional ML content classifier; the shipped classifier is the heuristic path. Treat those two as planned-but-deferred options, not released features.

The reason this work lives inside a transparent proxy rather than an external service is the same reason 2.0 exists at all: the analysis runs on a pixel buffer the worker already has in memory, with no extra network hop, no API call per image, and no separate place for your images to live. Classification is part of deciding format and quality; denoising is part of preparing the buffer; both happen in the same pass that was already going to decode and re-encode the image. For how that decode-once-encode-many flow works end to end, see [how asynchronous rewriting works](/how-it-works/async-rewriting/).

## Related

- [Set how good it should look: a model predicts the encoding parameters](/blog/learned-quality-prediction/)
- [AVIF vs WebP in 2026: which format actually wins](/blog/avif-vs-webp-2026/)
- [Verifying image quality with SSIMULACRA2](/blog/ssimulacra2-image-quality-verification/)
- [Self-hosted image optimization](/blog/self-hosted-image-optimization/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [How asynchronous rewriting works](/how-it-works/async-rewriting/)

If your traffic is a mix of photos, screenshots, and illustrations, a single quality setting is leaving bytes on the table in one direction and quality on the table in the other. Content-aware image optimization fixes that by looking at the buffer before the codec does. mod_pagespeed is maintained and independently developed; you can run the whole image pipeline on your own servers, with your images staying in your infrastructure. Grab a build from the [downloads](/download/) page and read the [configuration reference](/docs/configuration/) for the image filters and the content-analysis, denoise, and SSIMULACRA2 flags, all of which ship on by default. It is licensed under Apache-2.0 and free to run in development and in production, so you can measure the win on your own content.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
