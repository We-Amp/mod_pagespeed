---
title: 'SSIMULACRA2: how to verify image quality after compression'
description: 'How mod_pagespeed scores encoded images against a SSIMULACRA2 target in the optimizer worker and re-encodes until the result clears tolerance.'
date: 2026-06-14
lastUpdated: 2026-10-22
author: 'Otto van der Schaaf'
tags: ['images', 'image-optimization', 'performance', 'deep-dive']
draft: false
product: '2.0'
---

SSIMULACRA2 is a perceptual image-quality metric: it rates the distortions image codecs introduce on roughly a 0 to 100 scale, tuned against human quality ratings, so a given score means about the same thing for a JPEG, a WebP or an AVIF. This post shows how the mod_pagespeed optimizer worker uses it to check an encode and re-encode until it passes.

Set a JPEG encoder to quality 85 and you get whatever quality 85 happens to mean for that particular image. A flat illustration comes out near-lossless and bloated. A noisy night photo comes out with smeared shadows and visible blocking. The number on the dial is an input to the encoder, not a statement about how the result looks. The optimizer worker treats that gap as a bug to close: it measures the encoded output with a SSIMULACRA2 image quality score and, if the result misses the target band, re-encodes at a stepped quality until it lands inside. The function that does this is `VerifySsimulacra2Quality()` in `image_transcoder.cc`, and this post walks through exactly what it does and where it runs.

This is the post-encode side of the story. A sister mechanism, [learned quality prediction](/blog/learned-quality-prediction/), tries to pick a good starting quality *before* the first encode. Prediction picks a likely starting point; verification confirms the result actually meets the target. The two run together, but they solve different halves of the problem.

## Why SSIMULACRA2 instead of PSNR or SSIM

The whole approach depends on having a metric that agrees with human eyes. If your quality score doesn't correlate with what a person sees, then "re-encode until the score passes" just chases the wrong target faster.

PSNR measures mean squared error between pixels. It is cheap and it is what a lot of older tooling reports, but it has no model of perception: it punishes a small global brightness shift that nobody would notice, and it under-counts blocking and ringing artifacts that everybody notices. SSIM is better. It compares local luminance, contrast, and structure rather than raw pixel deltas, so it tracks perceived quality more closely than PSNR. But it still misses a lot of the artifacts modern lossy codecs actually produce.

SSIMULACRA2 is a perceptual metric built specifically to rate the kinds of distortions image codecs introduce, and it is tuned against large datasets of human quality ratings. It scores on roughly a 0 to 100 scale where higher is better, and a given score means about the same thing whether the file is a JPEG, a WebP, or an AVIF. That last property is what makes it usable as a single, format-independent target across a pipeline that emits all three. The optimizer worker calls `pagespeed::ComputeSSIMULACRA2()` on the decoded reference pixels and the decoded encoder output, comparing them at the same width, height, and bytes-per-pixel.

## The SSIMULACRA2 image quality verify-then-re-encode loop

`VerifySsimulacra2Quality()` is a template helper parameterized on an `EncodeFn` and a `DecodeFn` so the same code drives JPEG, WebP, and AVIF without `std::function` overhead. Here is what it actually does, step by step.

First it decodes the encoder output back to pixels and sanity-checks it against the reference. If the decoded output is empty, or its width or height don't match the reference, there is nothing comparable to score — and an incomparable candidate no longer ships quietly: the variant is declined. The one format mismatch that is tolerated is grayscale: a gray source can end up encoded in a form that decodes back as RGB, and near-monochrome RGB is still comparable to a gray reference (the green channel is scored at one byte per pixel, matching the reference). This guards against scoring two images that aren't actually the same picture.

Then it computes the SSIMULACRA2 score once. If verification is on but `ssimulacra2_max_attempts` is 1, that's the end of it: you get the measured score recorded and nothing is re-encoded. The config comment spells this out: `1=check only, 2+=re-encode attempts`. The default is `4`.

If re-encode attempts are allowed, the function computes an asymmetric tolerance band from the target and the base tolerance:

```cpp
float lo = target - tolerance * 0.6f;
float hi = target + tolerance * 1.6f;
```

The asymmetry is deliberate, and the header says why: "Under-quality is user-visible; over-quality just wastes bytes." Being below the target is a visible defect, so the lower edge is held close. Being above the target only costs you file size, so the upper edge is given more room before the loop bothers correcting it. With the shipped defaults (`target_ssimulacra2 = 70.0f`, `ssimulacra2_tolerance = 5.0f`) that band is `[67, 78]`, which the header comment summarizes as `-3/+8`.

The loop then runs up to `max_attempts - 1` times:

```cpp
for (int attempt = 1; attempt < max_attempts; ++attempt) {
  if (score >= lo && score <= hi) break;

  if (score < lo) {
    quality = std::min(100, quality + quality_step);
  } else {
    quality = std::max(min_quality, quality - quality_step);
  }
  // ... re-encode at the new quality, decode, re-score ...
}
```

If the score is below the band, quality goes up by `ssimulacra2_quality_step` (default `5`), clamped at 100. If it's above the band, quality goes down by the same step, clamped at a per-format floor. The new encode is decoded, validated against the reference dimensions again, and re-scored. As soon as the score lands inside `[lo, hi]`, the loop stops early and that encode ships. When the attempts run out without landing in the band, what ships is the attempt closest to the band: from below, the highest-scoring attempt; from above, the last down-stepped one, which is also the smallest. The reported encoder quality is always the shipped attempt's, and a failed encode, a dimension mismatch, or a negative score simply ends the search and falls back to that same band-closest choice.

The verdict is binding. On the WebP and AVIF arms, an encode whose final score sits below the floor of the band is not shipped at all: the variant is declined, the measured score is kept as the reason, and the downstream size gates see a plain failure — the same way an output that saved no bytes is dropped. A candidate the verifier cannot even measure, because the decode-back failed or came back with different dimensions or a different pixel format, is declined on every arm, JPEG included. Two cases still ship by design. A score the metric cannot produce — SSIMULACRA2 needs at least an 8×8 image — is an instrumental limit, not a verdict. And the same-format JPEG arm is exempt from the below-floor decline: its search runs under the source-quality cap, and when the band is unreachable under the cap, the best encode the cap allows is the right thing to ship.

The per-format quality floor is set by the caller. The WebP and AVIF verify calls in `TranscodeMulti` pass a `min_quality` of `0`; the JPEG verify call in `TranscodeMultiResized` passes `1`, so the downward correction for an over-quality JPEG won't drive it below quality 1. The format name passed in (`""` for JPEG, `"WebP"`, `"AVIF"`) just controls the log prefix. The caller reads back two values: a `score` and a `reencoded` flag, surfaced per format on `MultiTranscodeResult` as `ssimulacra2_score` / `ssimulacra2_reencoded`, `webp_ssimulacra2_score` / `webp_ssimulacra2_reencoded`, and the AVIF pair.

## Which encodes actually get scored

The loop above is the same for every format, but it does not run on every encode. Coverage depends on the code path that produced the output.

WebP and AVIF are verified whenever `quality_verify` is on and the encode succeeded. In `TranscodeMulti`, the WebP branch (`EncodeWebpFromPixels`) and the AVIF branch (`EncodeAvifFromPixels`) each call `VerifySsimulacra2Quality()` right after encoding, so the pixel-derived WebP and AVIF variants are always scored.

JPEG is the exception. `TranscodeMulti` optimizes JPEG with the stream-based `OptimizeJpeg`, which never decodes to pixels and runs no verification, so the desktop fast path emits an optimized JPEG with no SSIMULACRA2 score attached. JPEG verification only happens on the pixel re-encode path in `TranscodeMultiResized`, where a resized or denoised image is encoded with `EncodeJpegFromPixels` and then passed to `VerifySsimulacra2Quality()`. The same function also has two JPEG paths that skip scoring entirely: the early exit when the source quality is already at or below the target, and the stream fallback when there was no resize or denoise. Those run `Transcode()` and print no score, because the score log fires only when the measured value is `>= 0.0f`.

## Save-Data shifts the target, not the metric

A visitor sending the `Save-Data` request hint is telling you they would rather have a smaller file than a prettier one. The optimizer worker honors that by [moving the target down rather than turning verification off](/blog/save-data-bandwidth/). In the resized transcode path, when save-data is active, the target is reduced by `savedata_score_reduction` (default `15.0f`), floored at zero:

```cpp
local_config.target_ssimulacra2 =
    std::max(0.0f, local_config.target_ssimulacra2 -
                       local_config.savedata_score_reduction);
```

The whole `[lo, hi]` band slides down with it, so the same loop now converges on a deliberately lower-quality, smaller file. The verification machinery is unchanged; only the goalpost moves. That keeps the behavior predictable: there is exactly one quality target in play, and Save-Data is a transformation on that target, not a separate code path with its own surprises.

A couple of points worth keeping straight. Verification only kicks in when `quality_verify` is true and the encode succeeded. It is a check on output quality, not a size optimizer on its own. Separate size gates handle that: `TranscodeMulti` drops a WebP or AVIF variant that ends up at least as large as the source, and `TranscodeMultiResized` rejects a non-resized pixel-based JPEG that produced no size savings with the message "JPEG re-encode produced no size savings." And because the loop re-encodes and re-decodes on each miss, it does real work; the [learned quality prediction](/blog/learned-quality-prediction/) model exists precisely to make the first encode land in the band so the loop usually has nothing to do.

## Related

- [Set how good it should look: a model predicts the encoding parameters](/blog/learned-quality-prediction/)
- [Content classification and noise-adaptive denoising before encode](/blog/image-content-classification-and-denoise/)
- [SVG auto-vectorization for flat graphics](/blog/svg-auto-vectorization/)
- [Viewport-aware image optimization](/blog/viewport-aware-image-optimization/)
- [AVIF vs WebP in 2026](/blog/avif-vs-webp-2026/)
- [The economics of image optimization](/blog/economics-of-image-optimization/)
- [Self-hosted image optimization](/blog/self-hosted-image-optimization/)
- [How async rewriting works](/how-it-works/async-rewriting/)
- [Largest Contentful Paint](/core-web-vitals/lcp/)

If you want to see the verify loop run on your own images, [download mod_pagespeed](/download/) and watch the worker log: every scored encode prints its SSIMULACRA2 value, and a `re-encoded` note whenever the loop had to correct a miss. The defaults (`target_ssimulacra2 = 70`, four attempts, a step of 5) are a sensible starting point, and every one of those fields is a config knob you can move once you've seen the scores your traffic produces. The full set lives under image transcoding in the [configuration reference](/docs/configuration/). It is licensed under Apache-2.0 and free to run, so you can measure the wins on your own images.

---

*mod_pagespeed and PageSpeed are trademarks of Google LLC; We-Amp B.V. is not affiliated with, endorsed by, or sponsored by Google, and maintains the open-source mod_pagespeed project independently.*
