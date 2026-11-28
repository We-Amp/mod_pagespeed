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
//
// THE FLOOR IS 12 BECAUSE EVERY CACHE CALL THIS MODULE MAKES IS SITED.  1.12
// publishes the _site cache entry points and the `site` and `notify_flags`
// fields of the notification.  A worker cache entry is read or written for a
// request only under the site of the server block, virtual host or IIS site
// that admitted it, and every notification names that site and says it comes
// from this module.  There is no unsited fallback to degrade to: the unsited
// entry points share one keyspace across every site on the worker, which is
// exactly what the site exists to prevent, so a library without the _site
// entry points is refused here -- whatever version it reports -- and the
// unsited ones are never bound.
inline constexpr int kRequiredAbiMajor = 1;
inline constexpr int kRequiredAbiMinor = 12;

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
// compile time, in the internal parity harness's overlay that IS built inside
// that tree at the pin.  A pin bump that renumbers
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
// and is checked against the peer's own header at compile time in the
// internal parity harness's overlay built inside that tree at the pin.
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
// the checking is uneven: in the internal parity harness's overlay that is
// built inside the peer's tree, the two flag
// mirrors above are asserted against the peer's own PS_FLAG_* macros, and
// the viewport constants below through every PS_SENTINEL_* id the peer's
// header publishes -- each asserted to sit at the reserved viewport value,
// which is the fact the serve arm's sentinel guard rests on.  The peer
// publishes NO constant for the format field itself, so these two bits have
// no compile-time check anywhere; they are carried by the harness's mirrored
// decode table instead, which is a weaker guarantee and is stated as one.
//
// WHAT CHANGED AT THE CURRENT PIN, because the original reason for this
// mirror was sharper than the one that survives. The scorer used to award 200
// to a variant whose format the client never advertised, so the selection
// really did hand back AVIF bytes to a client that asked for WebP and this
// module's disqualify was the only thing standing between that and the wire.
// The peer's own fix (a later release) now hard-disqualifies exactly that case,
// which makes the disqualify here DEFENSE IN DEPTH rather than a workaround. It
// stays, and the mirror stays with it, for two reasons that do not depend on
// the defect: the peer's scoring is not part of its published contract and may
// move again, and a by-id read bypasses the scorer entirely -- so this module
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

// Transfer-encoding values in that field, as the peer numbers them.
// Identity is what this module serves unless told otherwise; gzip and
// brotli are served only as the optimizer's STORED copies, by a reader whose
// switch is on, to a client that listed the coding (see ServeSelectionMask).
// The fourth value is reserved: it names no coding and is never served.
inline constexpr uint32_t kPsTransferEncodingIdentity = 0;
inline constexpr uint32_t kPsTransferEncodingGzip = 1;
inline constexpr uint32_t kPsTransferEncodingBrotli = 2;

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

// The pixel-density field of a capability mask: bit 4, 0 for 1x and 1 for 2x
// and above.  Mirrored like the fields above, for ApplySecChDprHint.
inline constexpr uint32_t kPsMaskPixelDensityShift = 4;
inline constexpr uint32_t kPsMaskPixelDensityBits = 0x01;

// THE REQUEST HEADERS AN IMAGE VARIANT IS SELECTED BY, as the `Vary` field
// value a response served from a selected image variant carries.  MIRRORED
// from the peer's kImageSelectionVary (lib/classify/capability_mask.h), byte
// for byte, and checked against the pinned peer's source by
// tools/ci/check_image_selection_vary.py: the module selects by the same
// inputs the peer's selector reads, so a downstream cache keys a response the
// same way whichever front end served it.  `Accept` sets the format,
// `User-Agent` the viewport and the density, `Sec-CH-DPR` the density when
// the client sends it, `Save-Data` the Save-Data field.
inline constexpr char kPsImageSelectionVary[] =
    "Accept, Save-Data, User-Agent, Sec-CH-DPR";

// The capability mask with the `Sec-CH-DPR` client hint applied, the peer's
// rule mirrored.  The peer's classifier entry point (Classify below) takes no
// hint, while the peer's own front end reads one: an explicit hint of 1.5 or
// more sets the density field to 2x, any other parseable value ("1", "1.25")
// sets it to 1x, and an absent or unparseable value ("", "2x", "auto") leaves
// the classifier's User-Agent answer alone.  Applied by BOTH arms right after
// Classify, so the mask a request selects at is the mask its recording
// notifies at, and both match what the peer's front end would select.
uint32_t ApplySecChDprHint(uint32_t mask, StringPiece sec_ch_dpr);

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
// the same way: the internal parity harness's overlay built inside the peer's
// tree at the pin asserts it against the
// peer's own constant at compile time, so a pin that renumbers the value
// fails that build rather than letting this module send a mask the worker
// no longer reads as the sentinel.
inline constexpr uint32_t kOriginRefreshedSentinel = 0xFFFFFFFC;

// The site a cache call or a notification belongs to: the peer's
// PS_SITE_ID_CHARS lowercase hex characters (SiteIdFromServerFragment).
inline constexpr size_t kPsSiteIdChars = 64;

// The peer's PS_NOTIFY_FLAG_* bits (published at 1.12), mirrored.  This
// module sends exactly kPsNotifyFlagFromModule on every notification: it
// never vouches for a host, and the peer refuses a notification from the
// module that carries HTML, an agent request, no site, or the vouched bit.
inline constexpr uint32_t kPsNotifyFlagHostVouched = 0x1u;
inline constexpr uint32_t kPsNotifyFlagFromModule = 0x2u;
// Published at 1.14: "the module's HTML store wrote a present entry for this
// URL in this volume generation".  Legal only with kPsNotifyFlagFromModule,
// kPsNotifyFlagHostVouched and HTML; nothing in this module sends it unless
// the HTML serve set is bound.
inline constexpr uint32_t kPsNotifyFlagHtmlStored = 0x8u;
// Published at 1.16: "compress the stored Markdown copy of this page": the
// module served the plain copy because a coded copy was missing or not
// bound.  Legal only together with kPsNotifyFlagFromModule,
// kPsNotifyFlagHostVouched, kPsNotifyFlagHtmlStored and HTML, and sent only
// to a worker that advertises its Markdown codings (the 1.16 set).
inline constexpr uint32_t kPsNotifyFlagMarkdownCompress = 0x10u;

// Content types, as the peer numbers them.
enum PsContentType {
  kPsContentHtml = 0,
  kPsContentCss = 1,
  kPsContentJs = 2,
  kPsContentImage = 3,
  kPsContentOther = 4,
};

// Origin Cache-Control directives, as the peer's parser reports them
// (PS_CC_ORIGIN_*), every bit the peer publishes.  The record arm acts on
// no-store and header-present; the rest ride through to the entry unread,
// which is the point of storing the whole field, and are named so that a
// reader of a stored entry's flags never spells one as a literal.
inline constexpr uint16_t kPsCcOriginNoCache = 0x0001u;
inline constexpr uint16_t kPsCcOriginMustRevalidate = 0x0002u;
inline constexpr uint16_t kPsCcOriginNoStore = 0x0004u;
inline constexpr uint16_t kPsCcOriginPrivate = 0x0008u;
inline constexpr uint16_t kPsCcOriginPublic = 0x0010u;
inline constexpr uint16_t kPsCcOriginImmutable = 0x0020u;
inline constexpr uint16_t kPsCcOriginSMaxagePresent = 0x0040u;
inline constexpr uint16_t kPsCcOriginProxyRevalidate = 0x0080u;
inline constexpr uint16_t kPsCcOriginNoTransform = 0x0100u;
inline constexpr uint16_t kPsCcOriginPrivateQualified = 0x0400u;
inline constexpr uint16_t kPsCcOriginPrivateBare = 0x0800u;
inline constexpr uint16_t kPsCcOriginNoCacheQualified = 0x1000u;
inline constexpr uint16_t kPsCcOriginNoCacheBare = 0x2000u;
inline constexpr uint16_t kPsCcOriginSMaxageRepeated = 0x4000u;
// The bit the peer's parser stamps when the origin sent a `Cache-Control`
// line at all, whatever it said.  At serve time it means "the lifetime on
// this entry is the origin's statement, not silence", which is what
// suppresses the peer evaluator's content-type defaults (300 s for
// stylesheets and scripts, 3600 s for images): those apply only to an entry
// that recorded neither a lifetime nor a `Cache-Control` line.  The record
// arm stamps it itself in exactly one case, beside a lifetime derived from
// an `Expires`-only response -- see the gate in daemon_record_arm.cc.
inline constexpr uint16_t kPsCcOriginHeaderPresent = 0x0200u;

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
  // 1.13: the SHA-256 of the stored bytes, the entry's content binding, or
  // nullptr for an unbound entry.  A peer older than 1.13 snaps the declared
  // size down to the 1.12 shape and never reads it, which is the unbound
  // entry this module wrote before -- so the field costs nothing against an
  // older library.  Set only by the HTML store, and only when the HTML serve
  // set is bound (DaemonAbi::HasHtmlServeSet).
  const uint8_t* origin_html_hash;
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
  // 1.12.  Set only through BuildModuleNotifyParams (daemon_notify_params.h).
  const char* site;
  uint32_t notify_flags;
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
// here rather than a class of cache entry that is quietly wrong.  The guard
// names BOTH 64-bit data models: on LLP64 (Win64) the layouts coincide with
// LP64 because no field's type is model-dependent -- these structs use only
// fixed-width integers, int, size_t and pointers, never a long, a time_t or
// an enum -- so the same numbers must hold there, and a model-dependent type
// entering one of these structs on Win64 would fail exactly here.
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
static_assert(sizeof(PsWriteParams) == 88,
              "the cache-write parameter layout no longer matches the "
              "published one; a mismatched size is silently truncated by the "
              "peer, not rejected");
static_assert(offsetof(PsWriteParams, origin_html_hash) == 80,
              "the content binding is not where the peer reads it");
static_assert(sizeof(PsCacheControl) == 24,
              "the Cache-Control state layout no longer matches the published "
              "one");
static_assert(sizeof(PsNotifyParams) == 88,
              "the notification parameter layout no longer matches the "
              "published one; short of the full size the peer drops the "
              "site and the options context and processes the notification "
              "unsited, under the default one");
// The size alone does not catch two fields trading places.
static_assert(offsetof(PsNotifyParams, option_signature) == 64,
              "the notification parameter layout no longer matches the "
              "published one");
static_assert(offsetof(PsNotifyParams, site) == 72,
              "the notification's site is not where the peer reads it");
static_assert(offsetof(PsNotifyParams, notify_flags) == 80,
              "the notification's flags are not where the peer reads them");
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

// ---------------------------------------------------------------------------
// The HTML serve set: C API 1.13 (site-scoped sidecar, removal, purge epoch)
// and 1.14 (serve verdict, store parameters, stored facts, Markdown entity
// tag, weak If-None-Match, the worker's HTML mode).
//
// BOUND OPTIONALLY AND AS ONE SET, and the floor does not move.  A library
// below 1.14, or one that lacks any entry point of the set, still loads and
// still records and serves exactly as before; the HTML cache then stays
// report-only.  That keeps every 1.12 deployment working while the module
// and the optimizer are upgraded in either order.  A partial set is treated
// as no set: serving Markdown needs every piece (the verdict, the facts, the
// sidecar pairing and the purge fence), and a cache that could store but not
// fence a purge is the one combination that serves wrong bytes.
//
// The values are hand-copied, like every mirror in this file, and checked
// against the optimizer's own header at the test pin by
// tools/ci/check_daemon_abi_mirrors.py.
// ---------------------------------------------------------------------------

// The minor that publishes the whole set.
inline constexpr int kHtmlServeSetMinor = 14;

// The 1.15 serve set: serving cached HTML (the serve scope, agent class,
// freshness cap, fresh-only and the policy stamp in the request; the entry
// record and its stamp in the facts; header block format 3), and the request
// policy (the request guard, cookie classification, the ignore-list guard,
// the policy stamp and the unsafe-method predicate).
//
// BOUND OPTIONALLY, AS ONE SET, AND ONLY ON TOP OF THE 1.14 SET.  A library
// below 1.15 reads only the 1.14 prefix of a larger request and applies the
// earlier cookie rule, so this module never sends it the serve-cached-html
// mode and never relies on the credentialed policy, the entry check or the
// request guard there: without the set the HTML cache runs serve-variants with
// the 1.14 rules, and every struct of the serve set goes out at its 1.14
// size (see the size constants below).
inline constexpr int kHtmlServe15SetMinor = 15;

// The 1.16 set: compressed Markdown copies (the coding choice, the facts call
// that reads one coded copy, the coded entity tag and the worker's published
// codings), with the request's markdown_encoding word and the verdict's
// markdown_encoding field.
//
// BOUND OPTIONALLY, AS ONE SET, AND ONLY ON TOP OF THE 1.15 SET.  Without it
// the Markdown path serves the plain copy exactly as before: no coded read,
// no Content-Encoding, the 1.15 verdict size, and neither of the two 1.16
// request bits.
inline constexpr int kHtmlServe16SetMinor = 16;

// Sentinel-shaped alternate ids the HTML cache reads by exact id.  The
// content-hash record (0x3C) and the Markdown copy (0x7C) are the
// optimizer's; the withheld marker (0xCC, published at 1.13) is its record
// that a copy was permanently refused for one stored page.  None is ever
// returned by a selection read.
inline constexpr uint8_t kPsSentinelContentHash = 0x3C;
inline constexpr uint8_t kPsSentinelAgentMarkdown = 0x7C;
inline constexpr uint8_t kPsSentinelMarkdownWithheld = 0xCC;
// Published at 1.16: the brotli and the gzip copy of the Markdown copy, read
// by exact id and handed to HtmlStoredFactsFromReadsCoded, which alone says
// whether one may be sent.  Never written by this module.
inline constexpr uint8_t kPsSentinelMarkdownBr = 0xDC;
inline constexpr uint8_t kPsSentinelMarkdownGzip = 0xEC;

// Stored-entry flags published at 1.13.  CLASSIFIED: the writer ran the
// response through the headers-sidecar gate; with SIDECAR_REQUIRED a
// non-empty sidecar was stored and a serve must replay it or fall through.
inline constexpr uint8_t kPsFlagHeadersClassified = 0x10;
inline constexpr uint8_t kPsFlagHeadersSidecarRequired = 0x20;
// Published at 1.15, set by ps_html_store_write_params only: an HTML original
// recorded under the credentialed policy (STORE_V2), and recorded from an
// anonymous request (ANONYMOUS_RECORDED, which counts on an HTML original only
// together with STORE_V2).
inline constexpr uint8_t kPsFlagAnonymousRecorded = 0x40;
inline constexpr uint8_t kPsFlagHtmlStoreV2 = 0x80;

// What HeaderBlockPairing answers.
inline constexpr int kPsSidecarUnpaired = 0;
inline constexpr int kPsSidecarPaired = 1;

// The HTML cache mode both classifiers take (ps_html_cache_mode_t).
inline constexpr int kPsHtmlCacheOff = 0;
inline constexpr int kPsHtmlCacheReportOnly = 1;
inline constexpr int kPsHtmlCacheServeVariants = 2;
inline constexpr int kPsHtmlCacheServeCachedHtml = 3;

// ps_html_store_classify's actions.  Only STORE and STORE_NO_NOTIFY store.
inline constexpr int kPsHtmlStoreActionRefuse = 0;
inline constexpr int kPsHtmlStoreActionStore = 1;
inline constexpr int kPsHtmlStoreActionStoreNoNotify = 2;
inline constexpr int kPsHtmlStoreActionWouldStore = 3;
inline constexpr int kPsHtmlStoreActionWouldStoreNoNotify = 4;

// ps_html_store_classify: the explicit-public policy bit of `request_flags`
// (1.15), and the out-flag saying the request was anonymous (1.15), which
// ps_html_store_write_params records on the entry.
inline constexpr uint32_t kPsHtmlStoreRequestExplicitPublic = 16u;
inline constexpr uint32_t kPsHtmlStoreFlagRequestAnonymous = 64u;

// ps_html_serve_classify's actions.  VALIDATE is returned only in the
// serve-cached-html mode; a caller with no validation step falls through.
inline constexpr int kPsHtmlServeActionFallThrough = 0;
inline constexpr int kPsHtmlServeActionServeHtml = 1;
inline constexpr int kPsHtmlServeActionServeMarkdown = 2;
inline constexpr int kPsHtmlServeActionValidate = 3;

// PsHtmlServeRequest::request_flags.
inline constexpr uint32_t kPsHtmlServeRequestHasAuthorization = 1u;
inline constexpr uint32_t kPsHtmlServeRequestAuthenticatedUser = 2u;
inline constexpr uint32_t kPsHtmlServeRequestHasCookie = 4u;
inline constexpr uint32_t kPsHtmlServeRequestHasRange = 8u;
inline constexpr uint32_t kPsHtmlServeRequestHasIfRange = 16u;
inline constexpr uint32_t kPsHtmlServeRequestHasOrigin = 32u;
// 1.15, read only from a request of the 1.15 size.
inline constexpr uint32_t kPsHtmlServeRequestFreshOnly = 64u;
inline constexpr uint32_t kPsHtmlServeRequestHtmlFallback = 128u;
inline constexpr uint32_t kPsHtmlServeRequestConditional = 256u;
inline constexpr uint32_t kPsHtmlServeRequestExplicitPublic = 512u;
// 1.16, read only together with a verdict of the 1.16 size: identity is in
// the order MarkdownSelectEncoding returned (the plain copy may stand in for
// a coded one that is not bound), and that order was empty ("none
// acceptable": the Markdown path falls through).
inline constexpr uint32_t kPsHtmlServeRequestIdentityOk = 1024u;
inline constexpr uint32_t kPsHtmlServeRequestEncodingNone = 2048u;

// The codings a stored Markdown copy exists in (1.16): the plain copy and
// its gzip and brotli copies.  PsHtmlServeRequest::markdown_encoding,
// PsHtmlServeVerdict::markdown_encoding, MarkdownSelectEncoding's order.
inline constexpr int kPsMarkdownEncodingIdentity = 0;
inline constexpr int kPsMarkdownEncodingGzip = 1;
inline constexpr int kPsMarkdownEncodingBr = 2;

// PsHtmlServeRequest::serve_scope (1.15); any other value reads as agents.
inline constexpr int kPsHtmlServeScopeAgents = 0;
inline constexpr int kPsHtmlServeScopeAll = 1;
// The highest PsHtmlServeRequest::agent_class; a larger one reads as 0.
inline constexpr int kPsHtmlAgentClassMax = 3;

// PsHtmlStoredFacts::flags.  PURGED is the caller's to set; nothing the
// library reads can know it.
inline constexpr uint32_t kPsHtmlFactsPresent = 0x1u;
inline constexpr uint32_t kPsHtmlFactsNeedsValidation = 0x2u;
inline constexpr uint32_t kPsHtmlFactsExplicitShared = 0x4u;
inline constexpr uint32_t kPsHtmlFactsHeadersNotReproducible = 0x8u;
inline constexpr uint32_t kPsHtmlFactsSidecarRequired = 0x10u;
inline constexpr uint32_t kPsHtmlFactsSidecarPresent = 0x20u;
inline constexpr uint32_t kPsHtmlFactsPurged = 0x40u;
inline constexpr uint32_t kPsHtmlFactsValidatedThisRequest = 0x80u;
inline constexpr uint32_t kPsHtmlFactsVariantPresent = 0x100u;
inline constexpr uint32_t kPsHtmlFactsBindingLive = 0x200u;
inline constexpr uint32_t kPsHtmlFactsNoTransform = 0x400u;
inline constexpr uint32_t kPsHtmlFactsWithheld = 0x800u;
// 1.15: the entry carries kPsFlagHtmlStoreV2 / kPsFlagAnonymousRecorded.
inline constexpr uint32_t kPsHtmlFactsStoreV2 = 0x1000u;
inline constexpr uint32_t kPsHtmlFactsAnonymousRecorded = 0x2000u;
// 1.16, set by HtmlStoredFactsFromReadsCoded only, for the one coded read it
// was given: that copy is bound to the page and the plain copy (it may be
// sent), or it is the optimizer's record that this coding of the current
// plain copy is not smaller (none will be written, so none is asked for).
inline constexpr uint32_t kPsHtmlFactsGzipCopyBound = 0x4000u;
inline constexpr uint32_t kPsHtmlFactsBrCopyBound = 0x8000u;
inline constexpr uint32_t kPsHtmlFactsGzipNotSmaller = 0x10000u;
inline constexpr uint32_t kPsHtmlFactsBrNotSmaller = 0x20000u;

// PsHtmlStoredFacts::withheld_reason, with kPsHtmlFactsWithheld.
inline constexpr int kPsHtmlWithheldBelowFloor = 1;
inline constexpr int kPsHtmlWithheldTooLarge = 2;
inline constexpr int kPsHtmlWithheldEncoded = 3;
inline constexpr int kPsHtmlWithheldHashMismatch = 4;
inline constexpr int kPsHtmlWithheldExtractFailed = 5;

// The length of the binding and Markdown content hashes.
inline constexpr size_t kPsHtmlHashBytes = 32;

// Characters of a Markdown entity tag, W/"psm-" plus 16 hex digits and the
// closing quote, not counting the NUL.
inline constexpr size_t kPsHtmlMarkdownEtagChars = 24;

// The worker's HTML mode as ReadSharedConfigHtmlMode reports it.  Only the
// two MARKDOWN values mean the worker derives a copy of a page this module
// stored.
inline constexpr int kPsHtmlModeOff = 0;
inline constexpr int kPsHtmlModeMarkdownStatic = 1;
inline constexpr int kPsHtmlModeMarkdownRendered = 2;
inline constexpr int kPsHtmlModeFull = 3;

// PsSidecarWriteParams::write_flags (1.15): store header block format 3,
// which carries the policy stamp and the Cache-Control and Expires lines.
inline constexpr uint32_t kPsSidecarWritePolicyBlock = 1u;

// What ps_html_request_guard reports (1.15).
inline constexpr int kPsHtmlGuardNone = 0;
inline constexpr int kPsHtmlGuardRequestHeader = 1;
inline constexpr int kPsHtmlGuardPath = 2;

// What ps_html_classify_cookies reports (1.15), and the built-in ignore
// list's version, an input to the policy stamp.
inline constexpr int kPsHtmlCookiesNone = 0;
inline constexpr int kPsHtmlCookiesIgnorableOnly = 1;
inline constexpr int kPsHtmlCookiesBlocking = 2;
inline constexpr int kPsHtmlCookieListVersion = 1;

// The operator's credentialed policy (ps_html_policy_stamp, 1.15).
inline constexpr int kPsHtmlCredentialedAnonymousOnly = 0;
inline constexpr int kPsHtmlCredentialedExplicitPublic = 1;

// Layout-compatible mirrors of the set's parameter structs, under the same
// rules as the ones above: copied field for field, never tidied, and
// struct_size stamped by this module (the peer has no initializer for them).
// A request or facts struct smaller than its 1.14 size is a malformed call to
// the peer; the verdict is written only as far as its struct_size reaches.
//
// The 1.15 fields are appended.  A caller stamps struct_size with
// HtmlServeRequestSize, HtmlStoredFactsSize or SidecarWriteParamsSize (after
// DaemonAbi below): the full size only for a library that binds the 1.15
// set, and the 1.14 size (1.13 for the sidecar parameters) for every other,
// which then reads none of the appended fields.
struct PsSidecarWriteParams {
  size_t struct_size;
  const char* url;
  const char* hostname;
  const char* scheme;
  const char* site;
  // The stamp of the durable original this block belongs to, or nullptr for
  // an unpaired block.  Never 32 zero bytes: the peer refuses them.
  const uint8_t* origin_html_hash;
  // 1.15, read only with kPsSidecarWritePolicyBlock: the policy stamp.
  uint64_t policy_stamp;
  // 1.15: kPsSidecarWrite* bits; the peer refuses an unknown one.
  uint32_t write_flags;
};

struct PsHtmlServeRequest {
  size_t struct_size;
  int mode;
  const char* method;
  const char* accept;
  const char* cache_control;
  const char* pragma;
  // The client's If-None-Match as it arrived, before this module removed its
  // own entity tags from the request to the origin.
  const char* if_none_match;
  uint32_t request_flags;
  uint32_t now_seconds;
  // 1.15.
  int serve_scope;               // kPsHtmlServeScope*
  int agent_class;               // 0 unknown, 1 to kPsHtmlAgentClassMax
  uint32_t html_max_lifetime_s;  // 0 = no cap; only shortens the lifetime
  // 1.16, in the word 1.15 reserved (zero): the kPsMarkdownEncoding* coding
  // asked for.  Both names are the same word, as in the peer's header.
  union {
    uint32_t markdown_encoding;
    uint32_t reserved;
  };
  uint64_t policy_stamp;  // 0 = none
};

struct PsHtmlStoredFacts {
  size_t struct_size;
  uint32_t flags;
  uint32_t stored_at;
  uint32_t age_at_store;
  uint32_t lifetime_s;
  uint32_t render_ttl_s;
  int withheld_reason;
  uint8_t binding_hash[kPsHtmlHashBytes];
  uint8_t markdown_hash[kPsHtmlHashBytes];
  // 1.15: the stamp the entry's header block carries; 0 = none.
  uint64_t entry_policy_stamp;
};

// The request policy's results (1.15), stamped by this module.
struct PsHtmlRequestGuard {
  size_t struct_size;
  int reason;        // kPsHtmlGuard*
  int header_index;  // the refused line in the caller's arrays, or -1
  int xfp;           // 1 for an X-Forwarded-Proto that is not the keyed scheme
};

// A byte range in one of the caller's Cookie lines.
struct PsHtmlCookieSpan {
  size_t line;
  size_t offset;
  size_t length;
};

struct PsHtmlCookieClass {
  size_t struct_size;
  int cookie_class;  // kPsHtmlCookies*
  int malformed;     // blocking because a pair did not parse: log "malformed"
  PsHtmlCookieSpan first_blocking;  // the first blocking cookie's NAME
  size_t ignorable_count;
};

// The sizes a library below 1.15 knows: what this module sends it.  Each is
// where the first 1.15 field starts, so it holds on any layout (48, 64 and
// 96 on a 64-bit build, asserted below).
inline constexpr size_t kPsSidecarWriteParamsSize13 =
    offsetof(PsSidecarWriteParams, policy_stamp);
inline constexpr size_t kPsHtmlServeRequestSize14 =
    offsetof(PsHtmlServeRequest, serve_scope);
inline constexpr size_t kPsHtmlStoredFactsSize14 =
    offsetof(PsHtmlStoredFacts, entry_policy_stamp);

struct PsHtmlServeVerdict {
  size_t struct_size;
  int action;
  int reason;
  int enforce;
  int wants_markdown;
  uint32_t age_s;
  int not_modified;
  char etag[kPsHtmlMarkdownEtagChars + 8];
  // 1.16: with the Markdown action, the kPsMarkdownEncoding* coding of the
  // copy to send; written only into a verdict of the 1.16 size.
  int markdown_encoding;
  int reserved_1_16;  // zero
};

// The verdict size a library below 1.16 knows: where the 1.16 field starts.
inline constexpr size_t kPsHtmlServeVerdictSize15 =
    offsetof(PsHtmlServeVerdict, markdown_encoding);

#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
static_assert(sizeof(PsSidecarWriteParams) == 64,
              "the sidecar write parameter layout no longer matches the "
              "published one");
static_assert(offsetof(PsSidecarWriteParams, origin_html_hash) == 40,
              "the sidecar's pairing hash is not where the peer reads it");
static_assert(kPsSidecarWriteParamsSize13 == 48,
              "the 1.15 sidecar fields must start where the 1.13 size ends");
static_assert(offsetof(PsSidecarWriteParams, write_flags) == 56,
              "the sidecar write parameter layout no longer matches the "
              "published one");
static_assert(sizeof(PsHtmlServeRequest) == 88,
              "the serve request layout no longer matches the published one; "
              "a request short of it is a malformed call to the peer");
static_assert(offsetof(PsHtmlServeRequest, if_none_match) == 48,
              "the serve request layout no longer matches the published one");
static_assert(offsetof(PsHtmlServeRequest, now_seconds) == 60,
              "the serve request layout no longer matches the published one");
static_assert(kPsHtmlServeRequestSize14 == 64,
              "the 1.15 request fields must start where the 1.14 size ends");
static_assert(offsetof(PsHtmlServeRequest, policy_stamp) == 80,
              "the serve request layout no longer matches the published one");
static_assert(sizeof(PsHtmlStoredFacts) == 104,
              "the stored-facts layout no longer matches the published one");
static_assert(offsetof(PsHtmlStoredFacts, binding_hash) == 32,
              "the stored-facts layout no longer matches the published one");
static_assert(kPsHtmlStoredFactsSize14 == 96,
              "the 1.15 facts field must start where the 1.14 size ends");
static_assert(sizeof(PsHtmlRequestGuard) == 24,
              "the request guard layout no longer matches the published one");
static_assert(sizeof(PsHtmlCookieSpan) == 24,
              "the cookie span layout no longer matches the published one");
static_assert(sizeof(PsHtmlCookieClass) == 48 &&
                  offsetof(PsHtmlCookieClass, first_blocking) == 16,
              "the cookie class layout no longer matches the published one");
static_assert(sizeof(PsHtmlServeVerdict) == 72,
              "the serve verdict layout no longer matches the published one");
static_assert(kPsHtmlServeVerdictSize15 == 64,
              "the 1.16 verdict field must start where the 1.15 size ends");
static_assert(offsetof(PsHtmlServeVerdict, reserved_1_16) == 68,
              "the serve verdict layout no longer matches the published one");
static_assert(offsetof(PsHtmlServeRequest, markdown_encoding) ==
                  offsetof(PsHtmlServeRequest, reserved),
              "markdown_encoding is the word 1.15 reserved");
static_assert(offsetof(PsHtmlServeVerdict, etag) == 32,
              "the serve verdict layout no longer matches the published one");
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

  // The HTML cacheability policy (PS_API 1.12), bound OPTIONALLY and as a
  // set: all three entry points or none.  False means a library older than
  // the policy, for which DaemonHtmlCache reports "unavailable"; nothing else
  // depends on them.  Not pure, so a stand-in for an older library says so by
  // saying nothing.
  virtual bool PublishesHtmlStorePolicy() const { return false; }

  // ps_html_store_classify: a pure function of its arguments.  Returns the
  // action, or a negative value for a malformed call or an unbound policy.
  virtual int HtmlStoreClassify(
      int /*mode*/, const char* /*method*/, const char* /*url_path*/,
      uint32_t /*request_flags*/, int /*status*/, const char* const* /*names*/,
      const char* const* /*values*/, size_t /*count*/, uint64_t /*body_bytes*/,
      int /*body_complete*/, const char* /*body_prefix*/,
      size_t /*body_prefix_len*/, uint64_t /*max_body_bytes*/,
      uint32_t /*now_seconds*/, int* /*out_reason*/,
      uint32_t* /*out_lifetime_s*/, uint32_t* /*out_flags*/,
      uint16_t* /*out_cc_flags*/, int* /*out_sidecar*/, char* /*detail_buf*/,
      size_t /*detail_capacity*/, size_t* /*out_detail_len*/) const {
    return -1;
  }

  // ps_html_decision_format: 0 on success, the library's negative code
  // otherwise, -1 for an unbound policy.
  virtual int HtmlDecisionFormat(const char* const* /*keys*/,
                                 const char* const* /*values*/,
                                 size_t /*count*/, char* /*buf*/,
                                 size_t /*capacity*/,
                                 size_t* /*out_len*/) const {
    return -1;
  }

  // ps_html_store_reason_name, or nullptr for an unknown reason or an unbound
  // policy.
  virtual const char* HtmlStoreReasonName(int /*reason*/) const {
    return nullptr;
  }

  // ---------------------------------------------------------------------
  // The HTML serve set (C API 1.13 + 1.14; see the constants above).
  //
  // True only when the library reports minor kHtmlServeSetMinor or later
  // AND exports every entry point of the set, which here includes the two
  // 1.2 readers the serve gate needs (ps_wants_agent_markdown,
  // ps_read_shared_config_agent_entitled).  When false, no entry point of
  // the set is bound -- not even the ones the library does export -- and
  // every method below answers its fail-closed value without reaching the
  // library: -1 for the int-returning policy calls, kPsErrInvalidArg for the
  // cache calls (never kPsErrNotFound, which a remover treats as success),
  // nullptr, 0 or "HTML preferred" elsewhere.  The HTML cache stays
  // report-only then; the caller logs HtmlServeSetUnavailableReason once.
  // Not pure, so a stand-in for an older library says so by saying nothing.
  virtual bool HasHtmlServeSet() const { return false; }

  // Why HasHtmlServeSet() is false, as one clause for an operator's log
  // ("the optimizer library reports C API 1.13; the HTML serve set needs
  // 1.14", or the first entry point it lacks).  Empty when the set is bound.
  virtual GoogleString HtmlServeSetUnavailableReason() const {
    return "the optimizer library binding does not provide the HTML serve "
           "set";
  }

  // ps_html_serve_classify: 0 with *out written up to out->struct_size, or
  // -1 for a malformed call (or an unbound set).
  virtual int HtmlServeClassify(const PsHtmlServeRequest* /*request*/,
                                const PsHtmlStoredFacts* /*facts*/,
                                PsHtmlServeVerdict* /*out*/) const {
    return -1;
  }

  // ps_html_serve_reason_name: a static string, or nullptr for a reason
  // outside the set (which a caller treats as a fall-through).
  virtual const char* HtmlServeReasonName(int /*reason*/) const {
    return nullptr;
  }

  // ps_html_store_write_params: fills `params` (initialized with
  // WriteParamsInit) for the durable-original write of one HTML page.  The
  // ONE place the insert time is computed: `capture_start_ms` floored to
  // seconds and moved back by the inbound Age lines as received.  0, or -1
  // when the call is malformed or the Age is refused -- and then nothing
  // may be stored.  origin_html_hash is the caller's to set.
  virtual int HtmlStoreWriteParams(
      int /*action*/, uint32_t /*lifetime_s*/, uint32_t /*store_flags*/,
      uint16_t /*cc_flags*/, int /*sidecar_stored*/,
      int64_t /*capture_start_ms*/, const char* const* /*age_lines*/,
      size_t /*age_count*/, uint64_t /*content_length*/,
      const char* /*origin_content_type*/, PsWriteParams* /*params*/) const {
    return -1;
  }

  // ps_html_stored_facts_from_reads, over three exact-id read results (each
  // nullptr when that read missed): the original (kPsSentinelOriginal), the
  // Markdown copy (kPsSentinelAgentMarkdown) and the withheld marker
  // (kPsSentinelMarkdownWithheld).  kPsHtmlFactsPurged is the caller's to
  // add.  0, or -1 for a malformed call.
  virtual int HtmlStoredFactsFromReads(const void* /*original*/,
                                       const void* /*markdown*/,
                                       const void* /*withheld_marker*/,
                                       int /*sidecar_present*/,
                                       PsHtmlStoredFacts* /*out*/) const {
    return -1;
  }

  // ps_html_markdown_etag: 0; -1 for a bad argument; -3 when `capacity` is
  // too small (*out_len is then the length needed).
  virtual int HtmlMarkdownEtag(const uint8_t* /*binding_hash*/,
                               const uint8_t* /*markdown_hash*/, char* /*buf*/,
                               size_t /*capacity*/, size_t* /*out_len*/) const {
    return -1;
  }

  // ps_if_none_match_matches_weak: 1 match, 0 no match, -1 bad etag.
  virtual int IfNoneMatchMatchesWeak(const char* /*if_none_match*/,
                                     const char* /*etag*/) const {
    return -1;
  }

  // ps_cache_write_headers_sidecar_site.  Write the sidecar BEFORE the
  // durable original it pairs with; see the peer's header.
  virtual int CacheWriteHeadersSidecarSite(
      void* /*cache*/, const PsSidecarWriteParams* /*params*/,
      const char* const* /*names*/, const char* const* /*values*/,
      size_t /*count*/, int* /*out_verdict*/, int* /*out_stored*/) const {
    return kPsErrInvalidArg;
  }

  // ps_headers_sidecar_read: kPsOk with *out_block set to a block the caller
  // frees with HeaderBlockFree; kPsErrNotFound when there is none (also for
  // a block in a format the library does not know).  *out_block is nullptr
  // on every other return.
  virtual int HeadersSidecarRead(void* /*cache*/, const char* /*url*/,
                                 const char* /*hostname*/,
                                 const char* /*scheme*/, const char* /*site*/,
                                 void** out_block) const {
    if (out_block != nullptr) *out_block = nullptr;
    return kPsErrInvalidArg;
  }

  // The ps_header_block_* accessors over a block from HeadersSidecarRead.
  virtual size_t HeaderBlockCount(const void* /*block*/) const { return 0; }
  virtual int HeaderBlockGet(const void* /*block*/, size_t /*index*/,
                             const char** /*out_name*/,
                             size_t* /*out_name_len*/,
                             const char** /*out_value*/,
                             size_t* /*out_value_len*/) const {
    return kPsErrInvalidArg;
  }
  virtual const uint8_t* HeaderBlockPairingHash(const void* /*block*/) const {
    return nullptr;
  }
  // kPsSidecarPaired, kPsSidecarUnpaired, or -1 for a null block.
  virtual int HeaderBlockPairing(const void* /*block*/,
                                 const uint8_t* /*entry_hash*/) const {
    return -1;
  }
  virtual void HeaderBlockFree(void* /*block*/) const {}

  // ps_cache_remove_site: the whole key in one site.  kPsErrNotFound means
  // nothing was stored, which a remover treats as success.
  virtual int CacheRemoveSite(void* /*cache*/, const char* /*url*/,
                              const char* /*hostname*/, const char* /*scheme*/,
                              const char* /*site*/) const {
    return kPsErrInvalidArg;
  }

  // The per-site purge epoch (a max-merge file beside the volume).  ANY
  // non-kPsOk read means every entry of the site counts as purged, and
  // *out_epoch_s is then left untouched.
  virtual int CacheWriteSitePurgeEpoch(void* /*cache*/, const char* /*site*/,
                                       uint64_t /*epoch_s*/) const {
    return kPsErrInvalidArg;
  }
  virtual int CacheReadSitePurgeEpoch(void* /*cache*/, const char* /*site*/,
                                      uint64_t* /*out_epoch_s*/) const {
    return kPsErrInvalidArg;
  }

  // The worker's published state, read from a file under the cache path:
  // call these on a timer, never per request.  ps_read_shared_config_html_mode
  // gives a kPsHtmlMode* value; ps_read_shared_config_agent_entitled gives 1
  // when agent-optimize is on and 0 when off.  Both -1 for a bad argument.
  virtual int ReadSharedConfigHtmlMode(const char* /*cache_path*/) const {
    return -1;
  }
  virtual int ReadSharedConfigAgentEntitled(const char* /*cache_path*/) const {
    return -1;
  }

  // ps_wants_agent_markdown: 1 when `accept` (every line joined with ", ")
  // ranks text/markdown at least as high as text/html, else 0.
  virtual int WantsAgentMarkdown(const char* /*accept*/) const { return 0; }

  // ---------------------------------------------------------------------
  // The 1.15 serve set (see kHtmlServe15SetMinor).
  //
  // True only when HasHtmlServeSet() is, the library reports minor
  // kHtmlServe15SetMinor or later, AND it exports every entry point below.
  // When false none of them is bound and each answers without reaching the
  // library: -1, or 0 for the invalidation predicate (which is the
  // permissive answer there; a caller must check HasHtmlServe15Set() before
  // relying on it).  Then, too, the binding never sends the
  // serve-cached-html mode (HtmlServeClassify and HtmlStoreClassify answer
  // -1 for it) and sends every struct of the serve set at its 1.14 size,
  // whatever size the caller stamped.
  virtual bool HasHtmlServe15Set() const { return false; }

  // Why HasHtmlServe15Set() is false, as one clause for an operator's log.
  // Empty when the set is bound.
  virtual GoogleString HtmlServe15SetUnavailableReason() const {
    return "the optimizer library binding does not provide C API 1.15";
  }

  // ps_html_request_guard: 0 with *out written up to out->struct_size (the
  // caller stamps it), -1 for a malformed call or an unbound set.
  virtual int HtmlRequestGuard(const char* /*raw_path*/,
                               const char* const* /*names*/,
                               const char* const* /*values*/, size_t /*count*/,
                               const char* /*keyed_scheme*/,
                               PsHtmlRequestGuard* /*out*/) const {
    return -1;
  }

  // ps_html_classify_cookies: 0 with *out written, -1 otherwise.
  virtual int HtmlClassifyCookies(const char* const* /*lines*/,
                                  size_t /*line_count*/, int /*ignore_builtin*/,
                                  const char* const* /*additions*/,
                                  size_t /*addition_count*/,
                                  PsHtmlCookieSpan* /*ignorable_values*/,
                                  size_t /*ignorable_capacity*/,
                                  PsHtmlCookieClass* /*out*/) const {
    return -1;
  }

  // ps_html_ignore_cookie_guard: 1 accepted, 0 refused, -1 for a NULL
  // addition or an unbound set.
  virtual int HtmlIgnoreCookieGuard(const char* /*addition*/,
                                    int* /*out_warning*/) const {
    return -1;
  }

  // ps_html_policy_stamp: 0 with *out set, -1 otherwise.
  virtual int HtmlPolicyStamp(int /*credentialed*/, int /*ignore_builtin*/,
                              uint32_t /*list_version*/,
                              const char* const* /*additions*/,
                              size_t /*addition_count*/,
                              uint64_t* /*out*/) const {
    return -1;
  }

  // ps_html_unsafe_method_invalidates: 1 when the response invalidates the
  // stored page, else 0.  An unbound set also answers 0, which here means
  // "does not invalidate", NOT a fail-closed answer: check
  // HasHtmlServe15Set() first.
  virtual int HtmlUnsafeMethodInvalidates(const char* /*method*/,
                                          int /*status*/) const {
    return 0;
  }

  // ps_header_block_policy_stamp: 1 with *out set, 0 with *out = 0 for an
  // earlier format, -1 for a NULL argument or an unbound set.
  virtual int HeaderBlockPolicyStamp(const void* /*block*/,
                                     uint64_t* /*out*/) const {
    return -1;
  }

  // ps_html_stored_facts_from_reads_ex: as HtmlStoredFactsFromReads, with
  // entry_policy_stamp read from `block` (nullptr when there was none).
  virtual int HtmlStoredFactsFromReadsEx(const void* /*original*/,
                                         const void* /*markdown*/,
                                         const void* /*withheld_marker*/,
                                         int /*sidecar_present*/,
                                         const void* /*block*/,
                                         PsHtmlStoredFacts* /*out*/) const {
    return -1;
  }

  // ---------------------------------------------------------------------
  // The 1.16 set (see kHtmlServe16SetMinor).
  //
  // True only when HasHtmlServe15Set() is, the library reports minor
  // kHtmlServe16SetMinor or later, AND it exports every entry point below.
  // When false none of them is bound and each answers -1 without reaching
  // the library; HtmlServeClassify then sends the 1.15 verdict size, a zero
  // markdown_encoding and neither 1.16 request bit, whatever the caller set.
  virtual bool HasHtmlServe16Set() const { return false; }

  // Why HasHtmlServe16Set() is false, as one clause for an operator's log.
  // Empty when the set is bound.
  virtual GoogleString HtmlServe16SetUnavailableReason() const {
    return "the optimizer library binding does not provide C API 1.16";
  }

  // ps_markdown_select_encoding: the number of kPsMarkdownEncoding* values
  // written to out_order (room for 3), most preferred first; 0 for "none
  // acceptable"; -1 for a malformed call or an unbound set.  `lines` are the
  // request's Accept-Encoding lines as received, `count` of them (0: absent).
  virtual int MarkdownSelectEncoding(const char* const* /*lines*/,
                                     size_t /*count*/,
                                     int* /*out_order*/) const {
    return -1;
  }

  // ps_html_stored_facts_from_reads_coded: as HtmlStoredFactsFromReadsEx,
  // with the exact-id read of one coded copy (`coded`, null when it missed)
  // of `coding` (gzip or br), which sets that coding's COPY_BOUND or
  // NOT_SMALLER fact.  -1 for a malformed call or an unbound set.
  virtual int HtmlStoredFactsFromReadsCoded(
      const void* /*original*/, const void* /*markdown*/,
      const void* /*withheld_marker*/, int /*sidecar_present*/,
      const void* /*block*/, const void* /*coded*/, int /*coding*/,
      PsHtmlStoredFacts* /*out*/) const {
    return -1;
  }

  // ps_html_markdown_etag_coded: the copy's tag in one coding (identity is
  // HtmlMarkdownEtag's), as HtmlMarkdownEtag answers; -1 for an unbound set.
  virtual int HtmlMarkdownEtagCoded(const uint8_t* /*binding_hash*/,
                                    const uint8_t* /*markdown_hash*/,
                                    int /*coding*/, char* /*buf*/,
                                    size_t /*capacity*/,
                                    size_t* /*out_len*/) const {
    return -1;
  }

  // ps_read_shared_config_markdown_codings: a bit set of the codings the
  // worker writes coded copies in (1 << kPsMarkdownEncodingGzip,
  // 1 << kPsMarkdownEncodingBr), 0 for neither, -1 for a worker that
  // publishes none (it writes no coded copies and refuses
  // kPsNotifyFlagMarkdownCompress) or an unbound set.  Reads a file: on a
  // timer, never per request.
  virtual int ReadSharedConfigMarkdownCodings(
      const char* /*cache_path*/) const {
    return -1;
  }

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

  // Whether the library EXPORTS the reporter at all.  That is a different
  // question from LastErrorMessage(): a library can export the entry point
  // and have no pending error to report (nullptr either way then), so a
  // call cannot tell "not exported" from "nothing to say".  Defaults false
  // for implementations that have no symbol table to ask.
  virtual bool PublishesLastErrorMessage() const { return false; }

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

  // Opens a streaming write of the DURABLE ORIGINAL for `url`, in the
  // keyspace of `site` (ps_cache_write_original_site).  The peer refuses any
  // alternate id but the originals class, which is what makes this the only
  // writer this module can be.  The callers never pass anything but a site
  // id (IsSiteId); the unsited entry point is not bound.
  virtual int CacheWriteOriginalSite(void* cache, const char* url,
                                     const char* hostname, const char* scheme,
                                     const char* site,
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
  // Bound unconditionally beside the record arm's: the two sited reads are
  // published at 1.12 with the sited writer, and everything else here at or
  // before 1.2, which means a daemon this module will record into always
  // exports all of it.  See the floor discussion above for why that matters.
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
  //
  // In the keyspace of `site` (ps_cache_read_best_site); see
  // CacheWriteOriginalSite.
  virtual int CacheReadBestSite(void* cache, const char* url,
                                const char* hostname, const char* scheme,
                                const char* site, uint32_t mask,
                                void** out_result) const = 0;

  // Reads ONE stored entry by its exact alternate id.  This is how the
  // durable-originals class is read back, and it is the only read that can
  // reach it.
  // In the keyspace of `site` (ps_cache_read_alternate_site).
  virtual int CacheReadAlternateSite(void* cache, const char* url,
                                     const char* hostname, const char* scheme,
                                     const char* site, uint8_t alternate_id,
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

  // The size of the origin bytes encoded the way this entry is served (the
  // origin length for an identity entry; the gzip or brotli size of the origin
  // for a coded copy).  Published at 1.12, so an older daemon lacks it: this
  // then returns false and leaves *out alone, as it does when the entry does
  // not carry the value.  The default implementation is that older daemon.
  virtual bool ReadEncodedOriginLength(const void* /*result*/,
                                       uint64_t* /*out*/) const {
    return false;
  }

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

  // ONE serve HIT as above, attributed to the host the response was served
  // for (`host`: the request's host; the peer ignores a port and case).
  // Published at 1.11, after every floor this module has had, so an older
  // daemon lacks it: this then records nothing and returns false, and the
  // caller records the hit with ServeStatsRecordHit instead.  The default
  // implementation is that older daemon.
  virtual bool ServeStatsRecordHitForHost(void* /*handle*/,
                                          int /*content_type*/,
                                          uint64_t /*original_bytes*/,
                                          uint64_t /*optimized_bytes*/,
                                          uint32_t /*mask*/,
                                          StringPiece /*host*/) const {
    return false;
  }

  // ONE serve HIT as ServeStatsRecordHitForHost records it, also accounted in
  // the split of the saving into what the served encoding saved and what the
  // rewrite saved (`encoded_origin_bytes`: the value ReadEncodedOriginLength
  // returned).  Published at 1.12: an older daemon lacks it, this records
  // nothing and returns false, and the caller falls back to the forms above.
  virtual bool ServeStatsRecordHitSplit(void* /*handle*/, int /*content_type*/,
                                        uint64_t /*original_bytes*/,
                                        uint64_t /*encoded_origin_bytes*/,
                                        uint64_t /*optimized_bytes*/,
                                        uint32_t /*mask*/,
                                        StringPiece /*host*/) const {
    return false;
  }
  virtual void ServeStatsClose(void* handle) const = 0;

 protected:
  DaemonAbi();
};

// The struct_size a caller stamps on a serve-set struct for `abi`: the size
// this build knows when the library binds the 1.15 set, and otherwise the
// size a 1.14 library knows, so no field it cannot read is ever sent to it.
inline size_t HtmlServeRequestSize(const DaemonAbi& abi) {
  return abi.HasHtmlServe15Set() ? sizeof(PsHtmlServeRequest)
                                 : kPsHtmlServeRequestSize14;
}
inline size_t HtmlStoredFactsSize(const DaemonAbi& abi) {
  return abi.HasHtmlServe15Set() ? sizeof(PsHtmlStoredFacts)
                                 : kPsHtmlStoredFactsSize14;
}
// The verdict's struct_size: the full size only for a library that binds
// the 1.16 set, which is the only one that writes markdown_encoding.
inline size_t HtmlServeVerdictSize(const DaemonAbi& abi) {
  return abi.HasHtmlServe16Set() ? sizeof(PsHtmlServeVerdict)
                                 : kPsHtmlServeVerdictSize15;
}
inline size_t SidecarWriteParamsSize(const DaemonAbi& abi) {
  return abi.HasHtmlServe15Set() ? sizeof(PsSidecarWriteParams)
                                 : kPsSidecarWriteParamsSize13;
}

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
