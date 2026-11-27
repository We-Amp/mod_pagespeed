---
title: 'Learned image quality: a model predicts encoding parameters'
description: 'One perception-based quality knob: a trained model predicts JPEG, WebP and AVIF encoding parameters per image in microseconds. No per-codec tuning.'
date: 2026-06-06
lastUpdated: 2026-10-25
author: 'Otto van der Schaaf'
tags: ['images', 'architecture', 'deep-dive']
draft: false
product: '2.0'
---

## You shouldn't have to be a codec engineer

Most image pipelines hand you a panel of quality dials. JPEG quality runs 1 to 100. WebP quality also runs 1 to 100, but the same number doesn't mean what it means for JPEG. [AVIF](/blog/avif-vs-webp-2026/) runs 0 to 100 like the others, and its default is 60. Then there's a second copy of all three for Save-Data visitors. Six numbers across three codecs, none of them interchangeable.

Setting them well means knowing how each encoder behaves. Most people don't, so they copy a number off a blog post (usually 82) and apply it everywhere.

That number is wrong for nearly every image you serve. A photo of a clear sky compresses to almost nothing and still looks perfect. A screenshot of code at the same setting picks up visible ringing around every glyph. A single global quality has to be cautious enough to survive the screenshot, which means it wastes bytes on the photo.

The mod_pagespeed optimizer worker removes that panel of dials. There is nothing to tune.

## Out of the box: nothing to configure

Install it and image optimization runs. You don't set a JPEG quality, a WebP quality, or anything on the AVIF scale, and you don't repeat any of it for Save-Data. There are no encoding parameters to get right, because the worker chooses them per image instead of asking you to.

The defaults aren't a starting point you're meant to tune away from. For image encoding, no configuration is the configuration.

## The one optional knob: how good should it look

If you want to shift the quality-versus-size tradeoff for your whole site, there's a single lever for it. Not a per-codec one. One number: the target perceptual quality, on a 0 to 100 scale.

```
target_ssimulacra2 = 70   # the default; you can leave it alone
```

It reads the way a person thinks about quality rather than the way a codec does:

- 80+ means differences are imperceptible
- 70 to 80 is very good
- 60 to 70 is good

The default is 70. Set 85 for near-pristine output, or 45 for a bandwidth-constrained audience. The number means the same thing across JPEG, WebP, and AVIF. You set the intent; the worker decides the encoder quality for each format and each image.

The scale is SSIMULACRA2, an open perceptual quality metric. It scores how different an encoded image looks from the original, weighted toward what human eyes actually notice rather than raw pixel differences. It tracks human ratings far better than [PSNR or plain SSIM](/blog/ssimulacra2-image-quality-verification/), which is why people who tune codecs for a living have adopted it.

## How it figures out the rest

Say you've left it at 70. The worker now has to find, for each image and format, the encoder setting that lands near a SSIMULACRA2 of 70. The obvious approach is a search: encode at 70, measure, adjust, encode again. That's five or six encode-and-measure rounds per image per format, and neither the SSIMULACRA2 computation nor an AVIF encode is cheap. At that cost the optimization pass crawls.

Instead of searching, the worker predicts. For each output format there's a trained AI model, a LightGBM gradient-boosted decision tree, fit on a large corpus of images. It maps cheap-to-compute features of an image to the encoder quality that hits a given SSIMULACRA2 target. At optimization time it reads the image and your target and returns the setting directly, with no search loop.

The model runs where your images already are. It's compiled from the trained LightGBM tree to C via TL2cgen and built into the binary, so there is no GPU, no inference server, and no call out to a model-as-a-service. Inference takes about 5 microseconds per format, with no runtime dependencies beyond the compiled model. For comparison, decoding the source JPEG takes on the order of 20 milliseconds, several thousand times longer, so the prediction barely registers in the optimization budget.

## The model can be wrong, so it checks itself

A prediction can still miss on an unusual image, and you wouldn't catch it by eye, because you set a target rather than a number.

So every encode is verified. The worker computes the actual SSIMULACRA2 score of the output and compares it to your target. The tolerance band is asymmetric, tighter on the low side: a score below target means quality loss a visitor might see, while a score slightly above just leaves a few bytes unspent. When the output falls outside the band, the worker treats it as an outlier and re-encodes. The model handles the common case and the verification step covers the tail.

## Content awareness

The prediction doesn't start blind. Before compression, each image is [classified as a photo, screenshot, illustration, or noisy](/blog/image-content-classification-and-denoise/), and that class feeds the quality decision.

This is where the per-image savings come from. A photo tolerates aggressive compression, because grain and gradients hide artifacts. A screenshot doesn't: sharp edges and text make artifacts obvious, so it's handled more gently. The model has learned these patterns from the corpus and conditions on the content class, so the sky photo and the code screenshot reach the same perceptual target through different encoder settings. None of that is something you configure.

## Save-Data, also without configuring it

Data-constrained visitors used to mean a second set of per-codec quality numbers. Here they need nothing extra. When a client sends the [Save-Data header](/blog/save-data-bandwidth/), the worker aims a fixed number of perceptual points lower (15 by default) and the same machinery hits the reduced target. If you ever want to change that offset, it's the same kind of single number as the main knob.

It also works with the rest of 2.0. JPEG encoding goes through Jpegli, which gets more quality per byte than libjpeg-turbo at the same setting, so the chosen quality runs on a better encoder to begin with. If you do want manual control, the per-format models can be switched off individually (`--no-learned-quality-jpeg` and so on) and the original codec dials are still there underneath. That's an escape hatch, not a required step.

## Why this matters

Per-image quality has been a known problem for years. The reason most tools still ship one flat number is that doing it properly took solving two hard things at once: measuring quality the way people perceive it, and finding the encoder setting that hits that measure fast enough to run on every image.

SSIMULACRA2 handles the measuring. A trained model compiled to a microsecond-scale function call handles the finding. Together they take the configuration down to a single optional number: say how good it should look, and every image is compressed about as far as it can go without looking worse. Most sites will leave even that number alone.

And it runs on your servers — [self-hosted image optimization](/self-hosted-image-optimization/) in the literal sense. The models are in the binary, the images never leave your infrastructure, and there is no per-image call to a quality-as-a-service vendor.

---

Learned quality prediction ships in the optimizer worker. [Run it in 60 seconds with Docker Compose](/docs/installation-docker/), or [read how the variant matrix works](/blog/viewport-aware-image-optimization/).
