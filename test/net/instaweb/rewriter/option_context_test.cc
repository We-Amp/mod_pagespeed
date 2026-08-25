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

#include "net/instaweb/rewriter/public/option_context.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/net/instaweb/rewriter/option_context_worst_case.h"
#include "test/net/instaweb/rewriter/rewrite_options_test_base.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

const char kGoldenPath[] =
    "/test/net/instaweb/rewriter/testdata/option_context_goldens.txt";

// The shared golden file's own SHA-256, over its bytes with every CR removed.
//
// The file is a contract between two repositories and has to stay byte-
// identical in both. Nothing in either build system can see the other copy, so
// each side pins the file it has; editing one alone fails there. The CR
// stripping is so that a CRLF checkout on Windows is not mistaken for a
// divergence.
//
// If a deliberate format change edits the file, recompute this with:
//   tr -d '\r' < option_context_goldens.txt | shasum -a 256
// and make the identical edit and update in the optimizer repository.
const char kGoldenFileSha256[] =
    "4435bdd8b4969c44664c2370c419e5cc63560c86c9bbe9366ca6c7bcd60be6e0";

// The number of vectors the file is expected to carry. Counted, not asserted as
// a literal elsewhere: a parser bug that silently read zero vectors would
// otherwise let every golden assertion vacuously pass.
const size_t kExpectedGoldenVectors = 16;

// How many of those name a construction recipe rather than a bare byte string.
// Counted for the same reason the total is: a file that quietly lost its
// recipes would leave only proof that SHA-256 works.
const size_t kExpectedGoldenRecipes = 7;

struct GoldenVector {
  GoogleString recipe;  // "-" for a format-only vector.
  GoogleString payload;
  GoogleString signature;
};

// Reverses the file's escape: "\\" is a backslash, "\xHH" is one byte.
bool Unescape(StringPiece in, GoogleString* out) {
  out->clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '\\') {
      out->push_back(in[i]);
      continue;
    }
    if (i + 1 >= in.size()) {
      return false;
    }
    if (in[i + 1] == '\\') {
      out->push_back('\\');
      i += 1;
      continue;
    }
    if (in[i + 1] != 'x' || i + 3 >= in.size()) {
      return false;
    }
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

GoogleString ReadGoldenFileStrippingCr() {
  const GoogleString path = StrCat(GTestSrcDir(), kGoldenPath);
  std::ifstream in(path.c_str(), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  GoogleString contents = buffer.str();
  contents.erase(std::remove(contents.begin(), contents.end(), '\r'),
                 contents.end());
  return contents;
}

std::vector<GoldenVector> ParseGoldens(StringPiece contents) {
  std::vector<GoldenVector> vectors;
  StringPieceVector lines;
  SplitStringPieceToVector(contents, "\n", &lines, false);
  for (size_t i = 0; i < lines.size(); ++i) {
    StringPiece line = lines[i];
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t tab1 = line.find('\t');
    if (tab1 == StringPiece::npos) {
      continue;
    }
    const size_t tab2 = line.find('\t', tab1 + 1);
    if (tab2 == StringPiece::npos) {
      continue;
    }
    GoldenVector vector;
    line.substr(0, tab1).CopyToString(&vector.recipe);
    if (!Unescape(line.substr(tab1 + 1, tab2 - tab1 - 1), &vector.payload)) {
      continue;
    }
    line.substr(tab2 + 1).CopyToString(&vector.signature);
    vectors.push_back(vector);
  }
  return vectors;
}

// The `# bound: N` line. Shared with the consuming component, which parses the
// same line out of its own copy and compares against its own constant.
bool ParseGoldenBound(StringPiece contents, size_t* out) {
  StringPieceVector lines;
  SplitStringPieceToVector(contents, "\n", &lines, false);
  for (size_t i = 0; i < lines.size(); ++i) {
    StringPiece line = lines[i];
    const StringPiece marker("# bound: ");
    if (!line.starts_with(marker)) {
      continue;
    }
    int value = 0;
    if (!StringToInt(line.substr(marker.size()).as_string(), &value) ||
        value <= 0) {
      return false;
    }
    *out = static_cast<size_t>(value);
    return true;
  }
  return false;
}

// Reaches set_default_x_header_value, which is protected on RewriteOptions.
// It changes the SHARED Property, not this instance, which is exactly the
// lever ChangingAnUnsetOptionsDefaultDoesNotMoveTheSignature needs: it is what
// a later release does when it revises a default.
class DefaultMutatingOptions : public RewriteOptions {
 public:
  explicit DefaultMutatingOptions(ThreadSystem* thread_system)
      : RewriteOptions(thread_system) {}
  using RewriteOptions::set_default_x_header_value;
};

}  // namespace

class OptionContextTest : public RewriteOptionsTestBase<RewriteOptions> {
 protected:
  OptionContextTest() : options_(thread_system_.get()) {}

  GoogleString PayloadOf(const RewriteOptions& options) {
    GoogleString payload;
    EXPECT_EQ(OptionContextStatus::kOk,
              OptionContext::Serialize(options, &payload));
    return payload;
  }

  // The payload of an options object nobody has touched.
  //
  // It is NOT just the format version token: kHtmlWriterFilter ("hw") is on at
  // every rewrite level, including kPassThrough, because it is the structural
  // HTML writer rather than an optimization. So the baseline is a fact about
  // this build, and tests below assert what a change ADDS to it rather than
  // pinning a literal that a future always-on filter would silently rot.
  GoogleString BaselinePayload() {
    std::unique_ptr<RewriteOptions> untouched(NewOptions());
    return PayloadOf(*untouched);
  }

  // The records present in `payload` and absent from the baseline, in payload
  // order, each still terminated by its newline.
  GoogleString RecordsAddedTo(const GoogleString& payload) {
    // NAMED, not inlined into the call below. SplitStringPieceToVector stores
    // StringPieces INTO its input, so passing BaselinePayload() directly
    // leaves every piece dangling into a temporary that died at the end of
    // that full-expression -- which ASan reports as stack-use-after-scope and
    // which, unsanitized, shows up as this test failing a few runs in ten.
    const GoogleString baseline = BaselinePayload();
    StringPieceVector baseline_lines;
    SplitStringPieceToVector(baseline, "\n", &baseline_lines, true);
    StringPieceVector lines;
    SplitStringPieceToVector(payload, "\n", &lines, true);
    GoogleString added;
    for (size_t i = 0; i < lines.size(); ++i) {
      bool in_baseline = false;
      for (size_t j = 0; j < baseline_lines.size(); ++j) {
        if (baseline_lines[j] == lines[i]) {
          in_baseline = true;
          break;
        }
      }
      if (!in_baseline) {
        StrAppend(&added, lines[i], "\n");
      }
    }
    return added;
  }

  RewriteOptions options_;
};

// ---------------------------------------------------------------------------
// The golden vectors, and the shared-file contract behind them.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest, GoldenFileIsTheSharedCopy) {
  const GoogleString contents = ReadGoldenFileStrippingCr();
  ASSERT_FALSE(contents.empty())
      << "golden file missing or empty; expected it at "
      << StrCat(GTestSrcDir(), kGoldenPath);
  EXPECT_STREQ(kGoldenFileSha256, OptionContext::Signature(contents).c_str())
      << "The shared golden file has changed. If that was deliberate, make "
         "the identical edit in the optimizer repository and update the "
         "pinned hash on BOTH sides.";
}

TEST_F(OptionContextTest, GoldenVectorsSignAsRecorded) {
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldenFileStrippingCr());
  ASSERT_EQ(kExpectedGoldenVectors, vectors.size())
      << "parsed a different number of vectors than the file carries; a "
         "parser that silently read none would make every case below pass";
  for (size_t i = 0; i < vectors.size(); ++i) {
    EXPECT_STREQ(vectors[i].signature.c_str(),
                 OptionContext::Signature(vectors[i].payload).c_str())
        << "vector " << i << " payload=[" << vectors[i].payload << "]";
  }
}

// The recipes the golden file names.
//
// THIS IS THE HALF THAT PINS THE MAPPING RATHER THAN THE HASH. The
// format-only vectors below prove that SHA-256 is SHA-256; they would survive
// an option being renamed, a record prefix changing, or the sort order moving,
// because they are hand-written byte strings that never touch RewriteOptions.
// These do: each names a construction, the test performs it, serializes, and
// requires the bytes AND the signature to match what is checked in. Rename
// ImageInlineMaxBytes, change 'o:' to something else, or stop sorting, and
// these fail.
void ApplyRecipe(StringPiece recipe, RewriteOptions* options) {
  if (recipe == "untouched") {
    // Nothing: pins that "no configuration" is not the empty payload.
  } else if (recipe == "inline-max-bytes") {
    options->set_image_inline_max_bytes(3072);
    options->set_js_inline_max_bytes(2048);
  } else if (recipe == "four-filters") {
    options->EnableFilter(RewriteOptions::kInlineImages);
    options->EnableFilter(RewriteOptions::kAddInstrumentation);
    options->EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
    options->EnableFilter(RewriteOptions::kExtendCacheCss);
  } else if (recipe == "redacted-signing-key") {
    options->set_url_signing_key("hunter2");
  } else if (recipe == "escaped-x-header") {
    GoogleString awkward;
    awkward.push_back('a');
    awkward.push_back('\\');
    awkward.push_back('b');
    awkward.push_back('\n');
    awkward.push_back('c');
    awkward.push_back(static_cast<char>(0x80));
    options->set_x_header_value(awkward);
  } else if (recipe == "empty-x-header") {
    options->set_x_header_value("");
  } else if (recipe == "mixed") {
    options->EnableFilter(RewriteOptions::kInlineImages);
    options->set_image_inline_max_bytes(3072);
    options->set_url_signing_key("hunter2");
  } else {
    ADD_FAILURE() << "golden file names a recipe this test does not implement: "
                  << recipe;
  }
}

TEST_F(OptionContextTest, DefaultSignatureIsTheSignatureOfTheEmptyPayload) {
  const GoogleString empty_payload = StrCat(kOptionContextFormatVersion, "\n");
  EXPECT_STREQ(OptionContext::Signature(empty_payload).c_str(),
               OptionContext::DefaultSignature().c_str());

  // And it is the file's first vector, which is where the peer reads it from.
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldenFileStrippingCr());
  ASSERT_FALSE(vectors.empty());
  EXPECT_STREQ(vectors[0].signature.c_str(),
               OptionContext::DefaultSignature().c_str());
}

TEST_F(OptionContextTest, GoldenRecipesSerializeToTheRecordedBytes) {
  const std::vector<GoldenVector> vectors =
      ParseGoldens(ReadGoldenFileStrippingCr());
  ASSERT_EQ(kExpectedGoldenVectors, vectors.size());

  size_t recipes_checked = 0;
  for (size_t i = 0; i < vectors.size(); ++i) {
    if (vectors[i].recipe == "-") {
      continue;
    }
    ++recipes_checked;
    SCOPED_TRACE(vectors[i].recipe);
    std::unique_ptr<RewriteOptions> options(NewOptions());
    ApplyRecipe(vectors[i].recipe, options.get());

    GoogleString payload;
    ASSERT_EQ(OptionContextStatus::kOk,
              OptionContext::Serialize(*options, &payload));
    EXPECT_STREQ(vectors[i].payload.c_str(), payload.c_str())
        << "recipe '" << vectors[i].recipe
        << "' no longer serializes to the recorded bytes. If the change was "
           "deliberate this is a FORMAT change: update the shared golden file "
           "in both places, and consider whether "
           "kOptionContextFormatVersion has to move -- it re-keys every "
           "context everywhere.";
    EXPECT_STREQ(vectors[i].signature.c_str(),
                 OptionContext::Signature(payload).c_str());
  }
  EXPECT_EQ(kExpectedGoldenRecipes, recipes_checked)
      << "the golden file stopped carrying construction recipes, which would "
         "leave only hash-of-a-literal vectors and no proof that any "
         "configuration still maps to the signature it used to";
}

TEST_F(OptionContextTest, GoldenFileCarriesTheSharedBound) {
  // The bound is a cross-component constant. Carrying it in the shared file and
  // comparing it here turns "both sides were reviewed together" into something
  // a build can check.
  size_t bound = 0;
  ASSERT_TRUE(ParseGoldenBound(ReadGoldenFileStrippingCr(), &bound))
      << "the shared golden file has no `# bound:` line";
  EXPECT_EQ(kMaxOptionContextBytes, bound)
      << "this build's kMaxOptionContextBytes disagrees with the shared golden "
         "file. Both sides of this contract and the file must move together.";
}

TEST_F(OptionContextTest, SignatureIsSixtyFourLowercaseHexCharacters) {
  const GoogleString signature = OptionContext::Signature("anything");
  ASSERT_EQ(kOptionContextSignatureChars, signature.size());
  for (size_t i = 0; i < signature.size(); ++i) {
    const char c = signature[i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
        << "non-lowercase-hex character at " << i;
  }
}

// ---------------------------------------------------------------------------
// Serialization shape.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest,
       UntouchedOptionsSerializeToTheVersionAndTheAlwaysOnFilter) {
  // Pinned as a literal deliberately: this is the one place the whole shape of
  // a payload is visible, and it documents that "nothing configured" is not the
  // empty payload -- the structural HTML writer is on at every rewrite level.
  EXPECT_STREQ("psoc1\nf:hw\n", PayloadOf(options_).c_str());
}

TEST_F(OptionContextTest, TheEmptyPayloadIsAFormatConstantNotADefaultConfig) {
  // The empty payload means "no option context supplied at all", which is a
  // different statement from "here is my configuration, which happens to be the
  // default". Conflating them would let a peer that declares a real context be
  // treated as one that declared nothing.
  EXPECT_STRNE(StrCat(kOptionContextFormatVersion, "\n").c_str(),
               PayloadOf(options_).c_str());
  EXPECT_STRNE(OptionContext::DefaultSignature().c_str(),
               OptionContext::Signature(PayloadOf(options_)).c_str());
}

TEST_F(OptionContextTest, SetOptionsAppearKeyedByConfigurationName) {
  options_.set_image_inline_max_bytes(3072);
  options_.set_js_inline_max_bytes(2048);
  // CssImageInlineMaxBytes rides along because set_image_inline_max_bytes sets
  // both -- a property of the setter, not of this serialization, and exactly
  // the kind of thing a payload should show rather than hide.
  EXPECT_STREQ(
      "o:CssImageInlineMaxBytes=3072\n"
      "o:ImageInlineMaxBytes=3072\n"
      "o:JsInlineMaxBytes=2048\n",
      RecordsAddedTo(PayloadOf(options_)).c_str());
}

TEST_F(OptionContextTest, UnsetOptionsAreAbsentEvenWhenTheyHaveValues) {
  // Every registered option HAS a value at all times; only the ones somebody
  // set are part of the context. This is the property that lets a later
  // release add options without moving anybody's signature.
  const GoogleString payload = PayloadOf(options_);
  EXPECT_EQ(GoogleString::npos, payload.find("\no:"))
      << "an option nobody set reached the payload: " << payload;
  EXPECT_EQ(GoogleString::npos, payload.find("\nr:")) << payload;
  EXPECT_GT(options_.all_options().size(), 100u)
      << "sanity: there should be a large option table for the previous "
         "assertion to be interesting";
}

TEST_F(OptionContextTest, EnabledFiltersAppearSortedByIdNotByEnumPosition) {
  // Enabled in an order that is neither enum order nor id order.
  options_.EnableFilter(RewriteOptions::kInlineImages);        // ii
  options_.EnableFilter(RewriteOptions::kAddInstrumentation);  // ai
  options_.EnableFilter(RewriteOptions::kRewriteJavascriptExternal);
  options_.EnableFilter(RewriteOptions::kExtendCacheCss);  // ec

  const GoogleString payload = PayloadOf(options_);
  StringPieceVector lines;
  SplitStringPieceToVector(payload, "\n", &lines, true);
  ASSERT_FALSE(lines.empty());
  EXPECT_EQ(StringPiece(kOptionContextFormatVersion), lines[0]);

  std::vector<StringPiece> filter_lines;
  for (size_t i = 1; i < lines.size(); ++i) {
    if (lines[i].starts_with("f:")) {
      filter_lines.push_back(lines[i]);
    }
  }
  // Four enabled here plus the always-on structural writer. Asserted as "at
  // least the four" rather than a literal count, so that a future always-on
  // filter does not fail a test about ORDERING.
  ASSERT_GE(filter_lines.size(), 4u);
  for (const char* wanted : {"f:ai", "f:ec", "f:ii", "f:jm"}) {
    bool found = false;
    for (size_t i = 0; i < filter_lines.size(); ++i) {
      if (filter_lines[i] == StringPiece(wanted)) found = true;
    }
    EXPECT_TRUE(found) << "missing " << wanted << " in " << payload;
  }
  for (size_t i = 1; i < filter_lines.size(); ++i) {
    EXPECT_LT(filter_lines[i - 1], filter_lines[i])
        << "filter records must be strictly ascending; enum position must not "
           "be observable";
  }
}

TEST_F(OptionContextTest, DebugFilterIsExcluded) {
  const GoogleString before = PayloadOf(options_);
  options_.EnableFilter(RewriteOptions::kDebug);
  EXPECT_STREQ(before.c_str(), PayloadOf(options_).c_str())
      << "turning on debug output must not split the cache";
}

TEST_F(OptionContextTest, ValuesAreCarriedByteForByteIncludingSpaces) {
  options_.SetRewriteLevel(RewriteOptions::kCoreFilters);
  const GoogleString payload = PayloadOf(options_);
  EXPECT_NE(GoogleString::npos, payload.find("o:RewriteLevel=Core Filters\n"))
      << payload;
}

TEST_F(OptionContextTest, ValuesWithReservedBytesAreEscapedOntoOneLine) {
  GoogleString awkward;
  awkward.push_back('a');
  awkward.push_back('\\');
  awkward.push_back('b');
  awkward.push_back('\n');
  awkward.push_back('c');
  awkward.push_back(static_cast<char>(0x80));
  options_.set_x_header_value(awkward);

  const GoogleString payload = PayloadOf(options_);
  EXPECT_NE(GoogleString::npos,
            payload.find("o:XHeaderValue=a\\\\b\\x0ac\\x80\n"))
      << payload;

  // One record per line, always: the embedded newline must not have produced
  // an extra line.
  StringPieceVector lines;
  SplitStringPieceToVector(payload, "\n", &lines, true);
  // Version token, the always-on filter, and the one option record: the
  // embedded newline produced no extra line.
  EXPECT_EQ(3u, lines.size()) << payload;
}

TEST_F(OptionContextTest, AnEmptyValueIsAValueAndNotAnAbsence) {
  std::unique_ptr<RewriteOptions> untouched(NewOptions());
  options_.set_x_header_value("");
  EXPECT_STREQ("o:XHeaderValue=\n",
               RecordsAddedTo(PayloadOf(options_)).c_str());
  EXPECT_STRNE(PayloadOf(*untouched).c_str(), PayloadOf(options_).c_str());
}

// ---------------------------------------------------------------------------
// Canonical serialization is order- and construction-invariant.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest, OrderOfAssignmentDoesNotChangeOneByte) {
  // Six settings applied in several different orders. Every permutation has to
  // produce the identical payload, because the payload is a sorted set and not
  // a transcript of what happened.
  std::vector<GoogleString> payloads;

  {
    std::unique_ptr<RewriteOptions> o(NewOptions());
    o->set_image_inline_max_bytes(3072);
    o->set_js_inline_max_bytes(2048);
    o->EnableFilter(RewriteOptions::kInlineImages);
    o->set_x_header_value("hello");
    o->EnableFilter(RewriteOptions::kAddInstrumentation);
    o->set_css_inline_max_bytes(1024);
    payloads.push_back(PayloadOf(*o));
  }
  {
    std::unique_ptr<RewriteOptions> o(NewOptions());
    o->set_css_inline_max_bytes(1024);
    o->EnableFilter(RewriteOptions::kAddInstrumentation);
    o->set_x_header_value("hello");
    o->EnableFilter(RewriteOptions::kInlineImages);
    o->set_js_inline_max_bytes(2048);
    o->set_image_inline_max_bytes(3072);
    payloads.push_back(PayloadOf(*o));
  }
  {
    std::unique_ptr<RewriteOptions> o(NewOptions());
    o->EnableFilter(RewriteOptions::kInlineImages);
    o->set_js_inline_max_bytes(2048);
    o->set_css_inline_max_bytes(1024);
    o->set_image_inline_max_bytes(3072);
    o->EnableFilter(RewriteOptions::kAddInstrumentation);
    o->set_x_header_value("hello");
    payloads.push_back(PayloadOf(*o));
  }

  for (size_t i = 1; i < payloads.size(); ++i) {
    EXPECT_STREQ(payloads[0].c_str(), payloads[i].c_str())
        << "permutation " << i << " produced different bytes";
    EXPECT_STREQ(OptionContext::Signature(payloads[0]).c_str(),
                 OptionContext::Signature(payloads[i]).c_str());
  }
}

TEST_F(OptionContextTest, RepeatedAssignmentOfTheSameValueChangesNothing) {
  options_.set_image_inline_max_bytes(3072);
  const GoogleString once = PayloadOf(options_);
  options_.set_image_inline_max_bytes(3072);
  options_.set_image_inline_max_bytes(3072);
  EXPECT_STREQ(once.c_str(), PayloadOf(options_).c_str());
}

TEST_F(OptionContextTest, SettingByNameAndBySetterAgreeByteForByte) {
  // XHeaderValue deliberately, not ImageInlineMaxBytes: the latter's typed
  // setter also sets CssImageInlineMaxBytes while the by-name path does not, so
  // the two genuinely resolve to different option sets. That is a property of
  // RewriteOptions and this serialization is right to show it -- but it makes
  // that option useless for asking the question this test is asking.
  std::unique_ptr<RewriteOptions> by_setter(NewOptions());
  by_setter->set_x_header_value("a-value");

  std::unique_ptr<RewriteOptions> by_name(NewOptions());
  GoogleString msg;
  NullMessageHandler handler;
  ASSERT_EQ(RewriteOptions::kOptionOk,
            by_name->ParseAndSetOptionFromName1("XHeaderValue", "a-value", &msg,
                                                &handler))
      << msg;

  EXPECT_STREQ(PayloadOf(*by_setter).c_str(), PayloadOf(*by_name).c_str())
      << "the path a value arrived by must not be observable";
}

TEST_F(OptionContextTest, SerializingTwiceIsIdempotent) {
  options_.set_image_inline_max_bytes(3072);
  options_.EnableFilter(RewriteOptions::kInlineImages);
  const GoogleString first = PayloadOf(options_);
  const GoogleString second = PayloadOf(options_);
  EXPECT_STREQ(first.c_str(), second.c_str());
}

// ---------------------------------------------------------------------------
// Cross-release stability.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest,
       PayloadCarriesNoReleaseIdentifierAlthoughTheOldOneDoes) {
  options_.set_image_inline_max_bytes(3072);
  options_.ComputeSignature();

  const GoogleString payload = PayloadOf(options_);
  const GoogleString old_signature = options_.signature();

  // The metadata-cache signature opens with kOptionsVersion, whose documented
  // job is to move on any default change and flush everything. That is exactly
  // the property an option context must not have.
  const GoogleString options_version =
      IntegerToString(RewriteOptions::kOptionsVersion);
  EXPECT_TRUE(old_signature.starts_with(options_version))
      << "premise of this test changed: the existing signature no longer "
         "opens with kOptionsVersion";
  EXPECT_TRUE(payload.starts_with(StrCat(kOptionContextFormatVersion, "\n")))
      << payload;

  // The old signature also embeds the global invalidation timestamp; ours has
  // no such field at all.
  EXPECT_NE(GoogleString::npos, old_signature.find("GTS:"));
  EXPECT_EQ(GoogleString::npos, payload.find("GTS:")) << payload;
}

TEST_F(OptionContextTest,
       ChangingAnUnsetOptionsDefaultDoesNotMoveTheSignature) {
  // The closest thing to a release bump this test can actually perform: change
  // a DEFAULT out from under options that never set that option, exactly as a
  // later release would, and require the context to be unmoved.
  //
  // set_default_x_header_value reaches the shared Property, so it is process
  // state; it is restored below whatever happens.
  DefaultMutatingOptions probe(thread_system_.get());
  const GoogleString original_default = probe.x_header_value();

  std::unique_ptr<RewriteOptions> before(NewOptions());
  before->set_image_inline_max_bytes(3072);
  const GoogleString payload_before = PayloadOf(*before);
  const GoogleString signature_before =
      OptionContext::Signature(payload_before);

  probe.set_default_x_header_value("a-value-a-later-release-chose");
  GoogleString payload_after;
  GoogleString signature_after;
  {
    std::unique_ptr<RewriteOptions> after(NewOptions());
    // Confirm the simulation actually took effect, or the assertion below
    // would pass for the wrong reason.
    EXPECT_STREQ("a-value-a-later-release-chose",
                 after->x_header_value().c_str());
    after->set_image_inline_max_bytes(3072);
    payload_after = PayloadOf(*after);
    signature_after = OptionContext::Signature(payload_after);
  }
  probe.set_default_x_header_value(original_default);

  EXPECT_STREQ(payload_before.c_str(), payload_after.c_str())
      << "a default change moved the payload of options that never set it";
  EXPECT_STREQ(signature_before.c_str(), signature_after.c_str());

  // And the restore worked, so the rest of this binary is unaffected.
  std::unique_ptr<RewriteOptions> restored(NewOptions());
  EXPECT_STREQ(original_default.c_str(), restored->x_header_value().c_str());
}

TEST_F(OptionContextTest, AnOptionSetToItsOwnDefaultStillCountsAsSet) {
  // Documented consequence of following the existing was_set() rule: this
  // over-separates rather than under-separates, which is the safe direction.
  std::unique_ptr<RewriteOptions> untouched(NewOptions());
  std::unique_ptr<RewriteOptions> explicitly_defaulted(NewOptions());
  explicitly_defaulted->set_x_header_value(untouched->x_header_value());

  EXPECT_STRNE(PayloadOf(*untouched).c_str(),
               PayloadOf(*explicitly_defaulted).c_str());
}

// ---------------------------------------------------------------------------
// The size bound and its failure mode.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest, WorstCasePayloadFitsTheBoundWithTheReserveUnspent) {
  // The base class only. This is NOT the real worst case -- every shipping
  // server flavour subclasses RewriteOptions and adds ~54 more options, and
  // those are the long-named infrastructure ones with the largest records. The
  // real bound is derived in ApacheConfigOptionContextTest, against the largest
  // class that links. This case exists so that a base-table explosion still
  // trips on a leg that does not link the server flavours.
  std::unique_ptr<RewriteOptions> maximal(NewOptions());
  size_t payload_size = 0;
  AssertWorstCaseOptionContextFitsTheBound(maximal.get(), "RewriteOptions",
                                           &payload_size);
  EXPECT_GT(payload_size, 0u);
}

TEST_F(OptionContextTest, TheBaseClassIsNotTheLargestOptionsClass) {
  // Guards the note above from rotting into a comfortable lie: if the base
  // class ever DID carry everything, the measurement above would be the real
  // worst case and the Apache case would be redundant. Today it carries
  // markedly fewer, which is exactly why that other test has to exist.
  std::unique_ptr<RewriteOptions> base(NewOptions());
  EXPECT_LT(base->all_options().size(), 200u)
      << "base RewriteOptions has grown to the size of the server-flavour "
         "classes; re-check which class the bound should be derived against";
}

TEST_F(OptionContextTest, OversizedContextIsRefusedWholeAndNeverTruncated) {
  std::unique_ptr<RewriteOptions> huge(NewOptions());
  huge->set_x_header_value(GoogleString(kMaxOptionContextBytes + 1, 'x'));

  GoogleString payload("sentinel-that-must-be-cleared");
  EXPECT_EQ(OptionContextStatus::kTooLarge,
            OptionContext::Serialize(*huge, &payload));
  EXPECT_TRUE(payload.empty())
      << "a partial payload signs as a context that does not exist";
}

TEST_F(OptionContextTest, ComputeClearsBothOutputsWhenItRefuses) {
  std::unique_ptr<RewriteOptions> huge(NewOptions());
  huge->set_x_header_value(GoogleString(kMaxOptionContextBytes + 1, 'x'));

  GoogleString payload("stale");
  GoogleString signature("stale");
  EXPECT_EQ(OptionContextStatus::kTooLarge,
            OptionContext::Compute(*huge, &payload, &signature));
  EXPECT_TRUE(payload.empty());
  EXPECT_TRUE(signature.empty())
      << "a caller that ignores the status must not find a usable signature";
}

// ---------------------------------------------------------------------------
// Redaction.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest, RedactedOptionValuesNeverReachThePayload) {
  const char kSecret[] = "hunter2-this-must-not-travel";
  options_.set_url_signing_key(kSecret);

  const GoogleString payload = PayloadOf(options_);
  EXPECT_EQ(GoogleString::npos, payload.find(kSecret))
      << "a secret-bearing option value reached the payload: " << payload;
  EXPECT_NE(GoogleString::npos,
            payload.find(
                StrCat("r:UrlSigningKey=", OptionContext::Signature(kSecret))))
      << payload;
}

TEST_F(OptionContextTest, RedactionStillSeparatesContexts) {
  std::unique_ptr<RewriteOptions> a(NewOptions());
  std::unique_ptr<RewriteOptions> b(NewOptions());
  a->set_url_signing_key("key-one");
  b->set_url_signing_key("key-two");
  EXPECT_STRNE(PayloadOf(*a).c_str(), PayloadOf(*b).c_str())
      << "withholding a value must not merge two contexts";
}

TEST_F(OptionContextTest, RedactedAndPlainRecordsCannotAlias) {
  // A plain record and a redacted one for the same key differ in their tag, so
  // an option whose plain value happened to look like a hash cannot collide
  // with a redacted record.
  std::unique_ptr<RewriteOptions> plain(NewOptions());
  plain->set_x_header_value(OptionContext::Signature("anything"));
  const GoogleString payload = PayloadOf(*plain);
  EXPECT_TRUE(payload.find("o:XHeaderValue=") != GoogleString::npos) << payload;
  EXPECT_EQ(GoogleString::npos, payload.find("r:XHeaderValue=")) << payload;
}

// ---------------------------------------------------------------------------
// The option table itself has to stay serializable.
// ---------------------------------------------------------------------------

TEST_F(OptionContextTest, EveryRegisteredOptionHasASerializableKey) {
  const RewriteOptions::OptionBaseVector& all = options_.all_options();
  ASSERT_FALSE(all.empty());
  for (size_t i = 0; i < all.size(); ++i) {
    const StringPiece name = all[i]->option_name();
    const StringPiece id = all[i]->id();
    const StringPiece key = name.empty() ? id : name;
    ASSERT_FALSE(key.empty())
        << "option " << i << " has neither a name nor an id";
    for (size_t c = 0; c < key.size(); ++c) {
      const uint8_t byte = static_cast<uint8_t>(key[c]);
      EXPECT_GT(byte, 0x20u) << "key [" << key << "] has a control/space byte";
      EXPECT_LT(byte, 0x7Fu) << "key [" << key << "] has a non-ASCII byte";
      EXPECT_NE('=', key[c]) << "key [" << key << "] contains the separator";
    }
    if (!name.empty()) {
      EXPECT_NE('@', name[0])
          << "a configuration name beginning with '@' would collide with the "
             "id-keyed namespace: "
          << name;
    }
  }
}

TEST_F(OptionContextTest, EveryRegisteredOptionNameIsUnique) {
  // Two options sharing a key would produce two records that sort adjacently
  // and mean different things.
  const RewriteOptions::OptionBaseVector& all = options_.all_options();
  std::vector<GoogleString> keys;
  for (size_t i = 0; i < all.size(); ++i) {
    const StringPiece name = all[i]->option_name();
    keys.push_back(name.empty() ? StrCat("@", all[i]->id()) : name.as_string());
  }
  std::sort(keys.begin(), keys.end());
  for (size_t i = 1; i < keys.size(); ++i) {
    EXPECT_STRNE(keys[i - 1].c_str(), keys[i].c_str())
        << "duplicate option-context key";
  }
}

}  // namespace net_instaweb
