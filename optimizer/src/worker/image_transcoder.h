// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Image Transcoder for Worker
//
// Transcodes images between formats based on client capability mask.
// Bridges the worker (pagespeed namespace) and image library
// (pagespeed::image_compression namespace).

#ifndef PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_
#define PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lib/base/message_handler.h"
#include "lib/classify/capability_mask.h"
#include "lib/image/content_analyzer.h"

namespace pagespeed {

class MessageHandler;

// Target widths for viewport-based image resizing.
// Desktop uses 0 (no resize) by default since it receives full-size images.
struct ViewportWidths {
  uint32_t mobile = 480;
  uint32_t tablet = 768;
  uint32_t desktop = 0;  // 0 = no resize
};

// Configuration for image transcoding.
struct ImageTranscoderConfig {
  // JPEG output quality (1-100). 85 is a good default.
  int jpeg_quality = 85;

  // WebP output quality (0-100). 75 is a good default for lossy.
  int webp_quality = 75;

  // libwebp lossy encoder effort (0 = fastest, 6 = smallest output at the
  // same quality). Used by the pixel-path lossy encodes in the quality
  // search. 6 trades extra encode time for about 5 percent smaller WebP at
  // the same SSIMULACRA2 target, which keeps more photos below the size of
  // their optimized original-format image (#406). Not exposed as a config
  // option (command line or PATCH /v1/config).
  int webp_method = 6;

  // WebP alpha quality (0-100). 100 for lossless alpha.
  int webp_alpha_quality = 100;

  // AVIF quality (0-100, higher = better). 60 is a good default.
  int avif_quality = 60;

  // AVIF encoder speed (0-10, higher = faster). 6 is a good default.
  int avif_speed = 6;

  // Whether to produce progressive JPEGs.
  bool progressive_jpeg = true;

  // Whether to use lossy JPEG compression.
  bool lossy_jpeg = true;

  // Target widths per viewport class.  0 = no resize.
  ViewportWidths viewport_widths;

  // Save-Data quality overrides (lower quality for bandwidth savings).
  int savedata_jpeg_quality = 60;
  int savedata_webp_quality = 50;
  int savedata_avif_quality = 45;

  // Content-aware quality presets: analyze decoded pixels to adjust
  // quality per content class (photo, screenshot, illustration, noisy).
  bool content_analysis = true;

  // SSIMULACRA2 perceptual quality verification for all image encodings.
  // When enabled, checks encoded output quality and re-encodes if outside
  // the asymmetric tolerance band [target - 0.6*tol, target + 1.6*tol].
  // Under-quality is user-visible; over-quality just wastes bytes.
  bool quality_verify = true;
  float target_ssimulacra2 = 70.0f;    // Target score (0-100)
  float ssimulacra2_tolerance = 5.0f;  // Base tolerance (asymmetric: -3/+8)
  // 1 = verify the first encode only, with the verdict binding: a
  // below-floor score declines the variant with no rescue re-encode
  // attempt (#1381). 2+ = re-encode attempts before the verdict.
  int ssimulacra2_max_attempts = 4;
  // How far BELOW its starting quality the search may look, as
  // |ssimulacra2_quality_step| x (|ssimulacra2_max_attempts| - 1) points.
  // It bounds only that direction: upward the search may reach the whole
  // quality range, because a variant that scores under the floor is not
  // served at all, while an over-quality one only costs bytes.
  int ssimulacra2_quality_step = 5;

  // Extra encodes a WebP or AVIF arm may spend, at the same quality floor,
  // to come in under the optimized original-format image of its slot before
  // the size gate drops it (#406). WebP uses up to this many, AVIF at most
  // one. 0 disables the recovery and restores the plain drop. Independent of
  // ssimulacra2_max_attempts: it applies after the verify loop has finished,
  // even when that loop was limited to one encode. Not exposed on the command
  // line or through PATCH /v1/config.
  int budget_rescue_attempts = 2;

  // Learned quality prediction: use per-format ML models to predict
  // the encoder quality parameter for the target SSIMULACRA2 score.
  // Falls back to base*factor when the model returns an invalid prediction.
  bool learned_quality = true;
  bool learned_quality_jpeg = true;
  bool learned_quality_webp = true;
  bool learned_quality_avif = true;
  float savedata_score_reduction =
      15.0f;  // Reduce target by this for Save-Data

  // Quality cap: a same-format (JPEG->JPEG) re-encode never exceeds the
  // detected source quality.  Encoding above the source spends bytes on
  // artifacts already baked into the source and yields no visual gain.
  // A floor of 30 prevents over-aggressive capping when the DQT-based
  // estimate UNDERSTATES the source (Guetzli, Photoshop custom tables).
  //
  // |quality_cap_margin| is retained for configuration compatibility but no
  // longer raises the cap above the source quality (#1284); the floor and
  // the margin are separate concerns and only the floor survives.
  int quality_cap_margin = 10;  // deprecated: no longer widens the cap
  bool no_quality_cap = false;  // --no-quality-cap

  // Preserve C2PA / Content Credentials provenance, on by default.
  // JPEG->JPEG (kOriginal) recompress carries the APP11/JUMBF manifest verbatim.
  // For every path that cannot carry it -- AVIF/WebP transcode, viewport resize, PNG,
  // and the XMP/APP1 form -- the transcoder serves the ORIGINAL bytes unchanged
  // (skip-not-strip) rather than silently dropping provenance; it never strips. The
  // detector only scans for marker signatures, never decoding/validating/re-emitting
  // a manifest (D3). Byte-exact carry THROUGH transcode (e.g. C2PA in AVIF/WebP
  // boxes) is Track B, demand-gated and deferred.
  bool preserve_c2pa = true;  // --no-preserve-c2pa

  // Level A (carry-through), PNG-only, OFF by default (--c2pa-carry).
  // Strict enhancement of preserve_c2pa: when a manifest-bearing PNG would stay in
  // PNG form (kOriginal), recompress it and re-splice its original caBX/iTXt chunks
  // before IEND, so the PNG is BOTH optimized AND keeps its manifest, instead of the
  // default skip-not-strip (serve original, no optimization). PNG-only because the
  // JPEG->JPEG codec already carries APP11/JUMBF; AVIF/WebP/resize still cannot carry
  // and remain skip-not-strip even with this on. Fail-safe to skip-not-strip on any
  // anomaly; never decodes/validates/re-emits the manifest (D3). Requires
  // preserve_c2pa (the carry runs inside the preserve gate). Demand-gated.
  bool c2pa_carry = false;  // --c2pa-carry
};

// JPEG quality inputs for a multi-format transcode.
//
// These two values are both "a JPEG quality number" but mean opposite
// things, and passing one where the other belongs is silent: the cap goes
// inert, or the encoder is pinned to an unrelated quality.  They are grouped
// in a named struct so every call site has to name the field it is filling
// and a short-by-one argument list fails to compile instead of misbinding
// (#1284).
struct JpegQualityInputs {
  // Encoder quality already discovered for THIS image by an earlier call in
  // the same variant loop (-1 = none).  Reuses that result instead of
  // repeating the quality search; it is a target, not a source measurement.
  // Ignored when no resize applies: a full-size pass never runs the search,
  // so its JPEG must not depend on the passes before it (#482).
  int carried_hint = -1;

  // Source JPEG quality estimated from the input's DQT tables
  // (-1 = unknown or non-JPEG).  Ceiling for a same-format re-encode.
  int source_quality = -1;
};

// Result of an image transcoding operation.
struct TranscodeResult {
  bool success = false;
  std::string output_data;
  std::string output_mime_type;
  std::string error_message;
};

// Verify outcome of a same-format WebP re-encode (#1385), surfaced by
// OptimizeWebp for callers that record per-arm verification evidence.
// Plain data on purpose: the full verify result type is file-local to
// the implementation.
struct SameFormatWebpVerify {
  float score = -1.0f;  // SSIMULACRA2 verdict (-1 = not measured)
  bool reencoded = false;
  bool declined = false;
};

// Decoded pixel buffer from a source image.
struct DecodedImage {
  std::string pixel_buffer;  // Raw RGB/RGBA/Gray pixels
  uint32_t width = 0;
  uint32_t height = 0;
  int bytes_per_pixel = 3;  // 3=RGB, 4=RGBA, 1=Gray
  bool has_alpha = false;
};

// Result of multi-format transcoding from a single decode pass.
struct MultiTranscodeResult {
  TranscodeResult webp;
  TranscodeResult avif;
  TranscodeResult optimized_original;
  // How many converted-format slots this call refused because the per-format
  // fall-through produced no conversion (#1374).  Counted so the refusal is
  // observable in production: without it an operator cannot tell "there was
  // nothing to convert" from "the conversion was never attempted", which is
  // the exact blind spot that let #1374 sit unnoticed.
  uint32_t unconverted_fallthrough = 0;

  // Cross-format size gate outcomes (#406). A refusal is a WebP or AVIF
  // that was not smaller than the optimized original-format image of its
  // slot, counted before any recovery attempt; a rescue is a refusal that a
  // smaller encode at the same quality floor turned into a variant that
  // ships. refused minus rescued is what was dropped. Refusals against a
  // stored image (made by the worker, which gets no recovery attempt) count
  // as refused too.
  uint8_t size_refused_webp = 0;
  uint8_t size_refused_avif = 0;
  // Save-Data arms refused by the worker's gate because they were not smaller
  // than the same variant without Save-Data. Set by the worker, not by the
  // transcoder; counts arms of any format, so it can reach 3.
  uint8_t size_refused_savedata = 0;
  uint8_t size_rescued_webp = 0;
  uint8_t size_rescued_avif = 0;
  // SSIMULACRA2 score of the original-format image the converted arm lost
  // to, measured only when a converted arm is refused (-1 = not measured).
  float refused_baseline_ssimulacra2 = -1.0f;

  // Content analysis result (populated when content_analysis is enabled).
  QualityPreset applied_preset;

  // Learned quality prediction tracking.
  bool used_learned_quality = false;  // At least one format used ML prediction
  uint8_t learned_quality_fallbacks = 0;  // Formats that fell back to heuristic

  // SSIMULACRA2 quality verification results (per format, -1=N/A).
  // The unprefixed pair is the ORIGINAL-FORMAT arm: a JPEG re-encode,
  // or a same-format WebP re-encode (#1385).
  float ssimulacra2_score = -1.0f;     // Final original-format arm score
  bool ssimulacra2_reencoded = false;  // Whether that arm re-encoded
  float webp_ssimulacra2_score = -1.0f;
  bool webp_ssimulacra2_reencoded = false;
  float avif_ssimulacra2_score = -1.0f;
  bool avif_ssimulacra2_reencoded = false;

  // Verification declined the variant (mpp #790): the shipped candidate
  // scored below the band floor (the cross-format arms, and since #1385
  // the same-format WebP re-encode lane -- the capped same-format JPEG
  // arm stays accept-at-the-cap, #1284) or could not be measured at all.
  // The arm's TranscodeResult carries success=false with the decline
  // reason; the score fields above keep the measured evidence.
  bool ssimulacra2_declined = false;
  bool webp_ssimulacra2_declined = false;
  bool avif_ssimulacra2_declined = false;

  // Final qualities after SSIMULACRA2 adjustment (-1 = N/A).
  // Carried to subsequent calls in the same proactive loop to
  // avoid redundant re-encode searches.
  int final_jpeg_quality = -1;
  int final_webp_quality = -1;
  int final_avif_quality = -1;

  // Source JPEG quality from DQT tables (-1 if unavailable/non-JPEG).
  int source_jpeg_quality = -1;

  // Quality baselining tracking.
  int quality_capped_count = 0;  // Number of times JPEG quality was capped
  bool skipped_jpeg_reencode =
      false;  // JPEG re-encode skipped (source <= target)
};

// One candidate encode in the SSIMULACRA2 verify loop's selection:
// attempts[0] is the caller's initial encode, the rest are retries in the
// order they were produced.
struct VerifyAttempt {
  float score = 0.0f;  // SSIMULACRA2 verdict for this attempt
  size_t size = 0;     // encoded byte size
};

// Band-closest attempt selection (mpp #790 D2), extracted as a free
// function so the mechanism is directly testable: a compiling ship-last
// mutant survived the whole suite while this logic lived inline, because
// every encoder-driven fixture's score ladder is monotone or terminates
// in-band -- configurations where band-closest and the pre-fix ship-last
// agree.  Synthetic (score, size) lists have no such blind spot.
//
// Returns the index of the attempt to ship.  Distance is zero inside
// [lo, hi], else the gap to the nearer edge: below the floor the highest
// score wins, above the ceiling the down-stepped later attempt wins.
// Ties resolve to the smaller byte size, full ties to the later attempt.
//
// With |decline_below_floor| armed, the policy is asymmetric: an in-band
// or above-band attempt is shippable, a below-floor one is declined.  A
// below-floor attempt therefore never displaces a shippable one, however
// much closer to the band it measures -- otherwise a score cliff wider
// than the band across one quality step would decline a variant whose
// above-band attempt was perfectly servable.  When nothing is shippable
// (or the policy is ship-below-floor), plain band-closest applies.
size_t SelectVerifyAttempt(const std::vector<VerifyAttempt>& attempts, float lo,
                           float hi, bool decline_below_floor);

// Candidate pixels made comparable with the reference for scoring.
// ok=false means NO VERDICT is possible (dims/format mismatch or a
// chroma-ghost candidate): the caller declines (fail-closed).
struct ComparablePixels {
  bool ok = false;
  const uint8_t* pixels = nullptr;  // into candidate, or gray buffer below
  std::string gray;                 // extracted G channel (1<->3 rule)
};

// S3.2 comparability guard: dims must match and bpp must match, with one
// carve-out -- a bpp=1 reference accepts a bpp=3 MONOCHROME candidate and
// the metric then runs at bpp=1 on the candidate's G channel. Everything
// else (1<->4, 4<->3, 3<->1, chroma ghosts) is not comparable.
ComparablePixels MakeComparable(const DecodedImage& reference,
                                const DecodedImage& candidate);

// The lower edge of the SSIMULACRA2 acceptance band for |target|: the floor a
// converted encode must reach.  Shared by the verify loop and the budget
// recovery so the two cannot drift apart.
inline float BandFloor(float target, float tolerance) {
  return target - tolerance * 0.6f;
}

// Result of SSIMULACRA2 quality verification and optional re-encode loop.
struct Ssimulacra2VerifyResult {
  float score = -1.0f;
  bool reencoded = false;
  // The verify refused the variant (mpp #790): callers must treat the
  // encode as failure-to-produce. Either the shipped candidate scored
  // below the band floor (decline policy armed; negative scores are
  // legitimate catastrophic verdicts) or no verdict existed at all
  // (fail-closed on an unmeasurable candidate or a metric failure).
  bool declined = false;
  // No verdict was ever produced: the decode-back failed, was not
  // comparable, or the metric itself could not run, so |score| is the
  // untouched sentinel, not a measurement.  Distinct from a measured
  // (possibly negative) score, which is evidence.
  bool verdict_missing = false;
  // The floor actually applied (target - 0.6*tol) -- decline evidence for
  // the caller's failure reason.
  float band_lo = -1.0f;
};

// Generation of the quality search below.  A refusal is only as good as the
// search that produced it: when the search changes, a variant it could not
// reach before may now be reachable, so anything that REMEMBERS a refusal
// must record this value and ignore what an older generation wrote.  Bump it
// whenever the search's reach changes.
inline constexpr std::uint8_t kVerifyQualitySearchVersion = 2;

// Whether a below-floor final score refuses the variant.
enum class VerifyDeclinePolicy : std::uint8_t {
  // Below-floor scores ship as today. The same-format JPEG arm: the
  // source-quality cap can make the band genuinely unreachable, and the
  // best encode under the cap must still ship (#1284 accept-at-ceiling).
  // NOTE: never infer this from max_quality -- EffectiveJpegQualityCap
  // returns 100 when the source quality is unknown, which would silently
  // arm decline on the JPEG arm.
  kShipBelowFloor,
  // A shipped candidate scoring 0 <= score < lo is declined; the variant
  // is not produced. The four cross-format arms only.
  kDeclineBelowFloor,
};

// Verify encoded output quality against a SSIMULACRA2 target and optionally
// re-encode with adjusted quality until the score falls within the asymmetric
// tolerance band [target - 0.6*tol, target + 1.6*tol].
//
// The search is a BISECTION: a below-floor score means the band lies above
// the probed quality, an above-ceiling score means it lies below, and the
// next probe is the midpoint of what remains.  The attempt budget
// (|max_attempts|, the initial encode included) is unchanged.  Upward those
// attempts reach all the way to |max_quality|, which is what stops an
// encodable variant being refused as unreachable; downward they reach
// |quality_step| x (|max_attempts| - 1) points below the starting quality
// and no further.  The asymmetry is deliberate and is explained at the
// search itself.
//
// Attempt selection is band-closest (mpp #790 D2): an attempt landing inside
// the band terminates the search and ships; otherwise the attempt closest to
// the band ships (below the floor: the highest score; above the ceiling: the
// smaller-scoring body, which is also the smallest), ties broken by smaller
// byte size. The shipped attempt's quality is written back through |quality|
// and |reencoded| reports whether it is a retry rather than the initial
// encode.
//
// Decline: with |decline_policy| == kDeclineBelowFloor a SHIPPED score
// below the floor refuses the variant (verify.declined) -- INCLUDING
// negative scores, which the metric returns as legitimate catastrophic
// verdicts ("very different images"). The one instrumental limit is a
// sub-8x8 REFERENCE (the metric needs 8x8): tiny icons keep shipping
// without a verdict. A shipped candidate that cannot be measured at all --
// decode-back failure, a non-comparable decode, or a metric-internal
// failure (score_fn returning nullopt) -- is declined at EVERY call site,
// including the ship-below-floor JPEG arm: fail-closed; a RETRY that cannot
// be measured keeps the already-verified prior attempt.
//
// |quality| is searched within [min_quality, max_quality].  max_quality is a
// hard ceiling, not a preference: for a same-format JPEG re-encode it is the
// source-quality cap, and the loop accepts the best encode reachable under it
// rather than climbing past it to reach the score band (#1284).
//
// Template parameters avoid std::function overhead; each is injected so the
// loop is directly testable against synthetic encoders/decoders/scorers
// (#1382 -- the same reason SelectVerifyAttempt left the .cc):
//   EncodeFn: (const DecodedImage&) -> TranscodeResult
//   DecodeFn: (std::string_view)    -> DecodedImage
//   ScoreFn:  (const ComparablePixels&) -> std::optional<float>
//             nullopt = the metric could not produce a verdict.
template <typename EncodeFn, typename DecodeFn, typename ScoreFn>
Ssimulacra2VerifyResult VerifySsimulacra2Quality(
    const DecodedImage& reference, TranscodeResult& encoded, EncodeFn encode_fn,
    DecodeFn decode_fn, ScoreFn score_fn, int& quality, int min_quality,
    int max_quality, const char* format_name, float target, float tolerance,
    int max_attempts, int quality_step, VerifyDeclinePolicy decline_policy,
    MessageHandler* handler) {
  Ssimulacra2VerifyResult verify;

  const float lo = BandFloor(target, tolerance);
  const float hi = target + tolerance * 1.6f;
  verify.band_lo = lo;

  // The metric cannot run on images smaller than 8x8 (quality_verifier.cc
  // makes the same structural check). A sub-8x8 REFERENCE has no verdict
  // to bind -- tiny icons keep shipping -- while any verdict below the
  // floor, negative ones included, declines.
  const bool reference_too_small = reference.width < 8 || reference.height < 8;

  DecodedImage decoded_output = decode_fn(encoded.output_data);
  ComparablePixels comparable = MakeComparable(reference, decoded_output);
  if (!comparable.ok) {
    // Silent score-N/A is what let the AVIF arm ship unverified for
    // months (#1274) -- a no-op verify must be LOUD, and an unverifiable
    // shipped candidate is now declined rather than shipped UNVERIFIED.
    if (handler != nullptr) {
      handler->Warning(
          "SSIMULACRA2 verify declined for %s output: decode of the "
          "candidate failed or dims/format mismatch -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name);
    }
    verify.declined = true;
    verify.verdict_missing = true;
    return verify;
  }

  std::optional<float> initial_verdict = score_fn(comparable);
  if (!initial_verdict.has_value()) {
    // A sub-8x8 REFERENCE is the one structural cause: the metric cannot
    // run on it, no verdict exists to bind, and tiny icons keep shipping
    // without one (the carve-out above). Every other absent verdict is a
    // metric-internal failure on a measurable image: the score channel's
    // negative values are legitimate catastrophic verdicts, so the failure
    // must arrive here -- on its own channel -- and decline fail-closed on
    // EVERY arm, the ship-below-floor JPEG arm included, rather than
    // shipping a sentinel score that reads as a measurement (#1382).
    if (reference_too_small) {
      return verify;  // Ships; |score| keeps the no-verdict sentinel.
    }
    if (handler != nullptr) {
      handler->Warning(
          "SSIMULACRA2 verify declined for %s output: the metric failed to "
          "produce a verdict -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name);
    }
    verify.declined = true;
    verify.verdict_missing = true;
    return verify;
  }
  float score = *initial_verdict;
  verify.score = score;

  // Attempt bookkeeping for band-closest selection. The caller's initial
  // encode is attempt 0 and stays in |encoded| until selection ships a
  // retry instead.
  const float initial_score = score;
  const int initial_quality = quality;
  struct Attempt {
    float score;
    size_t size;
    int quality;
    TranscodeResult result;
  };
  std::vector<Attempt> retries;

  // The quality search bisects the window it is allowed to probe: a
  // below-floor score proves the band lies ABOVE the probed quality and an
  // above-ceiling score proves it lies BELOW, so each verdict excludes the
  // probed quality along with everything on the wrong side of it and the
  // next probe splits what is left.  The window shrinks strictly, so no
  // quality is probed twice and the loop ends on its own.
  //
  // The window is deliberately ASYMMETRIC.  Upward it is the whole range:
  // an image whose band opens near the top used to be refused for being
  // out of the SEARCH's reach rather than out of the encoder's, since a
  // fixed step could only travel |quality_step| x (max_attempts - 1)
  // points from where it started.  Downward it keeps exactly that reach,
  // because the two directions are not equally costly to get wrong -- see
  // |quality_verify| above: under-quality is user-visible, over-quality
  // only wastes bytes, which is why the band itself is asymmetric.  A
  // metric that reads generously on a small or flat source must not be
  // able to talk the encoder down an order of magnitude on the strength of
  // one reading, so the cheap direction stays the timid one.
  //
  // One consequence worth knowing: the window bounds are monotone, so a
  // verdict is irreversible.  A fixed walk could turn around after a
  // misleading reading; this cannot.  Encoder curves are gentle enough
  // over an 11-point score band that this trades far more in-band encodes
  // found than lost, but a genuinely non-monotonic curve is the case where
  // it costs.
  int search_lo =
      std::max(min_quality, quality - quality_step * (max_attempts - 1));
  int search_hi = max_quality;

  if (score >= 0.0f && max_attempts > 1) {
    for (int attempt = 1; attempt < max_attempts; ++attempt) {
      if (score >= lo && score <= hi) break;

      // The window the verdict is about to close, kept for the log: once
      // the clamp below crosses the bounds they no longer name a range.
      const int window_lo = search_lo;
      const int window_hi = search_hi;
      if (score < lo) {
        search_lo = std::max(search_lo, quality + 1);
      } else {
        search_hi = std::min(search_hi, quality - 1);
      }
      if (search_lo > search_hi) {
        // Nothing untried is left on the side the score points to.
        // Re-encoding could only reproduce an output already measured, so
        // accept what we have instead of spending another encode on it.
        if (handler != nullptr) {
          const bool at_range_end =
              (score < lo) ? quality >= max_quality : quality <= min_quality;
          if (at_range_end) {
            handler->Info(
                "SSIMULACRA2 %.1f outside [%.1f, %.1f] but %s quality q=%d is "
                "at the %s of the allowed range -- accepting this encode",
                score, lo, hi, format_name[0] == '\0' ? "JPEG" : format_name,
                quality, score < lo ? "ceiling" : "floor");
          } else {
            handler->Info(
                "SSIMULACRA2 %.1f outside [%.1f, %.1f] but no untried %s "
                "quality is left in q=[%d, %d] -- accepting this encode",
                score, lo, hi, format_name[0] == '\0' ? "JPEG" : format_name,
                window_lo, window_hi);
          }
        }
        break;
      }
      quality = search_lo + (search_hi - search_lo) / 2;

      if (handler) {
        handler->Info(
            "SSIMULACRA2 %.1f outside [%.1f, %.1f], "
            "re-encoding %s at q=%d (attempt %d)",
            score, lo, hi, format_name, quality, attempt + 1);
      }

      auto retry = encode_fn(reference);
      if (!retry.success) break;

      auto retry_decoded = decode_fn(retry.output_data);
      ComparablePixels retry_comparable =
          MakeComparable(reference, retry_decoded);
      if (!retry_comparable.ok) break;  // Keep the verified prior attempt.

      std::optional<float> retry_verdict = score_fn(retry_comparable);
      // A retry the metric cannot measure keeps the verified prior
      // attempt; a negative retry verdict is likewise not recorded (it
      // displaces nothing).
      if (!retry_verdict.has_value() || *retry_verdict < 0.0f) break;

      score = *retry_verdict;
      verify.score = score;
      retries.push_back(
          {score, retry.output_data.size(), quality, std::move(retry)});
    }
  }

  // Band-closest selection over {initial encode} + retries, via the
  // directly-tested free function (see SelectVerifyAttempt above for the
  // full contract, including the decline-armed asymmetry rule).
  std::vector<VerifyAttempt> attempts;
  attempts.reserve(retries.size() + 1);
  attempts.push_back({initial_score, encoded.output_data.size()});
  for (const Attempt& r : retries) {
    attempts.push_back({r.score, r.size});
  }
  const size_t best = SelectVerifyAttempt(
      attempts, lo, hi,
      decline_policy == VerifyDeclinePolicy::kDeclineBelowFloor);
  if (best > 0) {
    quality = retries[best - 1].quality;
    encoded = std::move(retries[best - 1].result);
    verify.reencoded = true;
    verify.score = retries[best - 1].score;
  } else {
    quality = initial_quality;
    verify.reencoded = false;
    verify.score = initial_score;
  }
  score = verify.score;

  if (decline_policy == VerifyDeclinePolicy::kDeclineBelowFloor &&
      !reference_too_small && verify.score < lo) {
    verify.declined = true;
    if (handler != nullptr) {
      handler->Info(
          "SSIMULACRA2 decline for %s: score %.1f below floor %.1f "
          "(band [%.1f, %.1f]) -- variant not produced",
          format_name[0] == '\0' ? "JPEG" : format_name, verify.score, lo, lo,
          hi);
    }
  }

  // Print any finite verdict -- a declined negative score must be visible
  // in logs. Only the structurally verdict-less sub-8x8 reference stays
  // silent (nothing was measured).
  if (handler && (score >= 0.0f || !reference_too_small)) {
    // JPEG uses no format prefix in the log message for backward compat.
    if (format_name[0] == '\0') {
      handler->Info("SSIMULACRA2 score: %.1f (q=%d%s)", score, quality,
                    verify.reencoded ? ", re-encoded" : "");
    } else {
      handler->Info("SSIMULACRA2 score: %.1f (%s q=%d%s)", score, format_name,
                    quality, verify.reencoded ? ", re-encoded" : "");
    }
  }

  return verify;
}

// Outcome of RescueUnderBudget.
struct BudgetRescueResult {
  bool rescued = false;  // |encoded| was replaced by an in-budget encode
  int encodes = 0;       // extra encodes spent, rescued or not
  float score = -1.0f;   // score of the accepted encode
};

// Recovers a converted (WebP/AVIF) arm that the cross-format size gate would
// drop for not being smaller than the optimized original-format image
// (#406).  |encoded| passed verification at |quality| but is not under
// |budget_bytes|.  This probes at most |max_attempts| LOWER qualities,
// bisecting [quality - 4 * quality_step, quality - 1]: an encode over budget
// moves the window down, an encode under budget that scores below
// |floor| moves it up.  The first encode that is strictly under budget AND
// scores at least |floor| replaces |encoded| and |quality|; otherwise both
// are left untouched, so the caller drops the arm exactly as it did before.
// |floor| is the verify band's lower edge, so the quality contract does not
// change: nothing ships below the score a first-pass encode must reach.
// The first acceptable encode is kept rather than the smallest, which spends
// the fewest encodes and keeps the most quality.  The window opens about ten
// points below the verified quality, so the first probe is a sizeable step.
//
// The callables are those VerifySsimulacra2Quality takes: |encode_fn| reads
// the quality |quality| refers to.
template <typename EncodeFn, typename DecodeFn, typename ScoreFn>
BudgetRescueResult RescueUnderBudget(
    const DecodedImage& reference, TranscodeResult& encoded, EncodeFn encode_fn,
    DecodeFn decode_fn, ScoreFn score_fn, int& quality, size_t budget_bytes,
    float floor, int max_attempts, int quality_step, const char* format_name,
    MessageHandler* handler) {
  BudgetRescueResult out;
  const int verified_quality = quality;
  const int step = std::max(1, quality_step);
  int lo = std::max(1, verified_quality - 4 * step);
  int hi = verified_quality - 1;
  while (out.encodes < max_attempts && lo <= hi) {
    quality = lo + (hi - lo) / 2;
    const int probe = quality;
    ++out.encodes;
    TranscodeResult candidate = encode_fn(reference);
    if (!candidate.success) break;
    DecodedImage decoded_candidate = decode_fn(candidate.output_data);
    ComparablePixels comparable = MakeComparable(reference, decoded_candidate);
    if (!comparable.ok) break;
    std::optional<float> verdict = score_fn(comparable);
    if (!verdict.has_value()) break;
    const bool under_budget = candidate.output_data.size() < budget_bytes;
    if (handler != nullptr) {
      handler->Info(
          "Budget recovery for %s: q=%d gives %zu bytes (budget < %zu), "
          "SSIMULACRA2 %.1f (floor %.1f)",
          format_name, probe, candidate.output_data.size(), budget_bytes,
          *verdict, floor);
    }
    if (under_budget && *verdict >= floor) {
      encoded = std::move(candidate);
      out.rescued = true;
      out.score = *verdict;
      return out;
    }
    if (*verdict < floor && !under_budget) {
      break;  // Higher is too big, lower scores lower: nothing can fit.
    }
    if (*verdict < floor) {
      lo = probe + 1;  // Too low: the answer, if any, is above this.
    } else {
      hi = probe - 1;  // Good enough but still too big: go lower.
    }
  }
  quality = verified_quality;
  return out;
}

// Transcodes images between formats based on client capabilities.
//
// Usage:
//   ImageTranscoderConfig config;
//   ImageTranscoder transcoder(config, &handler);  // fixed config
//   // Or with a config accessor for hot-reload:
//   ImageTranscoder transcoder([&]{ return get_config(); }, &handler);
//
//   CapabilityMask mask;
//   mask.set_image_format(CapabilityMask::ImageFormat::kWebP);
//
//   auto result = transcoder.Transcode(jpeg_data, mask);
//   if (result.success) {
//     // result.output_data is WebP-encoded
//   }
class ImageTranscoder {
 public:
  // Construct with a config accessor that is called at transcode time.
  // This enables hot-reload: the accessor can return a fresh config
  // snapshot on every call (e.g., from an atomic shared_ptr).
  using ConfigAccessor = std::function<ImageTranscoderConfig()>;
  ImageTranscoder(ConfigAccessor config_accessor, MessageHandler* handler);

  // Convenience constructor for a fixed (non-reloadable) config.
  ImageTranscoder(const ImageTranscoderConfig& config, MessageHandler* handler);

  // Transcode image data based on the target mask's image format.
  //
  // If the target format matches the source format, the image is
  // optimized in-place (e.g., lossless JPEG optimization).
  //
  // If the target format is different, the image is converted
  // (e.g., JPEG -> WebP).
  //
  // If the target format is not supported (e.g., AVIF), falls back
  // to optimizing the original format.
  TranscodeResult Transcode(std::string_view input_data,
                            const CapabilityMask& target_mask);

  // Decode image to raw pixels.  Returns empty DecodedImage on failure.
  // Enforces max_decoded_pixels to prevent OOM.
  DecodedImage DecodeToPixels(std::string_view input_data);

  // Generate multiple format variants from a single decode pass.
  // Only produces formats listed in |formats|.  GIF input is handled
  // specially (animated GIF -> WebP uses its own pipeline).
  // |source_jpeg_quality| is the source JPEG quality from DQT tables
  // (-1 if unavailable/non-JPEG), used for quality capping.
  MultiTranscodeResult TranscodeMulti(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      int source_jpeg_quality = -1);

  // Config-accepting overload: uses |config_override| instead of config().
  // Enables callers (e.g., TranscodeMultiResized fast-path) to pass
  // pre-adjusted quality settings (learned quality, content-class factors).
  // When |skip_quality_cap| is true, ApplyJpegQualityCap is not called
  // (caller already applied it).  |caller_decoded| is the caller's own
  // full-size decode of |input_data|, when it has one: this call reuses
  // that frame instead of decoding the source a second time (#1407).
  MultiTranscodeResult TranscodeMulti(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      int source_jpeg_quality, const ImageTranscoderConfig& config_override,
      bool skip_quality_cap = false,
      const DecodedImage* caller_decoded = nullptr);

  // Resize a decoded pixel buffer to the target width for a viewport class.
  // Returns an empty DecodedImage if no resize is needed (Desktop with
  // viewport_widths.desktop == 0, or image already smaller).
  // Preserves aspect ratio.  When density is k2xPlus, the target width
  // is doubled (e.g., mobile 480px * 2 = 960px for retina).
  DecodedImage ResizeForViewport(
      const DecodedImage& decoded, CapabilityMask::Viewport viewport,
      CapabilityMask::PixelDensity density = CapabilityMask::PixelDensity::k1x);

  // TranscodeMulti with viewport-based resizing.  Decodes once,
  // resizes if the viewport target width is set, then encodes to
  // all requested formats.  When save_data is kOn, uses lower
  // quality settings from config.  When density is k2xPlus,
  // doubles the viewport target width.
  //
  // |quality| carries the two JPEG quality inputs by name; see
  // JpegQualityInputs.  Callers that have the source bytes can fill
  // source_quality with DetectSourceJpegQuality().
  MultiTranscodeResult TranscodeMultiResized(
      std::string_view input_data,
      const std::vector<CapabilityMask::ImageFormat>& formats,
      CapabilityMask::Viewport viewport,
      CapabilityMask::PixelDensity density = CapabilityMask::PixelDensity::k1x,
      CapabilityMask::SaveData save_data = CapabilityMask::SaveData::kOff,
      JpegQualityInputs quality = {});

  // Estimate the source JPEG quality from the input's DQT tables.
  // Returns -1 for non-JPEG input or when the quality cannot be determined.
  // Callers hoist this out of per-variant loops: the estimate depends only
  // on the input bytes.
  static int DetectSourceJpegQuality(std::string_view input_data,
                                     MessageHandler* handler);

  // Replace the message handler (e.g., after wrapping with
  // TeeMessageHandler).
  void set_handler(MessageHandler* handler) { handler_ = handler; }

  // Check if GIF data contains multiple frames (animated).
  static bool IsAnimatedGif(std::string_view data);

  // Pixel decodes this transcoder has performed: one count per
  // DecodeToPixels entry on non-empty input, source decodes and verify
  // decode-backs alike.  The multi-format contract is "decode once,
  // encode many", so the count is the per-instance cost observable that
  // pins it: a call that decodes its source twice is measurably wrong
  // here (#1380).
  uint64_t decode_count() const {
    return decode_count_.load(std::memory_order_relaxed);
  }

  // Maximum decoded pixel buffer size (default 50MB).
  static constexpr size_t kMaxDecodedPixels = 50 * 1024 * 1024;

 private:
  // Decode AVIF to raw pixels via libavif. The scanline-reader framework
  // has no AVIF reader (AVIF is an output-only format in the encode
  // arms), so the SSIMULACRA2 verify floor decodes through this instead
  // (#1274 -- without it the floor's decode_fn returns empty and the
  // verify silently no-ops).
  DecodedImage DecodeAvifToPixels(std::string_view input_data);

  // Optimize JPEG in-place (lossless or lossy).
  // Uses quality settings from |cfg|.
  TranscodeResult OptimizeJpeg(std::string_view input_data,
                               const ImageTranscoderConfig& cfg);

  // Optimize PNG in-place.
  TranscodeResult OptimizePng(std::string_view input_data);

  // Optimize WebP in-place: decode, re-encode at |cfg|'s WebP settings, and
  // keep the result only if it is STRICTLY smaller than the input -- the same
  // re-encode-and-accept-if-smaller rule OptimizeJpeg and OptimizePng apply
  // (#1375). |decoded| may be a decode the caller already performed for this
  // same input, which a multi-format call has; pass nullptr to decode here.
  //
  // The re-encode consults the binding quality verdict (#1385): with
  // cfg.quality_verify on, a candidate the verifier condemns (below the
  // band floor -- negative scores are legitimate catastrophic verdicts)
  // or cannot measure is DECLINED, success=false with the decline
  // reason, and the caller falls back exactly as it does for a
  // no-savings re-encode: the origin's own bytes keep serving. This
  // lane used to ship on the size test alone. The capped same-format
  // JPEG arm's accept-at-the-cap rule is deliberately NOT inherited:
  // that rule exists because a source-quality cap can make the band
  // unreachable by design (#1284), and no such cap binds this search.
  // |verify_out|, when non-null, receives the verdict evidence so multi
  // callers can record it on the original-format arm's fields.
  TranscodeResult OptimizeWebp(std::string_view input_data,
                               const ImageTranscoderConfig& cfg,
                               const DecodedImage* decoded = nullptr,
                               SameFormatWebpVerify* verify_out = nullptr);

  // Level A PNG carry-through (opt-in via cfg.c2pa_carry). Recompresses
  // the PNG via OptimizePng, then splices the ORIGINAL caBX/iTXt manifest chunks
  // back in immediately before the trailing IEND. Fail-safe to serving the
  // original bytes verbatim (skip-not-strip) on ANY anomaly -- recompress failed
  // or saved nothing, output is not a well-formed PNG, no carrier chunk found, or
  // no terminating IEND -- so a manifest is never silently dropped. Never parses
  // or re-authors the manifest (D3). Callers must guarantee `input_data` is a PNG.
  TranscodeResult OptimizePngWithC2paCarry(std::string_view input_data);

  // Convert image to WebP.
  TranscodeResult ConvertToWebp(std::string_view input_data);

  // Convert image to AVIF.
  TranscodeResult ConvertToAvif(std::string_view input_data);

  // Convert GIF (static or animated) to WebP.
  TranscodeResult ConvertGifToWebp(std::string_view input_data);

  // Cross-format size gate recovery (#406).  |arm| (WebP, or AVIF when
  // |avif|) is about to be refused for not being smaller than |baseline|,
  // the optimized original-format image of its slot, whose pixels are
  // |reference|'s counterpart.  Records the refusal and, when it succeeds,
  // the rescue on |out|, scores |baseline| once per call for the log, and
  // may replace |arm| with a smaller encode that still meets the quality
  // floor.  The caller refuses |arm| afterwards if it is still not smaller.
  void RecoverOversizedConverted(TranscodeResult& arm, bool avif,
                                 const DecodedImage& reference,
                                 const TranscodeResult& baseline,
                                 const ImageTranscoderConfig& cfg, int& quality,
                                 MultiTranscodeResult& out);

  // Encode from decoded pixel buffer using quality settings from |cfg|.
  TranscodeResult EncodeJpegFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);
  TranscodeResult EncodeWebpFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);
  TranscodeResult EncodeAvifFromPixels(const DecodedImage& decoded,
                                       const ImageTranscoderConfig& cfg);

  // Returns a snapshot of the current config.  Called at the start
  // of each transcode operation so hot-reloaded values take effect.
  ImageTranscoderConfig config() const { return config_accessor_(); }

  ConfigAccessor config_accessor_;
  MessageHandler* handler_;
  std::atomic<uint64_t> decode_count_{0};
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_IMAGE_TRANSCODER_H_
