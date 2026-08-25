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

#ifndef PAGESPEED_SYSTEM_DAEMON_RECORD_ARM_H_
#define PAGESPEED_SYSTEM_DAEMON_RECORD_ARM_H_

#include <cstdint>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/daemon_abi.h"

namespace net_instaweb {

// ---------------------------------------------------------------------------
// The record gate: what this module may put in the optimizer daemon's shared
// cache for one origin response, and whether it may ask for that response to
// be optimized.
//
// The gate is a pure function of the response and the request.  It performs no
// I/O, so every rule below is testable without a cache, a socket or a server,
// and the arm that does the I/O has no policy in it at all.
//
// THE ONE THING THIS MODULE MAY WRITE is the durable ORIGINAL -- the bytes the
// origin sent, before anything touched them.  Optimized variants belong to the
// optimizer, and each entry class has a single writer.  A second writer inside
// a class is not a merge, it is two processes overwriting each other with
// different ideas of what the entry means.
// ---------------------------------------------------------------------------

// What the gate decided.  The three outcomes are deliberately not two: "store
// but do not ask for optimization" is a real, required state, and collapsing
// it into either neighbour loses a rule.
enum class DaemonRecordVerdict {
  // Store the original and ask the optimizer to work on it.
  kStoreAndNotify,

  // Store the original; do NOT ask for optimization.  The response is kept so
  // that a later request can be served it, and so that a later change of
  // circumstances does not need a fresh trip to the origin -- but nothing may
  // transform it.
  kStoreOnly,

  // Store nothing and ask for nothing.
  kRefuse,
};

// Why the gate decided what it decided.  Reported as a stable identifier
// rather than a sentence, because it is asserted on: tests name a rule, and a
// prose change must not silently move an assertion.  Never null.
namespace daemon_record_reason {
inline constexpr char kEligible[] = "eligible";
// The URL is one this module's own rewriting produced.  Recording it as an
// "original" would make the shared cache the origin of a derived artifact,
// and both engines would then optimize each other's output.
inline constexpr char kRewrittenUrlShape[] = "rewritten-url-shape";
// The response's Vary names an axis the shared cache's key cannot tell apart.
// The predicate applied is the PEER'S, not this module's -- see the gate.
inline constexpr char kVaryNotStorable[] = "vary-not-storable";
// The origin said no-store.
inline constexpr char kOriginNoStore[] = "origin-no-store";
// The origin said no-transform: store, never optimize.
inline constexpr char kOriginNoTransform[] = "origin-no-transform";
// The response declared more bytes than the durable-originals class holds.
inline constexpr char kOverContentCap[] = "over-content-cap";
// The response is an encoded form; what would be stored is not the original.
inline constexpr char kContentEncoded[] = "content-encoded";
// The URL is not in a form this module can be sure names the same cache entry
// the peer would compose for it.  See UrlIsKeySafe.
inline constexpr char kUrlNotKeySafe[] = "url-not-key-safe";
// This request's resolved configuration could not be stated as a value, so
// there is no context to name the work with.  Storing is still correct; asking
// for optimization is not, because the peer would file the result under the
// default context, which is a different configuration's namespace.
inline constexpr char kNoOptionContext[] = "no-option-context";
}  // namespace daemon_record_reason

// One origin response header, name and value, exactly as the origin sent it.
// Both pieces are borrowed for the duration of the call.
struct DaemonOriginHeader {
  StringPiece name;
  StringPiece value;
};

// Everything the gate reads, gathered by the caller from the port's own
// structures.  Every StringPiece is borrowed for the duration of the call.
//
// `vary` must be the response's Vary lines JOINED WITH ",".  They are one
// list, and offering them one at a time admits combinations the predicate as a
// whole refuses.
//
// `cache_control_lines` is likewise every Cache-Control response line, in
// order, because the peer's parser accumulates and a later lifetime overwrites
// an earlier one.
//
// The four request fields are what the peer's classifier derives the
// requesting client's capabilities from.  Absent headers are empty here and
// are passed to the peer as absent, not as "".
struct DaemonRecordInput {
  StringPiece url;
  StringPiece hostname;
  StringPiece scheme;

  StringPiece content_type;      // Content-Type field value.
  StringPiece content_encoding;  // Content-Encoding field value.
  StringPiece vary;
  std::vector<GoogleString> cache_control_lines;
  StringPiece etag;
  int64 last_modified_ms = -1;  // -1 when the response carried none.
  int64 age_seconds = 0;        // 0 when the response carried no Age.
  int64 content_length = -1;    // -1 when not declared.

  StringPiece accept;
  StringPiece user_agent;
  StringPiece save_data;
  StringPiece accept_encoding;

  // The canonical options context this request resolved to and its signature,
  // or both empty when the caller could not produce one.  Both or neither:
  // a payload with no signature has no name, and a signature with no payload
  // names something the peer never saw.
  StringPiece option_context;
  StringPiece option_signature;

  // EVERY response header the origin sent, verbatim and in order, including
  // the ones named individually above.  Read by exactly one rule -- the
  // reproducible-set predicate below -- and by nothing else.
  //
  // ALL OF THEM, deliberately, because the predicate is fail-closed: it
  // answers "is there anything here a later serve could not put back", and a
  // caller that pre-filtered would be answering that question itself, with a
  // list that drifts from the one the predicate keeps.  Repeated fields
  // appear once per line, which is what lets the `Vary` tokens be combined
  // the way RFC 9110 says they combine.
  //
  // An EMPTY vector means "the caller did not gather them", not "the origin
  // sent none": every real response carries at least a Content-Type, so the
  // predicate treats emptiness as the reproducible answer and the gate stamps
  // nothing.  A port that has not been taught to fill this in therefore keeps
  // its pre-1.8 behaviour rather than marking every entry it records.
  std::vector<DaemonOriginHeader> response_headers;
};

// The gate's answer, plus everything the arm needs to act on it.  `params` is
// meaningful only when the verdict is not kRefuse.
//
// `params.flags` carries the two bits this module sets, and NEITHER is a
// verdict: a response that earns either is stored and, if nothing else forbids
// it, notified exactly like any other.  Both record something only the writer
// can still see, because by the time anything reads the entry back the
// origin's own headers are gone.
//
//   kPsFlagOriginVariesAccept -- the peer's own predicate says the origin
//     negotiates on `Accept`.  Unset, the optimizer derives a variant family
//     from the single representation one arbitrary client's negotiation
//     produced.
//   kPsFlagOriginHeadersNotReproducible -- the origin sent a header outside
//     the set a later serve from this entry can put back.  Unset, the serve
//     arm has no way to know, and serves the entry with that header silently
//     dropped.
//
// See both constants in daemon_abi.h.
struct DaemonRecordDecision {
  DaemonRecordVerdict verdict = DaemonRecordVerdict::kRefuse;
  const char* reason = daemon_record_reason::kEligible;
  PsWriteParams params = {};
  uint32_t notify_mask = 0;
  int content_type = kPsContentOther;
};

// The durable-originals content cap, mirrored from the peer's published one.
//
// ADVISORY, and deliberately not a second refuse-to-start mirror: a
// disagreement here costs a skipped store, never a split cache.  It is applied
// on the WRITE side and while the body accumulates, rather than to a finished
// buffer, because the point is not to hold 16 MB in order to discover it was
// 16 MB.
inline constexpr int64 kDaemonOriginalContentCapBytes = 16 * 1024 * 1024;

// Evaluates the record gate.  `abi` supplies the peer's own predicates; it is
// borrowed and must outlive the call.
DaemonRecordDecision EvaluateDaemonRecordGate(const DaemonAbi& abi,
                                              const DaemonRecordInput& input,
                                              int64 now_seconds);

// True when EVERY header in `headers` is one a later serve from the stored
// entry can put back -- i.e. when nothing about this response would be
// silently dropped by serving it from cache.  False is what earns
// kPsFlagOriginHeadersNotReproducible.
//
// THE SET IS DERIVED FROM WHAT THIS MODULE'S SERVE ARM ACTUALLY RECONSTRUCTS,
// which is the whole point of it existing separately, and it is NARROWER than
// the peer's sidecar admission verdict.  Asked of the peer instead, a fixed
// ACAO or a static Content-Security-Policy comes back reproducible -- because
// the peer keeps a per-URL header sidecar and can replay them.  This module
// writes the durable original and nothing else; there is no sidecar for a URL
// it recorded and no later writer can reconstruct one, so those same headers
// are exactly what a serve from here would drop.  Reusing the peer's verdict
// would therefore admit the responses the flag exists to mark, which is the
// one wrong answer that looks right.  The two predicates are halves of nothing
// -- they answer different questions about different serving stacks.
//
// WHAT IS IN THE SET, and where each member comes from (daemon_serve_arm.cc's
// Compose is the source; nothing here is a guess about a future arm):
//
//   Content-Type      stored on the entry and emitted on every serve, and on
//                     ONE class of serve the emitted field is not the stored
//                     one: a format-converted variant carries the media type
//                     of the format that was SERVED.  That is fidelity on
//                     this axis rather than a loss of it -- the origin's
//                     field describes the origin's bytes, and a response
//                     whose declared media type does not describe its own
//                     payload fails in the consumer past every check the
//                     serving path makes.  The member stays in the set for
//                     that reason; see ServeMediaTypeForServedBytes.
//   Cache-Control     REBUILT, NOT REPLAYED, and the difference is worth
//                     stating rather than glossing: the peer rebuilds the
//                     header from three stored scalars -- the origin's
//                     max-age, its s-maxage and its directive flags -- so what
//                     survives is the LIFETIME and the directives that have a
//                     flag bit.  What does not survive is everything else the
//                     origin may have written on that line: `immutable`,
//                     `stale-while-revalidate`, `stale-if-error`,
//                     `must-revalidate`, `proxy-revalidate` and any extension
//                     token.  The field is in rule 6's set verbatim and stays
//                     there -- a rebuilt `Cache-Control` is a correct
//                     statement of the entry's lifetime, which is what the
//                     header is for -- but a deployment that depends on a
//                     directive outside that set does not get it back, and
//                     this predicate does not catch that.
//   Content-Length    the served bytes are the stored bytes.
//   ETag              a validator IS emitted on every serve.  It is the
//                     module's DERIVED weak tag rather than the origin's, and
//                     that is not a fidelity loss on this axis: the response
//                     carries a working validator and conditional requests
//                     over it are answered.  The origin's own tag is stored
//                     and feeds that derivation.
//   Last-Modified     stored as seconds and emitted on an ORIGINAL serve,
//                     which is the only serve whose bytes it describes.
//   Expires           in D6.4 rule 6's set verbatim, and kept there -- but on
//                     a NARROWER argument than the others, so the weakness is
//                     stated rather than buried.  The serve arm never composes
//                     an `Expires` line: freshness is expressed as
//                     `Cache-Control` built from the stored origin state.  An
//                     origin that sent BOTH loses the `Expires` line and keeps
//                     an equivalent lifetime; an origin that expressed its
//                     lifetime ONLY through `Expires` stores a zero max-age,
//                     because nothing on the record path parses `Expires` into
//                     the stored state (see daemon_ipro_recorder.cc: the
//                     stored lifetime is parsed from `Cache-Control` lines and
//                     from nothing else).  A zero-lifetime entry is refused as
//                     stale at serve time rather than served with a wrong
//                     freshness -- a fall-through, never a wrong header.  That
//                     last step is the peer evaluator's, MODELLED by the test
//                     stub and pinned by a unit test here; the peer's own
//                     arithmetic for a zero lifetime with no recorded
//                     `Cache-Control` is not observable from this tree.
//   Vary              CONDITIONALLY, on `Accept-Encoding` and `Accept`.
//                     Both are here for the same reason every other member is:
//                     the serve arm puts them back.  `Accept-Encoding` is an
//                     axis the selection deliberately does not vary over -- it
//                     chooses as though the client asked for uncompressed
//                     bytes.  `Accept` is RECONSTRUCTED: an origin that
//                     negotiates on it is recorded with that fact on the entry
//                     and the serve arm emits `Vary: Accept` back off that
//                     marker -- read from the DURABLE ORIGINAL by exact id,
//                     through the same per-request probe that answers rule 6,
//                     and never from the entry being served, which on an
//                     optimized serve is a variant carrying a clear flags
//                     byte.  That is clause (ii) of the `Vary` biconditional
//                     in daemon_serve_arm.cc, and it holds on an optimized
//                     serve as well as an original one.
//                     THE HONEST LIMIT, because it is not the same statement
//                     as the origin's: what is reproduced is the VARIES-ON-
//                     ACCEPT FACT for the recorded entry, not the origin's
//                     per-representation bodies.  This substrate answers a
//                     stable URL from one recorded original plus the variants
//                     the optimizer derived from it; it does not go back to an
//                     origin for a second representation.  The header a
//                     downstream cache needs in order not to serve one
//                     client's copy to everybody IS emitted, which is the
//                     fidelity question rule 6 asks.  The checked-in table
//                     carries the same reading (tools/parity/adapter_asserts/
//                     reconstructible.py, HANDLED_VARY) and declares the
//                     divergence from the peer's store-side gate there.
//
// AND ONE IGNORED CLASS: headers the serving stack stamps per response rather
// than reproducing from an entry -- `Date`, `Server`, `Age`, `Accept-Ranges`
// and the hop-by-hop names.  Without it the rule is vacuous in the strongest
// sense: the seam hands this predicate the SERVER'S FINALIZED headers, so
// every response carries a `Date` and an `Accept-Ranges`, every CDN-fronted
// one carries an `Age`, and everything would be marked for ever.  See
// kServeGeneratedHeaders for why each member is in that class.
//
// WHAT THIS PREDICATE CANNOT SEE, stated because a reader will otherwise
// assume it can: the list it is handed is the response as the SERVER finalized
// it, not as the origin sent it, and a header a site-wide server directive
// adds to every response is indistinguishable here from one the origin sent
// for this URL alone.  The serving layer re-applies such a directive to this
// substrate's own responses too, so that header is in fact reproduced and
// marking on it is a false positive.  The predicate marks anyway, because
// nothing at this seam can tell the two apart.  Erring this way costs a
// resource its in-place optimization; erring the other way drops a header the
// serve path cannot put back.
//
// EVERYTHING ELSE IS A FALL-THROUGH REASON, including near neighbours of names
// that are in the set: the set is a CLOSED list of exact field names, matched
// case-insensitively.  The error directions are not symmetric -- a missed
// member costs one resource its in-place optimization, a missed exclusion
// drops a header the serve path cannot put back -- so the predicate is
// deliberately biased toward the cheap error, exactly as the checked-in table
// is.
//
// THE TABLE IS THE OTHER HALF OF THIS DECLARATION.  reconstructible.py holds
// the same rule as data, with the sidecar names carried; running it with an
// EMPTY sidecar set is this predicate, and its self-test pins that
// correspondence row by row so the two cannot drift apart in silence.
bool OriginHeadersAreReproducible(
    const std::vector<DaemonOriginHeader>& headers);

// True for a URL this module's own rewriting produced.
//
// Substring, not suffix: the marker sits mid-name in a rewritten filename, and
// a suffix test would miss every real one.  Deliberately conservative --
// refusing a URL that merely looks rewritten costs one recording, while
// missing one is the failure the check exists for.
bool IsRewrittenUrlShape(StringPiece url);

// True when `url` is a form for which this module can be certain that the
// entry it writes is the entry the peer would look for.
//
// WHY THIS EXISTS, and it is a real restriction rather than caution.  The
// shared cache's key is composed from the URL in a CANONICAL form, and
// canonicalizing a URL into that form -- percent-escape folding, query
// parameter ordering and de-duplication, and the operator-configured
// strip/alias rules -- is the peer's, done before the key is composed.  It is
// not part of the peer's published interface, so this module cannot perform
// it and cannot ask for it to be performed.  The peer's own optimizer
// canonicalizes the URL it is NOTIFIED with, so a notification always lands
// on the right entry; a WRITE does not get that treatment, so a URL that
// canonicalizes to something else is stored where nothing will look.
//
// What is safe is the subset the canonical form leaves alone: a path with no
// query string, no percent escape and no fragment is already canonical under
// every configuration.  Recording is restricted to that subset, so this
// module never writes an entry at a key that may not be read.  The cost is
// that in-place recording does not cover query-bearing or escaped URLs.
bool UrlIsKeySafe(StringPiece url);

// Carries out a decision the gate already made: streams the original into the
// shared cache and, if the decision allows it, notifies the optimizer.
//
// Holds no policy.  It cannot store anything but the durable original,
// because the only write entry point it can reach is the one the peer refuses
// to write any other class through -- so "originals only" is a property of
// what this object CAN do, not of what it happens to do.
class DaemonRecordWriter {
 public:
  // Neither `abi` nor `cache` is owned; both must outlive this object.
  DaemonRecordWriter(const DaemonAbi* abi, void* cache);
  ~DaemonRecordWriter();

  DaemonRecordWriter(const DaemonRecordWriter&) = delete;
  DaemonRecordWriter& operator=(const DaemonRecordWriter&) = delete;

  // Opens the write.  Returns the peer's status; on anything but success no
  // handle is held and Append/Commit do nothing.
  int Begin(const DaemonRecordInput& input,
            const DaemonRecordDecision& decision);

  // Streams one chunk.  Stops at the first failure and remembers it: the peer
  // abandons the entry on the write that crosses the content cap, and every
  // later write on that handle is refused, so continuing to push bytes at it
  // only turns one honest error into many.
  int Append(const void* data, size_t length);

  // Commits.  Returns the peer's status.  Nothing partial is ever stored: a
  // commit that fails leaves no entry, which reads as "no original", which is
  // the correct answer.
  int Commit();

  // Drops the write without committing.  Safe to call after a failed Begin.
  void Abandon();

  bool open() const { return handle_ != nullptr; }
  int last_error() const { return last_error_; }

 private:
  const DaemonAbi* abi_;
  void* cache_;
  void* handle_ = nullptr;
  int last_error_ = kPsOk;
};

// Sends the notification a kStoreAndNotify decision earned.  Separate from the
// writer because it is a separate transport with a separate failure mode: the
// original is already stored and durable when this runs, and a notification
// that does not arrive costs this response its optimization and nothing else.
int NotifyDaemon(const DaemonAbi& abi, StringPiece socket_path,
                 const DaemonRecordInput& input,
                 const DaemonRecordDecision& decision);

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_DAEMON_RECORD_ARM_H_
