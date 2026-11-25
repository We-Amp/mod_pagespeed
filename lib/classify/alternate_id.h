// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_LIB_CLASSIFY_ALTERNATE_ID_H_
#define PAGESPEED_LIB_CLASSIFY_ALTERNATE_ID_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// NOTE: This header does NOT include Cyclone's alternate.hpp. The AlternateId
// type used here is PageSpeed's own uint8_t alias. Conversion to/from
// cyclone::AlternateId happens at the cache API boundary (lib/cache/cache.h).

namespace pagespeed {

// PageSpeed AlternateId — an opaque uint8_t that maps 1:1 to the low byte
// of CapabilityMask::Encode(). Used as cyclone::AlternateId at the cache
// boundary.
//
// IMPORTANT: PageSpeed uses PageSpeedSelector exclusively for alternate
// selection. Never use Cyclone's CompressionAwareSelector,
// DefaultStorageSelector, or any plugin that interprets AlternateId
// semantically. The named Cyclone enum values (Brotli=1, WebP=16, etc.)
// are irrelevant to PageSpeed's usage.
using AlternateId = uint8_t;

// Convert the low byte of a capability mask to an AlternateId.
// Direct cast — no offset or transformation.
inline constexpr AlternateId MaskToAlternateId(uint8_t mask_byte) {
  return static_cast<AlternateId>(mask_byte);
}

// Convert an AlternateId back to a capability mask byte.
inline constexpr uint8_t AlternateIdToMask(AlternateId id) {
  return static_cast<uint8_t>(id);
}

// Sentinel AlternateId values.
// All sentinels have viewport bits (bits 2-3) = 3 (0b11), which
// FromHeaders() never produces (valid viewports are 0, 1, 2).
enum class SentinelId : uint8_t {
  kOriginalContent = 0x0C,      // Durable original (unoptimized) content, the
                                // bytes the origin sent.  Written ONLY through
                                // PageSpeedCache::WriteOriginalAlternate (and
                                // its C entry point), never through the
                                // generic sentinel write surface — see
                                // SentinelWriter::kDedicated below.
  kEarlyHints = 0x1C,           // Early Hints preload data
  kWarmupRequest = 0x2C,        // Warmup trigger from nginx
  kContentHash = 0x3C,          // Content hash for idempotency
  kSubresourceManifest = 0x4C,  // Subresource manifest
  kBrowserProfile = 0x5C,       // Browser optimization profile
  kHeadersSidecar = 0x6C,       // The request-independent response-header
                                // sidecar: ONE entry per URL (the block is
                                // request-independent, so a per-alternate copy
                                // would be pure duplication), carrying the
                                // curated header block verbatim under its own
                                // payload format version.  Written ONLY
                                // through PageSpeedCache::WriteHeadersSidecar
                                // (and its C entry point), which is where the
                                // admission gate lives — see
                                // SentinelWriter::kDedicated below.
  kAgentMarkdown = 0x7C,        // agent_optimize rendered-DOM markdown variant
  // (operator-gated, selectable ONLY for
  // an agent request with the flag on — see PageSpeedSelector)
  kLlmsTxt = 0x8C,          // Synthesized /llms.txt site-index
                            // body (operator-gated; read by exact id via
                            // ReadAlternateByKey, NOT the PageSpeedSelector)
  kLlmsTxtMeta = 0x9C,      // /llms.txt freshness record —
                            // [32B sitemap SHA-256][8B next-refresh epoch BE]
  kNegativeVerdict = 0xAC,  // RESERVED: per-URL "this resource will not be
                            // optimized" verdict, so a serving path can stop
                            // re-asking.  FORMAT ONLY at v8 — the id and its
                            // payload format version are reserved here; no
                            // writer, no reader, and the current cooldown-based
                            // suppression is unchanged.  Reserving it now is
                            // what keeps the behaviour from costing a second
                            // format version later.
  kDeclineTombstone = 0xBC,  // Per-URL decline tombstone (#1382): the record
                             // of which image variant slots the SSIMULACRA2
                             // verify DECLINED for the current source, so a
                             // proactive notification does not re-pay the full
                             // attempt ladder for a variant that was already
                             // refused and never stored.  Worker-written,
                             // worker-read; the payload carries the source
                             // SHA-256, and a changed source (whose change path
                             // purges the whole key) never matches it.
                             // SERVING SAFETY, by construction: a sentinel id
                             // is unconditionally skipped by PageSpeedSelector,
                             // so a tombstone is never served, and no client
                             // mask can name it (viewport bits = 3 is
                             // unproducible from headers).  It is not a
                             // capability advertisement: it appears in
                             // diagnostics listings exactly as the other
  // sentinel classes do, named by the registry below.
  kAgentMarkdownWithheld = 0xCC,  // RESERVED: the record that the Markdown
                                  // copy of a page was permanently refused for
                                  // one parent hash and deriver version, so a
                                  // serve answers "withheld" instead of asking
                                  // for the copy again.  A DISTINCT record and
                                  // never a readable kAgentMarkdown entry: it
                                  // is sentinel-shaped, and PageSpeedSelector
                                  // skips every sentinel except kAgentMarkdown,
                                  // so no selection read can return it, and no
                                  // exact-id reader of another class asks for
                                  // it.  A durable original written for the
                                  // key (WriteOriginalAlternate) removes it in
                                  // the same call.  Written only by the
                                  // worker's Markdown deriver, through the
                                  // in-tree PageSpeedCache::WriteSentinel;
                                  // the payload is lib/cache/
                                  // html_withheld_marker.h.
};

// Payload shape of an entry class, i.e. how a reader of that class knows what
// version of what layout it is holding.  This is the per-entry version
// discipline in one place: every class either carries an AlternateMetadata
// prefix (versioned by AlternateMetadata::kCurrentVersion) or begins with its
// own 1-byte payload format version.  Classes that predate the discipline are
// recorded as kOpaqueLegacy and their shape is FROZEN BY CHOICE, not by
// impossibility: every one of them is fixed-width or self-describing, which is
// exactly the property that would let a length-discriminated retrofit work (a
// 32 B kContentHash payload and a 33 B versioned one are distinguishable
// without a flag day).  We do not do it, because a retrofit buys nothing these
// classes need and costs a reader that must know both shapes forever.  Stating
// it as a decision rather than a law is deliberate: someone reading this years
// from now should find the escape hatch documented, not have to rediscover it
// and conclude the rest of this file is approximate.
// Anything ADDED from v8 onward is kVersionedPayload or kMetadataPrefix; there
// is no third option and no new kOpaqueLegacy — enforced below, at compile
// time, against the frozen id list rather than described here.
enum class SentinelPayload : uint8_t {
  kOpaqueLegacy = 0,      // Class-specific bytes, no version byte (frozen).
  kMetadataPrefix = 1,    // AlternateMetadata prefix, then content.
  kVersionedPayload = 2,  // [1B payload format version][class-specific bytes].
};

// Payload format version of each kVersionedPayload class.  Reserved with the
// id: a reader that finds a byte it does not recognise here stops rather than
// guessing, which is the whole reason the byte is spent.
inline constexpr uint8_t kHeadersSidecarFormatVersion = 1;  // kHeadersSidecar
// The paired form of the same class: format 1 preceded by the 32-byte hash of
// the durable original the block was recorded with, so a reader can tell a
// block that belongs to the stored original from one that does not.  A build
// that knows only format 1 reads it as absent, which is the safe answer.
inline constexpr uint8_t kHeadersSidecarPairedFormatVersion = 2;
inline constexpr uint8_t kNegativeVerdictFormatVersion = 1;  // kNegativeVerdict
// Payload format version of the kDeclineTombstone class.  v2 added the
// quality-search generation byte that the payload carries after this one.
inline constexpr uint8_t kDeclineTombstoneFormatVersion = 2;
inline constexpr uint8_t kAgentMarkdownWithheldFormatVersion =
    1;  // kAgentMarkdownWithheld

// WHO may write an entry class through the generic sentinel write surface —
// a different question from whether the class exists.
//
// The gate is enforced where a third party can actually reach it:
// ps_cache_write_sentinel admits kEmbedder and refuses everything else, so an
// embedder can neither occupy a slot before the class that owns it exists nor
// write into a class that has its own writer with its own invariants.  The
// in-tree C++ writer (PageSpeedCache::WriteSentinel) is deliberately NOT
// gated — the lane that lands each class writes through it.
enum class SentinelWriter : uint8_t {
  // Anyone may write it, including an embedder through the C surface.  The
  // class either predates the discipline or is deliberately open.
  kEmbedder = 0,
  // RESERVED: the id and its shape are claimed, and NOTHING writes it yet.  A
  // reserved id must never be reused for anything else — if an embedder could
  // occupy the slot, the class would arrive to find foreign bytes under its
  // own id, unversioned and indistinguishable from its own.
  kReserved = 1,
  // A dedicated in-tree entry point owns the id and is the only writer.  The
  // generic surface still refuses it, for a reason that outlives the
  // reservation: the class's invariants live in that entry point — a metadata
  // prefix that must self-describe and a content cap enforced while streaming
  // for the durable original, an admission gate that decides what may be
  // stored at all for the headers sidecar — and a write that bypasses it
  // produces an entry the class's own reader cannot trust.  The sidecar makes
  // that concrete: its gate is what keeps a per-response security nonce out of
  // a durable entry, so a raw write into 0x6C is not a shortcut, it is the
  // failure the class exists to prevent.  "The class exists now" is therefore
  // NOT a reason to move a row to kEmbedder.
  kDedicated = 2,
};

// One row of the sentinel registry.
struct SentinelRegistryEntry {
  AlternateId id;
  std::string_view name;
  SentinelPayload payload;
  SentinelWriter writer;
};

// The registry.  Every sentinel id PageSpeed knows about appears exactly once,
// live or reserved.
//
// WHAT CONSULTS THIS TODAY, exactly: sentinel NAMING (diagnostics) and the
// admission gate on the embedder write surface (ps_cache_write_sentinel admits
// only SentinelWriter::kEmbedder rows and unregistered ids).  Nothing else
// DECIDES anything from it.  In particular the notify-side drop in
// src/worker/worker.cc is VIEWPORT-BITS-shaped and predates this table — it
// treats a registered id exactly like an unregistered one, and deliberately
// still does: what a notification may trigger is a fixed set of whole-mask
// specials, not "whatever class this build happens to know".  That path does
// now consult SentinelName, but for the LOG LINE only — so a peer naming a
// class this build has never heard of ("unknown_sentinel") is distinguishable
// from a peer misusing a class it knows.  Naming is not gating: "unregistered"
// is still not a runtime concept on the read/notify paths, and making those
// paths registry-aware in the deciding sense belongs to the lanes that consume
// these classes.
inline constexpr SentinelRegistryEntry kSentinelRegistry[] = {
    {static_cast<AlternateId>(SentinelId::kOriginalContent), "original_content",
     SentinelPayload::kMetadataPrefix, SentinelWriter::kDedicated},
    {static_cast<AlternateId>(SentinelId::kEarlyHints), "early_hints",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kWarmupRequest), "warmup",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kContentHash), "content_hash",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kSubresourceManifest),
     "subresource_manifest", SentinelPayload::kOpaqueLegacy,
     SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kBrowserProfile), "browser_profile",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kHeadersSidecar), "headers_sidecar",
     SentinelPayload::kVersionedPayload, SentinelWriter::kDedicated},
    {static_cast<AlternateId>(SentinelId::kAgentMarkdown), "agent_markdown",
     SentinelPayload::kMetadataPrefix, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kLlmsTxt), "llms_txt",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kLlmsTxtMeta), "llms_txt_meta",
     SentinelPayload::kOpaqueLegacy, SentinelWriter::kEmbedder},
    {static_cast<AlternateId>(SentinelId::kNegativeVerdict), "negative_verdict",
     SentinelPayload::kVersionedPayload, SentinelWriter::kReserved},
    {static_cast<AlternateId>(SentinelId::kDeclineTombstone),
     "decline_tombstone", SentinelPayload::kVersionedPayload,
     SentinelWriter::kDedicated},
    {static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld),
     "agent_markdown_withheld", SentinelPayload::kVersionedPayload,
     SentinelWriter::kDedicated},
};

inline constexpr size_t kSentinelRegistrySize =
    sizeof(kSentinelRegistry) / sizeof(kSentinelRegistry[0]);

// Look up a sentinel id in the registry.  Returns nullptr for an id that is
// not registered (including non-sentinel ids).
inline constexpr const SentinelRegistryEntry* FindSentinel(AlternateId id) {
  for (const auto& entry : kSentinelRegistry) {
    if (entry.id == id) return &entry;
  }
  return nullptr;
}

// True when `id` is a sentinel this build knows about.  A sentinel that is NOT
// registered is a peer speaking a namespace this build does not have: the
// caller drops the work and logs, it does not guess.
inline constexpr bool IsRegisteredSentinel(AlternateId id) {
  return FindSentinel(id) != nullptr;
}

// True when the generic embedder write surface may write `id`.  An id that
// names no class at all stays writable: a free slot is an embedder's to use, a
// claimed one is not.
inline constexpr bool IsEmbedderWritableSentinel(AlternateId id) {
  const auto* entry = FindSentinel(id);
  return entry == nullptr || entry->writer == SentinelWriter::kEmbedder;
}

// Diagnostic name for a sentinel id; "unknown_sentinel" when unregistered.
inline constexpr std::string_view SentinelName(AlternateId id) {
  const auto* entry = FindSentinel(id);
  return entry != nullptr ? entry->name : std::string_view("unknown_sentinel");
}

// Payload shape of a registered sentinel; nullopt when unregistered.
inline constexpr std::optional<SentinelPayload> SentinelPayloadOf(
    AlternateId id) {
  const auto* entry = FindSentinel(id);
  if (entry == nullptr) return std::nullopt;
  return entry->payload;
}

// Check if an AlternateId is a sentinel (viewport bits = 3).
inline constexpr bool IsSentinel(AlternateId id) {
  return ((id >> 2) & 0x03) == 0x03;
}

// Check if a viewport value is valid (not the sentinel value 3).
inline constexpr bool IsValidViewport(uint8_t viewport) {
  return viewport <= 2;
}

// Compile-time verification that all sentinels have viewport=3.
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kOriginalContent)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kEarlyHints)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kWarmupRequest)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kContentHash)));
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kSubresourceManifest)));
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kBrowserProfile)));
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kHeadersSidecar)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kAgentMarkdown)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kLlmsTxt)));
static_assert(IsSentinel(static_cast<AlternateId>(SentinelId::kLlmsTxtMeta)));
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kNegativeVerdict)));
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kDeclineTombstone)));
// The withheld marker MUST be sentinel-shaped: PageSpeedSelector skips every
// sentinel except kAgentMarkdown and would score any other byte as a content
// variant, so a non-sentinel id here could be served as a page.
static_assert(
    IsSentinel(static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld)),
    "the withheld marker must never be selectable");
static_assert(static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld) !=
              static_cast<AlternateId>(SentinelId::kAgentMarkdown));

// Registry invariants, checked at compile time rather than trusted:
//   - every row is a sentinel id (viewport bits = 3);
//   - no id appears twice — a duplicate is a silently stolen class.
// The row-count assert below is a tripwire, not a proof: C++ cannot enumerate
// an enum, so "every enumerator has a row" is asserted in the test
// (AlternateIdTest.EverySentinelIdIsRegisteredExactlyOnce, which lists them).
// What the assert buys is that changing the table forces the author to look at
// the test rather than silently shipping a class as "unknown".
static_assert([] {
  for (size_t i = 0; i < kSentinelRegistrySize; ++i) {
    if (!IsSentinel(kSentinelRegistry[i].id)) return false;
    for (size_t j = i + 1; j < kSentinelRegistrySize; ++j) {
      if (kSentinelRegistry[i].id == kSentinelRegistry[j].id) return false;
    }
  }
  return true;
}());
static_assert(kSentinelRegistrySize == 13,
              "a SentinelId enumerator was added or removed without updating "
              "kSentinelRegistry");

// The kOpaqueLegacy freeze, enforced rather than described.
//
// These seven ids are the complete set of classes that predate the per-entry
// version discipline.  The set is closed: no id may join it and no id may
// leave it.  Asserting the SET rather than the COUNT matters — a count-only
// check passes if one row flips to kOpaqueLegacy while another flips away, and
// the likeliest violation is not a "reserved" row at all but a NEW LIVE class
// (reserved == false) added with the legacy shape, which is exactly what a
// count would wave through.
inline constexpr AlternateId kFrozenOpaqueLegacySentinels[] = {
    static_cast<AlternateId>(SentinelId::kEarlyHints),
    static_cast<AlternateId>(SentinelId::kWarmupRequest),
    static_cast<AlternateId>(SentinelId::kContentHash),
    static_cast<AlternateId>(SentinelId::kSubresourceManifest),
    static_cast<AlternateId>(SentinelId::kBrowserProfile),
    static_cast<AlternateId>(SentinelId::kLlmsTxt),
    static_cast<AlternateId>(SentinelId::kLlmsTxtMeta),
};
inline constexpr size_t kFrozenOpaqueLegacyCount =
    sizeof(kFrozenOpaqueLegacySentinels) /
    sizeof(kFrozenOpaqueLegacySentinels[0]);

static_assert(kFrozenOpaqueLegacyCount == 7);
static_assert(
    [] {
      // Every kOpaqueLegacy row is on the frozen list, and every frozen id is
      // still kOpaqueLegacy.  Either direction failing is a build break.
      size_t legacy_rows = 0;
      for (size_t i = 0; i < kSentinelRegistrySize; ++i) {
        if (kSentinelRegistry[i].payload != SentinelPayload::kOpaqueLegacy) {
          continue;
        }
        ++legacy_rows;
        bool frozen = false;
        for (size_t j = 0; j < kFrozenOpaqueLegacyCount; ++j) {
          if (kSentinelRegistry[i].id == kFrozenOpaqueLegacySentinels[j]) {
            frozen = true;
          }
        }
        if (!frozen) return false;
      }
      if (legacy_rows != kFrozenOpaqueLegacyCount) return false;
      for (size_t j = 0; j < kFrozenOpaqueLegacyCount; ++j) {
        const auto* entry = FindSentinel(kFrozenOpaqueLegacySentinels[j]);
        if (entry == nullptr) return false;
        if (entry->payload != SentinelPayload::kOpaqueLegacy) return false;
      }
      return true;
    }(),
    "a sentinel class was added with, or moved to, the kOpaqueLegacy shape. "
    "Classes added from entry-metadata v8 onward must carry a version: "
    "kVersionedPayload (own version byte) or kMetadataPrefix.");
// The durable original-content class has a writer now, and it is a DEDICATED
// one: the generic embedder surface must keep refusing 0x0C.  Asserting it
// here means flipping the row to kEmbedder — the one-token change that would
// silently open the class to writes that skip its metadata prefix and its
// content cap — is a build break rather than a review miss.
static_assert(!IsEmbedderWritableSentinel(
                  static_cast<AlternateId>(SentinelId::kOriginalContent)),
              "0x0C is written only through WriteOriginalAlternate; the "
              "generic sentinel write surface must refuse it");
// The headers sidecar has a dedicated writer for the same reason and keeps
// the same refusal: its entry point IS the admission gate, so a raw write
// into 0x6C would store precisely the header blocks the gate refuses.
static_assert(!IsEmbedderWritableSentinel(
                  static_cast<AlternateId>(SentinelId::kHeadersSidecar)),
              "0x6C is written only through WriteHeadersSidecar; the generic "
              "sentinel write surface must refuse it");
// A reserved class is not writable through that surface either, and an
// unregistered sentinel-shaped id still is.
static_assert(!IsEmbedderWritableSentinel(
    static_cast<AlternateId>(SentinelId::kNegativeVerdict)));
// The decline tombstone is written only by the worker's tombstone code
// (through the in-tree PageSpeedCache::WriteSentinel, which the registry
// does not gate); the embedder surface must keep refusing it, exactly as
// it refuses the other classes with writer-side invariants.
static_assert(!IsEmbedderWritableSentinel(
    static_cast<AlternateId>(SentinelId::kDeclineTombstone)));
// The withheld marker has one writer, the worker's Markdown deriver: nothing
// may write it through the embedder surface.
static_assert(!IsEmbedderWritableSentinel(
    static_cast<AlternateId>(SentinelId::kAgentMarkdownWithheld)));
static_assert(IsEmbedderWritableSentinel(0xFC));

static_assert(IsRegisteredSentinel(
    static_cast<AlternateId>(SentinelId::kHeadersSidecar)));
static_assert(IsRegisteredSentinel(
    static_cast<AlternateId>(SentinelId::kOriginalContent)));
static_assert(IsRegisteredSentinel(
    static_cast<AlternateId>(SentinelId::kNegativeVerdict)));
static_assert(IsRegisteredSentinel(
    static_cast<AlternateId>(SentinelId::kDeclineTombstone)));
// 0xFC is a sentinel by construction but belongs to no class.
static_assert(IsSentinel(0xFC) && !IsRegisteredSentinel(0xFC));
static_assert(SentinelName(0xFC) == "unknown_sentinel");

// Verify that valid content mask bytes are never sentinels.
// Valid content masks have viewport 0, 1, or 2 — never 3.
// Maximum valid mask byte: format=3, viewport=2, density=1, savedata=1, enc=3
//   = 0b11_1_1_10_11 = 0xFB... but viewport=2 means bits 2-3 = 10.
// So we just verify that viewport=3 AlternateIds are sentinels.
static_assert(!IsSentinel(0x00));          // Mobile/Original/1x/off/Identity
static_assert(!IsSentinel(0x08));          // Desktop/Identity default
static_assert(!IsSentinel(0xFF & ~0x0C));  // All bits set except viewport

}  // namespace pagespeed

#endif  // PAGESPEED_LIB_CLASSIFY_ALTERNATE_ID_H_
