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

//
// The canonical option context: a byte-exact, order-independent rendering of
// the options a request actually resolved to, plus a signature over it.
//
// WHAT THIS IS FOR.  Option resolution stays here.  The global/vhost/
// directory/query merge is RewriteOptions' job and nothing about that moves.
// What this file adds is a way to state the RESULT of that merge as a value —
// a string of bytes that says "these options, exactly" — so that a component
// which does not and must not do option resolution can be handed the answer
// instead of the inputs.  It is a value, not a rule set: there is nothing in a
// canonical payload to merge, inherit from, or interpret as policy.
//
// WHY NOT RewriteOptions::signature().  The obvious move is to reuse the
// signature that already exists, and it does not work.  That signature is
// built to key the metadata cache within one running binary, and it is correct
// for that; it is wrong for this in four independent ways, each of which alone
// is disqualifying:
//
//   1. It opens with kOptionsVersion (rewrite_options.cc, ComputeSignatureLock-
//      Held), a number whose documented purpose is to be bumped whenever a
//      default changes, precisely so that every signature in the world moves
//      and the metadata cache flushes.  A value with that property cannot key
//      anything that is supposed to survive an upgrade.
//   2. It embeds the global cache-invalidation timestamp ("GTS:"), a wall-clock
//      value that changes on every cache flush.  The same configuration
//      signs differently before and after a flush.
//   3. The subclass tail differs per server flavour, so the same logical
//      configuration signs differently under Apache and under nginx.
//   4. Its encoding is not self-delimiting — it joins with '_' and ':' and
//      neither is escaped — so two different configurations can in principle
//      produce one string.
//
// There is a fifth reason that is about safety rather than stability, and it
// is the one that decides the shape of what follows: ApacheConfig's tail puts
// measurement_proxy_password_ into the signature in cleartext.  A string with
// that property must never leave the process.  This serialization therefore
// carries values ONLY for options whose property is marked safe_to_print, and
// replaces every other value with a hash of itself.  A redacted option still
// separates contexts — a different secret still yields a different signature —
// but the secret itself is not in the bytes.
//
// STABILITY CONTRACT.  Two properties, and they are what the tests pin:
//
//   Order- and format-invariance.  The payload is a sorted set of lines.
//   Setting the same options in a different order, or via a different code
//   path, produces byte-identical output and therefore an identical signature.
//
//   Cross-release stability.  The payload contains no version number, no build
//   metadata, no git hash, no timestamp, and no server-flavour tail.  A release
//   that adds options, renumbers the Filter enum, or bumps kOptionsVersion does
//   not move the signature of a configuration that was already expressible.
//   This matters because the signature separates cache contexts: a signature
//   that shifted on upgrade would silently strand every optimized variant on
//   the far side of the change.
//
// The one lever that DOES move every signature is the format version token
// that opens the payload.  That is deliberate — it is the same lever
// kOptionsVersion is in the metadata cache, held here explicitly rather than
// entangled with unrelated release mechanics, and spending it is a decision
// someone has to write down.
//
// WHAT IS AND IS NOT COVERED.  Covered: enabled filters, and every registered
// option that participates in signature computation and was explicitly set.
//
// THIS SIGNATURE COVERS STRICTLY LESS THAN ComputeSignatureLockHeld, and the
// consequence has to be stated as a precondition rather than as a caveat: TWO
// DIFFERENT EFFECTIVE CONFIGURATIONS CAN SHARE A SIGNATURE.  Specifically, the
// following participate in the metadata-cache signature and are absent here
// (see the tail of ComputeSignatureLockHeld in rewrite_options.cc):
//
//   javascript_library_identification    domain_lawyer_
//   allow_resources_ / AWIR             retain_comments_
//   lazyload_enabled_classes_           CCPI / BRRU wildcard groups
//   url_cache_invalidation_entries_     override_caching_wildcard_
//
// They are excluded because they are not option-shaped — they are wildcard
// groups, URL maps and invalidation records — and because every one of them is
// resolved and applied HERE, on this side.  That is the precondition, and it
// is binding on whoever consumes a payload: A CONSUMER MUST NOT ACT ON ANY OF
// THE ABOVE.  A consumer that started making decisions from domain mapping or
// from a wildcard group would be reading policy this signature does not
// separate, and two configurations that differ only there would silently share
// its results.  If a consumer ever needs one of them, it becomes part of the
// payload — and that is a format-version bump, not an addition.
//
// The purge set and the global invalidation timestamp are excluded for a
// different reason: they are invalidation state, not identity.  Including them
// would make the same configuration sign differently before and after a flush.
//
// SERVER-SCOPE OPTIONS ARE INCLUDED, AND THAT HAS A COST WORTH NAMING.  Options
// that can only be set per server (file cache path, log directory, memcached
// and redis endpoints, ports, and so on) are part of the payload like any
// other.  So two virtual hosts with identical REWRITING policy but different
// local infrastructure sign differently and do not share optimized output —
// and differing local infrastructure between virtual hosts is the ordinary
// case, not the exotic one.  That erodes exactly the sharing a single shared
// optimizer exists to provide.
//
// Excluding them by OptionScope was considered and REJECTED, on evidence
// rather than taste: server-scope options are not all infrastructure.
// kImageWebpTimeoutMs and kImageAvifTimeoutMs decide whether an image is
// converted at all (image_rewrite_filter.cc), kHonorCsp gates rewriting
// against the page's CSP (scan_filter.cc), kRewriteUncacheableResources
// decides whether uncacheable resources are rewritten at all
// (in_place_rewrite_context.cc), kUrlSigningKey changes the URLs emitted
// (rewrite_driver.cc), and kFlushHtml changes the HTML
// (rewrite_driver_filter_init.cc).  A scope gate would drop every one of
// those, so two configurations that produce visibly different bytes would
// share one cache namespace.
//
// That is the asymmetry the whole choice turns on.  Including too much costs
// CACHE WARMTH; including too little costs CORRECTNESS, silently, and in a way
// no later request can detect.  The expensive direction is the safe one.  A
// future narrowing is possible, but its precondition is an audit establishing
// that nothing outside directory and query scope can change served bytes — and
// narrowing is a format-version bump, so it is a cold cache on both sides.
//
// The set-gating deserves the same honesty.  Following the existing signature,
// a payload carries options that were EXPLICITLY SET, not options that differ
// from their default.  Two consequences: two installs with identical effective
// behaviour but different explicit settings get different signatures, which
// costs cache sharing and nothing else; and a release that changes a DEFAULT
// changes behaviour without moving the signature of a config that never set
// that option.  The second is the real one, and the format version token above
// is its remedy.

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_OPTION_CONTEXT_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_OPTION_CONTEXT_H_

#include <cstddef>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

namespace net_instaweb {

class RewriteOptions;

// The token that opens every canonical payload, and the whole of the payload
// when nothing is set.  Changing it re-keys every context everywhere; see the
// file comment.
extern const char kOptionContextFormatVersion[];

// Upper bound on a canonical payload.
//
// DERIVED, not chosen — and derived against the LARGEST options class that
// ships, not against the base one.  That distinction is most of the value of
// the number.  Every server flavour subclasses RewriteOptions and registers its
// own properties (SystemRewriteOptions adds ~51, ApacheConfig ~3 more), and
// those additions are precisely the long-named infrastructure options whose
// records are the largest in the table.  A bound derived from base
// RewriteOptions alone measures roughly three quarters of the real worst case
// and reads as comfortable while being wrong.
//
// Two tests hold it.  ApacheConfigOptionContextTest.WorstCasePayloadFits-
// TheBoundWithTheReserveUnspent builds the true worst case — every registered
// option explicitly set, every filter enabled, on the largest linked class —
// and asserts it fits with the reserve below still unspent.
// OptionContextTest.WorstCasePayloadFitsTheBoundWithTheReserveUnspent does the
// same against the base class, so a base-table explosion still trips on a leg
// that does not link the server flavours.
//
// THE BOUND IS SHARED with the component on the other side of this contract.
// Changing it is a change in two places at once, so the shared golden-vector
// file carries it on a `# bound:` line that each side parses and compares
// against its own constant — a one-sided edit fails a test rather than drifting
// into a runtime mismatch.
inline constexpr size_t kMaxOptionContextBytes = 16384;

// Headroom inside the bound, and what it is for: roughly a doubling of the
// registered option table without a format change or a bound change. Stated as
// a number so that spending it is visible.
inline constexpr size_t kOptionContextSizeReserve = 4096;

// Length of a rendered signature: SHA-256 as lowercase hex.
inline constexpr size_t kOptionContextSignatureChars = 64;

// Outcome of building a canonical payload.
//
// FAIL DIRECTION, stated once and depended on by callers: every non-kOk value
// means the caller has NO usable option context, and the only correct response
// is to proceed WITHOUT one — not to substitute the default context.  A default
// context is a different context; handing this request's work to it is exactly
// the cross-context reuse the signature exists to prevent.  A caller that
// cannot build a context must skip the optimization it would have requested and
// serve the response through.  That costs cache warmth for the request; the
// alternative costs correctness for every request that shares the URL.
enum class OptionContextStatus {
  kOk = 0,
  // The canonical payload exceeded kMaxOptionContextBytes.  *payload is
  // cleared: there is no truncated form, because half a payload signs as a
  // context that does not exist.
  kTooLarge = 1,
  // An option key or a filter id contained a byte the format reserves.  This
  // is a build-time defect in the option table rather than a runtime input
  // problem, and OptionContextTest.EveryRegisteredOptionHasASerializableKey
  // pins it, but it is reported rather than asserted so that a release cannot
  // turn it into a crash.
  kUnserializable = 2,
};

// Builds canonical option-context payloads and signs them.
//
// Every entry point is a pure read of `options`.  Call after the options are
// frozen (ComputeSignature() freezes them), which is where the resolved
// per-request values are final; nothing here takes a lock, so serializing an
// options object another thread is still mutating is the caller's bug.
class OptionContext {
 public:
  // Renders the canonical payload for `options` into *payload.
  //
  // On any non-kOk return *payload is cleared.
  static OptionContextStatus Serialize(const RewriteOptions& options,
                                       GoogleString* payload);

  // Lowercase-hex SHA-256 over `payload`.  Exactly
  // kOptionContextSignatureChars characters.
  static GoogleString Signature(StringPiece payload);

  // Serialize + Signature.  On any non-kOk return both outputs are cleared.
  static OptionContextStatus Compute(const RewriteOptions& options,
                                     GoogleString* payload,
                                     GoogleString* signature);

  // The signature of the empty context — the payload produced by options with
  // nothing set and no filter enabled, i.e. the format version token alone.
  //
  // This is the value a peer uses to mean "no option context was supplied",
  // and it is a constant of the FORMAT, not of this build: it must agree
  // byte-for-byte with the peer's idea of the same thing.  The golden vector
  // file carries it as its first entry for exactly that reason.
  static GoogleString DefaultSignature();

 private:
  OptionContext() = delete;
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_OPTION_CONTEXT_H_
