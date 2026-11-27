// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/alternate_id.h"

#include <cstddef>
#include <cstdint>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

TEST(AlternateIdTest, RoundTripAllValidMaskBytes) {
  // All 256 possible mask bytes should round-trip.
  for (int i = 0; i < 256; ++i) {
    auto mask_byte = static_cast<uint8_t>(i);
    AlternateId id = MaskToAlternateId(mask_byte);
    EXPECT_EQ(AlternateIdToMask(id), mask_byte)
        << "Round-trip failed for mask_byte=" << i;
  }
}

TEST(AlternateIdTest, NegativeVerdictIsASentinel) {
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kNegativeVerdict)));
}

// ===========================================================================
// Sentinel registry.
// ===========================================================================

TEST(AlternateIdTest, EverySentinelIdIsRegisteredExactlyOnce) {
  const SentinelId ids[] = {
      SentinelId::kOriginalContent,
      SentinelId::kEarlyHints,
      SentinelId::kWarmupRequest,
      SentinelId::kContentHash,
      SentinelId::kSubresourceManifest,
      SentinelId::kBrowserProfile,
      SentinelId::kHeadersSidecar,
      SentinelId::kAgentMarkdown,
      SentinelId::kLlmsTxt,
      SentinelId::kLlmsTxtMeta,
      SentinelId::kNegativeVerdict,
      SentinelId::kDeclineTombstone,
      SentinelId::kAgentMarkdownWithheld,
  };
  for (SentinelId id : ids) {
    auto byte = static_cast<AlternateId>(id);
    SCOPED_TRACE(static_cast<int>(byte));
    EXPECT_TRUE(IsRegisteredSentinel(byte));
    int matches = 0;
    for (size_t i = 0; i < kSentinelRegistrySize; ++i) {
      if (kSentinelRegistry[i].id == byte) ++matches;
    }
    EXPECT_EQ(matches, 1) << "an id claimed by two classes is a stolen class";
  }
  EXPECT_EQ(kSentinelRegistrySize, sizeof(ids) / sizeof(ids[0]));
}

TEST(AlternateIdTest, UnregisteredSentinelsAreReportedAsUnknown) {
  // Every sentinel-shaped byte that is not in the registry.
  for (int i = 0; i < 256; ++i) {
    auto b = static_cast<AlternateId>(i);
    if (!IsSentinel(b)) continue;
    if (IsRegisteredSentinel(b)) continue;
    SCOPED_TRACE(i);
    EXPECT_EQ(SentinelName(b), "unknown_sentinel");
    EXPECT_FALSE(SentinelPayloadOf(b).has_value())
        << "an unregistered id has no payload shape — a reader must drop it, "
           "not guess at the bytes";
  }
}

TEST(AlternateIdTest, ContentAlternateIdsAreNotRegisteredSentinels) {
  for (int i = 0; i < 256; ++i) {
    auto b = static_cast<AlternateId>(i);
    if (IsSentinel(b)) continue;
    EXPECT_FALSE(IsRegisteredSentinel(b)) << i;
  }
}

TEST(AlternateIdTest, ReservedClassesAreClaimedButUnwritten) {
  // The ids reserved ahead of their behaviour.  Reserved means the id and its
  // shape are fixed; it does NOT mean anything writes them yet.
  const SentinelId reserved[] = {
      SentinelId::kNegativeVerdict,
  };
  for (SentinelId id : reserved) {
    const auto* entry = FindSentinel(static_cast<AlternateId>(id));
    ASSERT_NE(entry, nullptr);
    SCOPED_TRACE(entry->name);
    EXPECT_EQ(entry->writer, SentinelWriter::kReserved);
  }
}

TEST(AlternateIdTest, TheDurableOriginalHasADedicatedWriter) {
  // 0x0C is no longer waiting for its class — it HAS one, with its own write
  // entry point.  The distinction that has to survive: having a writer is not
  // the same as being open to the generic embedder write surface, because the
  // class's invariants (a metadata prefix, a content cap) live in that entry
  // point and a raw sentinel write would skip both.
  const auto* entry =
      FindSentinel(static_cast<AlternateId>(SentinelId::kOriginalContent));
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->writer, SentinelWriter::kDedicated);
  EXPECT_FALSE(IsEmbedderWritableSentinel(
      static_cast<AlternateId>(SentinelId::kOriginalContent)));
  EXPECT_EQ(entry->payload, SentinelPayload::kMetadataPrefix);
}

TEST(AlternateIdTest, TheHeaderSidecarHasADedicatedWriter) {
  // Same shape as the durable original, for a sharper reason: this class's
  // write entry point IS its admission gate — what may be stored at all is
  // decided there — so opening the generic surface to 0x6C would store
  // precisely the header blocks the gate refuses.
  const auto* entry =
      FindSentinel(static_cast<AlternateId>(SentinelId::kHeadersSidecar));
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->writer, SentinelWriter::kDedicated);
  EXPECT_FALSE(IsEmbedderWritableSentinel(
      static_cast<AlternateId>(SentinelId::kHeadersSidecar)));
  EXPECT_EQ(entry->payload, SentinelPayload::kVersionedPayload);
  EXPECT_EQ(entry->name, "headers_sidecar");
}

TEST(AlternateIdTest, EmbedderWritabilityIsExactlyTheOpenClasses) {
  // Refused: every claimed class that is reserved or has its own writer.
  // Allowed: the open classes, and any sentinel-shaped id naming no class at
  // all — a free slot is an embedder's to use, a claimed one is not.
  for (int i = 0; i < 256; ++i) {
    auto b = static_cast<AlternateId>(i);
    if (!IsSentinel(b)) continue;
    SCOPED_TRACE(i);
    const auto* entry = FindSentinel(b);
    const bool expected =
        entry == nullptr || entry->writer == SentinelWriter::kEmbedder;
    EXPECT_EQ(IsEmbedderWritableSentinel(b), expected);
  }
}

TEST(AlternateIdTest, RegisteredPayloadShapesAreTheDeclaredOnes) {
  EXPECT_EQ(
      SentinelPayloadOf(static_cast<AlternateId>(SentinelId::kOriginalContent)),
      SentinelPayload::kMetadataPrefix);
  EXPECT_EQ(
      SentinelPayloadOf(static_cast<AlternateId>(SentinelId::kAgentMarkdown)),
      SentinelPayload::kMetadataPrefix);
  EXPECT_EQ(
      SentinelPayloadOf(static_cast<AlternateId>(SentinelId::kHeadersSidecar)),
      SentinelPayload::kVersionedPayload);
  EXPECT_EQ(
      SentinelPayloadOf(static_cast<AlternateId>(SentinelId::kNegativeVerdict)),
      SentinelPayload::kVersionedPayload);
  EXPECT_EQ(
      SentinelPayloadOf(static_cast<AlternateId>(SentinelId::kEarlyHints)),
      SentinelPayload::kOpaqueLegacy);
}

TEST(AlternateIdTest, OpaqueLegacyClassesAreExactlyTheFrozenSet) {
  // The discipline, enforced over EVERY row rather than only the reserved ones
  // — a new LIVE class (reserved == false) added with the legacy shape is the
  // likeliest way to violate the rule, and skipping unreserved rows would wave
  // exactly that through.  The compile-time gate in alternate_id.h is the real
  // enforcement; this asserts the same thing in a form that names it.
  size_t legacy_rows = 0;
  for (size_t i = 0; i < kSentinelRegistrySize; ++i) {
    const auto& e = kSentinelRegistry[i];
    if (e.payload != SentinelPayload::kOpaqueLegacy) continue;
    ++legacy_rows;
    SCOPED_TRACE(e.name);
    bool frozen = false;
    for (size_t j = 0; j < kFrozenOpaqueLegacyCount; ++j) {
      if (e.id == kFrozenOpaqueLegacySentinels[j]) frozen = true;
    }
    EXPECT_TRUE(frozen)
        << "a class added or moved to the unversioned legacy shape; classes "
           "added from format v8 onward must carry a version";
  }
  EXPECT_EQ(legacy_rows, kFrozenOpaqueLegacyCount)
      << "the legacy set is closed: no id joins it and no id leaves it";
  EXPECT_EQ(kFrozenOpaqueLegacyCount, 7u);

  // And the frozen ids really are the unversioned ones, not merely listed.
  for (size_t j = 0; j < kFrozenOpaqueLegacyCount; ++j) {
    EXPECT_EQ(SentinelPayloadOf(kFrozenOpaqueLegacySentinels[j]),
              SentinelPayload::kOpaqueLegacy);
  }
}

TEST(AlternateIdTest, EveryReservedClassCarriesAVersion) {
  // The reserved half, stated separately now that the freeze covers all rows:
  // nothing may be reserved without a way to tell its versions apart.
  for (size_t i = 0; i < kSentinelRegistrySize; ++i) {
    const auto& e = kSentinelRegistry[i];
    if (e.writer != SentinelWriter::kReserved) continue;
    SCOPED_TRACE(e.name);
    EXPECT_NE(e.payload, SentinelPayload::kOpaqueLegacy);
  }
}

TEST(AlternateIdTest, VersionedPayloadClassesHaveAFormatVersion) {
  EXPECT_EQ(kHeadersSidecarFormatVersion, 1);
  EXPECT_EQ(kNegativeVerdictFormatVersion, 1);
}

TEST(AlternateIdTest, AllSentinelsHaveViewport3) {
  // Each sentinel must have bits 2-3 = 0b11.
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kOriginalContent)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kEarlyHints)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kWarmupRequest)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kContentHash)));
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kSubresourceManifest)));
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kBrowserProfile)));
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kHeadersSidecar)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kAgentMarkdown)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kLlmsTxt)));
  EXPECT_TRUE(IsSentinel(static_cast<AlternateId>(SentinelId::kLlmsTxtMeta)));
  EXPECT_TRUE(
      IsSentinel(static_cast<AlternateId>(SentinelId::kNegativeVerdict)));
}

TEST(AlternateIdTest, IsSentinelReturnsFalseForContentAlternates) {
  // Content alternates have viewport 0, 1, or 2 — never 3.
  // Iterate all 192 valid content mask combinations:
  // 4 formats x 3 viewports x 2 densities x 2 save-data x 4 connections
  // = 192
  for (int fmt = 0; fmt < 4; ++fmt) {
    for (int vp = 0; vp < 3; ++vp) {  // viewport 0, 1, 2 only
      for (int den = 0; den < 2; ++den) {
        for (int sd = 0; sd < 2; ++sd) {
          for (int conn = 0; conn < 4; ++conn) {
            auto mask = static_cast<uint8_t>(
                (fmt << 0) | (vp << 2) | (den << 4) | (sd << 5) | (conn << 6));
            EXPECT_FALSE(IsSentinel(mask))
                << "Content mask 0x" << std::hex << static_cast<int>(mask)
                << " incorrectly flagged as sentinel";
          }
        }
      }
    }
  }
}

TEST(AlternateIdTest, IsValidViewport) {
  EXPECT_TRUE(IsValidViewport(0));
  EXPECT_TRUE(IsValidViewport(1));
  EXPECT_TRUE(IsValidViewport(2));
  EXPECT_FALSE(IsValidViewport(3));
  EXPECT_FALSE(IsValidViewport(4));
  EXPECT_FALSE(IsValidViewport(255));
}

TEST(AlternateIdTest, SentinelValues) {
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kOriginalContent), 0x0C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kEarlyHints), 0x1C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kWarmupRequest), 0x2C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kContentHash), 0x3C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kSubresourceManifest), 0x4C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kBrowserProfile), 0x5C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kHeadersSidecar), 0x6C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kAgentMarkdown), 0x7C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kLlmsTxt), 0x8C);
  EXPECT_EQ(static_cast<uint8_t>(SentinelId::kLlmsTxtMeta), 0x9C);
}

}  // namespace
}  // namespace pagespeed
