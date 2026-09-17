/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef PAGESPEED_SYSTEM_DAEMON_SERVE_ARM_H_
#define PAGESPEED_SYSTEM_DAEMON_SERVE_ARM_H_

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

// ---------------------------------------------------------------------------
// The serve arm: what this module may hand back for one request from the
// optimizer daemon's shared cache, and the response state that goes with it.
//
// IT IS A DIRECT ALTERNATE READ, NOT AN ADAPTER UNDER THE CLASSIC CACHE.  The
// classic in-place path answers a request by looking in this module's own
// HTTPCache and then repairing the headers of whatever it found
// (FetchTryFallback / FixFetchFallbackHeaders).  Neither runs here, and that
// is a design property rather than an omission: those two exist to reconcile
// a stored REWRITE with the resource it was derived from, and on this
// substrate the stored thing is the optimizer's variant, whose validators and
// freshness this module composes from the ORIGIN STATE the peer stored beside
// it.  Routing this through the classic pair would mint a second, disagreeing
// answer to every question the peer already answers.
//
// THE THREE OUTCOMES, and they are deliberately not two.  A variant serve and
// an original serve are both serves, but they are different SERVE CLASSES and
// carry different `Vary`; and a fall-through is not a failure -- it is the
// substrate declining, and the request then goes out by the ordinary
// (non-in-place) path exactly as if this module were not installed.
//
// WHAT IS NOT HERE, and why:
//
//   * NO OPTION-CONTEXT / SIGNATURE AXIS.  Selection is (key, format
//     capability).  A serve-side signature parameter would file this
//     request's read under a configuration namespace, and the ruled design
//     files everything under the default one -- so a signature here would
//     look for entries nothing writes.  The retained 4-argument
//     ComposeKeyPreNormalized overload stays unused for the same reason.
//   * NO `stale-while-revalidate` SYNTHESIS.  A synthesized SWR emitted from
//     an origin authorises downstream staleness the origin never permitted.
//     The peer's Cache-Control builder is asked in its SAFE mode, which
//     cannot synthesize one.
//   * NO SECOND RFC 9111.  Freshness and the emitted `Cache-Control` are the
//     peer's own, over the origin state it stored; this module supplies
//     scalars and a clock.
// ---------------------------------------------------------------------------

enum class DaemonServeVerdict {
  // A worker-produced variant, selected on (key, format capability).
  kServeOptimized,

  // The durable ORIGINAL, read by exact id.  Origin bytes, from cache.
  kServeOriginal,

  // Nothing servable.  The request goes out by the plain, non-in-place path.
  kFallThrough,
};

// Why the arm decided what it decided.  A stable identifier rather than a
// sentence, because it is asserted on.  Never null.
namespace daemon_serve_reason {
inline constexpr char kOptimizedVariant[] = "optimized-variant";
inline constexpr char kDurableOriginal[] = "durable-original";
// No entry of any class for this key.  A cold URL, or one this module never
// recorded.
inline constexpr char kNoEntry[] = "no-entry";
// The URL is not in the canonical subset this module can name an entry with,
// so a read would ask for a key nothing writes.  The record arm applies the
// same restriction to what it stores; see UrlIsKeySafe.
inline constexpr char kUrlNotKeySafe[] = "url-not-key-safe";
// The URL is one this module's own rewriting produced.
inline constexpr char kRewrittenUrlShape[] = "rewritten-url-shape";
// Entries exist, but none whose image format this request advertises, and no
// durable original either.
inline constexpr char kNoCompatibleVariant[] = "no-compatible-variant";
// The entry the peer selected is stored pre-compressed in an encoding this
// arm will not label.  See ServeEncodingIsServable.
inline constexpr char kEncodingNotServable[] = "encoding-not-servable";
// The entry exists and is past its freshness, or the client demanded
// revalidation.  Falling through re-fetches from the origin, which is the
// revalidation.
inline constexpr char kNeedsRevalidation[] = "needs-revalidation";
// The read failed for a reason that is not absence.
inline constexpr char kReadFailed[] = "read-failed";
// The durable original for this key is marked as having carried an origin
// header no serve from the entry can reproduce, so serving from the substrate
// at all would drop it.  D6.4 rule 6's fall-through, and it has its OWN reason
// for a reason: `no-entry` is what a genuinely cold key reports, and the two
// are opposite conditions -- one is a URL nobody has recorded, the other is a
// URL that was recorded, was optimized, and is being declined on purpose.
// Collapsed into one reason the counter partition would say a substrate that
// is working exactly as ruled looks like an empty cache.
inline constexpr char kOriginHeadersNotReproducible[] =
    "origin-headers-not-reproducible";
// The arm could not run: no bound library, no volume handle.  DISTINCT from
// kNoEntry on purpose -- a substrate that cannot be read at all and a key
// that is genuinely cold look identical in a counter and are opposite in what
// an operator should do about them.
inline constexpr char kSubstrateUnavailable[] = "substrate-unavailable";
}  // namespace daemon_serve_reason

// Everything the arm reads about the request.  Every StringPiece is borrowed
// for the duration of the call.
//
// THE FOUR CAPABILITY FIELDS ARE REQUEST HEADERS AND NOTHING ELSE.  They are
// the exact four the peer's classifier takes, they are read off the client's
// request, and no configuration value reaches them -- see the invariant
// pinned in daemon_serve_arm_test.cc.  A capability mask that a
// RewriteOptions value could move would make the entry this arm selects a
// function of server configuration, which is the one thing the shared cache's
// key cannot express.
struct DaemonServeRequest {
  StringPiece url;  // Path form; see UrlIsKeySafe.
  StringPiece hostname;
  StringPiece scheme;

  StringPiece accept;
  StringPiece user_agent;
  StringPiece save_data;
  StringPiece accept_encoding;

  // The client's conditionals, verbatim.
  //
  // `if_none_match` is matched ONLY against the validator this arm derives;
  // see ServeDerivedValidatorMatches.
  //
  // `if_modified_since` is matched ONLY on the ORIGIN plane, and therefore
  // only against a response this arm is emitting the origin's own
  // `Last-Modified` on -- i.e. a durable-original serve.  An optimized
  // variant carries no `Last-Modified` (it is not the representation the
  // origin's validator describes), so a client that sends one for a variant
  // is holding a validator for different bytes and gets the full response.
  // Advertising a validator and then ignoring a conditional over it is the
  // asymmetry this field closes.
  StringPiece if_none_match;
  StringPiece if_modified_since;

  // The client demanded revalidation (a forced reload).  Yields a
  // fall-through without treating the entry as expired.
  bool force_revalidate = false;
};

// The arm's answer, plus the response state a serving seam has to emit.
//
// `body` borrows from the DaemonServeReader that produced this decision and
// is valid only while that object is alive and has not been asked for another
// selection.
struct DaemonServeDecision {
  DaemonServeVerdict verdict = DaemonServeVerdict::kFallThrough;
  const char* reason = daemon_serve_reason::kNoEntry;

  // Exactly one serve class per response, in the peer's numbering.  Written
  // through the peer's serve-stats recorder; see DaemonServeStats.
  int serve_class = kPsServeClassOriginalCold;

  // What the request classified as, and what was selected.  Reported so a
  // harness can grade the selection without asking this module what it
  // believes.
  //
  // `client_mask` is the peer classifier's own answer over this request's
  // headers, unmodified.  `selection_mask` is the mask the read was actually
  // taken at -- the same thing with the transfer-encoding field normalized to
  // identity; see ServeMaskWithIdentityEncoding.  Both are reported because
  // collapsing them would hide the one place this arm deviates from the
  // client's stated capabilities, and that deviation is load-bearing.
  uint32_t client_mask = 0;
  uint32_t selection_mask = 0;
  bool selected = false;
  uint8_t alternate_id = 0;
  uint32_t stored_mask = 0;
  uint8_t stored_flags = 0;
  bool worker_processed = false;

  // The ORIGIN length recorded on the served entry, or 0 when the entry
  // records none.  Half of the serve-hit accounting gate, with
  // `worker_processed`: the peer's serve-stats recorder counts a hit only
  // against a real origin length (the saving is measured against it), and 0
  // is "not recorded", never "the origin sent nothing".
  uint32_t origin_content_length = 0;

  // The peer's content-type class of the entry being served, in the kPsContent*
  // numbering.  Carried because a fallback re-notify names it; distinct from
  // `content_type` below, which is the media-type FIELD this response emits.
  int ps_content_type = 0;

  // FALLBACK HIT: an optimized variant WAS served, but it is not the variant
  // this client's capability mask names -- the stored mask differs from the
  // mask the selection was taken at on at least one of the viewport, density,
  // Save-Data or image-format axes.  Only ever set on a kServeOptimized
  // verdict; on every other outcome the field stays false and means nothing.
  //
  // THE COMPARISON IS AGAINST `selection_mask`, NOT `client_mask`, and that is
  // deliberate rather than a shorthand.  This arm normalizes the
  // transfer-encoding field to identity before selecting (see
  // ServeMaskWithIdentityEncoding), so the classifier's raw answer differs from
  // every stored mask this arm will serve on the encoding axis for EVERY real
  // browser, permanently and by design.  Compared against `client_mask`, the
  // predicate would fire on every optimized serve and the re-notify it feeds
  // would be permanent per-request traffic over an axis the worker does not
  // need to be asked about -- it derives the compressed siblings of any
  // variant itself.  Compared against the mask the read was taken at, the
  // predicate names exactly the axes the worker can converge the family on,
  // and it self-terminates: once the client's variant exists, the selection
  // returns it and the masks compare equal.
  //
  // Consumed by the serving seam, which answers it with ONE worker re-notify;
  // see DaemonServeFallbackRenotify.
  bool fallback_hit = false;

  // AGE-EXPIRED VARIANT: a SELECTED VARIANT was declined on freshness --
  // verdict revalidate or stale-retired -- and the peer's freshness evaluator
  // says the entry is stale because its AGE exceeded the effective lifetime
  // (`expired_by_age`), not because the client demanded revalidation and not
  // because the origin said `no-cache`.  Only ever set on a kFallThrough
  // verdict taken on the negotiated path; on every other outcome the field
  // stays false and means nothing.
  //
  // WHY THIS ONE DECLINE IS SINGLED OUT, of all the fall-throughs above.  A
  // fall-through re-fetches from the origin and re-records it, and the record
  // arm then notifies the worker -- but the worker's processed-set dedup
  // answers that notification "already processed, skipping", because the URL
  // was optimized once and nothing in the expiry lifecycle clears the entry.
  // The result is permanent: the stored variant ages past its lifetime once,
  // is declined on freshness on EVERY later request, and the plain
  // notification that would rebuild it is swallowed every time.  The
  // worker's heal for exactly this transition exists -- the origin-refreshed
  // sentinel (kOriginRefreshedSentinel), which purges the stale variant set,
  // clears dedup and rebuilds -- and this flag is what tells the seam the
  // decline it just took is the one that sentinel is for.
  //
  // THE DISTINCTION IS THE PEER'S, AND IT IS LOAD-BEARING.  The peer
  // guarantees `expired_by_age` false for a client force-revalidate and for
  // origin `no-cache` content, both of which yield the same revalidate
  // verdict: the first is the client asking, the second is permanent by
  // definition, and neither means the stored variants are outdated.  Ungated,
  // any anonymous client could purge a URL's entire optimized variant set
  // with a plain reload (Cache-Control: max-age=0).  Both guarantees are
  // pinned in daemon_serve_arm_test.cc.
  //
  // Consumed by the serving seam, which answers it with the origin-refreshed
  // sentinel; see DaemonServeOriginRefreshedNotify.
  bool stale_variant_expired_by_age = false;

  // THE TWO DISJOINT REASONS `Vary: Accept` CAN BE OWED, kept apart rather
  // than folded into the boolean below.  They are different facts about the
  // response and only one of them is about anything this module did, so a
  // single flag would make an origin's own negotiation indistinguishable
  // from ours -- and the second is the clause whose omission is the
  // composed-negotiation defect.
  //
  //   negotiated: THIS ARM chose the bytes from the client's `Accept`.
  //   origin_declared: the ORIGIN negotiates on `Accept` itself, recorded on
  //     the entry at store time because on a hit its `Vary` is gone.
  bool vary_accept_negotiated = false;
  bool vary_accept_origin_declared = false;
  bool emit_vary_accept = false;

  // The derived, WEAK validator for the bytes being served, and whether one
  // of the client's conditionals matched.  Empty when nothing is served.
  //
  // `not_modified` is set by `If-None-Match` against the derived tag on any
  // serve, and by `If-Modified-Since` against the origin's `Last-Modified` on
  // an ORIGINAL serve -- each on the plane whose validator this response
  // actually carries.
  GoogleString etag;
  bool not_modified = false;

  // The emitted `Cache-Control`, built by the peer from the stored origin
  // state.
  GoogleString cache_control;

  // The emitted media type, and it DESCRIBES THE BYTES rather than reporting
  // the origin's field.  For an image whose served bytes are a format this
  // module can name, it is that format; everywhere else -- a non-image
  // entry, a format the sniff does not name -- it is the origin's
  // `Content-Type` carried verbatim, which is what that field is right for.
  // See ServeMediaTypeForServedBytes for why the served bytes and the mask's
  // format field are not the same question.
  GoogleString content_type;

  // How old the entry is NOW, in seconds, as the peer's freshness evaluator
  // computed it.
  //
  // EMITTED AS `Age` ON EVERY HIT, and not optional.  What goes out is a
  // `Cache-Control` carrying the entry's full remaining lifetime; a cache
  // downstream that is told `max-age=600` and not told the response is
  // already 500 seconds old will keep it for 1100 (RFC 9111 s5.1: a cache
  // that has the information MUST send `Age`).  The peer's own front end
  // emits it on every hit path for the same reason.  The value was already
  // being computed here and discarded.
  uint32_t age_seconds = 0;

  // The origin's `Last-Modified`, in seconds, or 0 when none was recorded.
  // ON THE ORIGIN PLANE, and therefore NOT emitted on an optimized serve:
  // reported here so a seam can put it on an ORIGINAL serve, where it is the
  // origin's own bytes and the validator is truthful.
  uint32_t origin_last_modified = 0;

  StringPiece body;
};

// ---------------------------------------------------------------------------
// The pure half.  No I/O, no peer handle: every rule below is testable
// without a cache, a socket or a server.
// ---------------------------------------------------------------------------

// The image-format and transfer-encoding fields of a capability mask.
uint32_t ServeImageFormat(uint32_t mask);
uint32_t ServeTransferEncoding(uint32_t mask);

// The alternate id a capability mask names: the mask's low byte.
uint8_t ServeAlternateIdForMask(uint32_t mask);

// True when `id` is in the peer's SENTINEL space: every sentinel id carries
// the reserved viewport value in bits 2-3, and no capability vector ever
// does.  The durable-originals class (kPsSentinelOriginal, 0x0C) is one of
// these.
//
// WHY THIS MODULE NEEDS THE PREDICATE FOR ITSELF rather than inheriting the
// peer's: a selection read cannot hand back a sentinel entry, but that guard
// lives in the peer's SELECTOR -- it skips the sentinel space outright and
// scores a stored sentinel id at zero -- not in anything that validates the
// mask on the way in.  The by-id retry bypasses all of that: it names ids
// DIRECTLY, derived from a mask this module normalized itself
// (ServeMaskWithIdentityEncoding touches only the encoding field).  A
// selection mask that ever carried viewport==3 would make the retry's
// format-normalized candidate land exactly on 0x0C -- the durable original's
// id -- and the retry would hand back ORIGIN BYTES as an optimized variant:
// kServeOptimized, the negotiated `Vary: Accept`, and the wrong serve class,
// all off one reserved value no classifier produces today.  Unreachable now
// by the peer's classifier invariant is exactly the class of reasoning a
// guard should not rest on (see the #747 review round), so the retry refuses
// sentinel ids by name instead of by reachability.
bool ServeAlternateIdIsSentinel(uint8_t alternate_id);

// `mask` with its image-format field forced to kOriginal.
uint32_t ServeMaskWithOriginalFormat(uint32_t mask);

// `mask` with its transfer-encoding field forced to identity.
//
// THIS IS APPLIED TO THE MASK THE SELECTION IS TAKEN AT, and it is a
// correctness fix rather than a tuning knob.  A real browser sends
// `Accept-Encoding: gzip, deflate, br`, so the peer's classifier returns a
// mask whose encoding field is brotli -- and the peer's scorer then prefers
// the stored BROTLI variant (an exact encoding match, +60) over its identity
// sibling (+5).  This arm cannot emit pre-compressed bytes (see
// ServeEncodingIsServable), so without normalization every request from every
// real browser is handed a variant this arm must refuse, and the URL falls
// back to the durable ORIGINAL: unoptimized bytes, served to the entire
// population this substrate exists for, with the fall-through counter never
// moving because a serve did happen.
//
// Normalizing at SELECTION time instead of refusing after the fact makes the
// peer do the right thing with no second scorer here: with the client's
// encoding field at identity, a stored non-identity variant is HARD
// disqualified by the peer's own rule (mismatched non-identity encoding
// scores 0), so the identity sibling is what comes back.  That sibling exists
// in the family for exactly this reason.
//
// The mask REPORTED for the request is still the classifier's own answer;
// only the mask the read is taken at is normalized, and both travel in the
// decision so a harness can see the difference.
uint32_t ServeMaskWithIdentityEncoding(uint32_t mask);

// True when a stored variant's image format is one this request advertised.
//
// THIS WAS THE DISQUALIFY THE PEER'S SCORER DID NOT HAVE, AND AT THE CURRENT
// PIN IT IS DEFENSE IN DEPTH.  The history is what makes the current state
// legible.  The peer's ScoreAlternate used to award nothing for a format
// mismatch and yet not refuse one: a variant matching on viewport, density,
// Save-Data and encoding still scored 200, so for a client that advertised
// WebP and a URL with only an AVIF variant, `ps_cache_read_best` returned the
// AVIF one (We-Amp/pagespeed-optimizer#1331) and serving it emitted bytes in a
// format the client never said it could decode.  The peer's own fix
// (its #1334) hard-disqualifies a mismatched RASTER format in the scorer, so
// at the pinned daemon a family of WebP and AVIF variants no longer hands
// this module an AVIF one for a client that advertised neither -- with no
// compatible member it now selects nothing, and the arm falls through to the
// by-id read below.  kSvg is the case the fix does NOT cover, deliberately:
// the peer awards a stored SVG variant its universal bonus whatever the
// client's format is, so an SVG variant still arrives here and is still
// refused by this predicate.  That regime is what the retry below exists for.
//
// THE CHECK STAYS, and not out of caution.  Three reasons, none of which
// depend on the defect: the peer's scoring is not published contract and can
// move again in either direction, without this module noticing; the by-id
// retry a few lines down bypasses the scorer entirely, so it is the one path
// where an unservable entry can still arrive and the only thing that refuses
// it is this predicate; and a module that emits bytes a client cannot decode
// fails silently, which is the failure class worth paying a redundant check
// for.  What DID change is where its discriminating power is demonstrated:
// the serve-arm unit suite, whose stand-in reproduces the pre-fix scorer on
// purpose, rather than the parity rig, where the peer's fix now makes the
// branch unreachable.
//
// The rule is exact match, or the ORIGINAL format.  It is deliberately not
// "anything the client could probably handle": the mask records ONE format --
// the best the client advertised -- so "advertised WebP too" is not a fact
// this module has, and inferring it would be a second Accept parser
// disagreeing with the peer's in the cases nobody tests.  The original format
// is always servable because it is the format the origin sent for a URL the
// client asked for.  kSvg is never servable, because the mask cannot express
// a client that advertised it (the peer derives kSvg from nothing a client
// sends), and a serve loss is the correct cost of that.
//
// COMPLETENESS, and the arithmetic has TWO regimes rather than one.
//
// Among ordinary variants the refusal costs almost nothing, and at the pinned
// peer it is unreachable by them: a format-EXACT variant scores at least 1000
// while an incompatible one is hard-disqualified to 0, so the selection
// returns the compatible one whenever an exact one exists and returns nothing
// at all when it does not.  (Before the peer's fix an incompatible one capped
// at 200, which was still a losing score against an exact match -- the regime
// below is where that mattered.)
//
// kSvg IS THE EXCEPTION, and it is why this arm retries by id rather than
// simply falling through.  The peer's scorer awards a stored SVG variant
// +1200 UNIVERSALLY -- independent of the client's format -- plus the
// viewport, density and Save-Data bonuses unconditionally, so an SVG variant
// scores up to ~1400 and outranks every sibling in the family.  A refusal
// with no recovery would therefore make EVERY optimized variant of a
// vectorized URL unreachable: the WebP and AVIF siblings exist, are perfectly
// servable, and would never be returned.  So on a refusal the arm reads by
// exact id -- first the request's own (format-and-encoding) id, then its
// format-normalized one -- and both of those reads are reachable in exactly
// this case.
//
// SVG variants are worker-produced only and are not built by default; the
// optimizer builds them under its own vectorization mode. That makes this a
// configuration-reachable regime rather than a hypothetical one, which is the
// honest way to state it: rare, off by default, and catastrophic for the URLs
// it does apply to.
//
// What the refusal used to cost, after both retries: an ORIGINAL-format
// variant at some THIRD id -- a different viewport, say -- could score as
// little as 105 and lose to an incompatible variant at 200, so the arm
// refused what it was handed although a servable entry existed.  Recovering
// that from here would need a module-side re-selection over the whole
// alternate set, i.e. a second implementation of the peer's selector.  It is
// not needed: the peer's fix for the missing disqualify removes the case at
// the source, because an incompatible variant now scores 0 and cannot outrank
// the ORIGINAL-format one at 105.
bool ServeFormatIsAdvertised(uint32_t client_mask, uint32_t stored_mask);

// The media type that describes the BYTES being served, or an EMPTY piece
// when this module has nothing better to say than the origin's own field.
//
// WHY THE DECISION CANNOT JUST CARRY THE ORIGIN'S FIELD, which is what it
// used to do.  That field is right for an ORIGINAL serve and for a
// same-format re-encode, and it stops being right the moment the alternate
// handed back is in a different image format from the one the origin sent --
// which is the entire point of the format axis.  Three of three corpus image
// units served as an AVIF variant went out labelled `image/png`, and the same
// URL under `Accept: image/webp` returned RIFF/WEBP bytes, also labelled
// `image/png`.  The negotiation was correct on both halves -- the served
// format was one the request advertised, `Vary: Accept` was stamped -- and
// the response was still undecodable in the consumer, because a client
// decodes by the media type and a shared cache stores the mislabelling with
// it.
//
// WHY THE ANSWER IS THE BYTES AND NOT THE MASK'S FORMAT FIELD, which is the
// obvious source and is WRONG.  That field names the format slot a variant
// was derived FOR, not the format its content is in, and the two come apart
// in a case the corpus has seven of: the peer stores the ORIGINAL bytes in a
// converted-format slot when the conversion does not pay for itself (an
// animated GIF among them), so an entry at the AVIF id can hold GIF.  Reading
// the mask mislabels every one of those -- the same defect this rule exists
// to close, in the opposite direction, on responses that were correct before.
// It is not a peer bug: pick-smaller is the peer's own rule, and nothing in
// the ABI reports a stored entry's actual format.  The bytes do, they are
// already in hand, and they are what the client will decode.
//
// SNIFFED THE SAME WAY THE WIRE IS READ: the four raster formats this arm can
// serve, by their leading bytes.  Anything else -- an ICO, an SVG, a format
// this list does not name -- returns empty and the origin's field stands,
// which is the right answer for exactly those: the origin named a type this
// module cannot second-guess.
//
// EMPTY IS RETURNED, NOT THE ORIGIN'S FIELD, so the caller keeps the
// distinction between "this module can describe these bytes" and "the
// origin's answer stands".
StringPiece ServeMediaTypeForServedBytes(int ps_content_type, StringPiece body);

// True when a stored variant's transfer encoding is one this arm will emit.
//
// ONLY IDENTITY, and this is a correctness floor rather than caution.  A
// stored gzip or brotli variant is pre-compressed bytes; emitting them
// requires a `Content-Encoding` on the response, and the seams this arm
// serves through compress (or do not) downstream of it, by their own
// configuration.  An arm that handed compressed bytes to a seam that did not
// label them would produce a response no client can decode -- silently, and
// only for clients that advertised the encoding.
//
// WHAT THIS COSTS, corrected: it is a FLOOR and not the working path, and
// saying it "costs one optimized serve" was wrong in the way that matters.
// Every real browser advertises brotli, so on the naive path this refusal
// fired for the ENTIRE client population and each of those requests fell back
// to the unoptimized durable original -- a serve, so the fall-through counter
// never moved and nothing looked wrong.  The fix is upstream of the refusal:
// the selection mask is normalized to identity, so the peer disqualifies
// pre-compressed variants itself and returns the identity sibling.  Reaching
// this predicate at all now means a by-id read produced something the peer's
// own scoring would not have, and the cost is genuinely one serve.
bool ServeEncodingIsServable(uint32_t stored_mask);

// The largest body httpd's compressor passes through UNCOMPRESSED, and
// therefore the largest body it says nothing about `Accept-Encoding` for.
//
// MIRRORED, NOT BOUND, exactly like the peer mask layout in daemon_abi.h,
// and stated as one because a mirror is a weaker guarantee than a binding.
// mod_deflate skips a response it can already see is smaller than what
// compressing it would ADD -- the gzip header (10), the gzip trailer (8),
// and 50 bytes it budgets for the `Content-Encoding` and `Vary` fields and
// the ETag suffix it appends -- and it takes that shortcut BEFORE the line
// that states the axis.  So a body of 68 bytes or less goes out with no
// `Vary: Accept-Encoding` at all, and one of 69 bytes goes out with it.
//
// THE SHORTCUT ONLY REACHES A RESPONSE WHOSE WHOLE BODY IS ALREADY IN HAND,
// which is what makes it visible on this path and not on the classic one:
// this seam writes the entry's bytes in one go, the compressor sees the end
// of the response on its first call and takes the measurement; a streaming
// serve hands it the first buckets with no end in sight, the shortcut is
// skipped and the axis is stated whatever the length turns out to be.
inline constexpr size_t kServeCompressorPassThroughBytes = 68;

// The media type the serving layer's compressor will be given for this
// response, which is the one the `Vary` composition has to be asked about.
//
// NOT THE REQUEST'S INHERITED TYPE, and the difference is load-bearing on
// exactly the responses this arm exists for.  The 200 leg writes the response
// headers before it attaches the compressor, and writing a `Content-Type` is
// what sets the request's own -- so the attach tests the DECISION's string
// whenever the decision has one.  A substrate serving a format-converted
// variant is where the two disagree, and they can disagree about whether the
// compressor is attached at all: a stylesheet is handed to it, a converted
// image is not.  Asking about the inherited type would state the encoding
// axis for a response the compressor never sees, or omit it for one it does.
//
// `request_media_type` -- the serving layer's own type for this request, and
// null is an ordinary value -- is the answer only where the decision carries
// none, which is exactly the case no `Content-Type` is written for either, so
// the compressor really does see the inherited one.
const char* ServeCompressorMediaType(const DaemonServeDecision& decision,
                                     const char* request_media_type);

// The `Vary` field value for one serve from this substrate, composed in ONE
// PLACE FOR BOTH LEGS of the response.  Empty means emit no `Vary` at all.
//
// THE DEFECT THE SINGLE CALL SITE EXISTS TO PREVENT, because it is not
// hypothetical: the two legs used to compose this field independently and
// disagreed on the wire.  A 200 carried `Vary: Accept, Accept-Encoding` --
// `Accept` from this arm, `Accept-Encoding` from the compressor the serving
// seam hands the body to -- and the 304 that revalidated it, same URL, same
// entry, same validator, seconds later, carried `Vary: Accept`.  A cache
// updates its stored response's header fields from a 304 (RFC 9111 4.3.4),
// so a cache that had stored the 200 under a key including the encoding
// dimension was then told the response varies on `Accept` alone and could
// afterwards hand it to a client whose encoding capabilities differ.  The
// direction of that loss is the dangerous one: NARROWING a cache key after
// the fact, not widening it.
//
// `handed_to_compressor` is whether the serving seam will pass THIS
// response's body to the compressor -- a question about the media type
// being served, which is the seam's to answer and not this module's.
//
// WHY THE ENCODING TOKEN IS COMPOSED HERE ONLY ON THE 304 LEG.  On the 200
// leg the compressor is the emitter and stamps the token itself; composing
// a second copy here would put two emitters on one field, which is the
// condition a duplicated token is the symptom of and which no set-based
// reader of the response can see.  On the 304 leg there is no body, the
// compressor takes its pass-through shortcut on a zero-length response and
// states nothing, and this is the only emitter left.  So the token is
// composed against the SAME two facts the compressor decides on -- the
// media type, and the length of the representation being revalidated -- and
// the two legs agree by construction rather than by coincidence.
//
// WHAT THIS DOES NOT REPRODUCE, stated rather than left to be discovered.
// The compressor has SEVERAL early-outs that all sit in front of the line
// that states the axis, and every one of them suppresses the token on the
// 200 while this composition still states it on the 304.  Known members, and
// the list is explicitly PARTIAL -- it is the suppressors visible in the
// compressor's decision preamble at the time of writing, not a proof that
// there are no others:
//
//   * a DIFFERENT compressor on the response (a second output filter, a
//     front proxy) or this one turned off per request (`no-gzip`,
//     `gzip-only-text/html`);
//   * an existing content encoding.  THIS ONE IS REACHABLE, and the earlier
//     claim that it was not was simply wrong: the compressor reads the
//     request's own `Content-Encoding` -- which a suffix mapping
//     (`AddEncoding`) sets from the URL, with nothing to do with what this
//     arm stored -- and the error-header table beside it.
//     ServeEncodingIsServable governs the TRANSFER ENCODING OF THE STORED
//     VARIANT, which is a different question and does not close this one;
//   * compression already applied at the transport layer;
//   * a subrequest, a `204`, or a response carrying a `Content-Range`.
//
// THE DIRECTION OF EVERY ONE OF THEM IS THE SAFE ONE: the 200 says nothing
// about the encoding axis and the 304 says the response varies on it, so a
// cache updating its stored headers from the 304 WIDENS its key.  That costs
// a cache split; it cannot serve one client's representation to another,
// which is what the narrowing direction this composition exists to close
// does.
GoogleString ServeVaryFieldValue(const DaemonServeDecision& decision,
                                 bool handed_to_compressor);

// The weak validator for a cache-HIT response.
//
// THE SHAPE IS THE OPTIMIZER'S, byte for byte, and it has to be: the same
// URL may be served by this module on one host and by the optimizer's own
// front stage on another, and a client that revalidates across them must not
// be handed two different tags for the same representation.  What goes into
// it -- the full capability mask, the entry's flag byte, a 64-bit identity of
// the SOURCE content, and the served length -- is what makes the tag
// per-REPRESENTATION rather than per-URL: two encodings of one source differ
// in the mask, and an entry stored with the origin-negotiates marker must not
// validate against one stored without it, because the two are served under
// different cache keying.
//
// `origin_html_hash` is the entry's stamped 32-byte source binding, or null
// when it carries none; `origin_etag` and `origin_last_modified` are the
// origin's own validators as stored.  When none of the three is present the
// legacy, identity-free form is emitted -- the same one the optimizer emits
// for the same entry, so those clients keep their 304s.
//
// The result is always WEAK (`W/`), and never the origin's own tag: this
// module cannot promise byte-identity across re-optimizations of one source,
// and stamping an origin validator on optimized bytes would put two planes'
// validators on one representation.
GoogleString ServeDerivedETag(uint32_t full_mask, uint8_t flags,
                              const uint8_t* origin_html_hash,
                              StringPiece origin_etag,
                              uint32_t origin_last_modified,
                              size_t body_length);

// True when the client's `If-None-Match` names the validator THIS ARM
// derived.
//
// ON THE DERIVED PLANE ONLY.  The list is compared, entry by entry and under
// the weak comparison RFC 9110 requires for GET, against `derived` and
// against nothing else -- never against the origin's stored ETag.  A client
// holding an ORIGIN-plane tag is holding a validator for a representation
// this arm did not issue, and answering 304 to it would tell the client its
// origin-plane copy is current for bytes that are not the origin's.
//
// `*` IS DELIBERATELY NOT HONOURED.  It is not a validator on any plane; a
// 304 to it would validate whatever the client happens to hold, which is the
// same cross-plane answer by a shorter route.
bool ServeDerivedValidatorMatches(StringPiece if_none_match,
                                  StringPiece derived);

// True when the client's `If-Modified-Since` says it already has the
// representation stored at `origin_last_modified` (a Unix timestamp).
//
// ORIGIN PLANE ONLY, and the caller is responsible for asking it only where
// this arm is emitting `Last-Modified` -- see DaemonServeRequest.  An
// unparseable date is not a match: RFC 9110 requires an invalid
// `If-Modified-Since` to be ignored, and ignoring it means serving the
// response, never a 304.
bool ServeOriginDateIsUnmodified(StringPiece if_modified_since,
                                 uint32_t origin_last_modified);

// ---------------------------------------------------------------------------
// The I/O half.
// ---------------------------------------------------------------------------

// Reads the shared cache for one request and composes the response state.
//
// Holds the peer's read result for as long as it lives, because the served
// bytes borrow from it.  One instance serves one request.
class DaemonServeReader {
 public:
  // Neither `abi` nor `cache` is owned; both must outlive this object.
  // `cache` may be null, which yields a fall-through and is not an error.
  DaemonServeReader(const DaemonAbi* abi, void* cache);
  ~DaemonServeReader();

  DaemonServeReader(const DaemonServeReader&) = delete;
  DaemonServeReader& operator=(const DaemonServeReader&) = delete;

  // Selects, composes and returns the decision.  `now_seconds` is the
  // serving clock.  Call once.
  DaemonServeDecision Serve(const DaemonServeRequest& request,
                            int64 now_seconds);

 private:
  // Frees whatever read result is held.
  void Release();

  // Reads one entry, replacing whatever is held.  Returns the peer's status.
  int ReadBest(const DaemonServeRequest& request, uint32_t mask);
  int ReadById(const DaemonServeRequest& request, uint8_t alternate_id);

  // Reads the durable original by id and hands back its WHOLE flags byte
  // through `*flags`.  Returns the peer's read status and holds nothing
  // afterwards; `*flags` is zero on any status but kPsOk.
  //
  // THE BYTE IS THE ORIGIN'S STATEMENTS ABOUT THIS URL, and every one of them
  // is a property of the entry the writer stamped rather than of whatever the
  // arm ends up serving.  Two consumers, and both need this read rather than
  // the held result:
  //
  //   kPsFlagOriginHeadersNotReproducible -- the decline.  Read here and
  //     nowhere else, structurally: the optimizer's variants are built with a
  //     clear flags byte whatever the original's state, so a check taken on a
  //     SELECTION result reads clear for every marked URL and reports a
  //     permanent, silent all-clear.
  //   kPsFlagOriginVariesAccept -- clause (ii) of the `Vary` biconditional.
  //     The SAME hazard, and it bit once: sourced from the held result, an
  //     optimized serve of a non-image entry read the variant's clear byte and
  //     dropped the origin's `Vary: Accept` -- silently, on exactly the class
  //     the record arm now admits BECAUSE the serve arm reproduces that header.
  //
  // The by-id reads in RetryByExactId are VARIANT ids and are deliberately not
  // a flag source for either.
  //
  // IT IS TAKEN BEFORE THE SELECTION, and it has to be.  A marked URL must be
  // declined whether or not an optimized variant exists for it -- the variant
  // is precisely the thing that would have been served with the origin's other
  // headers missing -- so the question cannot wait until the arm knows what it
  // would otherwise have served.  The selection replaces whatever result is
  // held, so the answer cannot be carried forward in the held result either.
  //
  // The status is returned so the caller can reuse it: a key with no durable
  // original is not asked twice.  What remains is one extra read on the path
  // that ends up SERVING the original, which is the honest cost of asking a
  // question about one entry before reading another -- and it is now paying
  // for two answers rather than one.
  int ProbeOriginalFlags(const DaemonServeRequest& request, uint8_t* flags);

  // After a refusal, asks for a servable variant BY NAME rather than by
  // score.  Returns kPsOk with a usable entry held, or kPsErrNotFound with
  // nothing held.  See the kSvg discussion on ServeFormatIsAdvertised.
  int RetryByExactId(const DaemonServeRequest& request,
                     DaemonServeDecision* decision);

  // Fills the response state from the held result.
  //
  // `original_flags` is the DURABLE ORIGINAL's flags byte, from the by-id
  // probe, and it is a separate parameter rather than a read off the held
  // result because on an optimized serve the held result is a VARIANT and a
  // variant's flags byte is clear by construction.  Everything else here is a
  // property of the entry being served; the origin's own statements are
  // properties of the entry the writer stamped, and only one of the two is in
  // `result_` at a time.  Passed rather than cached in a member so that its
  // provenance is visible at both call sites and cannot go stale.
  void Compose(const DaemonServeRequest& request, int64 now_seconds,
               bool negotiated, uint8_t original_flags,
               DaemonServeDecision* decision);

  const DaemonAbi* abi_;
  void* cache_;
  void* result_ = nullptr;
};

// The peer's serve-stats mmap, opened lazily and held for the process.
//
// LAZILY, because the worker is the sole CREATOR of that file and a server
// that starts first would otherwise decide once, at startup, that there is no
// serve-stats surface and never look again.  Opened, never created: a front
// end that created one would author a second file the worker does not write.
//
// EXACTLY ONE CLASS PER RESPONSE.  The peer's recorder partitions serves into
// five disjoint classes and counts a value that is not one of them in a
// separate "unrecognised" total, so a double call for one response does not
// error -- it silently breaks the partition the counters exist to provide.
// Record() is therefore called once, from the one place that knows the whole
// response's outcome.
//
// BACK-PRESSURE IS OBSERVE-ONLY AT THIS REVISION, and the shape says so: the
// peer's back-pressure channel here is PS_SERVE_FLAG_NOTIFY_SUPPRESSED, which
// reports that a front end's OWN cooldown or dedup suppressed a request it
// would otherwise have made.  This arm has no such cooldown, so it has
// nothing truthful to report on that channel and always passes
// kPsServeFlagNone.  Nothing in this module branches on back-pressure.
class DaemonServeStats {
 public:
  // Neither `abi` nor the path is owned; `abi` must outlive this object.
  DaemonServeStats(const DaemonAbi* abi, StringPiece cache_path);
  ~DaemonServeStats();

  DaemonServeStats(const DaemonServeStats&) = delete;
  DaemonServeStats& operator=(const DaemonServeStats&) = delete;

  // Records ONE classified serve.  A no-op while the worker has not created
  // the file, which is an ordinary state and not an error.
  //
  // Thread-safe; the open happens once per object however many threads race
  // for it.
  void Record(int serve_class);

  // Records ONE worker-processed serve HIT: the per-type original/optimized
  // byte totals and the hit count, with the SVG-served counter answered from
  // `mask` (0 skips it).  The GATE is the caller's -- serve class
  // kPsServeClassOptimized, a worker-produced entry, a recorded origin
  // content length; this object owns the open, not the verdict.  Same
  // absent-file terms and threading as Record().
  void RecordHit(int content_type, uint64_t original_bytes,
                 uint64_t optimized_bytes, uint32_t mask);

  // Whether the mmap is currently open.  Test seam and evidence only.
  bool open() const { return handle_ != nullptr; }

 private:
  // How many recorded serves pass between attempts to open a file that is
  // not there yet.
  //
  // A LATCH WOULD BE WRONG HERE, and the failure is silent.  The worker is
  // the sole CREATOR of the serve-stats file, and a server that came up
  // first would decide once, on its first serve, that there is no such
  // surface -- and then never look again for the life of the process, so a
  // worker that started a second later would show zero serves for ever.
  // Retrying on EVERY serve is the other extreme: an open syscall per
  // request for as long as the worker is down.
  static constexpr int kReopenInterval = 64;

  // The lazy open, shared by both recorders.  mutex_ must be held.
  bool EnsureOpenLocked();

  const DaemonAbi* abi_;
  const GoogleString cache_path_;
  std::mutex mutex_;
  void* handle_ = nullptr;
  int since_attempt_ = 0;
  bool attempted_ = false;
};

class DaemonAdapter;
class Variable;

// The ONE place in this tree from which a serving seam may construct a
// daemon-side serve reader -- the counterpart of the record arm's factory,
// and held to the same rule: nullptr is returned WITHOUT constructing a
// reader first and discarding it.
//
// Returns nullptr unless the adapter's health says the daemon owns the
// in-place cache for this server AND this process has a usable handle on its
// volume.  The caller owns the returned reader; the served bytes borrow from
// it, so it must outlive the response.
DaemonServeReader* MakeDaemonServeReaderIfReady(DaemonAdapter* adapter);

// Number of daemon serve readers this process has ever CONSTRUCTED.
//
// The counterpart of the two recorders' counters, and for the same reason:
// it is the only way to assert that a seam did NOT build one, as opposed to
// building one and not using it.  Monotonic; never reset.
int64 DaemonServeReadersConstructed();

// Answers a FALLBACK HIT with one notification to the worker, so the family
// converges on the axes the fallback was taken on.  Returns true exactly when
// a notification was sent.
//
// WHY THIS EXISTS.  A fallback hit is SERVED -- the client gets an optimized
// variant, and nothing about the response is wrong.  But a serve records
// nothing, and recording is this substrate's only other way of asking the
// worker for anything, so without this call the family's viewport, density
// and Save-Data convergence would depend on a cache MISS that may never
// come: every request at the missing axes is answered by the fallback, for
// ever.  The peer's own front end re-notifies on exactly this condition; this
// arm's version is the same ask through the same ABI.
//
// FIRE-AND-FORGET, ONCE, WITH NO RETRY.  A delivery failure costs this
// request's convergence and nothing else -- the next fallback hit asks again,
// and the worker's dedup absorbs asks for variants it already has or is
// already building.  The mask sent is the RAW client mask, as the classifier
// answered over this request's headers: the value the record arm notifies at
// and the peer's own front end sends.  The worker normalizes it for dedup
// (the viewport, density and Save-Data axes survive that normalization), so
// sending the selection mask instead would only hide what the client actually
// advertised.
//
// TWO SUPPRESSIONS, and both are structural rather than tuning:
//
//   THE 0x04 GATE.  No re-notify when the durable original carries
//   kPsFlagOriginVariesAccept -- read here off
//   `decision.vary_accept_origin_declared`, which IS that probed bit (see
//   ProbeOriginalFlags: the byte is already read per request for the `Vary`
//   biconditional, so this gate costs no I/O).  At the current pin such URLs
//   never acquire a variant family, so every re-notify would be a request for
//   work the worker will never do -- permanent per-request notify traffic,
//   the regime the peer needed its own production fix for on its front end.
//
//   THE SVG CARVE-OUT, INHERITED AND ALREADY UNREACHABLE.  The peer's front
//   end suppresses its re-notify for a stored SVG variant, because SVG is
//   derived from nothing a client sends and therefore mismatches EVERY raster
//   client's mask -- a permanently unmatchable variant class would re-notify
//   on every request for ever.  That regime cannot arise on this arm: a
//   format the request did not advertise is refused by
//   ServeFormatIsAdvertised and recovered by exact id, so a stored SVG mask
//   can never be the mask this arm serves and the permanent-mismatch loop has
//   no serve to hang on.  INHERITANCE WATCH: any FUTURE peer variant class
//   that is permanently client-unmatchable changes that arithmetic and must
//   be evaluated against this site before it is inherited.
//
// `option_context` / `option_signature` are the resolved configuration's name,
// computed as the record arm computes it; an empty pair SKIPS the notify,
// mirroring the record gate's kNoOptionContext outcome -- a notification with
// no context would be processed under the default one, which is the outcome
// the context exists to prevent.
//
// `notified` and `notify_failed` are the seam's two counters, and they move
// on SEND OUTCOMES ONLY: `notified` when the worker accepted the datagram,
// `notify_failed` when it did not.  A suppression -- the gate, an unnameable
// context, a non-fallback decision -- moves NEITHER, which is what keeps the
// pair readable at rollout: permanent fallback traffic with `notified` flat
// and `notify_failed` climbing is a dead socket, and permanent traffic with
// neither moving is a suppression, and the two are opposite problems.  Either
// pointer may be null.  The return value is exactly "a notification was
// sent", so `notified` and a true return move together.
bool DaemonServeFallbackRenotify(const DaemonAbi& abi, StringPiece socket_path,
                                 const DaemonServeRequest& request,
                                 const DaemonServeDecision& decision,
                                 StringPiece option_context,
                                 StringPiece option_signature,
                                 Variable* notified, Variable* notify_failed);

// Answers an AGE-EXPIRED VARIANT fall-through with the origin-refreshed
// sentinel, so the worker purges the URL's stale variant set and clears the
// dedup entry that would otherwise swallow every later notification.
// Returns true exactly when a notification was sent.
//
// WHY THIS EXISTS.  The fall-through the flag marks is not an ordinary miss:
// entries EXIST for the URL -- it was recorded, optimized, and served -- but
// the selected variant aged past its effective freshness lifetime, so this
// arm declines it and the request re-fetches from the origin.  That
// re-fetch is recorded and the record arm notifies the worker, and there
// the loop closes the wrong way: the worker's processed-set dedup answers
// "already processed, skipping", because the URL was optimized once and
// nothing in the expiry lifecycle clears the dedup entry.  Declined on
// every request, swallowed on every notification: the URL regresses to
// origin-serving one freshness lifetime after optimization, permanently.
// The sentinel is the worker's heal for exactly this transition --
// it purges the stale variant set, clears dedup, rate-limits per URL and
// inline-rebuilds.
//
// THE SENTINEL IS FIRE-AND-FORGET, and EITHER arrival order converges -- no
// ordering between the purge and the in-flight re-record is assumed or
// needed.  If the purge lands first, the re-record then stores the fresh
// durable original and its plain notify, no longer dedup-blocked, triggers
// the rebuild.  If the re-record commits first and the purge deletes the
// fresh durable original along with the stale set, the URL is simply cold:
// no dedup entry is re-added for it (the worker's processed set populates
// only on successful variant writes), so the next request misses, re-records
// and re-notifies, and the rebuild happens then.  Bounded in both orders,
// and in neither can a stale variant be republished: the purge is the only
// event that clears the dedup entry, and everything recorded after it is a
// fresh original.
//
// SENT FOR AN ACCEPT-NEGOTIATING ORIGIN TOO, deliberately -- unlike the
// fallback re-notify, there is NO kPsFlagOriginVariesAccept suppression
// here.  The sentinel is the only thing that purges a URL's stale variant
// set, and a URL optimized BEFORE its origin started sending `Vary: Accept`
// holds exactly such a set; withholding the sentinel would strand it for
// ever.  What the worker does AFTER the purge is not claimed for this
// sender: its decline-to-rebuild check reads the marker at the identity id,
// and this module's marker lives on the durable original (0x0C), so the
// purge-only outcome the peer's own front stage relies on does not engage
// here without the companion worker change.  The purge is still the right
// ask on this side: it destroys the stale set, and clause (ii) of this arm's
// `Vary` biconditional puts the origin's `Vary: Accept` back on every serve
// of the URL -- off the durable original's marker -- which is what makes
// serving whatever is rebuilt from the fresh original correct on the
// negotiation axis.
//
// FIRE-AND-FORGET, ONCE, WITH NO RETRY, and rate-limited at the WORKER per
// URL rather than here: a delivery failure costs this request's heal and
// nothing else -- the next age-expired fall-through asks again, and a
// re-sent sentinel for an already-purged URL is absorbed by the worker's
// own rate limiter.
//
// TWO SUPPRESSIONS, both structural.  A decision that is not a flagged
// fall-through sends nothing -- an exact or fallback serve needs no purge,
// and every other fall-through (a cold key, a client force-revalidate,
// origin `no-cache`) has the ordinary record -> notify path to converge on
// or no outdated variants to purge.  And the option-context gate is the
// same both-halves-or-neither one the record gate and the fallback
// re-notify apply: an empty pair SKIPS the notify, because a notification
// with no context would be processed under the default one.
//
// `notified` and `notify_failed` are the seam's two counters, on the same
// terms as the fallback pair's: they move on SEND OUTCOMES ONLY, a
// suppression moves neither, and the return value is exactly "a
// notification was sent".  Either pointer may be null.
bool DaemonServeOriginRefreshedNotify(
    const DaemonAbi& abi, StringPiece socket_path,
    const DaemonServeRequest& request, const DaemonServeDecision& decision,
    StringPiece option_context, StringPiece option_signature,
    Variable* notified, Variable* notify_failed);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_SERVE_ARM_H_
