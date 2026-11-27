// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CACHE_HEADERS_SIDECAR_H_
#define PAGESPEED_LIB_CACHE_HEADERS_SIDECAR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pagespeed {

// The response-header sidecar: the curated, VERBATIM block of
// request-independent response headers an optimized entry has to reproduce,
// and the admission gate that decides whether a response may be optimized in
// place at all.
//
// WHY A SEPARATE BLOCK.  An entry's metadata reproduces exactly the headers
// the cache itself needs — Cache-Control, Content-Type, Content-Length, ETag,
// Last-Modified, Expires.  Everything else the origin sent is lost, so a
// response carrying (say) a Content-Security-Policy could be stored and served
// only by dropping a security header, which is not an optimization, it is a
// bug.  The historical answer was to leave that whole class un-optimized.  The
// sidecar is the other answer: keep the bytes, reproduce them exactly, and
// leave un-optimized only what genuinely cannot be reproduced.
//
// TWO PROPERTIES MAKE IT SAFE, and both are enforced here rather than
// described:
//
//   1. REQUEST-INDEPENDENT ONLY.  A header whose value depends on the request
//      (Set-Cookie, an Access-Control-Allow-Origin reflected from the request
//      Origin, anything personalised) must never enter the block: one client's
//      value replayed to the next is a correctness failure, not a stale
//      header.  The allowlist below is therefore a CLOSED SET of field names
//      that are properties of the resource, and the reflected-ACAO case is
//      closed from the other side — a response that reflects the request
//      Origin declares `Vary: Origin`, and VaryUncacheable refuses to store
//      it at all.  This gate COMPOSES with that predicate; it does not
//      re-implement it.
//
//   2. A NONCE-BEARING CSP OR LINK IS NEVER STORED.  A nonce is per-response
//      by construction: replaying it defeats the policy it is part of (or
//      hands every client a stale nonce in a preload hint).  A CSP or Link
//      that carries one puts the URL in the never-optimized class, and it is
//      a HARD refusal — not "store the other headers and drop this one".
//
// SCOPE.  This is a STORE-side admission gate and a byte-faithful codec.  It
// decides nothing about serving, negotiation, or which headers a response is
// eventually emitted with.
//
// Field names and values are stored EXACTLY as the origin sent them, byte for
// byte, including case, internal whitespace, empty values and 8-bit bytes.
// The block is not normalised, not de-duplicated, and not re-ordered: a
// response that sent two Content-Security-Policy lines gets two entries, in
// order, because that is what the origin sent and what a faithful relay has
// to reproduce.

// One response header line, as received.  Views must outlive the call.
struct HeaderField {
  std::string_view name;
  std::string_view value;
};

// What the gate decided about a response.
enum class SidecarVerdict : uint8_t {
  // Every header this response carries is either reproduced by the entry
  // metadata, stamped fresh by the serving stack, or carried verbatim in the
  // sidecar.  The response may be optimized in place.
  kSwapEligible = 0,
  // At least one header cannot be reproduced.  The response is served plain —
  // correctly, just not optimized in place.  This is the fall-through class,
  // and it is a normal outcome, not an error.
  kFallThrough = 1,
  // A Content-Security-Policy or Link carrying a nonce.  Distinguished from
  // kFallThrough because it is not a gap to be closed later: the class is
  // never optimized, permanently, by construction.
  kNeverOptimized = 2,
};

// Result of classifying one response.
struct SidecarClassification {
  SidecarVerdict verdict = SidecarVerdict::kSwapEligible;

  // Why, when the verdict is not kSwapEligible: sorted, de-duplicated, in the
  // stable forms
  //   "unreconstructible-header:<lowercased name>"
  //   "vary-unstorable:<combined vary value>"
  //   "nonce-csp"
  //   "nonce-link"
  //   "malformed-field:<lowercased name>"     (a name that is not a token, or
  //                                            a value carrying CR/LF/NUL)
  //   "field-too-large:<lowercased name>"
  //   "sidecar-too-large"
  // Diagnostics: nothing branches on the strings.
  std::vector<std::string> reasons;

  // The versioned sidecar payload, ready to store.  Non-empty exactly when
  // the verdict is kSwapEligible AND the response carries at least one header
  // the sidecar has to hold: a response whose headers are all reproduced by
  // the entry metadata is swap-eligible with nothing to carry, and storing an
  // empty block for it would be an entry that means nothing.
  std::string payload;

  // Header lines carried in `payload`.
  size_t fields = 0;

  [[nodiscard]] bool swap_eligible() const {
    return verdict == SidecarVerdict::kSwapEligible;
  }
};

// Classify a complete origin response header set.
//
// `headers` MUST be the whole set as received, one entry per received line —
// repeated field names are separate entries, and joining them changes the
// answer (a per-line Vary check accepts a combination the joined list
// refuses).  The gate is fail-closed: a name it does not positively recognise
// is a fall-through reason, never a header quietly dropped.  A missed
// allowlist entry costs optimization COVERAGE; a missed fall-through reason
// drops a header the optimized entry cannot reproduce, which is a correctness
// bug — so the cheap error is the one this leans toward.
SidecarClassification ClassifyForHeadersSidecar(
    std::span<const HeaderField> headers);

// True when `csp_value` carries a nonce and therefore may never be stored.
//
// DELIBERATELY OVER-BROAD: any ASCII-case-insensitive occurrence of the
// substring `nonce-` counts, wherever it appears — as the `'nonce-...'`
// source expression it normally is, but equally inside a host source such as
// `https://nonce-cdn.example`.  The two are not reliably separable without a
// full CSP grammar, and the error directions are not symmetric: a false
// positive costs one URL its in-place optimization, a false negative replays
// a per-response nonce to every subsequent client and silently defeats the
// policy.  The gate takes the cheap error.
bool CspCarriesNonce(std::string_view csp_value);

// The same rule for a Link value: any ASCII-case-insensitive `nonce` outside
// the `<...>` targets counts, whatever the spelling of the parameter
// (`nonce=`, `nonce = `, `nonce*=`), so a URL that merely contains the word is
// not affected.  A per-response nonce in a preload hint must never be
// replayed.
bool LinkCarriesNonce(std::string_view link_value);

// Field names the sidecar carries, lowercased, as one closed set.  Exposed so
// a caller can state the boundary rather than infer it from behaviour; the
// gate is the only thing that applies it.
std::span<const std::string_view> HeadersSidecarAllowlist();

// Field names reproduced by the entry metadata itself, lowercased.  Not
// carried by the sidecar (they are already stored) and never a fall-through
// reason.
std::span<const std::string_view> HeadersReproducedByEntryMetadata();

// Field names the serving stack stamps per response rather than reproducing
// from a cache entry, lowercased.  Never carried and never a fall-through
// reason — without this set the gate would be vacuous, since every response
// carries Date and Server.
std::span<const std::string_view> HeadersStampedPerResponse();

// Ceiling on a stored sidecar payload, in bytes.
//
// 8 KiB.  The block is a curated subset of one response's headers, so this is
// far above anything a real origin produces (a large CSP is a few hundred
// bytes) and far below anything that would matter against the volume.  It is
// a ceiling rather than a truncation point for the reason the metadata format
// gives for its own caps: half a header block is worse than none, so a
// response whose block does not fit is simply not optimized in place, and
// counts as a fall-through.
inline constexpr size_t kMaxHeadersSidecarPayloadBytes = size_t{8} * 1024;

// Per-field ceilings, imposed by the payload encoding's length fields.
inline constexpr size_t kMaxHeadersSidecarNameBytes = 255;
inline constexpr size_t kMaxHeadersSidecarValueBytes = 65535;

// Parse a stored sidecar payload.
//
// Returns nullopt — the caller then behaves exactly as it would on a miss —
// for a payload that is empty, carries a format version this build does not
// know, is truncated, or has bytes left over.  A reader that finds a version
// byte it does not recognise STOPS rather than guessing; that is the entire
// reason the byte is spent.
//
// The returned views point INTO `payload`, which must outlive them.
//
// Format 1 only: a paired (format 2) block is nullopt here, because a
// fields-only reader cannot check that the block belongs to the entry it is
// about to serve.  Use ParseHeadersSidecarBlock to read either format.
std::optional<std::vector<HeaderField>> ParseHeadersSidecar(
    std::string_view payload);

// ---- Pairing (payload format 2) ----
//
// A sidecar written beside a durable original can carry the 32-byte hash of
// that original's bytes (the original's origin_html_hash stamp).  The stamp is
// what lets a reader tell the two apart from a block that belongs to some
// OTHER write of the key: a re-store writes the new block before the new
// original, so for a moment the block is the new one and the original the old
// one, and a block whose write succeeded beside an original whose write
// failed stays that way.  Both read as unpaired, never as a matching pair.
//
// Format 2 is format 1 with the hash inserted after the version byte:
//
//   [1B  format version]                    == kHeadersSidecarPairedFormatVersion
//   [32B pairing hash]                      never all-zero
//   [2B  field count, big-endian]           then the fields, as in format 1
//
// A paired block may hold zero fields: "this response carries nothing beyond
// the entry metadata" is then a recorded, provable fact rather than the
// absence of an entry.
inline constexpr size_t kHeadersSidecarPairingHashBytes = 32;

struct HeadersSidecarBlock {
  std::vector<HeaderField> fields;  // views into the parsed payload
  // Present only for a format-2 payload.
  std::optional<std::array<std::byte, kHeadersSidecarPairingHashBytes>>
      pairing_hash;
};

// Parse a stored sidecar payload of either format.  Same miss rules as
// ParseHeadersSidecar, plus: a format-2 payload shorter than its hash, or
// carrying an all-zero hash, is not this class.
std::optional<HeadersSidecarBlock> ParseHeadersSidecarBlock(
    std::string_view payload);

// Re-encode a format-1 payload as format 2 bound to `hash`.  An empty
// `format1_payload` (a swap-eligible response with nothing to carry) becomes a
// paired block with zero fields.  The caller guarantees `hash` is not
// all-zero and that a non-empty `format1_payload` came from
// ClassifyForHeadersSidecar.
std::string PairHeadersSidecarPayload(
    std::string_view format1_payload,
    std::span<const std::byte, kHeadersSidecarPairingHashBytes> hash);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CACHE_HEADERS_SIDECAR_H_
