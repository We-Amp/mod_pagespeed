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

#ifndef PAGESPEED_SYSTEM_DAEMON_ABI_H_
#define PAGESPEED_SYSTEM_DAEMON_ABI_H_

#include <cstddef>
#include <cstdint>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

// ---------------------------------------------------------------------------
// The optimizer daemon's public C ABI: the subset this module binds.
//
// LICENSING AND BUILD BOUNDARY.  The optimizer daemon and its client library
// are a separate product with their own licence.  Nothing from that source
// tree is vendored into this repository and nothing from it is linked at build
// time.  What follows is an interface DESCRIPTION written against the
// published C ABI contract; the implementation is resolved at run time with
// dlopen()/dlsym() against whichever daemon package the operator installed.
//
// Run-time binding is not an implementation convenience, it is the behaviour
// the co-install contract requires: this module must load, and must keep
// serving, on a host where no daemon package is present.  A build-time link
// would make the absence of the daemon a module-load failure instead of the
// degraded mode described in daemon_adapter.h.
// ---------------------------------------------------------------------------

// Layout-compatible mirror of the daemon's cache-configuration struct.
//
// THE STRUCT-SIZE FIELD IS A REPORT, NOT A GUARD, AND THE DIFFERENCE MATTERS.
// The daemon's plain initializer is NOT size-aware: it writes sizeof(ITS OWN
// struct) and every field it knows about, unconditionally, before the caller
// can inspect anything.  So a daemon whose struct has grown overruns a caller
// whose struct is this one -- and the size it stamps describes the damage
// after the fact.  Checking `struct_size` on return is therefore necessary and
// nowhere near sufficient.
//
// Two things make the redeclaration safe here, and both are load-bearing:
//
//   1. The initializer is only ever called against an OVER-ALLOCATED buffer
//      (kCacheConfigProbeBytes below), never against a bare PsCacheConfig, so
//      a larger peer struct lands in slack this module owns rather than on
//      whatever follows it on the stack.
//   2. When the daemon exports the SIZE-AWARE initializer, that one is used
//      and is told sizeof(PsCacheConfig), so it writes nothing beyond it.
//
// The two paths are then verified DIFFERENTLY, because only one of them can
// tell us anything.  On the size-aware path `struct_size` merely echoes the
// number we passed in, so it proves nothing; what is checkable there is the
// contract itself — that no byte past the stated size was touched.  On the
// size-unaware path the peer stamps ITS OWN sizeof, so a layout that differs
// from this declaration is visible and the library is refused, loudly,
// without any of its values being used.
struct PsCacheConfig {
  size_t struct_size;
  const char* volume_path;
  uint64_t volume_size;
  int enable_checksum;
  size_t ram_cache_size;
  size_t max_metadata_size;
};

// The daemon's success code.  Every other value is an error whose text comes
// back from DaemonAbi::StrError.
inline constexpr int kPsOk = 0;

// The two daemon error codes this module distinguishes by value rather than
// only reporting.  Mirrored from the published error enum.
//
// kPsErrInvalidArg means the module built a call the peer refuses -- a defect
// here, never an operational condition, so it is never retried and never
// treated as a transient miss.  kPsErrNoSpace is the opposite: it is either
// the durable-originals content cap or a full volume, both of which are
// ordinary outcomes for one response.
inline constexpr int kPsErrNoSpace = 4;
inline constexpr int kPsErrInvalidArg = 5;

// The serve arm's third: an entry that is not there.  Distinguished by value
// because it is the ONLY read failure that is an ordinary outcome -- a cold
// URL, or one whose optimization has not happened yet -- while every other
// one is a fault worth reporting.  Serving falls through either way; the two
// are different SERVE CLASSES, and collapsing them would report every I/O
// error on the volume as a cold cache.
inline constexpr int kPsErrNotFound = 1;

// The ABI major/minor this module is written against.  The major must match
// exactly; the minor is a floor.
//
// THE FLOOR IS 8 BECAUSE THIS MODULE BINDS WHAT 1.7 AND 1.8 PUBLISH.  The
// startup half alone needed nothing past 1.0, and the floor said so.
// Recording an original and notifying the optimizer needs the
// durable-originals writer (1.3), the origin-state write fields and the
// Cache-Control / Vary predicates (1.2), and the notification that carries a
// per-request options context (1.5).  1.7 adds the store-side `Vary: Accept`
// predicate; 1.8 adds the origin-headers-not-reproducible flag bit.  Each
// moved the floor when the obligation that comes with it was taken up -- see
// kPsFlagOriginVariesAccept and kPsFlagOriginHeadersNotReproducible below.  A
// daemon older than the floor cannot serve this module at all, so binding
// against it and discovering the missing symbols one at a time would only move
// the same refusal later and make it quieter.
//
// NOTE 1.8's addition is a CONSTANT, not a symbol, so unlike the 1.7 move
// there is nothing for the binder to fail on: an old library exports every
// entry point this module uses and simply does not reserve the bit.  That is
// what makes the version check the whole of the enforcement here, and why the
// version is asked before the surface is bound.
//
// THE SERVE ARM DID NOT MOVE IT, and that is worth stating because the
// opposite would be the natural guess.  Everything the serve arm binds is
// OLDER than the floor: the cache reads and their accessors are 1.0, the
// serve-hit recorder is 1.0, the serve-class recorder is 1.0, the freshness
// evaluator and the `Cache-Control` builder are 1.2.  So a daemon that can be
// recorded into can also be served from, and there is no version in which this
// module records but declines to serve -- a state that would be
// indistinguishable, from the outside, from a serve arm that is simply broken.
//
// The volume-size reader is 1.6 and is deliberately NOT part of the floor: it
// is bound optionally and its absence degrades (in-place optimization off,
// loudly) rather than refusing the library, which is the treatment the sizing
// mirror's own state machine already gives it.  That treatment is available
// because the sizing mirror has a state machine to degrade INTO; the Vary
// predicate has none, which is exactly why it went into the floor instead.
//
// THE FLOOR MOVED TO 8 IN THE CHANGE THAT BINDS 1.8's CONSTANT, which is what
// the pin-advance recorded it would do.  1.8 adds one published constant -- a
// record-time marker for "the origin sent a response header no later serve can
// reproduce" -- and no new or changed symbol, struct or layout, so while
// nothing here wrote or read it the floor deliberately stayed at 7: raising it
// then would have refused a 1.7 daemon this module could still record into and
// serve from, in exchange for nothing.
//
// It buys something now.  The record arm STAMPS that constant and the serve
// arm READS it, and there is no safe answer for its absence -- exactly the
// argument that put the Vary-Accept predicate in the floor.  Against a 1.7
// daemon the bit is not reserved for this meaning: the flags byte's unknown
// bits are read and written back verbatim, so 0x08 would ride through
// untouched and this module would be the only thing that believed it meant
// anything, while some later 1.x could allocate it for something else.
// Stamping a bit whose meaning is not yet published, and then falling through
// on it, is a silent behaviour change for every entry written before the
// allocation -- so the library is refused at startup, in the operator's log,
// rather than used with one statement quietly meaning something else.
inline constexpr int kRequiredAbiMajor = 1;
inline constexpr int kRequiredAbiMinor = 8;

// The durable-originals entry class: the bytes the origin sent, before any
// optimization.  The ONLY class this module writes.  The optimizer owns every
// other class, and a module that wrote one would be a second writer inside a
// class with a single-writer contract.
inline constexpr uint8_t kPsSentinelOriginal = 0x0C;

// The stored-entry flag that says THE ORIGIN negotiates on `Accept` itself,
// mirrored from the peer's PS_FLAG_ORIGIN_VARIES_ACCEPT (published at 1.7).
//
// THIS IS AN OBLIGATION ON THIS MODULE, not a field it may fill in if
// convenient, and its omission is the quietest failure in the record arm.  An
// origin that sent `Vary: Accept` chose the representation from the requesting
// client's `Accept`.  What gets stored is therefore whichever representation
// the FIRST requester happened to elicit.  Unmarked, the optimizer treats that
// one copy as an input it may derive a whole variant family from -- and every
// later client is served bytes transcoded from a stranger's negotiation.
// Nothing errors, nothing downstream can notice, and the entry cannot be
// repaired later: on a cache hit the origin's own `Vary` is gone, so the bit on
// the entry IS the record.
//
// WHY THE PREDICATE IS IN THE FLOOR AND NOT BOUND OPTIONALLY.  An optional
// binding needs a safe answer for "absent", and there is none here.  Treating
// an unbound predicate as "does not vary on Accept" is the silent-wrong-bytes
// outcome above, applied to every Accept-negotiating origin.  Answering it in
// this module instead would be a second implementation of a rule whose whole
// point is that one side owns it -- the same reason the refusal predicate is
// the peer's (see VaryUncacheable below), and the two are documented as halves
// of ONE classification that cannot disagree.  Refusing every response that
// carries any `Vary` at all would be safe but is a policy change to a shipped
// gate, made in a branch nothing exercises, to keep alive a daemon that at 1.6
// cannot use a recorded original as an optimization source anyway.  A version
// refusal at startup, in the operator's log, is the loud form of the same
// answer.
//
// The value is hand-copied because nothing from the peer's tree may be
// included here.  It is checked against the peer's own header exactly once, at
// compile time, in the harness overlay that IS built inside that tree at the
// pin: tools/parity/daemon-probe/volume_inspect.cc.  A pin bump that renumbers
// the bit fails that build rather than letting this stamp a bit that has come
// to mean something else.
inline constexpr uint8_t kPsFlagOriginVariesAccept = 0x04;

// The stored-entry flag that says THE ORIGIN SENT A RESPONSE HEADER NO LATER
// SERVE FROM THIS ENTRY CAN REPRODUCE, mirrored from the peer's
// PS_FLAG_ORIGIN_HEADERS_NOT_REPRODUCIBLE (published at 1.8).
//
// WHY IT HAS TO BE RECORDED RATHER THAN DERIVED.  A stored entry carries the
// origin's Content-Type, its validators and its Cache-Control state, and
// nothing else: the origin's OTHER headers are gone the moment the response
// has been streamed.  So at serve time this module cannot tell a response it
// can reproduce from one it cannot -- and the difference matters, because
// serving from the entry silently DROPS whatever it could not keep.  An
// arbitrary `X-Custom`, a CORS grant, a security policy: all gone, no error,
// nothing downstream able to notice.  The bit is the writer's answer to a
// question only the writer can still ask.
//
// WHAT IT IS AND IS NOT.  It is ADVISORY at the peer: admission stores a
// marked response exactly as an unmarked one, selection never reads the flags
// byte, and the optimizer neither refuses nor prefers a marked entry.  It is
// NOT advisory here -- this module's serve arm falls through on it, which is
// rule 6's ruled destination -- and those two facts are consistent rather than
// in tension: the peer's own front stage keeps a per-URL header sidecar and
// can reproduce what this module cannot, so the bit states the FACT and each
// consumer decides.  That is also why the record arm still stores and still
// notifies a marked response: what the optimizer may do with a URL is not this
// module's to decide from one front end's inability to serve it.
//
// IT LIVES ONLY ON THE ENTRY THE WRITER STAMPED.  Optimizer-produced variants
// do not carry it (a call-site convention across the peer's write paths, not
// fresh-byte construction), so it is readable ONLY through an exact-id read of
// the durable original -- `ps_cache_read_alternate(..., kPsSentinelOriginal)`,
// which is the read the peer blesses for that class.  A selection result never
// carries it, and a serve-side check taken on one is therefore a structural
// false all-clear rather than a check that happens to pass.  See the read site
// in daemon_serve_arm.cc.
//
// ONE EFFECT IS OBSERVABLE BY DESIGN: the flags byte feeds the weak hit
// validator on both sides, so a marked entry does not validate against an
// unmarked one.  For this module that costs nothing -- it never emits a
// validator for a marked entry, because it never serves one.
//
// The value is hand-copied for the same reason kPsFlagOriginVariesAccept is,
// and is checked against the peer's own header at compile time in the harness
// overlay built inside that tree at the pin
// (tools/parity/daemon-probe/volume_inspect.cc).
inline constexpr uint8_t kPsFlagOriginHeadersNotReproducible = 0x08;

// The response-header sidecar class, mirrored from the peer's
// PS_SENTINEL_HEADERS_SIDECAR.
//
// NEITHER WRITTEN NOR READ BY THIS MODULE, and named here so that the
// omission is a decision on the record rather than a gap.  The sidecar is the
// class that carries the request-independent response headers an optimized
// entry has to reproduce -- the class rule 6's fall-through is really about.
// Nothing in this module writes it (the record arm writes the durable original
// and nothing else) and no other writer can reconstruct it for a URL this
// module recorded, because the origin's other headers are gone by the time the
// optimizer sees the entry.
//
// THAT IS WHY THE REPRODUCIBLE SET HERE IS NARROWER THAN THE PEER'S, and the
// difference is the whole reason this module does not reuse the peer's own
// sidecar admission verdict.  That verdict treats every sidecar-carried name
// -- a fixed ACAO, a static CSP, the security-header family -- as
// reproducible, on the strength of a sidecar that has NO WRITER for a URL this
// module recorded.  Applied here it would admit those responses and drop their
// headers exactly as an unmarked module would.  The predicate this module
// applies is the PRE-SIDECAR one, derived from what its own serve arm actually
// reconstructs; see OriginHeadersAreReproducible in daemon_record_arm.h.
//
// The serve-time signal itself is no longer missing: it is
// kPsFlagOriginHeadersNotReproducible above, stamped by the record arm and
// read back by id on the durable original.  It needs no sidecar writer and no
// second entry class.
inline constexpr uint8_t kPsSentinelHeadersSidecar = 0x6C;

// The capability mask's image-format field, mirrored from the peer's layout
// (bits 0-1 of the mask; the alternate id IS the mask's low byte).
//
// WHY THIS MODULE CARRIES THE LAYOUT AT ALL, when its policy everywhere else
// is to ask the peer.  There is still no exported predicate for "may this
// stored variant be served to this client": the peer publishes a SCORER
// (ps_score_alternate) and a selector built on it, and a score is not a
// refusal -- a caller that consumed one as a verdict would be reading a
// ranking as permission.  So the format axis is read here, from the two bits
// the peer's own header documents as the format field, and the disqualify is
// this module's.  WHAT IS CHECKED AT COMPILE TIME, stated exactly because
// the checking is uneven: in the harness overlay that is built inside the
// peer's tree (tools/parity/daemon-probe/volume_inspect.cc), the two flag
// mirrors above are asserted against the peer's own PS_FLAG_* macros, and
// the viewport constants below through every PS_SENTINEL_* id the peer's
// header publishes -- each asserted to sit at the reserved viewport value,
// which is the fact the serve arm's sentinel guard rests on.  The peer
// publishes NO constant for the format field itself, so these two bits have
// no compile-time check anywhere; they are carried by the harness's mirrored
// decode table instead, which is a weaker guarantee and is stated as one.
//
// WHAT CHANGED AT THE CURRENT PIN, because the original reason for this
// mirror was sharper than the one that survives.  The scorer used to award
// 200 to a variant whose format the client never advertised
// (We-Amp/pagespeed-optimizer#1331), so the selection really did hand back AVIF
// bytes to a client that asked for WebP and this module's disqualify was the
// only thing standing between that and the wire.  The peer's own fix
// (its #1334) now hard-disqualifies exactly that case, which makes the
// disqualify here DEFENSE IN DEPTH rather than a workaround.  It stays, and
// the mirror stays with it, for two reasons that do not depend on the defect:
// the peer's scoring is not part of its published contract and may move
// again, and a by-id read bypasses the scorer entirely -- so this module
// still needs its own answer to a question the ABI does not export.
inline constexpr uint32_t kPsMaskImageFormatShift = 0;
inline constexpr uint32_t kPsMaskImageFormatBits = 0x03;
inline constexpr uint32_t kPsMaskTransferEncodingShift = 6;
inline constexpr uint32_t kPsMaskTransferEncodingBits = 0x03;

// Image-format values in that field.
inline constexpr uint32_t kPsImageFormatOriginal = 0;
inline constexpr uint32_t kPsImageFormatWebp = 1;
inline constexpr uint32_t kPsImageFormatAvif = 2;
inline constexpr uint32_t kPsImageFormatSvg = 3;

// Transfer-encoding values in that field.  Only "identity" is named, because
// it is the only value this module will serve: see ServeFormatIsAdvertised.
inline constexpr uint32_t kPsTransferEncodingIdentity = 0;

// The viewport field of a capability mask: bits 2-3.  Real viewports are
// 0 (mobile), 1 (tablet) and 2 (desktop); the value 3 is NOT a viewport and
// never comes out of the peer's classifier -- it is what makes an alternate
// id a SENTINEL.  The peer's own alternate-id header says so twice over:
// every sentinel id (the originals class 0x0C, early hints, warmup, content
// hash, subresource manifest, browser profile, and the response-header
// sidecar 0x6C this file names kPsSentinelHeadersSidecar above) has
// viewport==3, and FromHeaders() cannot produce it
// (ps_mask_set_viewport_from_width maps every width into one of the three
// classes, and the classifier's viewport is one of three device classes).
//
// MIRRORED rather than bound, like the field shifts above: this module
// derives ids locally (the serve arm's by-id retry), so it needs the layout
// even where the peer would validate a mask it was handed.
inline constexpr uint32_t kPsMaskViewportShift = 2;
inline constexpr uint32_t kPsMaskViewportBits = 0x03;

// The reserved viewport value: any id carrying it is in the peer's sentinel
// space, not its capability-vector space.
inline constexpr uint32_t kPsViewportSentinel = 3;

// The origin-refreshed sentinel, mirrored from the peer's
// kOriginRefreshedSentinel: the notification MASK that tells the worker "the
// origin was re-fetched fresh for this URL while stale optimized variants
// may still be stored".  The worker answers by purging the URL's stale
// variant set, clearing its processed-set dedup entries, and rebuilding
// inline from the fresh original -- exactly the heal for the split-brain in
// which a variant aged past its freshness lifetime is declined on every
// request while every plain notification is dedup-skipped as already
// processed.
//
// A MASK VALUE, NOT AN ALTERNATE ID: it travels in PsNotifyParams.mask and
// never names a stored entry, which is what the high 24 bits are for -- no
// capability vector sets them, so the value can never collide with a request
// for an actual variant.  Its low byte (0xFC) decodes to viewport==3, the
// sentinel space above.
//
// HAND-COPIED like the flag mirrors above, for the same reason, and checked
// the same way: the harness overlay built inside the peer's tree at the pin
// (tools/parity/daemon-probe/volume_inspect.cc) asserts it against the
// peer's own constant at compile time, so a pin that renumbers the value
// fails that build rather than letting this module send a mask the worker
// no longer reads as the sentinel.
inline constexpr uint32_t kOriginRefreshedSentinel = 0xFFFFFFFC;

// Content types, as the peer numbers them.
enum PsContentType {
  kPsContentHtml = 0,
  kPsContentCss = 1,
  kPsContentJs = 2,
  kPsContentImage = 3,
  kPsContentOther = 4,
};

// Origin Cache-Control directives, as the peer's parser reports them.  Only
// the bits the record arm acts on are named; the rest ride through to the
// entry unread, which is the point of storing the whole field.
inline constexpr uint16_t kPsCcOriginNoStore = 0x0004u;
inline constexpr uint16_t kPsCcOriginNoTransform = 0x0100u;

// Rendered length of an options-context signature, not counting the NUL.
inline constexpr size_t kPsOptionContextSignatureChars = 64;

// Largest canonical options-context payload the peer accepts.  Stated here
// because a caller has to build against the same ceiling; this module's own
// producer derives the same number from its option table, and a unit test
// pins the two against each other.
inline constexpr size_t kPsMaxOptionContextBytes = 16384;

// Layout-compatible mirrors of the peer's published parameter structs.
//
// Every one of them carries `struct_size` and is APPEND-ONLY on the peer's
// side, so the contract is: this module stamps the size ITS OWN build sees
// through the size-aware initializer, and the peer reads exactly that many
// bytes and treats the rest as absent.  That makes a peer newer than this
// build safe in both directions -- but ONLY while the prefix these
// declarations describe is byte-identical to the peer's, so they are copied
// field for field from the published header and never "tidied".
//
// The reserved tails are not padding this module chose.  They are part of the
// published layout and exist so that sizeof() is the same number under every
// compiler rather than whatever tail padding each one picks; they must be
// left zero.
struct PsWriteParams {
  size_t struct_size;
  uint8_t alternate_id;
  uint64_t content_length;
  uint32_t full_mask;
  int content_type;
  uint8_t flags;
  const char* origin_ct;
  const char* origin_etag;
  uint32_t cache_inserted_at;
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;
  uint32_t origin_last_modified;
  uint16_t origin_cc_flags;
  uint8_t reserved_[6];
};

struct PsCacheControl {
  size_t struct_size;
  uint32_t max_age;
  uint32_t s_maxage;
  uint16_t cc_flags;
  uint8_t reserved_[2];
};

struct PsNotifyParams {
  size_t struct_size;
  const char* url;
  const char* hostname;
  const char* scheme;
  int content_type;
  uint32_t mask;
  int agent_request;
  const char* option_context;
  size_t option_context_length;
  const char* option_signature;
};

// ---------------------------------------------------------------------------
// The serve arm's mirrors.
//
// The same append-only, struct_size-stamped convention as the record arm's,
// and the same rule about not tidying them.  These three are NOT stamped by a
// peer initializer -- the peer exports a sized initializer only for the
// freshness CONFIG -- so this module stamps `struct_size` itself and the
// static_asserts below are the only thing standing between a layout change
// and a silently truncated read.
// ---------------------------------------------------------------------------

// The peer's freshness verdict.  Mapped to control flow in the serve arm; the
// retired value is kept so a switch over it stays exhaustive against the
// peer's own enum.
enum PsFreshnessVerdict {
  kPsFreshnessFresh = 0,
  kPsFreshnessStaleServeRetired = 1,
  kPsFreshnessServeNoCache = 2,
  kPsFreshnessRevalidate = 3,
};

// Shared (proxy) cache or private.  ZERO IS SHARED, which is the peer's own
// default and the fail-safe direction, and this module never sends the other
// value: the volume it reads is written by the optimizer and read by every
// request that reaches this server, which is a shared cache by definition.
enum PsCacheScope {
  kPsCacheScopeShared = 0,
  kPsCacheScopePrivate = 1,
};

// What the emitted `Cache-Control` may promise downstream.  This module sends
// only the SAFE mode: the aggressive one is what may synthesize
// `stale-while-revalidate`, and a synthesized SWR emitted from an origin
// authorises downstream staleness the origin never permitted.
enum PsCacheMode {
  kPsCacheModeSafe = 0,
  kPsCacheModeAggressive = 1,
};

// One classified serve.  Exactly one class per response; the values are
// disjoint single bits on the peer's side and combining two of them yields a
// value that is not a class at all, which the peer drops rather than
// mis-attributing.
enum PsServeClass {
  kPsServeClassOptimized = 1,
  kPsServeClassOriginalCold = 2,
  kPsServeClassOriginalPending = 4,
  kPsServeClassOriginalDeclined = 8,
  kPsServeClassOriginalSkew = 16,
};

inline constexpr uint32_t kPsServeFlagNone = 0u;

struct PsFreshnessConfig {
  size_t struct_size;
  uint32_t max_age_cap;
  uint32_t immutable_max_age_cap;
  uint32_t html_max_age;
  uint32_t css_max_age;
  uint32_t image_max_age;
  uint8_t reserved_[4];
};

struct PsFreshnessInput {
  size_t struct_size;
  uint32_t now_seconds;
  uint32_t cache_inserted_at;
  uint32_t origin_max_age;
  uint32_t origin_s_maxage;
  int content_type;
  int cache_scope;
  int force_revalidate;
  uint16_t origin_cc_flags;
  uint8_t reserved_[2];
};

struct PsFreshnessResult {
  size_t struct_size;
  int verdict;
  uint32_t age_seconds;
  uint32_t effective_max_age;
  uint32_t remaining_ttl;
  int is_stale;
  int expired_by_age;
};

struct PsCacheControlInput {
  size_t struct_size;
  int mode;
  int cache_scope;
  uint32_t effective_max_age;
  uint32_t origin_max_age;
  int content_type;
  uint16_t origin_cc_flags;
  uint8_t synthesize_swr;
  uint8_t relay_origin_no_cache;
  uint8_t forward_origin_restrictions;
  uint8_t reserved_[5];
};

// The peer documents 256 bytes as comfortably enough for every combination of
// directives it can emit.  Stated as a constant so the one buffer that has to
// be that size is not a literal at a call site.
inline constexpr size_t kPsCacheControlBufferBytes = 256;

// The published sizes of the three structs above, on the only data model the
// peer publishes a layout for.
//
// THESE ARE NOT DECORATION, and the failure they catch is silent.  The peer
// does not merely read `struct_size` bytes: for the write parameters it snaps
// the declared size DOWN to the nearest size it has ever published, so a
// struct that is one byte off is read as the much shorter earlier revision --
// and every origin validator, the Cache-Control state and the age-adjusted
// insertion time are dropped, with no error anywhere.  The entry is simply
// stored without the origin state it was given.  Likewise a notification
// struct short of the full size loses the whole options-context tail, and a
// notification processed under the default context is exactly what that
// context exists to prevent.
//
// So: if a compiler ever lays these out differently, that is a build failure
// here rather than a class of cache entry that is quietly wrong.
#if defined(__LP64__) || defined(_LP64)
static_assert(sizeof(PsWriteParams) == 80,
              "the cache-write parameter layout no longer matches the "
              "published one; a mismatched size is silently truncated by the "
              "peer, not rejected");
static_assert(sizeof(PsCacheControl) == 24,
              "the Cache-Control state layout no longer matches the published "
              "one");
static_assert(sizeof(PsNotifyParams) == 72,
              "the notification parameter layout no longer matches the "
              "published one; short of the full size the peer drops the "
              "options context and processes the notification under the "
              "default one");
static_assert(sizeof(PsFreshnessConfig) == 32,
              "the freshness-configuration layout no longer matches the "
              "published one");
static_assert(sizeof(PsFreshnessInput) == 40,
              "the freshness-input layout no longer matches the published "
              "one; the peer reads struct_size bytes, so a short struct is a "
              "freshness verdict taken over fields that were never supplied");
static_assert(sizeof(PsFreshnessResult) == 32,
              "the freshness-result layout no longer matches the published "
              "one");
static_assert(sizeof(PsCacheControlInput) == 40,
              "the Cache-Control input layout no longer matches the published "
              "one; a mis-sized struct is a downstream lifetime built from "
              "fields the peer read out of the wrong offsets");
#endif

// Bytes handed to the daemon's cache-configuration initializer.
//
// Generously larger than PsCacheConfig so that a daemon built against a grown
// struct writes into slack this module allocated rather than past the object.
// The multiple is arbitrary because it has to be: the overrun it absorbs is by
// definition a struct this build has never seen.  It is cheap (one stack
// buffer, once, at startup) and it is the only thing standing between an
// upgraded daemon package and stack corruption in the server's parent process.
inline constexpr size_t kCacheConfigProbeBytes = sizeof(PsCacheConfig) * 8;

// Byte the cache-config probe buffer is filled with before the daemon writes
// into it, so the slack past sizeof(PsCacheConfig) can be checked for damage
// afterwards.  Any value works; a non-zero, non-0xFF one makes an accidental
// memset or a plain overwrite both visible.
inline constexpr unsigned char kCacheConfigProbeCanary = 0xA5;

// A bound daemon client library.  Instances are produced by LoadDaemonAbi and
// are owned by the caller; destroying one unbinds the library.
class DaemonAbi {
 public:
  DaemonAbi(const DaemonAbi&) = delete;
  DaemonAbi& operator=(const DaemonAbi&) = delete;

  virtual ~DaemonAbi();

  virtual int VersionMajor() const = 0;
  virtual int VersionMinor() const = 0;

  // Fills *config with the DAEMON's own defaults.
  //
  // PRECONDITION: the caller owns at least kCacheConfigProbeBytes at that
  // address.  That is not the size this passes on — the size-aware spelling is
  // told sizeof(PsCacheConfig), because that is what this build owns AS A
  // STRUCT and telling it anything larger would have it describe a struct the
  // caller does not have.  The extra storage exists for the OTHER path: the
  // size-unaware spelling writes its own struct's worth whatever the caller
  // owns, and the slack is the only thing keeping a grown peer inside memory
  // this module allocated.
  virtual void CacheConfigInit(PsCacheConfig* config) const = 0;

  // Whether the daemon exports the size-aware initializer.  The caller needs
  // this because the two paths are checkable in different ways — see the
  // handshake in LoadDaemonAbi.
  virtual bool HasSizedCacheConfigInit() const = 0;

  // The volume size, in bytes, the daemon opened its cache with, or 0 when
  // that is NOT KNOWN -- including when the daemon package is too old to
  // export the entry point at all.  0 never means "use a default": see the
  // mirror discussion in daemon_adapter.h.
  virtual uint64_t SharedConfigVolumeSize(const char* volume_path) const = 0;

  // Whether this daemon publishes its volume sizing at all.  False means an
  // older package, which is a degrade, not a failure.
  virtual bool PublishesVolumeSize() const = 0;

  // The cache-directory generation the daemon published in its shared config
  // (the `cache_dir_generation` field of pagespeed-shared.conf), or 0 when
  // that is NOT KNOWN -- including when the daemon package predates the entry
  // point at all, which is exactly the pre-H1 daemon whose layout is the
  // legacy, unversioned one.  0 is "unknown", never a generation number: see
  // kCacheDirGeneration in daemon_adapter.h.
  virtual uint32_t SharedConfigGeneration(const char* volume_path) const = 0;

  // Whether this daemon publishes a cache-directory generation at all.  False
  // means a pre-H1 package, which the adapter tolerates as the legacy layout
  // (one loud line, keep working with the configured paths) rather than
  // refusing.
  virtual bool PublishesGeneration() const = 0;

  // Returns kPsOk and stores an opaque cache handle in *out_cache, or an
  // error code.  A successful open must be matched with CacheClose.
  virtual int CacheOpen(const PsCacheConfig* config,
                        void** out_cache) const = 0;
  virtual void CacheClose(void* cache) const = 0;

  virtual const char* StrError(int error) const = 0;

  // The peer's own sentence about the call that just failed, or nullptr when
  // the installed library does not publish one.
  //
  // WHY THIS IS NOT REDUNDANT WITH StrError.  StrError names the error CLASS
  // and nothing else -- the whole of what an operator got for a failed volume
  // open was "Input/output error", which is true and useless.  The peer keeps
  // the actual reason ("could not set its mode to 0660 (Operation not
  // permitted)") beside the code, and dropping it turned a one-line fix into
  // an investigation.  So the class is reported WITH the reason wherever the
  // reason exists.
  //
  // MUST BE READ IMMEDIATELY AFTER THE FAILING CALL, and the storage is
  // borrowed.  The peer keeps the text in a thread_local buffer that it
  // CLEARS on entry to ps_cache_open, so the value is private to the calling
  // thread (no cross-thread interleaving to reason about) but lives only
  // until this module calls the library again.  Call sites therefore read it
  // before anything else and copy it out, rather than holding the pointer.
  //
  // BOUND OPTIONALLY, so a daemon package older than the entry point still
  // loads and still works -- the degrade is one missing clause in a log line,
  // which is exactly the message this module emitted before, and nowhere near
  // enough to refuse a library over.
  //
  // nullptr AND THE EMPTY STRING ARE THE SAME ANSWER, and both are real: an
  // absent symbol gives nullptr, while a library that has one but is idle --
  // the buffer cleared and nothing written back into it -- gives "".  A
  // caller that checked only for nullptr would print a bare "()".
  virtual const char* LastErrorMessage() const = 0;

  // ---------------------------------------------------------------------
  // The record arm.
  //
  // Everything below is bound unconditionally: the version gate above
  // refuses any daemon that does not export it, so a bound DaemonAbi always
  // has a usable record arm and no caller has to test for one.
  // ---------------------------------------------------------------------

  // Zeroes *params and stamps struct_size = sizeof(PsWriteParams) as THIS
  // build sees it.  Never the size of a surrounding buffer: the number is
  // what the peer uses to decide how many bytes of the struct exist, so a
  // larger one has it read fields that are not there.
  virtual void WriteParamsInit(PsWriteParams* params) const = 0;
  virtual void NotifyParamsInit(PsNotifyParams* params) const = 0;

  // Opens a streaming write of the DURABLE ORIGINAL for `url`.  The peer
  // refuses any alternate id but the originals class, which is what makes
  // this the only writer this module can be.
  virtual int CacheWriteOriginal(void* cache, const char* url,
                                 const char* hostname, const char* scheme,
                                 const PsWriteParams* params,
                                 void** out_handle) const = 0;
  virtual int WriteData(void* handle, const void* data,
                        size_t length) const = 0;
  virtual int WriteClose(void* handle) const = 0;
  virtual void WriteAbort(void* handle) const = 0;

  // The peer's STORE-SIDE Vary refusal predicate: non-zero when a response
  // carrying this `Vary` must not be stored.  `vary` is the field value, or
  // nullptr for a response with no Vary.  Several Vary lines must be joined
  // with "," first -- they are one list, and asking per line accepts
  // combinations the predicate as a whole refuses.
  //
  // This module calls the PEER'S predicate rather than its own storability
  // logic for this axis, deliberately.  The two disagree, and the shared
  // cache plane is the peer's: an entry admitted under the looser rule is one
  // the peer's serving side would hand to a client the key cannot tell apart.
  virtual int VaryUncacheable(const char* vary) const = 0;

  // The OTHER half of the same store-side verdict: non-zero when the ORIGIN
  // negotiates on `Accept` itself.  `vary` has exactly the shape
  // VaryUncacheable takes -- the joined field value, or nullptr for none.
  //
  // The peer exports both halves as thin wrappers over ONE classification of
  // one value, so they cannot disagree about it, and it guarantees the pairing
  // in one direction: this is always 0 for a response VaryUncacheable refuses.
  // The gate relies on that ordering rather than re-checking it -- a refused
  // response has already returned by the time this is asked.
  //
  // A non-zero answer obliges the caller to set kPsFlagOriginVariesAccept on
  // what it stores; see that constant for what omitting it costs.
  virtual int VaryVariesAccept(const char* vary) const = 0;

  // Parses ONE Cache-Control response line into *out, ACCUMULATING.  Call
  // once per line, in order, against the same output.
  virtual int ParseCacheControl(const char* header_value,
                                PsCacheControl* out) const = 0;

  // Folds an inbound `Age` into an insertion timestamp: the time the ORIGIN
  // generated the response.  Exported by the peer precisely so that every
  // port's arithmetic here is the same one; a port that skips it stores
  // entries that look fresh for the whole of Age and are not.
  virtual uint32_t AgeAdjustedInsertTime(uint32_t now_seconds,
                                         uint32_t age_seconds) const = 0;

  // The peer's request classifier: the capability mask for one request's
  // headers.  Absent headers are passed as nullptr, not as "".
  virtual uint32_t Classify(const char* accept, const char* user_agent,
                            const char* save_data,
                            const char* accept_encoding) const = 0;

  // The peer's content-type classifier, over a Content-Type field value.
  virtual int ClassifyContentType(const char* content_type_header) const = 0;

  // Signs a canonical options-context payload.  `out` must have room for
  // kPsOptionContextSignatureChars + 1 bytes.
  virtual int OptionContextSignature(const char* payload, size_t payload_length,
                                     char* out, size_t out_size) const = 0;

  // Sends one notification.  Fire and forget: the peer answers nothing, and a
  // delivery failure costs this response its optimization and nothing else.
  virtual int NotifyWorker(const char* socket_path,
                           const PsNotifyParams* params) const = 0;

  // ---------------------------------------------------------------------
  // The serve arm.
  //
  // Bound unconditionally beside the record arm's: every entry point here was
  // published at or before 1.2 and so predates every floor this module has
  // had, which means a daemon this module will record into always exports all
  // of it.  See the floor discussion above for why that matters.
  //
  // A read RESULT is an opaque handle the peer owns until ReadFree; the
  // bytes and every `const char*` accessor below borrow from it and are
  // invalid afterwards.  The handle must also be released before the cache
  // it came from is closed.
  // ---------------------------------------------------------------------

  // Selects the best stored VARIANT for `mask`.  Returns the peer's status;
  // kPsErrNotFound when nothing was selected.
  //
  // NEVER RETURNS THE DURABLE ORIGINAL.  The peer's selector skips that
  // class unconditionally and by design, so that the original never competes
  // with the optimized variants -- which means a serving path built on this
  // call alone finds NOTHING for a URL this module recorded and nothing has
  // yet optimized.  Read the original with ReadAlternate and
  // kPsSentinelOriginal instead; the peer's own header blesses that as the
  // way to read the class.
  virtual int CacheReadBest(void* cache, const char* url, const char* hostname,
                            const char* scheme, uint32_t mask,
                            void** out_result) const = 0;

  // Reads ONE stored entry by its exact alternate id.  This is how the
  // durable-originals class is read back, and it is the only read that can
  // reach it.
  virtual int CacheReadAlternate(void* cache, const char* url,
                                 const char* hostname, const char* scheme,
                                 uint8_t alternate_id,
                                 void** out_result) const = 0;

  // The stored bytes.  `*out_data` borrows from `result`.
  virtual int ReadContent(const void* result, const uint8_t** out_data,
                          size_t* out_length) const = 0;

  // The stored entry's full capability mask, and its flag byte (the
  // kPsFlag* bits).
  virtual uint32_t ReadMask(const void* result) const = 0;
  virtual uint8_t ReadFlags(const void* result) const = 0;

  // The entry's content type, and the ORIGIN's `Content-Type` field value
  // stored verbatim (null when none was recorded).
  virtual int ReadContentType(const void* result) const = 0;
  virtual const char* ReadOriginContentType(const void* result) const = 0;

  // The origin's stored freshness and validator state, exactly the inputs
  // EvaluateFreshness and BuildCacheControl take.  Zero / null is "not
  // recorded" rather than "the origin said zero"; see the peer's header.
  virtual uint32_t ReadCacheInsertedAt(const void* result) const = 0;
  virtual uint32_t ReadOriginMaxAge(const void* result) const = 0;
  virtual uint32_t ReadOriginSMaxAge(const void* result) const = 0;
  virtual uint16_t ReadOriginCcFlags(const void* result) const = 0;
  virtual uint32_t ReadOriginLastModified(const void* result) const = 0;
  virtual const char* ReadOriginEtag(const void* result) const = 0;

  // The variant's stamped source-content hash (32 bytes), or null when the
  // entry carries no binding.  The first tier of the derived validator's
  // content-identity discriminator.
  virtual const uint8_t* ReadOriginHtmlHash(const void* result) const = 0;

  // Whether the OPTIMIZER produced this entry.  The half of the serve-stats
  // gate this module can answer from an entry; the other half is the stored
  // origin content length.
  virtual int ReadIsWorkerProcessed(const void* result) const = 0;
  virtual uint32_t ReadOriginContentLength(const void* result) const = 0;

  virtual void ReadFree(void* result) const = 0;

  // The peer's own RFC 9111 dialect, exported so that a front end does not
  // end up with a second implementation that disagrees with the engine's in
  // the cases nobody tests.
  virtual int EvaluateFreshness(const PsFreshnessInput* input,
                                const PsFreshnessConfig* config,
                                PsFreshnessResult* out) const = 0;
  virtual int BuildCacheControl(const PsCacheControlInput* input, char* buf,
                                size_t capacity, size_t* out_len,
                                uint32_t* out_final_max_age) const = 0;

  // The serve-stats mmap the optimizer creates beside its cache.  Opened,
  // never created: a front end that created one would author a second file
  // the worker does not write.  kPsErrNotFound before the worker has
  // started, which is an ordinary state and not an error.
  virtual int ServeStatsOpen(const char* cache_path,
                             void** out_handle) const = 0;
  virtual void ServeStatsRecordServeClass(void* handle, int serve_class,
                                          uint32_t flags) const = 0;

  // ONE worker-processed serve HIT: the per-type original/optimized byte
  // totals and the hit count, plus the SVG-served counter when `mask` names
  // an SVG variant (pass 0 to skip that accounting).  Published at 1.0, so it
  // predates every floor this module has had.  The GATE is the caller's --
  // serve class kPsServeClassOptimized, a worker-produced entry, and a
  // recorded origin content length -- exactly the gate the peer's own front
  // ends apply; the peer no-ops on a content type it does not count.
  virtual void ServeStatsRecordHit(void* handle, int content_type,
                                   uint64_t original_bytes,
                                   uint64_t optimized_bytes,
                                   uint32_t mask) const = 0;
  virtual void ServeStatsClose(void* handle) const = 0;

 protected:
  DaemonAbi();
};

// The soname the daemon package installs.  Resolved through the dynamic
// loader's normal search path, so an operator who installs the daemon
// somewhere unusual configures it the way they configure any other shared
// library on the host.
extern const char kDefaultDaemonLibraryName[];

// Binds `library_path` and every entry point of DaemonAbi, checking the ABI
// version and the cache-config struct size.  Returns nullptr on any failure,
// with *error set to a message fit for an operator's error log.  Never
// crashes and never aborts: an absent or mismatched daemon package is an
// expected state, not a bug.
DaemonAbi* LoadDaemonAbi(StringPiece library_path, GoogleString* error);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_ABI_H_
