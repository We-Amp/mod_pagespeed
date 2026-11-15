// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The `Vary` a response WE build carries (lib/cache/vary_emission.h).
//
// Two properties are load-bearing and they pull in opposite directions:
//
//   * A response served from a cache entry whose ORIGIN negotiated on
//     `Accept` must say so. On a HIT the origin's headers are gone, so if the
//     entry's marker does not reach this function the axis is dropped and a
//     shared cache downstream keys the response without `Accept`.
//
//   * A response whose origin did NOT negotiate on `Accept` must NOT say it
//     does. Adding `Accept` to the CSS/JS row unconditionally would over-key
//     every stylesheet and script at every downstream cache — a hit-rate
//     regression on the whole population, paid to cover a rare member of it.
//
// So the unflagged answers are pinned byte-for-byte against the table this
// replaced, and the flagged answers are pinned to be that value plus `Accept`,
// exactly once.

#include "lib/cache/vary_emission.h"

#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"

namespace pagespeed {
namespace {

// Lowercased list members of a Vary value.
std::vector<std::string> Tokens(std::string_view vary) {
  std::vector<std::string> out;
  while (!vary.empty()) {
    size_t comma = vary.find(',');
    std::string_view token =
        comma == std::string_view::npos ? vary : vary.substr(0, comma);
    vary = comma == std::string_view::npos ? std::string_view()
                                           : vary.substr(comma + 1);
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1);
    }
    if (token.empty()) continue;
    std::string lowered(token);
    for (char& c : lowered) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c | 0x20);
    }
    out.push_back(lowered);
  }
  return out;
}

size_t CountToken(std::string_view vary, std::string_view name) {
  size_t n = 0;
  for (const std::string& t : Tokens(vary)) {
    if (t == name) ++n;
  }
  return n;
}

constexpr ContentType kAllTypes[] = {ContentType::kHtml, ContentType::kCss,
                                     ContentType::kJs, ContentType::kImage,
                                     ContentType::kOther};

// ---------------------------------------------------------------------------
// The unflagged answers ARE the previous per-content-type table, verbatim.
// An extraction that changes what an unmarked response declares is a
// downstream cache-keying change for the entire population; this is the pin
// that says it did not happen.
// ---------------------------------------------------------------------------

TEST(VaryEmission, UnflaggedMatchesThePreviousTableExactly) {
  EXPECT_EQ(VaryForServedResponse(ContentType::kImage, false),
            "Accept, Save-Data, User-Agent, Sec-CH-DPR");
  EXPECT_EQ(VaryForServedResponse(ContentType::kHtml, false),
            "Accept-Encoding, User-Agent");
  EXPECT_EQ(VaryForServedResponse(ContentType::kCss, false), "Accept-Encoding");
  EXPECT_EQ(VaryForServedResponse(ContentType::kJs, false), "Accept-Encoding");
  // The old table had no row for kOther and emitted no header at all.
  EXPECT_TRUE(VaryForServedResponse(ContentType::kOther, false).empty());
}

// ---------------------------------------------------------------------------
// The flagged answers add exactly one axis, and never more than once.
// ---------------------------------------------------------------------------

TEST(VaryEmission, FlaggedAddsAcceptAndNothingElse) {
  for (ContentType ct : kAllTypes) {
    SCOPED_TRACE(static_cast<int>(ct));
    const std::string_view unflagged = VaryForServedResponse(ct, false);
    const std::string_view flagged = VaryForServedResponse(ct, true);

    EXPECT_GE(CountToken(flagged, "accept"), 1u)
        << "a marked entry must declare Accept";

    // Every axis the unflagged answer declared is still declared.
    for (const std::string& t : Tokens(unflagged)) {
      EXPECT_EQ(CountToken(flagged, t), 1u)
          << "flagged answer lost or duplicated '" << t << "'";
    }
    // And nothing beyond `accept` was introduced.
    for (const std::string& t : Tokens(flagged)) {
      if (t == "accept") continue;
      EXPECT_EQ(CountToken(unflagged, t), 1u)
          << "flagged answer introduced an unrelated axis '" << t << "'";
    }
  }
}

TEST(VaryEmission, AcceptIsNeverDuplicated) {
  // kImage is the case that would break: its row already names Accept, so an
  // unconditional append would emit it twice and put a malformed-looking
  // duplicate list member in front of every CDN.
  for (ContentType ct : kAllTypes) {
    SCOPED_TRACE(static_cast<int>(ct));
    EXPECT_LE(CountToken(VaryForServedResponse(ct, true), "accept"), 1u);
    EXPECT_LE(CountToken(VaryForServedResponse(ct, false), "accept"), 1u);
  }
}

TEST(VaryEmission, ImageAnswerIsUnchangedByTheMarker) {
  // Images already negotiate on Accept on our side, so the marker adds no
  // axis. Asserted as string equality (not just token equality) because the
  // serve path emits this value verbatim.
  EXPECT_EQ(VaryForServedResponse(ContentType::kImage, true),
            VaryForServedResponse(ContentType::kImage, false));
}

TEST(VaryEmission, MarkedOtherDeclaresAcceptAlone) {
  // The type we negotiate nothing for. Without the marker there is no header
  // at all; with it, `Accept` is the one axis the response genuinely varies
  // on and it must still be declared.
  EXPECT_EQ(VaryForServedResponse(ContentType::kOther, true), "Accept");
}

// ---------------------------------------------------------------------------
// Shape guarantees the caller relies on.
// ---------------------------------------------------------------------------

TEST(VaryEmission, EmptyOnlyForUnmarkedOther) {
  for (ContentType ct : kAllTypes) {
    SCOPED_TRACE(static_cast<int>(ct));
    EXPECT_FALSE(VaryForServedResponse(ct, true).empty())
        << "a marked entry always has at least Accept to declare";
    if (ct != ContentType::kOther) {
      EXPECT_FALSE(VaryForServedResponse(ct, false).empty());
    }
  }
}

TEST(VaryEmission, ValuesAreWellFormedListsWithNoEmptyMembers) {
  // The caller writes the returned bytes straight into a response header, so
  // a stray leading/trailing comma or an empty member would ship as-is.
  for (ContentType ct : kAllTypes) {
    for (bool flagged : {false, true}) {
      SCOPED_TRACE(static_cast<int>(ct));
      const std::string_view v = VaryForServedResponse(ct, flagged);
      if (v.empty()) continue;
      EXPECT_NE(v.front(), ',');
      EXPECT_NE(v.back(), ',');
      EXPECT_EQ(v.find(",,"), std::string_view::npos);
      EXPECT_EQ(v.find('\r'), std::string_view::npos);
      EXPECT_EQ(v.find('\n'), std::string_view::npos);
      EXPECT_FALSE(Tokens(v).empty());
    }
  }
}

TEST(VaryEmission, ReturnedViewsOutliveTheCall) {
  // The nginx call site stores the pointer in the response header list rather
  // than copying, so the backing bytes must have static storage duration.
  // Two calls must hand back the same address, not a per-call buffer.
  for (ContentType ct : kAllTypes) {
    for (bool flagged : {false, true}) {
      SCOPED_TRACE(static_cast<int>(ct));
      EXPECT_EQ(VaryForServedResponse(ct, flagged).data(),
                VaryForServedResponse(ct, flagged).data());
    }
  }
}

// The image row IS the selection's header list (capability_mask.h): every
// request header the image variant selection reads is declared, so a shared
// cache downstream keys on each dimension the selection uses.
TEST(VaryEmission, ImageRowNamesEverySelectionHeader) {
  EXPECT_EQ(VaryForServedResponse(ContentType::kImage, false),
            kImageSelectionVary);
  const std::string vary(VaryForServedResponse(ContentType::kImage, false));
  for (const char* header :
       {"Accept", "User-Agent", "Sec-CH-DPR", "Save-Data"}) {
    EXPECT_NE(vary.find(header), std::string::npos) << header;
  }
}

// ---------------------------------------------------------------------------
// EmittedVaryNamesAccept: when the line we emit names Accept.
// ---------------------------------------------------------------------------

// Agent optimization off: the rule the serve paths used before — the entry's
// marker, unless an origin line on the response already lists Accept.
TEST(EmittedVaryNamesAccept, AgentOffKeepsThePreviousRule) {
  for (ContentType ct : kAllTypes) {
    SCOPED_TRACE(static_cast<int>(ct));
    EXPECT_TRUE(EmittedVaryNamesAccept(ct, true, false, false));
    EXPECT_FALSE(EmittedVaryNamesAccept(ct, true, true, false));
    EXPECT_FALSE(EmittedVaryNamesAccept(ct, false, false, false));
    EXPECT_FALSE(EmittedVaryNamesAccept(ct, false, true, false));
  }
}

// Agent optimization on: an HTML response names Accept whether or not the
// entry is marked, as long as no origin line already lists it.
TEST(EmittedVaryNamesAccept, AgentOnHtmlNamesAccept) {
  EXPECT_TRUE(EmittedVaryNamesAccept(ContentType::kHtml, false, false, true));
  EXPECT_TRUE(EmittedVaryNamesAccept(ContentType::kHtml, true, false, true));
  EXPECT_EQ(VaryForServedResponse(
                ContentType::kHtml,
                EmittedVaryNamesAccept(ContentType::kHtml, false, false, true)),
            "Accept-Encoding, User-Agent, Accept");
}

// ...and never when an origin line on the response (the MISS passthrough, a
// 304's own line, a replayed stored line) already lists it.
TEST(EmittedVaryNamesAccept, AgentOnNeverDuplicatesTheOriginsAccept) {
  EXPECT_FALSE(EmittedVaryNamesAccept(ContentType::kHtml, false, true, true));
  EXPECT_FALSE(EmittedVaryNamesAccept(ContentType::kHtml, true, true, true));
}

// Only HTML has a Markdown copy: stylesheets, scripts, images and other types
// are keyed exactly as with agent optimization off.
TEST(EmittedVaryNamesAccept, AgentOnLeavesOtherTypesAlone) {
  for (ContentType ct : {ContentType::kCss, ContentType::kJs,
                         ContentType::kImage, ContentType::kOther}) {
    SCOPED_TRACE(static_cast<int>(ct));
    for (bool marker : {false, true}) {
      for (bool lists : {false, true}) {
        EXPECT_EQ(EmittedVaryNamesAccept(ct, marker, lists, true),
                  EmittedVaryNamesAccept(ct, marker, lists, false));
      }
    }
  }
}

// Whatever the inputs, our line plus an origin line that lists Accept never
// names it twice.  (Images are left out: their row names Accept for the
// format selection itself, independent of this decision.)
TEST(EmittedVaryNamesAccept, AcceptAppearsAtMostOnceAcrossBothLines) {
  for (ContentType ct : {ContentType::kHtml, ContentType::kCss,
                         ContentType::kJs, ContentType::kOther}) {
    for (bool marker : {false, true}) {
      for (bool lists : {false, true}) {
        for (bool agent : {false, true}) {
          const size_t ours = CountToken(
              VaryForServedResponse(
                  ct, EmittedVaryNamesAccept(ct, marker, lists, agent)),
              "accept");
          EXPECT_LE(ours + (lists ? 1u : 0u), 1u)
              << static_cast<int>(ct) << marker << lists << agent;
        }
      }
    }
  }
}

// VaryListsAccept: the field Accept as a list member, nothing else.
TEST(VaryListsAccept, FindsTheAcceptMember) {
  EXPECT_TRUE(VaryListsAccept("Accept"));
  EXPECT_TRUE(VaryListsAccept("accept"));
  EXPECT_TRUE(VaryListsAccept("Accept-Encoding, ACCEPT"));
  EXPECT_TRUE(VaryListsAccept(" \tAccept\t ,Origin"));
  EXPECT_TRUE(VaryListsAccept("Accept-Encoding,,Accept"));
}

TEST(VaryListsAccept, IgnoresOtherFields) {
  EXPECT_FALSE(VaryListsAccept(""));
  EXPECT_FALSE(VaryListsAccept("Accept-Encoding"));
  EXPECT_FALSE(VaryListsAccept("Accept-Language, User-Agent"));
  EXPECT_FALSE(VaryListsAccept("*"));
  EXPECT_FALSE(VaryListsAccept("Accepts"));
  EXPECT_FALSE(VaryListsAccept("X-Accept"));
}

}  // namespace
}  // namespace pagespeed
