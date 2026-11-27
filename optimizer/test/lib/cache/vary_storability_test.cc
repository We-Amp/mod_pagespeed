// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The store-side Vary refusal predicate.
//
// This decides whether a response may enter the shared cache at all. Getting
// it wrong in the permissive direction is the expensive one: a response that
// genuinely varies on something we do not key on gets stored once and then
// handed to every subsequent client regardless of what they asked for. So the
// table below is exhaustive about the allowed set, and deliberately paranoid
// about the shapes that could smuggle an unknown field-name past the parser.

#include "lib/cache/vary_storability.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "lib/classify/capability_mask.h"

namespace pagespeed {
namespace {

// --- the allowed set: the one field a stored response is keyed on --------

TEST(VaryStorability, NoVaryIsStorable) { EXPECT_FALSE(VaryUncacheable("")); }

TEST(VaryStorability, ContentCodingIsAllowed) {
  EXPECT_FALSE(VaryUncacheable("Accept-Encoding"));
}

// One original is kept per URL and served to every client, so a response the
// origin picked from one of these request fields would reach clients that sent
// something else.  The capability mask reading a field does not make the
// stored original keyed on it.
TEST(VaryStorability, ClientNegotiatedFieldsAreRefused) {
  EXPECT_TRUE(VaryUncacheable("User-Agent"));
  EXPECT_TRUE(VaryUncacheable("Accept"));
  EXPECT_TRUE(VaryUncacheable("Save-Data"));
  EXPECT_TRUE(VaryUncacheable("Accept-Encoding, User-Agent"));
  EXPECT_TRUE(VaryUncacheable("Accept-Encoding, Accept"));
  EXPECT_TRUE(VaryUncacheable("Save-Data, Accept-Encoding"));
  EXPECT_TRUE(
      VaryUncacheable("Accept-Encoding, User-Agent, Accept, Save-Data"));
}

TEST(VaryStorability, FieldNameMatchingIsCaseInsensitive) {
  EXPECT_FALSE(VaryUncacheable("accept-encoding"));
  EXPECT_FALSE(VaryUncacheable("ACCEPT-ENCODING"));
  EXPECT_FALSE(VaryUncacheable("aCcEpT-eNcOdInG"));
  EXPECT_TRUE(VaryUncacheable("ACCEPT"));
  EXPECT_TRUE(VaryUncacheable("uSeR-aGeNt"));
  EXPECT_TRUE(VaryUncacheable("save-DATA"));
}

// --- the wildcard --------------------------------------------------------

TEST(VaryStorability, StarIsRefused) { EXPECT_TRUE(VaryUncacheable("*")); }

TEST(VaryStorability, StarIsRefusedEvenBesideAllowedFields) {
  // RFC 9110 §12.5.5: `*` means it varies on things the request headers do not
  // even describe. An allowed field beside it does not make it storable.
  EXPECT_TRUE(VaryUncacheable("Accept, *"));
  EXPECT_TRUE(VaryUncacheable("*, Accept-Encoding"));
}

// --- anything else is refused --------------------------------------------

TEST(VaryStorability, UnhandledFieldsAreRefused) {
  EXPECT_TRUE(VaryUncacheable("Cookie"));
  EXPECT_TRUE(VaryUncacheable("Accept-Language"));
  EXPECT_TRUE(VaryUncacheable("Origin"));
  EXPECT_TRUE(VaryUncacheable("X-Custom-Thing"));
}

TEST(VaryStorability, OneUnhandledFieldPoisonsTheWholeList) {
  EXPECT_TRUE(VaryUncacheable("Accept-Encoding, Cookie"));
  EXPECT_TRUE(VaryUncacheable("Cookie, Accept-Encoding"));
  EXPECT_TRUE(VaryUncacheable("Accept, Accept-Language, User-Agent"));
}

TEST(VaryStorability, PrefixOfAnAllowedFieldIsNotAllowed) {
  // Length is compared before content, so a field-name that merely starts
  // like an allowed one must not match it.
  EXPECT_TRUE(VaryUncacheable("Accept-Encodings"));
  EXPECT_TRUE(VaryUncacheable("Accept-"));
  EXPECT_TRUE(VaryUncacheable("Acce"));
  EXPECT_TRUE(VaryUncacheable("User-Agent-Hint"));
}

// --- list syntax ---------------------------------------------------------

TEST(VaryStorability, SurroundingWhitespaceIsTrimmed) {
  // OWS per RFC 9110 §5.6.1: SP and HTAB, on either side of a member.
  EXPECT_FALSE(VaryUncacheable("  Accept-Encoding  "));
  EXPECT_FALSE(VaryUncacheable("\tAccept-Encoding\t"));
  EXPECT_FALSE(VaryUncacheable(" Accept-Encoding ,\taccept-encoding "));
}

TEST(VaryStorability, EmptyListMembersAreSkipped) {
  // An empty member is a syntax artefact, not a field-name; treating it as
  // one would refuse a list that is otherwise entirely allowed.
  EXPECT_FALSE(VaryUncacheable("Accept-Encoding,,Accept-Encoding"));
  EXPECT_FALSE(VaryUncacheable(",Accept-Encoding"));
  EXPECT_FALSE(VaryUncacheable("Accept-Encoding,"));
  EXPECT_FALSE(VaryUncacheable(",,,"));
  EXPECT_FALSE(VaryUncacheable("   "));
}

TEST(VaryStorability, MultipleHeaderLinesAreOneListWhenJoined) {
  // A response may carry several Vary lines; they combine as one comma list
  // (RFC 9110 §5.3). Joined, an unhandled field in the second line must still
  // refuse the response -- which is exactly what a per-line call would miss.
  EXPECT_FALSE(VaryUncacheable("Accept-Encoding,accept-encoding"));
  EXPECT_TRUE(VaryUncacheable("Accept-Encoding,Cookie"));
  EXPECT_TRUE(VaryUncacheable("Accept-Encoding,User-Agent"));
}

// ===========================================================================
// The allowed set is coupled to the mask, in the direction that can hurt
// ===========================================================================

// Every field this predicate ADMITS must be one the capability mask actually
// distinguishes on. If the mask stopped keying on an axis while the allowlist
// kept admitting it, the cache would store one representation of a response
// that genuinely varies and hand it to every client -- the exact failure the
// predicate exists to prevent.
//
// The expectation is derived by driving CapabilityMask::FromHeaders, not by
// comparing against a second copy of the list, which would only prove that two
// hand-written lists match.
//
// The other direction is deliberately NOT asserted: the mask also consumes
// `User-Agent`, `Accept`, `Save-Data` and `Sec-CH-DPR`, which this predicate
// refuses, because the mask keys only the optimizer's own variants and not
// the stored original. See the header.
TEST(VaryStorabilityAxes, EveryAllowedFieldIsOneTheMaskDistinguishesOn) {
  struct Axis {
    const char* field;
    const char* accept;
    const char* user_agent;
    const char* save_data;
    const char* accept_encoding;
  };
  // For each allowed field, two header sets differing ONLY in that field.
  const char* kUaDesktop = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)";
  const Axis base = {"", "image/png", kUaDesktop, "", "identity"};
  const Axis variants[] = {
      {"Accept-Encoding", "image/png", kUaDesktop, "", "br"},
  };

  const uint32_t base_mask =
      CapabilityMask::FromHeaders(base.accept, base.user_agent, base.save_data,
                                  base.accept_encoding)
          .Encode();

  for (const Axis& v : variants) {
    SCOPED_TRACE(v.field);
    // The predicate admits it...
    EXPECT_FALSE(VaryUncacheable(v.field))
        << v.field << " is refused by the predicate but listed as an axis";
    // ...and the mask really does tell the two requests apart.
    const uint32_t variant_mask =
        CapabilityMask::FromHeaders(v.accept, v.user_agent, v.save_data,
                                    v.accept_encoding)
            .Encode();
    EXPECT_NE(variant_mask, base_mask)
        << v.field
        << " is admitted by the storability predicate, but the capability "
           "mask does not distinguish on it -- responses varying on it would "
           "be stored under a key that cannot tell the variants apart";
  }
}

// ===========================================================================
// Equivalence with the front stage's previous inline implementation
// ===========================================================================
//
// This predicate used to live inside the nginx module as a static function.
// Consolidating it is only safe if the answer did not move for a single input
// -- a storability decision that shifts is a cache that starts serving the
// wrong representation, with nothing to notice it by.
//
// So the old implementation is transcribed below, verbatim, as an oracle, and
// the two are compared over every shape the parser can be handed. The one
// substitution the extraction actually made is the case fold: the module used
// nginx's ngx_strncasecmp, which is transcribed here too rather than assumed
// equivalent, because "it's just tolower" is exactly the kind of assumption
// that hides a difference on the bytes either side of the A-Z range.

namespace legacy {

// Transcribed from nginx's ngx_strncasecmp: folds ONLY A-Z, compares as
// unsigned, and stops early on a shared NUL.
int NgxStrncasecmp(const unsigned char* s1, const unsigned char* s2, size_t n) {
  while (n != 0) {
    unsigned int c1 = *s1++;
    unsigned int c2 = *s2++;
    c1 = (c1 >= 'A' && c1 <= 'Z') ? (c1 | 0x20) : c1;
    c2 = (c2 >= 'A' && c2 <= 'Z') ? (c2 | 0x20) : c2;
    if (c1 == c2) {
      if (c1 != 0) {
        n--;
        continue;
      }
      return 0;
    }
    return static_cast<int>(c1) - static_cast<int>(c2);
  }
  return 0;
}

// The pre-extraction body, unchanged except for taking the already-combined
// Vary value the caller used to build inline, and for its allowed set, which
// is the current one.  The set moved on purpose when `User-Agent`, `Accept`
// and `Save-Data` stopped being stored; what this oracle pins is that the
// PARSING did not move.
bool VaryUncacheableInline(const std::string& combined) {
  if (combined.empty()) {
    return false;
  }
  std::string_view val(combined);

  static constexpr std::string_view kAllowed[] = {
      "Accept-Encoding",
  };

  while (!val.empty()) {
    size_t comma = val.find(',');
    std::string_view token;
    if (comma == std::string_view::npos) {
      token = val;
      val = {};
    } else {
      token = val.substr(0, comma);
      val.remove_prefix(comma + 1);
    }
    auto is_ows = [](char c) { return c == ' ' || c == '\t'; };
    while (!token.empty() && is_ows(token.front())) token.remove_prefix(1);
    while (!token.empty() && is_ows(token.back())) token.remove_suffix(1);
    if (token.empty()) continue;
    if (token == "*") return true;

    bool allowed = false;
    for (const auto& a : kAllowed) {
      if (token.size() == a.size() &&
          NgxStrncasecmp(reinterpret_cast<const unsigned char*>(token.data()),
                         reinterpret_cast<const unsigned char*>(a.data()),
                         a.size()) == 0) {
        allowed = true;
        break;
      }
    }
    if (!allowed) {
      return true;
    }
  }
  return false;
}

}  // namespace legacy

TEST(VaryStorabilityEquivalence, AgreesWithTheInlineImplementationEverywhere) {
  // Built from the pieces a real Vary is made of, plus the ones that break
  // parsers: separators, whitespace, the wildcard, case variants, near-misses
  // on the allowed names, and bytes adjacent to A-Z in ASCII (`@`, `[`, and
  // the backtick / `{` on the lowercase side) where a sloppy fold goes wrong.
  const std::vector<std::string> pieces = {
      "",
      " ",
      "\t",
      ",",
      "*",
      "Accept",
      "accept",
      "ACCEPT",
      "Accept-Encoding",
      "accept-encoding",
      "Accept-Encodings",
      "Accept-",
      "User-Agent",
      "user-agent",
      "User-Agent-Hint",
      "Save-Data",
      "save-data",
      "SAVE-DATA",
      "Cookie",
      "Accept-Language",
      "Origin",
      "X-Thing",
      "@ccept",
      "[ccept",
      "`ccept",
      "{ccept",
      "Acce",
      "  Accept  ",
      "\tSave-Data\t",
      // The ONE input class where the two folds could actually disagree.
      // nginx's comparison returns early when both bytes are NUL; ours does
      // not. The divergence is unreachable in production -- no allowlist entry
      // contains a NUL, so "both NUL at the same index" cannot arise -- but
      // demonstrating that is the test's job, not assuming it.
      std::string("Acc\0pt", 6),
      std::string("Accept\0", 7),
      std::string("\0", 1),
      std::string("Accept-Encodin\0", 15),
      // High-bit bytes: outside A-Z, so neither fold touches them. Included
      // for the same reason -- provably equivalent, cheaply demonstrated.
      "Acc\xC3\xA9pt",
      "\x80\x81",
  };

  size_t compared = 0;
  for (const std::string& a : pieces) {
    for (const std::string& b : pieces) {
      for (const char* sep : {"", ",", ", ", " , ", ",,"}) {
        std::string input = a;
        input += sep;
        input += b;
        EXPECT_EQ(VaryUncacheable(input), legacy::VaryUncacheableInline(input))
            << "diverged on: [" << input << "]";
        ++compared;
      }
    }
  }
  // Guard against the loop silently collapsing to nothing.
  EXPECT_EQ(compared, pieces.size() * pieces.size() * 5);
}

TEST(VaryStorabilityEquivalence, AgreesOnThreeMemberLists) {
  const std::vector<std::string> members = {
      "Accept", "Accept-Encoding", "User-Agent", "Save-Data", "Cookie", "*", "",
      " ",
  };
  for (const std::string& a : members) {
    for (const std::string& b : members) {
      for (const std::string& c : members) {
        std::string input = a;
        input += ", ";
        input += b;
        input += ',';
        input += c;
        EXPECT_EQ(VaryUncacheable(input), legacy::VaryUncacheableInline(input))
            << "diverged on: [" << input << "]";
      }
    }
  }
}

// ===========================================================================
// The store/notify seam
// ===========================================================================
//
// The refusal above is only half of what a response's Vary decides. The other
// half is whether the stored original may be handed to the optimizer at all,
// and that half used to have NO Vary term: the store path admitted
// `Vary: Accept` and the notify path never looked at Vary, so those responses
// were stored AND optimized. The optimizer then derived a whole variant
// family from whichever representation the FIRST requester's Accept elicited
// from the origin, and every later client -- including ones that asked the
// origin for something else -- was served from it.
//
// Both answers now come from one call over one parse, which is what makes
// "they disagree" unrepresentable rather than merely fixed.

TEST(VaryStoreVerdictTest, NoVaryStoresAndOptimizes) {
  const VaryStoreVerdict v = ClassifyVaryForStore("");
  EXPECT_TRUE(v.storable);
  EXPECT_FALSE(v.varies_accept);
}

TEST(VaryStoreVerdictTest, AcceptIsRefusedAndNeverMarked) {
  // An origin that picks its answer from `Accept` is no longer stored: the one
  // stored copy would be the first requester's answer, served to everyone.
  for (const char* input : {
           "Accept",
           "Accept, Accept-Encoding",
           "Accept-Encoding, Accept",
           "User-Agent, Accept, Save-Data",
           "  accept  ",
           "ACCEPT",
           "aCcEpT",
           "\tAccept\t,User-Agent",
           "Accept-Encoding,,Accept",
       }) {
    SCOPED_TRACE(input);
    const VaryStoreVerdict v = ClassifyVaryForStore(input);
    EXPECT_FALSE(v.storable);
    EXPECT_FALSE(v.varies_accept);
  }
}

TEST(VaryStoreVerdictTest, AcceptEncodingIsStoredAndNotMarked) {
  // The near-miss that matters most: `Accept-Encoding` is on the allowed list
  // and shares a prefix with `Accept`.
  for (const char* input : {
           "Accept-Encoding",
           "accept-encoding, ACCEPT-ENCODING",
       }) {
    SCOPED_TRACE(input);
    const VaryStoreVerdict v = ClassifyVaryForStore(input);
    EXPECT_TRUE(v.storable);
    EXPECT_FALSE(v.varies_accept);
  }
  for (const char* input : {"Accept-Encodin", "Accept-"}) {
    SCOPED_TRACE(input);
    EXPECT_FALSE(ClassifyVaryForStore(input).varies_accept);
  }
}

TEST(VaryStoreVerdictTest, UserAgentAndSaveDataAreRefused) {
  for (const char* input :
       {"User-Agent", "Save-Data", "Accept-Encoding, User-Agent, Save-Data"}) {
    SCOPED_TRACE(input);
    const VaryStoreVerdict v = ClassifyVaryForStore(input);
    EXPECT_FALSE(v.storable);
    EXPECT_FALSE(v.varies_accept);
  }
}

// The stricter refusal keeps its semantics: a Vary this cache cannot key on
// still refuses the response outright, and an `Accept` sitting beside it does
// NOT downgrade that refusal into "store it but don't optimize it".
TEST(VaryStoreVerdictTest, RefusalWinsOverTheAcceptMarker) {
  for (const char* input : {
           "*",
           "Accept, *",
           "*, Accept",
           "Accept, Cookie",
           "Cookie, Accept",
           "Accept, Accept-Language",
           "Accept, Origin",
           "Accept, Sec-CH-DPR",
       }) {
    SCOPED_TRACE(input);
    const VaryStoreVerdict v = ClassifyVaryForStore(input);
    EXPECT_FALSE(v.storable);
    EXPECT_FALSE(v.varies_accept)
        << "a refused response has no entry to mark, and marking one would "
           "read as 'stored but not optimized' at the call site";
  }
}

// The documented invariant, over the same input space the equivalence oracle
// covers: `storable` is exactly the negation of the exported refusal, and
// `varies_accept` never claims a response that is not stored.
TEST(VaryStoreVerdictTest, InvariantsHoldOverTheWholeInputSpace) {
  const std::vector<std::string> members = {
      "Accept",     "Accept-Encoding",
      "User-Agent", "Save-Data",
      "Cookie",     "*",
      "",           " ",
      "accept",     "Accept-Encodings",
      "Origin",     "\tAccept ",
  };
  size_t compared = 0;
  for (const std::string& a : members) {
    for (const std::string& b : members) {
      for (const char* sep : {",", ", ", " , ", ",,"}) {
        std::string input = a;
        input += sep;
        input += b;
        SCOPED_TRACE(input);
        const VaryStoreVerdict v = ClassifyVaryForStore(input);
        EXPECT_EQ(v.storable, !VaryUncacheable(input));
        if (v.varies_accept) {
          EXPECT_TRUE(v.storable);
        }
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, members.size() * members.size() * 4);
}

}  // namespace
}  // namespace pagespeed
