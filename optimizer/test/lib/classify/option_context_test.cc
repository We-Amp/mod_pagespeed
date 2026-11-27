// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "lib/classify/option_context.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace pagespeed {
namespace {

// The shared golden file's own SHA-256, over its bytes with every CR removed.
//
// This file is a contract with the module that produces option contexts, and it
// is checked into both trees. Nothing in either build system can see the other
// copy, so each side pins the one it has; an edit to one alone fails there. The
// CR stripping keeps a CRLF checkout on Windows from reading as a divergence.
//
// If a deliberate format change edits the file, recompute with
//   tr -d '\r' < option_context_goldens.txt | shasum -a 256
// and make the identical edit and update on BOTH sides.
constexpr std::string_view kGoldenFileSha256 =
    "4435bdd8b4969c44664c2370c419e5cc63560c86c9bbe9366ca6c7bcd60be6e0";

// Counted rather than assumed: a parser that silently read zero vectors would
// make every golden assertion below pass without checking anything.
constexpr size_t kExpectedGoldenVectors = 16;

constexpr std::string_view kGoldenPath =
    "test/lib/classify/testdata/option_context_goldens.txt";

struct GoldenVector {
  std::string recipe;  // "-" for a format-only vector; a construction name
                       // otherwise. Meaningful only to the producing side,
                       // which can build the configuration it names; carried
                       // here so both copies of the file stay byte-identical.
  std::string payload;
  std::string signature;
};

bool Unescape(std::string_view in, std::string* out) {
  out->clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') {
      out->push_back(in[i]);
      continue;
    }
    if (i + 1 >= in.size()) return false;
    if (in[i + 1] == '\\') {
      out->push_back('\\');
      i += 1;
      continue;
    }
    if (in[i + 1] != 'x' || i + 3 >= in.size()) return false;
    int value = 0;
    for (int digit = 0; digit < 2; ++digit) {
      const char c = in[i + 2 + digit];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value |= c - 'a' + 10;
      } else {
        return false;
      }
    }
    out->push_back(static_cast<char>(value));
    i += 3;
  }
  return true;
}

std::string ReadGoldensStrippingCr() {
  // Bazel runs a test with the runfiles root as the working directory, so the
  // workspace-relative path resolves directly.
  std::ifstream in(std::string(kGoldenPath), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  std::string contents = buffer.str();
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'),
                 contents.end());
  return contents;
}

std::vector<GoldenVector> ParseGoldens(std::string_view contents) {
  std::vector<GoldenVector> vectors;
  size_t start = 0;
  while (start <= contents.size()) {
    const size_t nl = contents.find('\n', start);
    const std::string_view line = contents.substr(
        start,
        nl == std::string_view::npos ? std::string_view::npos : nl - start);
    start = (nl == std::string_view::npos) ? contents.size() + 1 : nl + 1;
    if (line.empty() || line[0] == '#') continue;
    const size_t tab1 = line.find('\t');
    if (tab1 == std::string_view::npos) continue;
    const size_t tab2 = line.find('\t', tab1 + 1);
    if (tab2 == std::string_view::npos) continue;
    GoldenVector vector;
    vector.recipe = std::string(line.substr(0, tab1));
    if (!Unescape(line.substr(tab1 + 1, tab2 - tab1 - 1), &vector.payload)) {
      continue;
    }
    vector.signature = std::string(line.substr(tab2 + 1));
    vectors.push_back(vector);
  }
  return vectors;
}

// The `# bound: N` line. The producing side parses the same line out of its own
// copy and compares it against its own constant, which is what turns the shared
// ceiling from a review convention into something a build checks.
bool ParseGoldenBound(std::string_view contents, size_t* out) {
  size_t start = 0;
  const std::string_view marker = "# bound: ";
  while (start <= contents.size()) {
    const size_t nl = contents.find('\n', start);
    const std::string_view line = contents.substr(
        start,
        nl == std::string_view::npos ? std::string_view::npos : nl - start);
    start = (nl == std::string_view::npos) ? contents.size() + 1 : nl + 1;
    if (line.size() <= marker.size() || line.substr(0, marker.size()) != marker)
      continue;
    size_t value = 0;
    const std::string_view digits = line.substr(marker.size());
    if (digits.empty()) return false;
    for (const char c : digits) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<size_t>(c - '0');
    }
    if (value == 0) return false;
    *out = value;
    return true;
  }
  return false;
}

std::string EmptyPayload() {
  return std::string(kOptionContextFormatVersion) + "\n";
}

// ---------------------------------------------------------------------------
// The golden vectors, and the shared-file contract behind them.
// ---------------------------------------------------------------------------

TEST(OptionContextTest, GoldenFileIsTheSharedCopy) {
  const std::string contents = ReadGoldensStrippingCr();
  ASSERT_FALSE(contents.empty())
      << "golden file missing or empty; expected it at " << kGoldenPath;
  EXPECT_EQ(kGoldenFileSha256, OptionContextSignature(contents))
      << "The shared golden file has changed. If that was deliberate, make the "
         "identical edit in the module repository and update the pinned hash "
         "on BOTH sides.";
}

TEST(OptionContextTest, GoldenVectorsSignAsRecorded) {
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldensStrippingCr());
  ASSERT_EQ(kExpectedGoldenVectors, vectors.size())
      << "parsed a different number of vectors than the file carries";
  for (size_t i = 0; i < vectors.size(); ++i) {
    EXPECT_EQ(vectors[i].signature, OptionContextSignature(vectors[i].payload))
        << "vector " << i;
  }
}

TEST(OptionContextTest, EveryGoldenVectorValidatesAgainstItsOwnSignature) {
  // The vectors are not just hash fixtures: each is a payload this side must
  // ACCEPT when a peer sends it with that signature.
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldensStrippingCr());
  ASSERT_EQ(kExpectedGoldenVectors, vectors.size());
  for (size_t i = 0; i < vectors.size(); ++i) {
    EXPECT_EQ(OptionContextStatus::kOk,
              ValidateOptionContext(vectors[i].payload, vectors[i].signature))
        << "vector " << i << " was refused";
  }
}

TEST(OptionContextTest, GoldenFileCarriesTheSharedBound) {
  // kMaxOptionContextBytes is a constant of the CONTRACT, not of this build:
  // the producing side derives it from its own option table and this side has
  // to accept exactly what that side will emit. Nothing links the two
  // declarations, so the shared file carries the number and both sides check
  // it. A one-sided raise now fails here instead of surfacing as a peer whose
  // notifications are refused for being too large.
  size_t bound = 0;
  ASSERT_TRUE(ParseGoldenBound(ReadGoldensStrippingCr(), &bound))
      << "the shared golden file has no `# bound:` line";
  EXPECT_EQ(kMaxOptionContextBytes, bound)
      << "this build's kMaxOptionContextBytes disagrees with the shared "
         "golden file; both sides and the file must move together";
}

TEST(OptionContextTest, GoldenFileStillCarriesConstructionRecipes) {
  // This side cannot BUILD a configuration -- it has no options table, by
  // design. The recipes are checked by the producing side. What is asserted
  // here is only that they are still present, so that a change which quietly
  // reduced the file to hash-of-a-literal vectors is visible from both copies.
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldensStrippingCr());
  ASSERT_EQ(kExpectedGoldenVectors, vectors.size());
  size_t recipes = 0;
  for (const GoldenVector& v : vectors) {
    if (v.recipe != "-") ++recipes;
  }
  EXPECT_EQ(7u, recipes)
      << "the shared golden file stopped carrying construction recipes";
}

TEST(OptionContextTest, DefaultSignatureConstantMatchesItsOwnPayload) {
  // The literal in the header is load-bearing across two repositories, so it is
  // checked against a locally computed hash rather than trusted.
  EXPECT_EQ(kDefaultOptionContextSignature,
            OptionContextSignature(EmptyPayload()));

  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldensStrippingCr());
  ASSERT_FALSE(vectors.empty());
  EXPECT_EQ(kDefaultOptionContextSignature, vectors[0].signature)
      << "the first golden vector is the default context by convention";
}

// ---------------------------------------------------------------------------
// The signature function itself.
// ---------------------------------------------------------------------------

TEST(OptionContextTest, SignatureIsSixtyFourLowercaseHexCharacters) {
  const std::string signature = OptionContextSignature("anything");
  ASSERT_EQ(kOptionContextSignatureChars, signature.size());
  for (const char c : signature) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    EXPECT_TRUE(ok) << "non-lowercase-hex character in " << signature;
  }
}

TEST(OptionContextTest, SignatureIsSensitiveToEveryByte) {
  const std::string base = "psoc1\no:ImageInlineMaxBytes=3072\n";
  std::string altered = base;
  altered[altered.size() - 2] = '3';  // 3072 -> 3073
  EXPECT_NE(OptionContextSignature(base), OptionContextSignature(altered));
}

TEST(OptionContextTest, EmptyStringHashesButIsNotTheDefaultContext) {
  // The empty STRING is not the empty CONTEXT: the latter still carries its
  // format version token. Confusing the two would make an unversioned payload
  // masquerade as the default.
  EXPECT_NE(OptionContextSignature(""),
            std::string(kDefaultOptionContextSignature));
}

// ---------------------------------------------------------------------------
// IsDefaultOptionContext.
// ---------------------------------------------------------------------------

TEST(OptionContextTest, AbsentAndEmptyContextsAreBothTheDefault) {
  EXPECT_TRUE(IsDefaultOptionContext(""));
  EXPECT_TRUE(IsDefaultOptionContext(kDefaultOptionContextSignature));
}

TEST(OptionContextTest, AnyOtherSignatureIsNotTheDefault) {
  EXPECT_FALSE(IsDefaultOptionContext(
      OptionContextSignature("psoc1\no:ImageInlineMaxBytes=3072\n")));
  // Not even one that differs in a single character.
  std::string near_default(kDefaultOptionContextSignature);
  near_default[0] = (near_default[0] == 'a') ? 'b' : 'a';
  EXPECT_FALSE(IsDefaultOptionContext(near_default))
      << "the comparison must be exact, with no nearest fit";
}

// ---------------------------------------------------------------------------
// Validation, including the size bound and its failure mode.
// ---------------------------------------------------------------------------

TEST(OptionContextTest, NeitherHalfPresentIsTheOrdinaryCaseNotAnError) {
  EXPECT_EQ(OptionContextStatus::kEmpty, ValidateOptionContext("", ""));
}

TEST(OptionContextTest, AWellFormedPairIsAccepted) {
  const std::string payload = "psoc1\nf:ce\no:ImageInlineMaxBytes=3072\n";
  EXPECT_EQ(OptionContextStatus::kOk,
            ValidateOptionContext(payload, OptionContextSignature(payload)));
}

TEST(OptionContextTest, OversizedPayloadIsRefusedAndNothingIsTruncated) {
  const std::string payload =
      EmptyPayload() + std::string(kMaxOptionContextBytes, 'x');
  ASSERT_GT(payload.size(), kMaxOptionContextBytes);
  EXPECT_EQ(OptionContextStatus::kTooLarge,
            ValidateOptionContext(payload, OptionContextSignature(payload)))
      << "an oversized context must be refused whole; there is no truncated "
         "form because half a payload signs as a context that does not exist";
}

TEST(OptionContextTest, APayloadExactlyAtTheBoundIsAccepted) {
  // The bound is inclusive, and the boundary is asserted rather than left to
  // whichever comparison somebody typed.
  std::string payload = EmptyPayload();
  payload.append(std::string(kMaxOptionContextBytes - payload.size(), 'x'));
  ASSERT_EQ(kMaxOptionContextBytes, payload.size());
  EXPECT_EQ(OptionContextStatus::kOk,
            ValidateOptionContext(payload, OptionContextSignature(payload)));
}

TEST(OptionContextTest, APayloadFromAnotherFormatVersionIsRefusedWhole) {
  const std::string payload = "psoc2\no:SomethingNew=1\n";
  EXPECT_EQ(OptionContextStatus::kUnknownFormat,
            ValidateOptionContext(payload, OptionContextSignature(payload)))
      << "a later format must be refused, not read best-effort: the signature "
         "is a cache key, and a misread payload keys work under a name that "
         "does not describe it";
}

TEST(OptionContextTest, AVersionTokenWithoutItsNewlineIsNotAValidOpening) {
  // "psoc10" starts with "psoc1" — a prefix sniff would accept it.
  const std::string payload = "psoc10\n";
  EXPECT_EQ(OptionContextStatus::kUnknownFormat,
            ValidateOptionContext(payload, OptionContextSignature(payload)));
}

TEST(OptionContextTest, APayloadWithNoSignatureIsRefused) {
  EXPECT_EQ(OptionContextStatus::kMalformedSignature,
            ValidateOptionContext(EmptyPayload(), ""));
}

TEST(OptionContextTest, ASignatureOfTheWrongShapeIsRefused) {
  const std::string payload = EmptyPayload();
  EXPECT_EQ(OptionContextStatus::kMalformedSignature,
            ValidateOptionContext(payload, "deadbeef"));
  // Uppercase hex is the wrong shape too: the rendering is part of the
  // contract, and accepting both spellings would mean one context with two
  // names -- so the byte-for-byte comparison that detects format drift would
  // stop meaning anything.
  std::string upper = OptionContextSignature(payload);
  upper[0] = static_cast<char>(std::toupper(upper[0]));
  if (upper != OptionContextSignature(payload)) {
    EXPECT_EQ(OptionContextStatus::kMalformedSignature,
              ValidateOptionContext(payload, upper));
  }
}

TEST(OptionContextTest, ASignatureThatDoesNotMatchItsPayloadIsRefused) {
  const std::string payload = "psoc1\no:ImageInlineMaxBytes=3072\n";
  const std::string other = "psoc1\no:ImageInlineMaxBytes=4096\n";
  EXPECT_EQ(OptionContextStatus::kSignatureMismatch,
            ValidateOptionContext(payload, OptionContextSignature(other)))
      << "the signature is re-derived, not trusted: this is the arm that turns "
         "a silent divergence between the two implementations of this format "
         "into a counted refusal on the first message";
}

TEST(OptionContextTest, EverySignatureStatusHasADistinctName) {
  const OptionContextStatus all[] = {OptionContextStatus::kOk,
                                     OptionContextStatus::kEmpty,
                                     OptionContextStatus::kTooLarge,
                                     OptionContextStatus::kUnknownFormat,
                                     OptionContextStatus::kMalformedSignature,
                                     OptionContextStatus::kSignatureMismatch};
  std::vector<std::string> names;
  for (const OptionContextStatus status : all) {
    const std::string name(OptionContextStatusName(status));
    EXPECT_FALSE(name.empty());
    EXPECT_NE("unknown", name) << "a status fell through the switch";
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names.end(), std::unique(names.begin(), names.end()))
      << "two statuses share a name, so a log cannot tell them apart";
}

}  // namespace
}  // namespace pagespeed
