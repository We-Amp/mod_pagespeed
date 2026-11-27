// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_ALTERNATE_METADATA_H_
#define PAGESPEED_LIB_CLASSIFY_ALTERNATE_METADATA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "lib/classify/content_type.h"

namespace pagespeed {

// Metadata stored in each Cyclone alternate's header field.
//
// Serialized format (version 9, layout identical to version 8):
//   [1B version=9][4B full_mask LE][1B content_type][1B flags]
//   [2B origin_ct_len LE][origin_ct...]
//   [4B cache_inserted_at LE][4B origin_max_age LE]
//   [4B origin_s_maxage LE][2B origin_cc_flags LE]
//   [2B ssimulacra2_score_x100 LE][1B content_class]
//   [4B origin_last_modified LE][2B etag_len LE][etag_len bytes...]
//   [4B origin_content_length LE]
//   [32B origin_html_hash][32B render_source_hash]   (v7)
//   [8B origin_epoch LE]                             (v8)
//   [2B origin_cc_len LE][origin_cc_len bytes...]    (v8)
//
// Version 9 changes no byte of the layout.  Deserialize refuses every HTML
// entry below version 9, and a page's Markdown version the worker wrote below
// version 9 (IsRetired), so it reads as a MISS and is rebuilt.  CSS, images
// and everything else below version 9 stay readable.
//
// Total: kFixedPrefixSize(9) + ct_len + kV8FixedSuffixSize(101) + etag_len
//        + cc_len
//
// The format is APPEND-ONLY across versions: every version's layout is a
// strict byte prefix of the next one.  That is what makes an old blob
// readable by a new reader without a conversion pass, and it is a rule, not
// an observation — a field inserted anywhere but the end re-points every
// field after it for every entry already on disk.
//
// v3/v4/v5/v6/v7 data is accepted on read (missing fields default; the v7
// hashes read back all-zero, which the D8 gates treat as "binding absent";
// the v8 fields read back zero/empty, which their consumers treat as absent).
// A version this build does not know is NOT parsed: Deserialize returns
// nullopt and the read path turns that into a MISS, so a newer peer's entry
// is re-recorded rather than half-understood.
//
// The full_mask is the complete 32-bit CapabilityMask, not just the low
// byte used as AlternateId.  This allows the selector to examine all
// dimensions.
struct AlternateMetadata {
  static constexpr uint8_t kCurrentVersion = 9;
  static constexpr size_t kFixedPrefixSize = 9;   // version through ct_len
  static constexpr size_t kFixedSuffixSize = 17;  // v3/v4 trailing cc fields
  static constexpr size_t kFixedTotalSize = kFixedPrefixSize + kFixedSuffixSize;
  // v5 fixed suffix: v4's 17 + 4 (last_modified) + 2 (etag_len) = 23.
  static constexpr size_t kV5FixedSuffixSize = 23;
  // v6 fixed suffix: v5's 23 + 4 (origin_content_length) = 27.
  static constexpr size_t kV6FixedSuffixSize = 27;
  // v7 fixed suffix: v6's 27 + 32 (origin_html_hash) + 32 (render_source_hash).
  static constexpr size_t kV7FixedSuffixSize = kV6FixedSuffixSize + 64;
  // v8 fixed suffix: v7's 91 + 8 (origin_epoch) + 2 (origin_cc_len).  As with
  // etag_len, the length field is fixed suffix; the bytes it counts are not.
  static constexpr size_t kV8FixedSuffixSize = kV7FixedSuffixSize + 8 + 2;
  // Size of each content-binding hash (SHA-256).
  static constexpr size_t kHashSize = 32;
  static constexpr uint16_t kMaxOriginCtLen = 256;
  // Cap on the stored raw origin Cache-Control string (v8).  A header longer
  // than this AFTER stripping (CR/LF/NUL are removed first, so a long header
  // that strips to within the cap is kept) is stored as ABSENT rather than
  // truncated: the string exists to be relayed byte-faithfully, and half a
  // directive list is worse than none — the parsed
  // origin_cc_flags/origin_max_age fields remain the authoritative
  // machine-readable form either way.
  static constexpr uint16_t kMaxOriginCcLen = 256;

  // ---------------------------------------------------------------------
  // Prefix budget (v8).
  //
  // Every alternate carries this blob twice: once as the Cyclone header and
  // once as the content prefix.  PageSpeedCacheConfig::max_metadata_size is
  // the ceiling a write is checked against, and a blob over the ceiling is
  // REFUSED — no partial write, no truncated field, the entry is simply not
  // stored.  So the ceiling has to be derived from the format, not chosen.
  //
  // Three members are variable-length, and only two of them are bounded by
  // construction:
  //
  //   origin_content_type  <= kMaxOriginCtLen (256)   enforced both ways
  //   origin_cache_control <= kMaxOriginCcLen (256)   enforced both ways
  //                                                   (measured after
  //                                                    control-char stripping)
  //   origin_etag          <= 65535 (uint16 length)   NOT capped
  //
  // The ETag is deliberately left uncapped.  Capping it means either
  // truncating a validator — which produces a value that compares unequal to
  // itself and silently disables conditional revalidation — or dropping one
  // that fits today, which is a regression for entries that store fine now.
  // It is BUDGETED instead, at kEtagBudget, and an entry whose actual ETag
  // does not fit the remaining room is refused by the existing size check.
  // That is strictly more permissive after this raise than before it.
  //
  // Budget:  128 B covers every ETag shape in the field with margin —
  // origin-server hex/inode forms are ~20 B, S3 multipart ~40 B, weak
  // base64-SHA-256 forms ~50 B.
  static constexpr size_t kEtagBudget = 128;

  // Worst-case serialized size with every member at its budget:
  //
  //     kFixedPrefixSize        9
  //   + kMaxOriginCtLen       256
  //   + kV8FixedSuffixSize    101   (91 v7 + 8 origin_epoch + 2 cc_len)
  //   + kEtagBudget           128
  //   + kMaxOriginCcLen       256
  //   ---------------------------
  //                           750
  //
  // (The pre-v8 arithmetic was 9 + ct_len + 91 + etag_len, quoted as leaving
  // ~156 B under a 512 B ceiling — that figure assumed a zero-length ETag and
  // is superseded here.)
  static constexpr size_t kMaxSerializedSizeAtBudget =
      kFixedPrefixSize + kMaxOriginCtLen + kV8FixedSuffixSize + kEtagBudget +
      kMaxOriginCcLen;

  // Headroom over the worst case, and what it is FOR: promoting the ETag
  // budget from 128 to 256 — the same class the other two origin-supplied
  // strings already sit in — without touching a config default or the
  // format.  It is sized as exactly that one move (+128) rather than rounded
  // to a convenient power of two, so that spending it is a visible decision.
  static constexpr size_t kMetadataSizeReserve = 128;

  // The value PageSpeedCacheConfig::max_metadata_size defaults to.
  static constexpr size_t kRecommendedMaxMetadataSize =
      kMaxSerializedSizeAtBudget + kMetadataSizeReserve;

  // V4 sentinel values.
  static constexpr uint16_t kScoreNA = 0xFFFF;

  // The 16-bit score field has two meanings, keyed on the transfer encoding
  //  the variant's mask names (see the accessors below; nothing outside this
  // class reads the raw field):
  //   - identity-encoded entries (images, and every plain variant): the
  //     quality score, 0-10000 (x100) or kScoreNA;
  //   - gzip or brotli coded entries (the pre-compressed copies of text and
  //     SVG; they never carry a quality score): kScoreNA, or a code in
  //     [kEncodedRatioBase, kScoreNA - 1] carrying the size of the origin
  //     bytes under this variant's own transfer encoding, as a fraction of
  //     origin_content_length.
  // Every released reader clamps a value above 10000 (other than kScoreNA)
  // to kScoreNA when it parses, so an entry carrying a code is read as
  // "score not available" by them; the metadata version does not change.
  static constexpr uint16_t kMaxScoreX100 = 10000;
  static constexpr uint16_t kEncodedRatioBase = kMaxScoreX100 + 1;
  // Code = kEncodedRatioBase + round(ratio * kEncodedRatioScale).  The largest
  // code is kScoreNA - 1, so the largest representable ratio is 1.1107.
  static constexpr uint32_t kEncodedRatioScale = 50000;
  static constexpr uint8_t kContentClassUnknown = 4;

  // Flag bits for the flags byte.
  // kFlagNeedsRevalidation: set on HTML variants when external CSS
  // dependencies were not yet cached at processing time, and on CSS
  // variants when @import dependencies could not be resolved.  Nginx
  // re-notifies the worker on HIT so the variant can be re-processed
  // once dependencies become available.
  static constexpr uint8_t kFlagNeedsRevalidation = 0x01;
  // kFlagWorkerProcessed: set on all variants written by the worker
  // (as opposed to nginx-written originals).  Used by notification
  // deduplication to distinguish "original at default mask" from
  // "already-processed variant at default mask".
  static constexpr uint8_t kFlagWorkerProcessed = 0x02;
  // kFlagOriginVariesAccept (v8): the origin's response carried a `Vary`
  // that includes `Accept`.  Persisted so that a serving path can tell
  // "deliberately left alone" from "not optimized yet" without a second
  // read — the same durability `kCCOriginNoTransform` has, for the same
  // reason.
  //
  // LIVE.  Set by the store path on an original whose origin declared it
  // negotiates on `Accept`, and read by three consumers:
  //   * the notify decision — a marked entry is never handed to the
  //     optimizer, at store time or on a later variant miss, because
  //     deriving variants from one captured representation would layer a
  //     second negotiation on the origin's own;
  //   * `Vary` emission on every response built from the entry — on a hit
  //     the origin's headers are gone, so this is the only surviving record
  //     that the response must be keyed on `Accept` downstream;
  //   * the weak ETag, via the flags byte, which keeps a marked entry from
  //     validating against an unmarked one (they are served with different
  //     downstream keying and are not the same representation).
  //
  // An entry written before this bit was set reads back unmarked, which is
  // the correct legacy answer: it is re-marked the next time the URL is
  // recorded.
  static constexpr uint8_t kFlagOriginVariesAccept = 0x04;
  // kFlagOriginHeadersNotReproducible: at record time the origin response
  // carried at least one response header outside the set a later serve can
  // reproduce — the set made up of what this metadata itself carries, what
  // the serving stack stamps fresh per response, and the curated verbatim
  // block in the headers sidecar.  The sidecar's own admission verdict
  // answers exactly that question (`PS_SIDECAR_FALL_THROUGH` and
  // `PS_SIDECAR_NEVER_OPTIMIZED` are the two "not reproducible" answers),
  // which is what a writer classifies against.
  //
  // STAMPED BY THE WRITER, at record time, alongside the
  // kFlagOriginVariesAccept stamping obligation, and never synthesised by
  // the engine: on a hit the origin's headers are gone, so the bit on the
  // entry is the whole record of a question that can only be answered while
  // they are still in hand.
  //
  // ADVISORY TO SERVE PATHS, and to nothing else.  What a reader may
  // conclude from it is one thing only: serving this entry's bytes in place
  // of the origin response would drop headers the origin sent.  A serve path
  // that cannot reproduce them falls through to its plain path on the bit,
  // which is the difference between a visible fidelity gap and a silent one.
  //
  // What it does NOT do, stated because a flag byte invites the assumption:
  // it does not gate storage or admission (a marked response is stored
  // exactly as an unmarked one is), it is not consulted by variant selection
  // (which scores the capability bits of the AlternateId and never reads
  // this byte), it does not enter the notify/optimize refusal, and it does
  // not propagate into derived variants. That last one is a call-site
  // convention, not a structural property: the worker's shared write helpers
  // copy source metadata (WriteVariant inherits the flags byte it is
  // handed), and every current caller composes the byte it passes in, none
  // forwarding this bit.
  //
  // One consequence IS observable and is intended: the whole flags byte
  // feeds the weak hit ETag (src/nginx/etag_util.h), so a marked entry does
  // not validate against an otherwise-identical unmarked one.  They are not
  // served the same way, so they are not the same representation.
  //
  // An entry written before this bit existed reads back unmarked, the same
  // legacy answer kFlagOriginVariesAccept gives, and for the same reason.
  static constexpr uint8_t kFlagOriginHeadersNotReproducible = 0x08;
  // kFlagHeadersClassified: this entry was recorded by a build that ran the
  // headers-sidecar gate (ClassifyForHeadersSidecar) on the origin response.
  // Together with kFlagHeadersSidecarRequired it tells a serve path which of
  // three states an entry is in: unmarked (recorded before the gate existed,
  // so serve as before), classified only (headers were checked and there is
  // nothing to carry), and classified plus required (see below).
  //
  // STAMPED BY THE WRITER at record time, and PROPAGATED by the worker into
  // every variant it derives from the entry: a hit reads the selected
  // variant rather than the identity, so the fact has to ride each one.
  // Agent markdown variants do not inherit it.  Set by the nginx record
  // arm (src/nginx/headers_sidecar_record.h).
  //
  // The whole flags byte feeds the weak hit ETag (src/nginx/etag_util.h), so
  // a classified entry does not validate against an otherwise-identical
  // unmarked one.  An entry written before this bit existed reads back
  // unmarked, the correct legacy answer.
  static constexpr uint8_t kFlagHeadersClassified = 0x10;
  // kFlagHeadersSidecarRequired: the gate stored a non-empty headers sidecar
  // for this URL, so a serve path must read it and replay it, and must treat
  // an unreadable sidecar as a miss rather than serve without the headers.
  // Only meaningful together with kFlagHeadersClassified; same stamping,
  // propagation and ETag notes as above.  Read by the nginx serve paths
  // (src/nginx/headers_sidecar_replay.h).
  static constexpr uint8_t kFlagHeadersSidecarRequired = 0x20;
  // kFlagAnonymousRecorded: the front stage recorded this entry from its
  // response to a request it judged anonymous (no credentials on the
  // request, no signed-in user, no authorization requirement on the
  // endpoint).  STAMPED BY THE WRITER at record time and PROPAGATED by the
  // worker into every variant it derives, with the same rules as
  // kFlagHeadersClassified (agent markdown does not inherit it).  Set by the
  // ASP.NET Core middleware, which serves only entries that carry it; the
  // nginx record arm does not set it and the nginx serve paths do not read
  // it.  Feeds the weak hit ETag like every other flag bit.
  static constexpr uint8_t kFlagAnonymousRecorded = 0x40;
  // The flags byte is OPAQUE on the wire: it is read verbatim and written
  // back verbatim, so a bit set by a peer this build does not know about
  // survives a read/rewrite cycle rather than being cleared.  Do not add a
  // mask that filters unknown bits — losing a flag is indistinguishable from
  // never having been told about it.  Bit 0x80 is free.
  static constexpr uint8_t kFlagsAssignedMask =
      kFlagNeedsRevalidation | kFlagWorkerProcessed | kFlagOriginVariesAccept |
      kFlagOriginHeadersNotReproducible | kFlagHeadersClassified |
      kFlagHeadersSidecarRequired | kFlagAnonymousRecorded;

  // Origin Cache-Control flag bits (origin_cc_flags).
  static constexpr uint16_t kCCOriginNoCache = 1 << 0;
  static constexpr uint16_t kCCOriginMustRevalidate = 1 << 1;
  static constexpr uint16_t kCCOriginNoStore = 1 << 2;
  static constexpr uint16_t kCCOriginPrivate = 1 << 3;
  static constexpr uint16_t kCCOriginPublic = 1 << 4;
  static constexpr uint16_t kCCOriginImmutable = 1 << 5;
  static constexpr uint16_t kCCOriginSMaxagePresent = 1 << 6;
  static constexpr uint16_t kCCOriginProxyRevalidate = 1 << 7;
  static constexpr uint16_t kCCOriginNoTransform = 1 << 8;
  static constexpr uint16_t kCCOriginHeaderPresent = 1 << 9;
  // Form markers for the two directives that take an optional argument
  // (RFC 9111 §5.2.2.7 / §5.2.2.4).  `private="Set-Cookie"` restricts ONLY
  // the listed field-names and explicitly permits a shared cache to store
  // the remainder, so a consumer that acts destructively on `private`
  // (issue #1016: the 304-restamp eviction) must distinguish the two
  // forms: treating `private="Set-Cookie", max-age=600` as blanket-private
  // would destroy an entire optimized variant set on every revalidation.
  //
  // BOTH forms are tracked, and the reason is the accumulation model:
  // cc_flags is OR-ed across every token of every Cache-Control header
  // line, so a "qualified" bit alone would only ever mean "SOME occurrence
  // was qualified" — and
  //     Cache-Control: private="Set-Cookie"
  //     Cache-Control: private
  // (origin sends the qualified form, an intermediary appends a bare one)
  // would then read as fully qualified and skip an eviction the bare
  // directive plainly demands.  "Bare seen" IS monotone under OR, so it is
  // the load-bearing bit; the qualified bit distinguishes a parsed
  // qualified-only response from legacy metadata that predates both bits.
  //
  // Decision rule for any destructive consumer:
  //   Bare set                      -> blanket restriction (act)
  //   Qualified set, Bare clear     -> qualified only (do not act)
  //   Neither set (legacy metadata) -> no per-occurrence information;
  //                                    treat as blanket (fail safe)
  //
  // Set alongside the base flag, never instead of it, so every existing
  // reader keeps its current (conservative) behavior unchanged.
  static constexpr uint16_t kCCOriginPrivateQualified = 1 << 10;
  static constexpr uint16_t kCCOriginPrivateBare = 1 << 11;
  static constexpr uint16_t kCCOriginNoCacheQualified = 1 << 12;
  static constexpr uint16_t kCCOriginNoCacheBare = 1 << 13;
  // More than one `s-maxage` directive, in one line or across lines.  The
  // value kept is the last one; the bit lets a consumer refuse to read the
  // conflicting set as an explicit statement (the cookie rule in
  // lib/cache/html_cache_policy.h does: repeated s-maxage is no sharing
  // mark).  Set alongside kCCOriginSMaxagePresent, never instead of it.
  static constexpr uint16_t kCCOriginSMaxageRepeated = 1 << 14;

  uint8_t version = kCurrentVersion;
  uint32_t full_mask = 0;
  ContentType content_type = ContentType::kOther;
  uint8_t flags = 0;
  std::string origin_content_type;

  // Wire ct_len as read from serialized data (set by Deserialize).
  // Used by ParseMetadataPrefix to compute correct content offset.
  uint16_t wire_ct_len = 0;

  // Wire cc_len as read from serialized data (set by Deserialize), for the
  // same reason wire_ct_len exists: origin_cache_control is sanitized on the
  // way in, so its size is not necessarily the number of bytes it occupied.
  uint16_t wire_cc_len = 0;

  // Cache-control fields (v3).
  uint32_t cache_inserted_at = 0;  // Unix timestamp (seconds)
  uint32_t origin_max_age = 0;     // Raw max-age from origin
  uint32_t origin_s_maxage = 0;    // Raw s-maxage (0 if absent)
  uint16_t origin_cc_flags = 0;    // Bitfield of origin CC directives

  // Quality + classification fields (v4).
  // score_x100: 0-10000 = score*100, kScoreNA = not available.
  // content_class: 0=Photo, 1=Screenshot, 2=Illustration, 3=Noisy,
  //   kContentClassUnknown=4.
  uint16_t ssimulacra2_score_x100 = kScoreNA;
  uint8_t content_class = kContentClassUnknown;

  // Conditional revalidation fields (v5).
  // origin_last_modified: Unix timestamp from Last-Modified header (0=absent).
  // origin_etag: ETag value stored verbatim (including quotes and W/ prefix).
  uint32_t origin_last_modified = 0;
  std::string origin_etag;

  // Origin content length (v6).
  // Size of the original (unoptimized) content in bytes.  Set by the worker
  // at write time so that nginx can compute bandwidth savings at serve time.
  // 0 means not available (older metadata versions, or nginx-written originals).
  uint32_t origin_content_length = 0;

  // Content-binding hashes (v7, content-hash unit invalidation).
  // origin_html_hash: SHA-256 of the raw, pre-rewrite origin HTML the variant
  //   was produced from.  The agent (kAgentMarkdown) variant is stamped with it;
  //   the serve-time equality gate refuses to serve a variant whose stamp != the
  //   live kContentHash sentinel for the key, and the store-time re-validation
  //   discards a render whose source advanced underneath it.
  // render_source_hash: on a kAgentMarkdown entry, SHA-256 of the Markdown
  //   bytes themselves.  The optimizer stamps it when it derives the copy
  //   from a stored HTML page without a browser, and the Markdown entity tag
  //   is computed from it (ps_html_stored_facts_from_reads), so a validator
  //   costs no pass over the copy.  All-zero on every other entry and on a
  //   copy written without it: no entity tag.  Not used by the serve gate.
  // All-zero is the reserved "no binding / cannot prove" value (the default for
  // v6-and-earlier blobs and for variants written without a binding); the D8
  // gates treat all-zero as absent and fail toward refuse-to-serve.
  std::array<std::byte, kHashSize> origin_html_hash{};
  std::array<std::byte, kHashSize> render_source_hash{};

  // Per-URL epoch (v8).
  //
  // The generation the entry was written under.  Its purpose is to make an
  // entry that outlived the URL state it belongs to *detectable*: an entry
  // stamped with an epoch older than the URL's current one is stale by
  // definition, however fresh its own cache-control fields look.  Without it,
  // a durably-stored copy of an origin response that was later purged is
  // indistinguishable from one that was never purged.
  //
  // 0 is the reserved "no epoch recorded" value — v7-and-earlier blobs and
  // any entry written without one — and consumers must treat it as absent
  // rather than as generation zero.  (The zero-means-absent convention is the
  // same one the v7 hashes use, and it is what makes the field safe to add to
  // a struct that is zero-initialised by default.)
  //
  // WIDTH: 64 bits, and the width is the design decision here.
  //   - Wrap must be impossible, not merely unlikely.  A wrapped counter
  //     compares EQUAL to a generation it is not, which resurrects exactly
  //     the stale entry the field exists to catch — the one failure mode that
  //     is silent, and the one this field is spent on preventing.  A 32-bit
  //     counter has a reachable wrap (a revalidation loop bumping it a few
  //     hundred times a second wraps inside a year); a 64-bit counter does
  //     not have one at any rate a cache can sustain.
  //   - It does not foreclose the encoding.  Whether the value is a sequence
  //     number or is derived from a clock (seconds, milliseconds since the
  //     epoch) is a decision for the code that MAINTAINS it, which is not
  //     this change.  64 bits holds either; 32 bits holds a sequence number
  //     and a seconds clock but not a millisecond one, so choosing it here
  //     would quietly decide a question that is not this lane's to decide.
  //   - The cost is 8 bytes against a budget with 128 B of stated reserve.
  //
  // Comparison is monotone: an entry is stale when its epoch is LESS than the
  // URL's current epoch.  Ordering, not equality — an equality test would
  // also reject entries from a future generation, which is a skew symptom,
  // not staleness.
  uint64_t origin_epoch = 0;

  // Raw origin Cache-Control header (v8).
  //
  // The origin's Cache-Control field value as received, verbatim, so that a
  // consumer that must reproduce or relay the origin's own directives has the
  // bytes rather than a re-synthesis of them.  origin_cc_flags/origin_max_age
  // /origin_s_maxage remain the parsed, machine-readable form and are what
  // every existing consumer uses; this is the faithful copy beside them, and
  // it exists because a re-synthesized header flattens what it did not model
  // (extension directives, ordering, an origin's exact spelling).
  //
  // Empty means absent: no Cache-Control header, or one still longer than
  // kMaxOriginCcLen once stripped (stored as absent rather than truncated —
  // see the cap).  CR, LF and NUL are stripped on write and on read, as for
  // origin_content_type: the value ends up in a response header, and a stored
  // CRLF is a header-splitting primitive waiting for a consumer that forwards
  // it unchecked.  The read-side strip is not redundant — it guards bytes this
  // build did not write.  On that path the stripped value can be shorter than
  // wire_cc_len, which is why wire_cc_len (not the string's size) is what
  // WireSize() and consumed_size are computed from.
  //
  // Multiple Cache-Control header lines are combined by the CALLER into one
  // comma-separated value before being stored, matching how a recipient is
  // required to treat them.
  std::string origin_cache_control;

  // Byte count consumed by Deserialize, computed by summing individual
  // field sizes (not via kV*FixedSuffixSize constants).  Set only by
  // Deserialize; 0 otherwise.  ParseMetadataPrefix cross-checks this
  // against WireSize() as a regression canary: both values are derived
  // from the same data, so a mismatch means a code change broke a
  // constant or omitted a field.
  size_t consumed_size = 0;

  // True when full_mask names gzip or brotli transfer encoding: the
  // pre-compressed copy of a text or SVG variant.
  bool IsEncodedVariant() const;

  // The quality score (x100) of an image-like entry; nullopt when it is not
  // available or the entry is gzip/brotli coded (whose field means something
  // else).
  std::optional<uint16_t> QualityScoreX100() const {
    if (IsEncodedVariant() || ssimulacra2_score_x100 > kMaxScoreX100) {
      return std::nullopt;
    }
    return ssimulacra2_score_x100;
  }
  // Sets the quality score (x100); nullopt or an out-of-range value stores
  // "not available".  No-op on a gzip/brotli coded entry.
  void SetQualityScoreX100(std::optional<uint16_t> score) {
    if (IsEncodedVariant()) return;
    ssimulacra2_score_x100 =
        (score.has_value() && *score <= kMaxScoreX100) ? *score : kScoreNA;
  }

  // The size of the origin bytes encoded the way THIS variant is served
  // (what the saving of the encoding itself is measured against).
  //   - identity encoding: origin_content_length (also what an image, which
  //     has no text encoding, is measured against);
  //   - a gzip/brotli coded variant: the stored value, or nullopt when the
  //     entry does not carry one (written before the value existed, or the
  //     encoded form is more than 1.11 times the origin);
  //   - nullopt also when origin_content_length is 0.
  std::optional<uint32_t> EncodedOriginLength() const;
  // Stores the encoded size for a gzip/brotli coded variant.  No-op for other
  // entries; an unrepresentable value stores "not available".
  void SetEncodedOriginLength(uint64_t encoded_bytes);
  // Stores "not available" on a gzip/brotli coded entry (drops a value
  // inherited from the entry this one was copied from).
  void ClearEncodedOriginLength();

  // Version-aware wire size (prefix + ct_len + suffix).
  size_t WireSize() const {
    if (version >= 8) {
      return kFixedPrefixSize + wire_ct_len + kV8FixedSuffixSize +
             origin_etag.size() + wire_cc_len;
    }
    if (version >= 7) {
      return kFixedPrefixSize + wire_ct_len + kV7FixedSuffixSize +
             origin_etag.size();
    }
    if (version >= 6) {
      return kFixedPrefixSize + wire_ct_len + kV6FixedSuffixSize +
             origin_etag.size();
    }
    if (version >= 5) {
      return kFixedPrefixSize + wire_ct_len + kV5FixedSuffixSize +
             origin_etag.size();
    }
    size_t suffix = (version >= 4) ? 17 : 14;
    return kFixedPrefixSize + wire_ct_len + suffix;
  }

  // Serialize to bytes for storage in Cyclone alternate header.
  // Uses memcpy-based serialization (no pointer casts).
  std::vector<std::byte> Serialize() const;

  // Deserialize from bytes read from Cyclone alternate header.
  // Returns std::nullopt on:
  //   - Data too short
  //   - Version not 3, 4, 5, 6, 7, 8, or 9  (a HIGHER version is a peer that
  //     knows something this build does not: nullopt, which the read path
  //     turns into a MISS, never a best-effort parse of the prefix)
  //   - an entry IsRetired accepts (the caller treats it as a MISS and
  //     rebuilds it)
  //   - origin_ct_len exceeds remaining data
  //   - origin_ct_len > kMaxOriginCtLen
  //   - content_type out of enum range
  //   - v5+: etag_len exceeds remaining data
  //   - v8+: origin_cc_len > kMaxOriginCcLen, or exceeds remaining data
  // Strips \r, \n, \0 from origin_content_type and origin_cache_control
  // (also stripped on write).
  // Sets wire_ct_len / wire_cc_len to the raw lengths from the serialized
  // data.
  static std::optional<AlternateMetadata> Deserialize(
      std::span<const std::byte> data);

  // True for an entry written below version 9 that is an HTML page, whoever
  // wrote it, or the Markdown version of a page written by the worker.  This
  // build no longer serves those: Deserialize refuses them and
  // PageSpeedCache::RemoveRetiredAlternates drops them.  `data` is the
  // serialized metadata, alone or followed by the content.
  static bool IsRetired(std::span<const std::byte> data);
};

// The derivation above, pinned.  These are not tautologies: they are the
// arithmetic that sizes max_metadata_size, and a field added to the format
// without re-deriving the budget fails HERE, at compile time, instead of
// showing up in the field as writes that are silently refused.
static_assert(AlternateMetadata::kV8FixedSuffixSize == 101);
static_assert(AlternateMetadata::kMaxSerializedSizeAtBudget == 750);
static_assert(AlternateMetadata::kRecommendedMaxMetadataSize == 878);
static_assert(AlternateMetadata::kRecommendedMaxMetadataSize >=
                  AlternateMetadata::kMaxSerializedSizeAtBudget,
              "the metadata ceiling must cover the format's worst case");
// The four flag bits are distinct and the byte still has room.
static_assert((AlternateMetadata::kFlagNeedsRevalidation &
               AlternateMetadata::kFlagWorkerProcessed) == 0);
static_assert((AlternateMetadata::kFlagOriginVariesAccept &
               (AlternateMetadata::kFlagNeedsRevalidation |
                AlternateMetadata::kFlagWorkerProcessed)) == 0);
static_assert((AlternateMetadata::kFlagOriginHeadersNotReproducible &
               (AlternateMetadata::kFlagNeedsRevalidation |
                AlternateMetadata::kFlagWorkerProcessed |
                AlternateMetadata::kFlagOriginVariesAccept)) == 0);
static_assert((AlternateMetadata::kFlagHeadersClassified &
               (AlternateMetadata::kFlagNeedsRevalidation |
                AlternateMetadata::kFlagWorkerProcessed |
                AlternateMetadata::kFlagOriginVariesAccept |
                AlternateMetadata::kFlagOriginHeadersNotReproducible)) == 0);
static_assert((AlternateMetadata::kFlagHeadersSidecarRequired &
               (AlternateMetadata::kFlagNeedsRevalidation |
                AlternateMetadata::kFlagWorkerProcessed |
                AlternateMetadata::kFlagOriginVariesAccept |
                AlternateMetadata::kFlagOriginHeadersNotReproducible |
                AlternateMetadata::kFlagHeadersClassified)) == 0);
static_assert((AlternateMetadata::kFlagAnonymousRecorded &
               (AlternateMetadata::kFlagNeedsRevalidation |
                AlternateMetadata::kFlagWorkerProcessed |
                AlternateMetadata::kFlagOriginVariesAccept |
                AlternateMetadata::kFlagOriginHeadersNotReproducible |
                AlternateMetadata::kFlagHeadersClassified |
                AlternateMetadata::kFlagHeadersSidecarRequired)) == 0);
static_assert(AlternateMetadata::kFlagsAssignedMask == 0x7F);

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_ALTERNATE_METADATA_H_
