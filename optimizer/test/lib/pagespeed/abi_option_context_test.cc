// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The PS_API 1.5 surface: the per-request options context on the notify path.
//
// Most of what is asserted here is refusal, which is the point. An embedder
// that gets this wrong does not get a crash or a wrong answer it can see; it
// gets notifications the receiver drops, which looks exactly like a cache that
// is a bit colder than expected. So every way of getting it wrong is an error
// return with a message — delivered at the call site, where it is actionable —
// and each of those is pinned.
//
// ps_notify_worker_ex is exercised WITHOUT a listening socket. Delivery is a
// separate concern with its own tests; everything below is about the argument
// checking that happens before a byte is sent, so PS_ERR_IO is the "arguments
// were fine" outcome here.

#include <stddef.h>

#include <cstring>
#include <string>

#include "gtest/gtest.h"
#include "lib/pagespeed/pagespeed.h"

namespace {

constexpr char kNoSuchSocket[] = "/nonexistent/pagespeed-notify-test.sock";

std::string SignatureOf(const std::string& payload) {
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1] = {};
  EXPECT_EQ(PS_OK, ps_option_context_signature(payload.data(), payload.size(),
                                               out, sizeof(out)));
  return std::string(out);
}

ps_notify_params_t MakeParams() {
  ps_notify_params_t params;
  ps_notify_params_init_auto(&params);
  params.url = "/style.css";
  params.hostname = "example.com";
  params.scheme = "https";
  params.content_type = PS_CONTENT_CSS;
  params.mask = 0x08;
  return params;
}

// ---------------------------------------------------------------------------
// Version.
// ---------------------------------------------------------------------------

TEST(AbiOptionContextTest, ApiVersionIsAtLeastOnePointFive) {
  EXPECT_EQ(1, ps_version_major());
  EXPECT_GE(ps_version_minor(), 5)
      << "the entry points below were added at 1.5; adding a symbol is a MINOR "
         "bump and the version has to move with it";
}

// ---------------------------------------------------------------------------
// ps_option_context_signature.
// ---------------------------------------------------------------------------

TEST(AbiOptionContextTest, SignatureIsSixtyFourLowercaseHexAndNulTerminated) {
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1];
  std::memset(out, 'Z', sizeof(out));
  ASSERT_EQ(PS_OK, ps_option_context_signature("psoc1\n", 6, out, sizeof(out)));
  EXPECT_EQ('\0', out[PS_OPTION_CONTEXT_SIGNATURE_CHARS]);
  EXPECT_EQ(static_cast<size_t>(PS_OPTION_CONTEXT_SIGNATURE_CHARS),
            std::strlen(out));
  for (int i = 0; i < PS_OPTION_CONTEXT_SIGNATURE_CHARS; ++i) {
    const char c = out[i];
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << out;
  }
}

TEST(AbiOptionContextTest, SignatureMatchesTheGoldenEmptyContext) {
  // The value every peer compares against to mean "no options context". It is
  // the first entry of the shared golden-vector file; pinned here as a literal
  // so that an embedder reading only this header's tests still sees it.
  EXPECT_EQ("c0908a1c05a3d6b96d4ea614f60993a83b1eabc4cf7dccfd2899610749554f01",
            SignatureOf("psoc1\n"));
}

TEST(AbiOptionContextTest, SignatureIsDeterministicAcrossCalls) {
  EXPECT_EQ(SignatureOf("psoc1\no:ImageInlineMaxBytes=3072\n"),
            SignatureOf("psoc1\no:ImageInlineMaxBytes=3072\n"));
}

TEST(AbiOptionContextTest, SignatureRefusesATooSmallBuffer) {
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS];  // one short: no room for NUL
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_option_context_signature("psoc1\n", 6, out, sizeof(out)));
}

TEST(AbiOptionContextTest, SignatureRefusesANullOutput) {
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_option_context_signature("psoc1\n", 6, nullptr, 65));
}

TEST(AbiOptionContextTest, SignatureRefusesANullPayloadWithNonZeroLength) {
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1];
  EXPECT_EQ(PS_ERR_INVALID_ARG,
            ps_option_context_signature(nullptr, 6, out, sizeof(out)));
}

TEST(AbiOptionContextTest, SignatureAcceptsAnEmptyPayload) {
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1];
  EXPECT_EQ(PS_OK, ps_option_context_signature(nullptr, 0, out, sizeof(out)));
}

TEST(AbiOptionContextTest, SignatureRefusesAnOversizedPayload) {
  const std::string huge(PS_MAX_OPTION_CONTEXT_BYTES + 1, 'x');
  char out[PS_OPTION_CONTEXT_SIGNATURE_CHARS + 1];
  EXPECT_EQ(
      PS_ERR_INVALID_ARG,
      ps_option_context_signature(huge.data(), huge.size(), out, sizeof(out)));
}

// ---------------------------------------------------------------------------
// ps_notify_params_init_sized.
// ---------------------------------------------------------------------------

TEST(AbiOptionContextTest, InitAutoZeroesEverythingAndStampsTheSize) {
  ps_notify_params_t params;
  std::memset(&params, 0xAB, sizeof(params));
  ps_notify_params_init_auto(&params);
  EXPECT_EQ(sizeof(ps_notify_params_t), params.struct_size);
  EXPECT_EQ(nullptr, params.url);
  EXPECT_EQ(nullptr, params.option_context);
  EXPECT_EQ(nullptr, params.option_signature);
  EXPECT_EQ(0u, params.option_context_length);
  EXPECT_EQ(0, params.agent_request);
}

TEST(AbiOptionContextTest, InitSizedBelowStructSizeWidthTouchesNothing) {
  // Stamping the size into a buffer too small to hold the field would be the
  // overrun the entry point exists to prevent.
  ps_notify_params_t params;
  std::memset(&params, 0xAB, sizeof(params));
  ps_notify_params_init_sized(&params, 1);
  unsigned char first;
  std::memcpy(&first, &params, 1);
  EXPECT_EQ(0xAB, first);
}

TEST(AbiOptionContextTest, InitSizedHandlesNull) {
  ps_notify_params_init_sized(nullptr, sizeof(ps_notify_params_t));
}

// ---------------------------------------------------------------------------
// ps_notify_worker_ex argument checking.
// ---------------------------------------------------------------------------

TEST(AbiOptionContextTest, RefusesNullSocketAndNullParams) {
  ps_notify_params_t params = MakeParams();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(nullptr, &params));
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, nullptr));
}

TEST(AbiOptionContextTest, RefusesAnUnsetStructSize) {
  ps_notify_params_t params = MakeParams();
  params.struct_size = 0;
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, RefusesAStructSizeTooSmallToHoldTheNotification) {
  ps_notify_params_t params = MakeParams();
  params.struct_size = sizeof(size_t);  // struct_size alone
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, RefusesANullOrUnknownScheme) {
  ps_notify_params_t params = MakeParams();
  params.scheme = nullptr;
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));

  params = MakeParams();
  params.scheme = "gopher";
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, WithNoOptionContextItGetsAsFarAsDelivery) {
  // The baseline: valid arguments, no context. PS_ERR_IO means every check
  // passed and the only thing missing is a listener.
  ps_notify_params_t params = MakeParams();
  EXPECT_EQ(PS_ERR_IO, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, WithAValidOptionContextItGetsAsFarAsDelivery) {
  const std::string payload = "psoc1\nf:hw\no:ImageInlineMaxBytes=3072\n";
  const std::string signature = SignatureOf(payload);
  ps_notify_params_t params = MakeParams();
  params.option_context = payload.data();
  params.option_context_length = payload.size();
  params.option_signature = signature.c_str();
  EXPECT_EQ(PS_ERR_IO, ps_notify_worker_ex(kNoSuchSocket, &params));
}

// --- the half-context refusals ---------------------------------------------

TEST(AbiOptionContextTest, RefusesAPayloadWithoutItsSignature) {
  const std::string payload = "psoc1\n";
  ps_notify_params_t params = MakeParams();
  params.option_context = payload.data();
  params.option_context_length = payload.size();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, RefusesASignatureWithoutItsPayload) {
  const std::string signature = SignatureOf("psoc1\n");
  ps_notify_params_t params = MakeParams();
  params.option_signature = signature.c_str();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, RefusesASignatureThatIsNotOfThisPayload) {
  // The check that matters most: an embedder computing the signature over
  // something other than what it sends would otherwise file its work under a
  // name nothing downstream could ever question.
  const std::string payload = "psoc1\no:ImageInlineMaxBytes=3072\n";
  const std::string wrong = SignatureOf("psoc1\no:ImageInlineMaxBytes=4096\n");
  ps_notify_params_t params = MakeParams();
  params.option_context = payload.data();
  params.option_context_length = payload.size();
  params.option_signature = wrong.c_str();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
  EXPECT_NE(nullptr, std::strstr(ps_last_error_message(), "signature-mismatch"))
      << ps_last_error_message();
}

TEST(AbiOptionContextTest, RefusesAPayloadFromAnUnknownFormatVersion) {
  const std::string payload = "psoc2\no:SomethingNew=1\n";
  const std::string signature = SignatureOf(payload);
  ps_notify_params_t params = MakeParams();
  params.option_context = payload.data();
  params.option_context_length = payload.size();
  params.option_signature = signature.c_str();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
  EXPECT_NE(nullptr, std::strstr(ps_last_error_message(), "unknown-format"))
      << ps_last_error_message();
}

TEST(AbiOptionContextTest, RefusesAnOversizedOptionContext) {
  const std::string payload =
      "psoc1\n" + std::string(PS_MAX_OPTION_CONTEXT_BYTES, 'x');
  ps_notify_params_t params = MakeParams();
  params.option_context = payload.data();
  params.option_context_length = payload.size();
  // A signature cannot even be computed for it, so a plausible-looking one is
  // supplied: the size check has to fire before anything else looks at it.
  const std::string signature = SignatureOf("psoc1\n");
  params.option_signature = signature.c_str();
  EXPECT_EQ(PS_ERR_INVALID_ARG, ps_notify_worker_ex(kNoSuchSocket, &params));
}

// --- struct_size skew -------------------------------------------------------

TEST(AbiOptionContextTest, AnOlderCallersStructIsReadOnlyAsFarAsItGoes) {
  // A consumer compiled before 1.5 has a struct that ends at `mask`. The
  // library must read exactly that much and treat everything after it as
  // absent -- not read the caller's stack and find garbage where the option
  // context would be.
  ps_notify_params_t params = MakeParams();
  params.struct_size = offsetof(ps_notify_params_t, mask) + sizeof(params.mask);
  // Fields the shorter struct does not contain, deliberately set to values
  // that would be refused if they were read.
  params.option_context = "not-a-payload";
  params.option_context_length = 13;
  params.option_signature = nullptr;  // a half context, if it were read
  EXPECT_EQ(PS_ERR_IO, ps_notify_worker_ex(kNoSuchSocket, &params))
      << "the library read past the caller's struct_size";
}

TEST(AbiOptionContextTest, AStructSizeCoveringAgentRequestButNotTheContext) {
  ps_notify_params_t params = MakeParams();
  params.struct_size = offsetof(ps_notify_params_t, agent_request) +
                       sizeof(params.agent_request);
  params.agent_request = 1;
  params.option_context = "not-a-payload";
  params.option_context_length = 13;
  EXPECT_EQ(PS_ERR_IO, ps_notify_worker_ex(kNoSuchSocket, &params));
}

TEST(AbiOptionContextTest, ALargerStructSizeThanThisLibraryKnowsIsAccepted) {
  // The forward direction: a consumer built against a LATER header, whose
  // struct is longer. The library reads the prefix it knows and ignores the
  // tail, which is what makes both directions of skew work.
  ps_notify_params_t params = MakeParams();
  params.struct_size = sizeof(ps_notify_params_t) + 64;
  EXPECT_EQ(PS_ERR_IO, ps_notify_worker_ex(kNoSuchSocket, &params));
}

}  // namespace
