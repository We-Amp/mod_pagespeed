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

// The ABI floor and the published layout this module binds.  Every cache call
// the in-place arms make is sited, so a library older than 1.12, or a 1.12
// library without the sited cache entry points, is refused at load, and the
// notification parameters are the published 1.12 layout field for field.
//
// The HTML serve set (1.13 + 1.14) is optional and bound as a whole: a
// library below 1.14, or one that lacks any entry point of the set, still
// loads, and then no entry point of the set is used.

#include "pagespeed/system/daemon_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {

namespace {

#ifdef _WIN32
int setenv(const char* name, const char* value, int) {
  return _putenv_s(name, value);
}
int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

class DaemonAbiTest : public testing::Test {
 protected:
  void SetUp() override { unsetenv("PS_STUB_MINOR"); }
  void TearDown() override { unsetenv("PS_STUB_MINOR"); }

  GoogleString error_;
};

TEST_F(DaemonAbiTest, TheFloorIsOneTwelve) {
  EXPECT_EQ(1, kRequiredAbiMajor);
  EXPECT_EQ(12, kRequiredAbiMinor);
}

TEST_F(DaemonAbiTest, ALibraryReportingElevenIsRefused) {
  setenv("PS_STUB_MINOR", "11", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("reports API 1.11")) << error_;
  EXPECT_NE(GoogleString::npos, error_.find("needs 1.12")) << error_;
}

TEST_F(DaemonAbiTest, ALibraryReportingTwelveLoads) {
  setenv("PS_STUB_MINOR", "12", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_EQ(12, abi->VersionMinor());
}

TEST_F(DaemonAbiTest, ATwelveLibraryWithoutTheSitedCallsIsRefused) {
  // It reports the floor and lacks what the floor publishes.  The sited
  // cache calls are bound unconditionally, so the library is refused, naming
  // the first one it lacks; nothing falls back to an unsited call.
  setenv("PS_STUB_MINOR", "12", 1);
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub_no_site.so"), &error_));
  EXPECT_TRUE(abi == nullptr);
  EXPECT_NE(GoogleString::npos, error_.find("_site")) << error_;
}

TEST_F(DaemonAbiTest, TheNotificationLayoutIsThePublishedOne) {
  // The size alone would not see two fields trade places.
  EXPECT_EQ(88u, sizeof(PsNotifyParams));
  EXPECT_EQ(0u, offsetof(PsNotifyParams, struct_size));
  EXPECT_EQ(8u, offsetof(PsNotifyParams, url));
  EXPECT_EQ(16u, offsetof(PsNotifyParams, hostname));
  EXPECT_EQ(24u, offsetof(PsNotifyParams, scheme));
  EXPECT_EQ(32u, offsetof(PsNotifyParams, content_type));
  EXPECT_EQ(36u, offsetof(PsNotifyParams, mask));
  EXPECT_EQ(40u, offsetof(PsNotifyParams, agent_request));
  EXPECT_EQ(48u, offsetof(PsNotifyParams, option_context));
  EXPECT_EQ(56u, offsetof(PsNotifyParams, option_context_length));
  EXPECT_EQ(64u, offsetof(PsNotifyParams, option_signature));
  EXPECT_EQ(72u, offsetof(PsNotifyParams, site));
  EXPECT_EQ(80u, offsetof(PsNotifyParams, notify_flags));
}

TEST_F(DaemonAbiTest, TheInitializerIsToldTheFullSize) {
  std::unique_ptr<DaemonAbi> abi(
      LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
  ASSERT_TRUE(abi != nullptr) << error_;
  PsNotifyParams params;
  abi->NotifyParamsInit(&params);
  EXPECT_EQ(sizeof(PsNotifyParams), params.struct_size);
  EXPECT_EQ(nullptr, params.site);
  EXPECT_EQ(0u, params.notify_flags);
}

TEST_F(DaemonAbiTest, TheFlagsAreThePublishedBits) {
  EXPECT_EQ(0x1u, kPsNotifyFlagHostVouched);
  EXPECT_EQ(0x2u, kPsNotifyFlagFromModule);
  EXPECT_EQ(0x8u, kPsNotifyFlagHtmlStored);
  EXPECT_EQ(64u, kPsSiteIdChars);
}

// --- The HTML serve set (C API 1.13 + 1.14), bound optionally -------------

TEST_F(DaemonAbiTest, TheWriteParametersCarryTheContentHash) {
  // 1.13 appended the content binding.  A peer older than that snaps the
  // declared size down to the 1.12 shape and never reads the field.
  EXPECT_EQ(88u, sizeof(PsWriteParams));
  EXPECT_EQ(72u, offsetof(PsWriteParams, origin_cc_flags));
  EXPECT_EQ(80u, offsetof(PsWriteParams, origin_html_hash));
}

TEST_F(DaemonAbiTest, TheServeSetLayoutsAreThePublishedOnes) {
  EXPECT_EQ(64u, sizeof(PsSidecarWriteParams));
  EXPECT_EQ(8u, offsetof(PsSidecarWriteParams, url));
  EXPECT_EQ(32u, offsetof(PsSidecarWriteParams, site));
  EXPECT_EQ(40u, offsetof(PsSidecarWriteParams, origin_html_hash));
  EXPECT_EQ(48u, offsetof(PsSidecarWriteParams, policy_stamp));
  EXPECT_EQ(56u, offsetof(PsSidecarWriteParams, write_flags));
  EXPECT_EQ(48u, kPsSidecarWriteParamsSize13);

  EXPECT_EQ(88u, sizeof(PsHtmlServeRequest));
  EXPECT_EQ(8u, offsetof(PsHtmlServeRequest, mode));
  EXPECT_EQ(16u, offsetof(PsHtmlServeRequest, method));
  EXPECT_EQ(48u, offsetof(PsHtmlServeRequest, if_none_match));
  EXPECT_EQ(56u, offsetof(PsHtmlServeRequest, request_flags));
  EXPECT_EQ(60u, offsetof(PsHtmlServeRequest, now_seconds));
  EXPECT_EQ(64u, offsetof(PsHtmlServeRequest, serve_scope));
  EXPECT_EQ(68u, offsetof(PsHtmlServeRequest, agent_class));
  EXPECT_EQ(72u, offsetof(PsHtmlServeRequest, html_max_lifetime_s));
  EXPECT_EQ(76u, offsetof(PsHtmlServeRequest, reserved));
  EXPECT_EQ(80u, offsetof(PsHtmlServeRequest, policy_stamp));
  EXPECT_EQ(64u, kPsHtmlServeRequestSize14);

  EXPECT_EQ(104u, sizeof(PsHtmlStoredFacts));
  EXPECT_EQ(96u, offsetof(PsHtmlStoredFacts, entry_policy_stamp));
  EXPECT_EQ(96u, kPsHtmlStoredFactsSize14);
  EXPECT_EQ(8u, offsetof(PsHtmlStoredFacts, flags));
  EXPECT_EQ(24u, offsetof(PsHtmlStoredFacts, render_ttl_s));
  EXPECT_EQ(28u, offsetof(PsHtmlStoredFacts, withheld_reason));
  EXPECT_EQ(32u, offsetof(PsHtmlStoredFacts, binding_hash));
  EXPECT_EQ(64u, offsetof(PsHtmlStoredFacts, markdown_hash));

  EXPECT_EQ(76u, offsetof(PsHtmlServeRequest, markdown_encoding));

  EXPECT_EQ(72u, sizeof(PsHtmlServeVerdict));
  EXPECT_EQ(8u, offsetof(PsHtmlServeVerdict, action));
  EXPECT_EQ(28u, offsetof(PsHtmlServeVerdict, not_modified));
  EXPECT_EQ(32u, offsetof(PsHtmlServeVerdict, etag));
  EXPECT_EQ(64u, offsetof(PsHtmlServeVerdict, markdown_encoding));
  EXPECT_EQ(68u, offsetof(PsHtmlServeVerdict, reserved_1_16));
  EXPECT_EQ(64u, kPsHtmlServeVerdictSize15);
  EXPECT_EQ(kPsHtmlMarkdownEtagChars + 8, sizeof(PsHtmlServeVerdict::etag));

  // The request policy's results.
  EXPECT_EQ(24u, sizeof(PsHtmlRequestGuard));
  EXPECT_EQ(8u, offsetof(PsHtmlRequestGuard, reason));
  EXPECT_EQ(12u, offsetof(PsHtmlRequestGuard, header_index));
  EXPECT_EQ(16u, offsetof(PsHtmlRequestGuard, xfp));
  EXPECT_EQ(24u, sizeof(PsHtmlCookieSpan));
  EXPECT_EQ(48u, sizeof(PsHtmlCookieClass));
  EXPECT_EQ(8u, offsetof(PsHtmlCookieClass, cookie_class));
  EXPECT_EQ(12u, offsetof(PsHtmlCookieClass, malformed));
  EXPECT_EQ(16u, offsetof(PsHtmlCookieClass, first_blocking));
  EXPECT_EQ(40u, offsetof(PsHtmlCookieClass, ignorable_count));
}

TEST_F(DaemonAbiTest, TheFifteenConstantsAreThePublishedValues) {
  EXPECT_EQ(15, kHtmlServe15SetMinor);
  EXPECT_EQ(0x40, kPsFlagAnonymousRecorded);
  EXPECT_EQ(0x80, kPsFlagHtmlStoreV2);
  EXPECT_EQ(16u, kPsHtmlStoreRequestExplicitPublic);
  EXPECT_EQ(64u, kPsHtmlStoreFlagRequestAnonymous);
  EXPECT_EQ(64u, kPsHtmlServeRequestFreshOnly);
  EXPECT_EQ(128u, kPsHtmlServeRequestHtmlFallback);
  EXPECT_EQ(256u, kPsHtmlServeRequestConditional);
  EXPECT_EQ(512u, kPsHtmlServeRequestExplicitPublic);
  EXPECT_EQ(0, kPsHtmlServeScopeAgents);
  EXPECT_EQ(1, kPsHtmlServeScopeAll);
  EXPECT_EQ(3, kPsHtmlAgentClassMax);
  EXPECT_EQ(0x1000u, kPsHtmlFactsStoreV2);
  EXPECT_EQ(0x2000u, kPsHtmlFactsAnonymousRecorded);
  EXPECT_EQ(1u, kPsSidecarWritePolicyBlock);
  EXPECT_EQ(1, kPsHtmlGuardRequestHeader);
  EXPECT_EQ(2, kPsHtmlGuardPath);
  EXPECT_EQ(2, kPsHtmlCookiesBlocking);
  EXPECT_EQ(1, kPsHtmlCookieListVersion);
  EXPECT_EQ(1, kPsHtmlCredentialedExplicitPublic);
  EXPECT_EQ(3, kPsHtmlCacheServeCachedHtml);
  // The new request bits do not overlap the 1.14 ones, nor the new facts
  // bits the 1.14 facts.
  EXPECT_EQ(
      0u, (kPsHtmlServeRequestFreshOnly | kPsHtmlServeRequestHtmlFallback |
           kPsHtmlServeRequestConditional | kPsHtmlServeRequestExplicitPublic) &
              63u);
  EXPECT_EQ(0u, (kPsHtmlFactsStoreV2 | kPsHtmlFactsAnonymousRecorded) & 0xFFFu);
}

TEST_F(DaemonAbiTest, TheSixteenConstantsAreThePublishedValues) {
  EXPECT_EQ(16, kHtmlServe16SetMinor);
  EXPECT_EQ(0xDC, kPsSentinelMarkdownBr);
  EXPECT_EQ(0xEC, kPsSentinelMarkdownGzip);
  EXPECT_EQ(0x10u, kPsNotifyFlagMarkdownCompress);
  EXPECT_EQ(1024u, kPsHtmlServeRequestIdentityOk);
  EXPECT_EQ(2048u, kPsHtmlServeRequestEncodingNone);
  EXPECT_EQ(0, kPsMarkdownEncodingIdentity);
  EXPECT_EQ(1, kPsMarkdownEncodingGzip);
  EXPECT_EQ(2, kPsMarkdownEncodingBr);
  EXPECT_EQ(0x4000u, kPsHtmlFactsGzipCopyBound);
  EXPECT_EQ(0x8000u, kPsHtmlFactsBrCopyBound);
  EXPECT_EQ(0x10000u, kPsHtmlFactsGzipNotSmaller);
  EXPECT_EQ(0x20000u, kPsHtmlFactsBrNotSmaller);
  // Both coded ids are sentinel-shaped, and new bits overlap no older one.
  for (uint8_t id : {kPsSentinelMarkdownBr, kPsSentinelMarkdownGzip}) {
    EXPECT_EQ(kPsViewportSentinel,
              (id >> kPsMaskViewportShift) & kPsMaskViewportBits);
  }
  EXPECT_EQ(0u,
            (kPsHtmlServeRequestIdentityOk | kPsHtmlServeRequestEncodingNone) &
                1023u);
  EXPECT_EQ(0u, (kPsHtmlFactsGzipCopyBound | kPsHtmlFactsBrCopyBound |
                 kPsHtmlFactsGzipNotSmaller | kPsHtmlFactsBrNotSmaller) &
                    0x3FFFu);
}

TEST_F(DaemonAbiTest, TheServeSetConstantsAreThePublishedValues) {
  EXPECT_EQ(14, kHtmlServeSetMinor);
  EXPECT_EQ(0x3C, kPsSentinelContentHash);
  EXPECT_EQ(0x7C, kPsSentinelAgentMarkdown);
  EXPECT_EQ(0xCC, kPsSentinelMarkdownWithheld);
  EXPECT_EQ(0x10, kPsFlagHeadersClassified);
  EXPECT_EQ(0x20, kPsFlagHeadersSidecarRequired);
  EXPECT_EQ(0, kPsSidecarUnpaired);
  EXPECT_EQ(1, kPsSidecarPaired);
  EXPECT_EQ(2, kPsHtmlCacheServeVariants);
  EXPECT_EQ(2, kPsHtmlServeActionServeMarkdown);
  EXPECT_EQ(0x200u, kPsHtmlFactsBindingLive);
  EXPECT_EQ(0x800u, kPsHtmlFactsWithheld);
  EXPECT_EQ(1, kPsHtmlModeMarkdownStatic);
  EXPECT_EQ(32u, kPsHtmlHashBytes);
  // Every Cache-Control bit the peer's parser reports.
  EXPECT_EQ(0x0001u, kPsCcOriginNoCache);
  EXPECT_EQ(0x0008u, kPsCcOriginPrivate);
  EXPECT_EQ(0x4000u, kPsCcOriginSMaxageRepeated);
  // The sentinel-shaped ids sit in the reserved viewport.
  for (uint8_t id : {kPsSentinelContentHash, kPsSentinelAgentMarkdown,
                     kPsSentinelMarkdownWithheld}) {
    EXPECT_EQ(kPsViewportSentinel,
              (id >> kPsMaskViewportShift) & kPsMaskViewportBits);
  }
}

class HtmlServeSetTest : public DaemonAbiTest {
 protected:
  std::unique_ptr<DaemonAbi> Load(const char* minor, const char* library) {
    setenv("PS_STUB_MINOR", minor, 1);
    return std::unique_ptr<DaemonAbi>(
        LoadDaemonAbi(StubLibraryPath(library), &error_));
  }

  // No call into an unbound set reaches anything, and every one answers the
  // fail-closed value.
  void ExpectUnboundAnswers(const DaemonAbi& abi) {
    PsHtmlServeRequest request = {};
    request.struct_size = sizeof(request);
    request.method = "GET";
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    PsHtmlServeVerdict verdict = {};
    verdict.struct_size = sizeof(verdict);
    EXPECT_EQ(-1, abi.HtmlServeClassify(&request, &facts, &verdict));
    EXPECT_EQ(nullptr, abi.HtmlServeReasonName(0));
    PsWriteParams params = {};
    params.struct_size = sizeof(params);
    EXPECT_EQ(-1, abi.HtmlStoreWriteParams(1, 60, 0, 0, 1, 1000000, nullptr, 0,
                                           10, "text/html", &params));
    EXPECT_EQ(
        -1, abi.HtmlStoredFactsFromReads(nullptr, nullptr, nullptr, 0, &facts));
    uint8_t hash[kPsHtmlHashBytes] = {1};
    char etag[64];
    size_t len = 0;
    EXPECT_EQ(-1, abi.HtmlMarkdownEtag(hash, hash, etag, sizeof(etag), &len));
    EXPECT_EQ(-1, abi.IfNoneMatchMatchesWeak("*", "W/\"psm-0\""));
    int cache = 0;
    PsSidecarWriteParams sidecar = {};
    sidecar.struct_size = sizeof(sidecar);
    int verdict_out = 0;
    int stored = 0;
    EXPECT_EQ(kPsErrInvalidArg, abi.CacheWriteHeadersSidecarSite(
                                    &cache, &sidecar, nullptr, nullptr, 0,
                                    &verdict_out, &stored));
    void* block = &cache;
    EXPECT_EQ(kPsErrInvalidArg, abi.HeadersSidecarRead(&cache, "http://a/", "a",
                                                       "http", kSite, &block));
    EXPECT_EQ(nullptr, block);
    EXPECT_EQ(0u, abi.HeaderBlockCount(nullptr));
    EXPECT_EQ(-1, abi.HeaderBlockPairing(nullptr, hash));
    EXPECT_EQ(nullptr, abi.HeaderBlockPairingHash(nullptr));
    abi.HeaderBlockFree(nullptr);
    // Never NOT_FOUND: a caller treats that as a removal that succeeded.
    EXPECT_EQ(kPsErrInvalidArg,
              abi.CacheRemoveSite(&cache, "http://a/", "a", "http", kSite));
    EXPECT_EQ(kPsErrInvalidArg, abi.CacheWriteSitePurgeEpoch(&cache, kSite, 1));
    uint64_t epoch = 77;
    EXPECT_EQ(kPsErrInvalidArg,
              abi.CacheReadSitePurgeEpoch(&cache, kSite, &epoch));
    EXPECT_EQ(77u, epoch);
    EXPECT_EQ(-1, abi.ReadSharedConfigHtmlMode("/unused"));
    EXPECT_EQ(-1, abi.ReadSharedConfigAgentEntitled("/unused"));
    EXPECT_EQ(0, abi.WantsAgentMarkdown("text/markdown"));
    ExpectUnbound15Answers(abi);
  }

  // No call into an unbound 1.15 set reaches anything.
  void ExpectUnbound15Answers(const DaemonAbi& abi) {
    EXPECT_FALSE(abi.HasHtmlServe15Set());
    EXPECT_FALSE(abi.HtmlServe15SetUnavailableReason().empty());
    PsHtmlRequestGuard guard = {};
    guard.struct_size = sizeof(guard);
    guard.reason = 77;
    const char* names[] = {"X-Forwarded-Host"};
    const char* values[] = {"evil.example"};
    EXPECT_EQ(-1,
              abi.HtmlRequestGuard("/a;b", names, values, 1, "http", &guard));
    EXPECT_EQ(77, guard.reason);
    const char* lines[] = {"sid=1"};
    PsHtmlCookieClass cookies = {};
    cookies.struct_size = sizeof(cookies);
    EXPECT_EQ(-1, abi.HtmlClassifyCookies(lines, 1, 1, nullptr, 0, nullptr, 0,
                                          &cookies));
    int warning = 0;
    EXPECT_EQ(-1, abi.HtmlIgnoreCookieGuard("_my_ab", &warning));
    uint64_t stamp = 5;
    EXPECT_EQ(-1, abi.HtmlPolicyStamp(0, 1, 1, nullptr, 0, &stamp));
    EXPECT_EQ(5u, stamp);
    EXPECT_EQ(0, abi.HtmlUnsafeMethodInvalidates("POST", 200));
    int block = 0;
    EXPECT_EQ(-1, abi.HeaderBlockPolicyStamp(&block, &stamp));
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    EXPECT_EQ(-1, abi.HtmlStoredFactsFromReadsEx(nullptr, nullptr, nullptr, 0,
                                                 nullptr, &facts));
    // Every struct goes out at the size a 1.14 library knows.
    EXPECT_EQ(kPsHtmlServeRequestSize14, HtmlServeRequestSize(abi));
    EXPECT_EQ(kPsHtmlStoredFactsSize14, HtmlStoredFactsSize(abi));
    EXPECT_EQ(kPsSidecarWriteParamsSize13, SidecarWriteParamsSize(abi));
    ExpectUnbound16Answers(abi);
  }

  // No call into an unbound 1.16 set reaches anything.
  void ExpectUnbound16Answers(const DaemonAbi& abi) {
    EXPECT_FALSE(abi.HasHtmlServe16Set());
    EXPECT_FALSE(abi.HtmlServe16SetUnavailableReason().empty());
    const char* lines[] = {"br, gzip"};
    int order[3] = {7, 7, 7};
    EXPECT_EQ(-1, abi.MarkdownSelectEncoding(lines, 1, order));
    EXPECT_EQ(7, order[0]);
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    int coded = 0;
    EXPECT_EQ(-1, abi.HtmlStoredFactsFromReadsCoded(
                      nullptr, nullptr, nullptr, 0, nullptr, &coded,
                      kPsMarkdownEncodingBr, &facts));
    uint8_t hash[kPsHtmlHashBytes] = {1};
    char etag[64];
    size_t len = 0;
    EXPECT_EQ(-1, abi.HtmlMarkdownEtagCoded(hash, hash, kPsMarkdownEncodingBr,
                                            etag, sizeof(etag), &len));
    EXPECT_EQ(-1, abi.ReadSharedConfigMarkdownCodings("/unused"));
    EXPECT_EQ(kPsHtmlServeVerdictSize15, HtmlServeVerdictSize(abi));
  }

  static constexpr char kSite[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
};

TEST_F(HtmlServeSetTest, AFourteenLibraryWithTheWholeSetBindsIt) {
  std::unique_ptr<DaemonAbi> abi = Load("14", "libdaemon_stub_html_serve.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  EXPECT_EQ("", abi->HtmlServeSetUnavailableReason());
  // Every entry point reaches the library, not a default.
  EXPECT_STREQ("fresh", abi->HtmlServeReasonName(0));
  EXPECT_STREQ("skew", abi->HtmlServeReasonName(18));
  EXPECT_EQ(nullptr, abi->HtmlServeReasonName(19));
  // A 1.14 library: the 1.15 set is not there.
  EXPECT_FALSE(abi->HasHtmlServe15Set());
  EXPECT_EQ(1, abi->WantsAgentMarkdown("text/markdown, text/html;q=0.9"));
  EXPECT_EQ(0, abi->WantsAgentMarkdown("text/html"));
  EXPECT_EQ(kPsHtmlModeMarkdownStatic, abi->ReadSharedConfigHtmlMode("/c"));
  EXPECT_EQ(1, abi->ReadSharedConfigAgentEntitled("/c"));
  EXPECT_EQ(1, abi->IfNoneMatchMatchesWeak("*", "W/\"psm-0\""));

  PsHtmlServeRequest request = {};
  request.struct_size = sizeof(request);
  request.mode = kPsHtmlCacheServeVariants;
  request.method = "GET";
  PsHtmlStoredFacts facts = {};
  // A facts struct with no size is a malformed call.
  EXPECT_EQ(
      -1, abi->HtmlStoredFactsFromReads(nullptr, nullptr, nullptr, 0, &facts));
  facts.struct_size = sizeof(facts);
  ASSERT_EQ(
      0, abi->HtmlStoredFactsFromReads(nullptr, nullptr, nullptr, 0, &facts));
  PsHtmlServeVerdict verdict = {};
  verdict.struct_size = sizeof(verdict);
  ASSERT_EQ(0, abi->HtmlServeClassify(&request, &facts, &verdict));
  EXPECT_EQ(kPsHtmlServeActionFallThrough, verdict.action);
  EXPECT_STREQ("absent", abi->HtmlServeReasonName(verdict.reason));

  uint8_t binding[kPsHtmlHashBytes] = {1};
  uint8_t markdown[kPsHtmlHashBytes] = {2};
  char etag[kPsHtmlMarkdownEtagChars + 1];
  size_t len = 0;
  ASSERT_EQ(0,
            abi->HtmlMarkdownEtag(binding, markdown, etag, sizeof(etag), &len));
  EXPECT_EQ(kPsHtmlMarkdownEtagChars, len);
  EXPECT_EQ(0, strncmp(etag, "W/\"psm-", 7)) << etag;

  PsWriteParams params;
  abi->WriteParamsInit(&params);
  const char* age[] = {"30"};
  ASSERT_EQ(0, abi->HtmlStoreWriteParams(kPsHtmlStoreActionStore, 60, 0, 0, 1,
                                         int64_t{100000} * 1000 + 999, age, 1,
                                         42, "text/html", &params));
  EXPECT_EQ(100000u - 30u, params.cache_inserted_at);
  EXPECT_EQ(42u, params.content_length);
  EXPECT_EQ(sizeof(PsWriteParams), params.struct_size);
}

TEST_F(HtmlServeSetTest, TheSidecarEpochAndRemovalReachTheLibrary) {
  std::unique_ptr<DaemonAbi> abi = Load("14", "libdaemon_stub_html_serve.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->HasHtmlServeSet());
  void* cache = nullptr;
  alignas(
      std::max_align_t) unsigned char config_storage[kCacheConfigProbeBytes];
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
  abi->CacheConfigInit(config);
  ASSERT_EQ(kPsOk, abi->CacheOpen(config, &cache));

  uint8_t hash[kPsHtmlHashBytes];
  memset(hash, 0xAB, sizeof(hash));
  PsSidecarWriteParams sidecar = {};
  sidecar.struct_size = sizeof(sidecar);
  sidecar.url = "http://a.example/p";
  sidecar.hostname = "a.example";
  sidecar.scheme = "http";
  sidecar.site = kSite;
  sidecar.origin_html_hash = hash;
  const char* names[] = {"Content-Language"};
  const char* values[] = {"en"};
  int verdict = -1;
  int stored = -1;
  ASSERT_EQ(kPsOk, abi->CacheWriteHeadersSidecarSite(
                       cache, &sidecar, names, values, 1, &verdict, &stored));
  EXPECT_EQ(1, stored);

  void* block = nullptr;
  ASSERT_EQ(kPsOk, abi->HeadersSidecarRead(cache, sidecar.url, "a.example",
                                           "http", kSite, &block));
  ASSERT_TRUE(block != nullptr);
  ASSERT_EQ(1u, abi->HeaderBlockCount(block));
  const char* name = nullptr;
  const char* value = nullptr;
  size_t name_len = 0;
  size_t value_len = 0;
  ASSERT_EQ(kPsOk, abi->HeaderBlockGet(block, 0, &name, &name_len, &value,
                                       &value_len));
  EXPECT_STREQ("Content-Language", name);
  EXPECT_EQ(2u, value_len);
  ASSERT_TRUE(abi->HeaderBlockPairingHash(block) != nullptr);
  EXPECT_EQ(kPsSidecarPaired, abi->HeaderBlockPairing(block, hash));
  uint8_t other[kPsHtmlHashBytes] = {7};
  EXPECT_EQ(kPsSidecarUnpaired, abi->HeaderBlockPairing(block, other));
  abi->HeaderBlockFree(block);

  // Another site's keyspace holds nothing.
  block = &cache;
  EXPECT_EQ(kPsErrNotFound,
            abi->HeadersSidecarRead(cache, sidecar.url, "a.example", "http",
                                    "f123456789abcdef0123456789abcdef"
                                    "0123456789abcdef0123456789abcdef",
                                    &block));
  EXPECT_EQ(nullptr, block);

  EXPECT_EQ(kPsOk, abi->CacheRemoveSite(cache, sidecar.url, "a.example", "http",
                                        kSite));
  EXPECT_EQ(kPsErrNotFound, abi->CacheRemoveSite(cache, sidecar.url,
                                                 "a.example", "http", kSite));

  uint64_t epoch = 99;
  ASSERT_EQ(kPsOk, abi->CacheReadSitePurgeEpoch(cache, kSite, &epoch));
  EXPECT_EQ(0u, epoch);
  ASSERT_EQ(kPsOk, abi->CacheWriteSitePurgeEpoch(cache, kSite, 1000));
  ASSERT_EQ(kPsOk, abi->CacheWriteSitePurgeEpoch(cache, kSite, 500));
  ASSERT_EQ(kPsOk, abi->CacheReadSitePurgeEpoch(cache, kSite, &epoch));
  EXPECT_EQ(1000u, epoch);  // a max-merge never decreases
  abi->CacheClose(cache);
}

TEST_F(HtmlServeSetTest, AThirteenLibraryDoesNotBindTheSet) {
  // The 1.13 half alone is not the set: the serve verdict is 1.14.
  std::unique_ptr<DaemonAbi> abi = Load("13", "libdaemon_stub_html_serve.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServeSet());
  const GoogleString reason = abi->HtmlServeSetUnavailableReason();
  EXPECT_NE(GoogleString::npos, reason.find("1.13")) << reason;
  EXPECT_NE(GoogleString::npos, reason.find("1.14")) << reason;
  ExpectUnboundAnswers(*abi);
  // The 1.12 store policy is unaffected.
  EXPECT_TRUE(abi->PublishesHtmlStorePolicy());
}

TEST_F(HtmlServeSetTest, ATwelveLibraryDoesNotBindTheSet) {
  std::unique_ptr<DaemonAbi> abi = Load("12", "libdaemon_stub_html_serve.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServeSet());
  EXPECT_NE(GoogleString::npos,
            abi->HtmlServeSetUnavailableReason().find("1.12"));
  ExpectUnboundAnswers(*abi);
}

TEST_F(HtmlServeSetTest, OneMissingSymbolLeavesTheWholeSetUnbound) {
  // A library that reports 1.14 but lacks one entry point of the set still
  // loads -- the set is optional -- and none of the set is used, not even
  // the entry points it does export.
  std::unique_ptr<DaemonAbi> abi =
      Load("14", "libdaemon_stub_html_serve_partial.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServeSet());
  const GoogleString reason = abi->HtmlServeSetUnavailableReason();
  EXPECT_NE(GoogleString::npos, reason.find("ps_cache_read_site_purge_epoch"))
      << reason;
  ExpectUnboundAnswers(*abi);
}

TEST_F(HtmlServeSetTest, ALibraryWithoutTheSetStillLoads) {
  std::unique_ptr<DaemonAbi> abi = Load("14", "libdaemon_stub.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServeSet());
  EXPECT_FALSE(abi->HtmlServeSetUnavailableReason().empty());
  ExpectUnboundAnswers(*abi);
}

// --- The C API 1.15 set, bound optionally on top of the 1.14 one ---------

class HtmlServe15SetTest : public HtmlServeSetTest {
 protected:
  void SetUp() override {
    HtmlServeSetTest::SetUp();
    unsetenv("PS_STUB_ECHO_SIZES");
    unsetenv("PS_STUB_POLICY_STAMP");
  }
  void TearDown() override {
    unsetenv("PS_STUB_ECHO_SIZES");
    unsetenv("PS_STUB_POLICY_STAMP");
    HtmlServeSetTest::TearDown();
  }

  // A serve-classify call with the sizes this build knows, echoing the
  // sizes the library was handed (PS_STUB_ECHO_SIZES).
  int Classify(const DaemonAbi& abi, int mode, size_t* request_size,
               size_t* facts_size, GoogleString* reason) {
    setenv("PS_STUB_ECHO_SIZES", "1", 1);
    PsHtmlServeRequest request = {};
    request.struct_size = sizeof(request);
    request.mode = mode;
    request.method = "GET";
    request.serve_scope = kPsHtmlServeScopeAll;
    request.agent_class = 3;
    request.html_max_lifetime_s = 60;
    request.policy_stamp = 9;
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    PsHtmlServeVerdict verdict = {};
    verdict.struct_size = sizeof(verdict);
    const int rc = abi.HtmlServeClassify(&request, &facts, &verdict);
    // The caller's own structs are left as it stamped them.
    EXPECT_EQ(sizeof(PsHtmlServeRequest), request.struct_size);
    EXPECT_EQ(sizeof(PsHtmlStoredFacts), facts.struct_size);
    *request_size = verdict.age_s;
    *facts_size = static_cast<size_t>(verdict.not_modified);
    const char* name = abi.HtmlServeReasonName(verdict.reason);
    *reason = name == nullptr ? "" : name;
    return rc;
  }
};

TEST_F(HtmlServe15SetTest, AFifteenLibraryBindsBothSets) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  EXPECT_TRUE(abi->HasHtmlServe15Set());
  EXPECT_EQ("", abi->HtmlServe15SetUnavailableReason());
  EXPECT_EQ(sizeof(PsHtmlServeRequest), HtmlServeRequestSize(*abi));
  EXPECT_EQ(sizeof(PsHtmlStoredFacts), HtmlStoredFactsSize(*abi));
  EXPECT_EQ(sizeof(PsSidecarWriteParams), SidecarWriteParamsSize(*abi));
  // The 1.15 reasons are the library's names.
  EXPECT_STREQ("scope", abi->HtmlServeReasonName(19));
  EXPECT_STREQ("request_header", abi->HtmlServeReasonName(25));
  EXPECT_STREQ("path", abi->HtmlServeReasonName(26));
  EXPECT_EQ(nullptr, abi->HtmlServeReasonName(27));
}

// The request guard reaches the library and its three answers come back:
// a path refusal, a refused header by index, and an X-Forwarded-Proto
// mismatch with its flag.
TEST_F(HtmlServe15SetTest, TheRequestGuardReachesTheLibrary) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->HasHtmlServe15Set());
  const char* names[] = {"Accept", "X-Forwarded-Proto", "X-Forwarded-Host"};
  const char* values[] = {"text/html", "http", "evil.example"};
  PsHtmlRequestGuard guard = {};
  guard.struct_size = sizeof(guard);
  ASSERT_EQ(0,
            abi->HtmlRequestGuard("/a;b?c", names, values, 3, "http", &guard));
  EXPECT_EQ(kPsHtmlGuardPath, guard.reason);
  EXPECT_EQ(-1, guard.header_index);
  ASSERT_EQ(0,
            abi->HtmlRequestGuard("/a?c;d", names, values, 3, "http", &guard));
  EXPECT_EQ(kPsHtmlGuardRequestHeader, guard.reason);
  EXPECT_EQ(2, guard.header_index);
  EXPECT_EQ(0, guard.xfp);
  ASSERT_EQ(0, abi->HtmlRequestGuard("/a", names, values, 2, "https", &guard));
  EXPECT_EQ(kPsHtmlGuardRequestHeader, guard.reason);
  EXPECT_EQ(1, guard.header_index);
  EXPECT_EQ(1, guard.xfp);
  ASSERT_EQ(0, abi->HtmlRequestGuard("/a", names, values, 2, "http", &guard));
  EXPECT_EQ(kPsHtmlGuardNone, guard.reason);
  // The library's argument checks stand: a short struct, a NULL path.
  PsHtmlRequestGuard short_guard = {};
  short_guard.struct_size = 8;
  EXPECT_EQ(
      -1, abi->HtmlRequestGuard("/a", names, values, 2, "http", &short_guard));
  EXPECT_EQ(-1,
            abi->HtmlRequestGuard(nullptr, names, values, 2, "http", &guard));
}

TEST_F(HtmlServe15SetTest, TheRequestPolicyCallsReachTheLibrary) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->HasHtmlServe15Set());

  const char* lines[] = {"_ga=GA1.2.3; _gid=GA1.4", "sid=abc"};
  PsHtmlCookieSpan values[4];
  PsHtmlCookieClass cookies = {};
  cookies.struct_size = sizeof(cookies);
  ASSERT_EQ(0, abi->HtmlClassifyCookies(lines, 1, 1, nullptr, 0, values, 4,
                                        &cookies));
  EXPECT_EQ(kPsHtmlCookiesIgnorableOnly, cookies.cookie_class);
  EXPECT_EQ(2u, cookies.ignorable_count);
  EXPECT_EQ(4u, values[0].offset);
  ASSERT_EQ(0, abi->HtmlClassifyCookies(lines, 2, 1, nullptr, 0, values, 4,
                                        &cookies));
  EXPECT_EQ(kPsHtmlCookiesBlocking, cookies.cookie_class);
  EXPECT_EQ(1u, cookies.first_blocking.line);
  EXPECT_EQ(3u, cookies.first_blocking.length);  // "sid", never its value

  int warning = -1;
  EXPECT_EQ(1, abi->HtmlIgnoreCookieGuard("_my_ab", &warning));
  EXPECT_EQ(0, warning);
  EXPECT_EQ(1, abi->HtmlIgnoreCookieGuard("_my*", &warning));
  EXPECT_EQ(1, warning);
  EXPECT_EQ(0, abi->HtmlIgnoreCookieGuard("*", &warning));
  EXPECT_EQ(-1, abi->HtmlIgnoreCookieGuard(nullptr, &warning));

  uint64_t a = 0;
  uint64_t b = 0;
  const char* additions[] = {"_my_ab"};
  ASSERT_EQ(0,
            abi->HtmlPolicyStamp(kPsHtmlCredentialedAnonymousOnly, 1,
                                 kPsHtmlCookieListVersion, additions, 1, &a));
  ASSERT_EQ(0,
            abi->HtmlPolicyStamp(kPsHtmlCredentialedExplicitPublic, 1,
                                 kPsHtmlCookieListVersion, additions, 1, &b));
  EXPECT_NE(0u, a);
  EXPECT_NE(a, b);
  EXPECT_EQ(-1, abi->HtmlPolicyStamp(7, 1, 1, nullptr, 0, &a));

  EXPECT_EQ(1, abi->HtmlUnsafeMethodInvalidates("POST", 200));
  EXPECT_EQ(1, abi->HtmlUnsafeMethodInvalidates("post", 303));
  EXPECT_EQ(1, abi->HtmlUnsafeMethodInvalidates(nullptr, 204));
  EXPECT_EQ(0, abi->HtmlUnsafeMethodInvalidates("POST", 404));
  EXPECT_EQ(0, abi->HtmlUnsafeMethodInvalidates("GET", 200));
}

// The block's stamp and the facts call that reads it.
TEST_F(HtmlServe15SetTest, TheEntryStampReachesTheFacts) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->HasHtmlServe15Set());
  setenv("PS_STUB_POLICY_STAMP", "4242", 1);
  int block = 0;
  uint64_t stamp = 0;
  EXPECT_EQ(1, abi->HeaderBlockPolicyStamp(&block, &stamp));
  EXPECT_EQ(4242u, stamp);
  EXPECT_EQ(-1, abi->HeaderBlockPolicyStamp(nullptr, &stamp));
  int original = 0;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  ASSERT_EQ(0, abi->HtmlStoredFactsFromReadsEx(&original, nullptr, nullptr, 1,
                                               &block, &facts));
  EXPECT_NE(0u, facts.flags & kPsHtmlFactsPresent);
  EXPECT_EQ(4242u, facts.entry_policy_stamp);
  ASSERT_EQ(0, abi->HtmlStoredFactsFromReadsEx(&original, nullptr, nullptr, 0,
                                               nullptr, &facts));
  EXPECT_EQ(0u, facts.entry_policy_stamp);
}

// A 1.15 library is sent the 1.15 sizes, and answers serve-cached-html.
TEST_F(HtmlServe15SetTest, AFifteenLibraryIsSentTheFifteenSizes) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  size_t request_size = 0;
  size_t facts_size = 0;
  GoogleString reason;
  ASSERT_EQ(0, Classify(*abi, kPsHtmlCacheServeCachedHtml, &request_size,
                        &facts_size, &reason));
  EXPECT_EQ(sizeof(PsHtmlServeRequest), request_size);
  EXPECT_EQ(sizeof(PsHtmlStoredFacts), facts_size);
  EXPECT_EQ("absent", reason);  // not `scope`: the 1.15 fields were read
}

// One store classification of a plain 200 in `mode`: the stub answers its
// fixed action, or the binding -1 when the call never reaches it.
int StoreClassifyIn(const DaemonAbi& abi, int mode) {
  const char* names[] = {"Content-Type"};
  const char* values[] = {"text/html"};
  int reason = 0;
  uint32_t lifetime_s = 0;
  uint32_t flags = 0;
  uint16_t cc_flags = 0;
  int sidecar = 0;
  char detail[64] = {0};
  size_t detail_len = 0;
  return abi.HtmlStoreClassify(mode, "GET", "/p", 0, 200, names, values, 1, 5,
                               1, "hello", 5, 1 << 20, 1789999999, &reason,
                               &lifetime_s, &flags, &cc_flags, &sidecar, detail,
                               sizeof(detail), &detail_len);
}

// The library refuses serve-cached-html for a request of the 1.14 size: the
// fail-safe zeros (scope agents, class unknown).  The store classifier is
// sent the mode as it is.
TEST_F(HtmlServe15SetTest, AFifteenLibraryRefusesAFourteenSizeCachedHtml) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  PsHtmlServeRequest request = {};
  request.struct_size = kPsHtmlServeRequestSize14;
  request.mode = kPsHtmlCacheServeCachedHtml;
  request.method = "GET";
  PsHtmlStoredFacts facts = {};
  facts.struct_size = kPsHtmlStoredFactsSize14;
  PsHtmlServeVerdict verdict = {};
  verdict.struct_size = sizeof(verdict);
  ASSERT_EQ(0, abi->HtmlServeClassify(&request, &facts, &verdict));
  EXPECT_STREQ("scope", abi->HtmlServeReasonName(verdict.reason));
  ASSERT_TRUE(abi->PublishesHtmlStorePolicy());
  EXPECT_LE(0, StoreClassifyIn(*abi, kPsHtmlCacheServeCachedHtml));
}

// The same library reporting 1.14: the 1.14 set binds, the 1.15 one does
// not, the binding sends nothing past the 1.14 sizes whatever the caller
// stamped, and never the serve-cached-html mode.
TEST_F(HtmlServe15SetTest, AFourteenLibraryIsSentOnlyTheFourteenShape) {
  std::unique_ptr<DaemonAbi> abi =
      Load("14", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  const GoogleString reason15 = abi->HtmlServe15SetUnavailableReason();
  EXPECT_NE(GoogleString::npos, reason15.find("reports C API 1.14"))
      << reason15;
  EXPECT_NE(GoogleString::npos, reason15.find("1.15")) << reason15;
  ExpectUnbound15Answers(*abi);

  size_t request_size = 0;
  size_t facts_size = 0;
  GoogleString reason;
  ASSERT_EQ(0, Classify(*abi, kPsHtmlCacheServeVariants, &request_size,
                        &facts_size, &reason));
  EXPECT_EQ(kPsHtmlServeRequestSize14, request_size);
  EXPECT_EQ(kPsHtmlStoredFactsSize14, facts_size);
  // Never serve-cached-html: refused before it reaches the library, on the
  // serve side and on the store side.
  EXPECT_EQ(-1, Classify(*abi, kPsHtmlCacheServeCachedHtml, &request_size,
                         &facts_size, &reason));
  ASSERT_TRUE(abi->PublishesHtmlStorePolicy());
  EXPECT_LE(0, StoreClassifyIn(*abi, kPsHtmlCacheServeVariants));
  EXPECT_EQ(-1, StoreClassifyIn(*abi, kPsHtmlCacheServeCachedHtml));

  // The facts are read at the 1.14 size and handed back at the caller's,
  // with no entry stamp.
  int original = 0;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  facts.entry_policy_stamp = 77;
  ASSERT_EQ(
      0, abi->HtmlStoredFactsFromReads(&original, nullptr, nullptr, 0, &facts));
  EXPECT_EQ(sizeof(PsHtmlStoredFacts), facts.struct_size);
  EXPECT_NE(0u, facts.flags & kPsHtmlFactsPresent);
  EXPECT_EQ(0u, facts.entry_policy_stamp);

  // The sidecar parameters go out at the 1.13 size, and a format the
  // library cannot write is refused rather than written as an older one.
  void* cache = nullptr;
  alignas(
      std::max_align_t) unsigned char config_storage[kCacheConfigProbeBytes];
  PsCacheConfig* config = reinterpret_cast<PsCacheConfig*>(config_storage);
  abi->CacheConfigInit(config);
  ASSERT_EQ(kPsOk, abi->CacheOpen(config, &cache));
  uint8_t hash[kPsHtmlHashBytes];
  memset(hash, 0xCD, sizeof(hash));
  PsSidecarWriteParams sidecar = {};
  sidecar.struct_size = sizeof(sidecar);
  sidecar.url = "http://b.example/p";
  sidecar.hostname = "b.example";
  sidecar.scheme = "http";
  sidecar.site = kSite;
  sidecar.origin_html_hash = hash;
  sidecar.write_flags = kPsSidecarWritePolicyBlock;
  const char* names[] = {"Content-Language"};
  const char* values[] = {"en"};
  int verdict = -1;
  int stored = -1;
  EXPECT_EQ(kPsErrInvalidArg,
            abi->CacheWriteHeadersSidecarSite(cache, &sidecar, names, values, 1,
                                              &verdict, &stored));
  sidecar.write_flags = 0;
  EXPECT_EQ(kPsOk, abi->CacheWriteHeadersSidecarSite(
                       cache, &sidecar, names, values, 1, &verdict, &stored));
  EXPECT_EQ(sizeof(PsSidecarWriteParams), sidecar.struct_size);
  abi->CacheClose(cache);
}

TEST_F(HtmlServe15SetTest, OneMissingFifteenSymbolLeavesOnlyTheFourteenSet) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_15_partial.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  const GoogleString reason = abi->HtmlServe15SetUnavailableReason();
  EXPECT_NE(GoogleString::npos,
            reason.find("ps_html_unsafe_method_invalidates"))
      << reason;
  ExpectUnbound15Answers(*abi);
}

// A library that reports 1.15 but exports none of its set: the 1.14 set
// still binds.
TEST_F(HtmlServe15SetTest, AFifteenLibraryWithoutTheSetKeepsTheFourteenSet) {
  std::unique_ptr<DaemonAbi> abi = Load("15", "libdaemon_stub_html_serve.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  ExpectUnbound15Answers(*abi);
}

// Without the 1.14 set there is no 1.15 set either.
TEST_F(HtmlServe15SetTest, NoFourteenSetMeansNoFifteenSet) {
  std::unique_ptr<DaemonAbi> abi =
      Load("13", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServeSet());
  ExpectUnbound15Answers(*abi);
}

class HtmlServe16SetTest : public HtmlServe15SetTest {
 protected:
  void SetUp() override {
    HtmlServe15SetTest::SetUp();
    unsetenv("PS_STUB_MARKDOWN_CODINGS");
    unsetenv("PS_STUB_NOT_SMALLER");
    setenv("PS_STUB_HTML_SERVE_ACTION", "2", 1);
  }
  void TearDown() override {
    unsetenv("PS_STUB_MARKDOWN_CODINGS");
    unsetenv("PS_STUB_NOT_SMALLER");
    unsetenv("PS_STUB_HTML_SERVE_ACTION");
    HtmlServe15SetTest::TearDown();
  }

  // A present page with a bound plain copy, as the facts call reports it.
  static PsHtmlStoredFacts BoundFacts(uint32_t extra) {
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    facts.flags = kPsHtmlFactsPresent | kPsHtmlFactsBindingLive | extra;
    memset(facts.binding_hash, 0x11, sizeof(facts.binding_hash));
    memset(facts.markdown_hash, 0x22, sizeof(facts.markdown_hash));
    return facts;
  }

  // One serve-variants Markdown classification asking for `coding`.
  static PsHtmlServeVerdict ServeCoded(const DaemonAbi& abi, int coding,
                                       uint32_t flags,
                                       const PsHtmlStoredFacts& facts) {
    PsHtmlServeRequest request = {};
    request.struct_size = sizeof(request);
    request.mode = kPsHtmlCacheServeVariants;
    request.method = "GET";
    request.accept = "text/markdown";
    request.request_flags = flags;
    request.markdown_encoding = static_cast<uint32_t>(coding);
    PsHtmlServeVerdict verdict;
    memset(&verdict, 0x5A, sizeof(verdict));  // a stale buffer
    verdict.struct_size = sizeof(verdict);
    EXPECT_EQ(0, abi.HtmlServeClassify(&request, &facts, &verdict));
    return verdict;
  }
};

TEST_F(HtmlServe16SetTest, ASixteenLibraryBindsEverySet) {
  std::unique_ptr<DaemonAbi> abi =
      Load("16", "libdaemon_stub_html_serve_16.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServeSet());
  EXPECT_TRUE(abi->HasHtmlServe15Set());
  EXPECT_TRUE(abi->HasHtmlServe16Set());
  EXPECT_EQ("", abi->HtmlServe16SetUnavailableReason());
  EXPECT_EQ(sizeof(PsHtmlServeVerdict), HtmlServeVerdictSize(*abi));
  EXPECT_STREQ("no_acceptable_encoding", abi->HtmlServeReasonName(27));
  EXPECT_EQ(nullptr, abi->HtmlServeReasonName(28));
}

TEST_F(HtmlServe16SetTest, TheCodingCallsReachTheLibrary) {
  std::unique_ptr<DaemonAbi> abi =
      Load("16", "libdaemon_stub_html_serve_16.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  ASSERT_TRUE(abi->HasHtmlServe16Set());
  const char* lines[] = {"gzip;q=1, br;q=0.5", "identity;q=0"};
  int order[3] = {-1, -1, -1};
  ASSERT_EQ(2, abi->MarkdownSelectEncoding(lines, 2, order));
  EXPECT_EQ(kPsMarkdownEncodingGzip, order[0]);
  EXPECT_EQ(kPsMarkdownEncodingBr, order[1]);
  ASSERT_EQ(1, abi->MarkdownSelectEncoding(nullptr, 0, order));
  EXPECT_EQ(kPsMarkdownEncodingIdentity, order[0]);
  const char* refused[] = {"identity;q=0"};
  EXPECT_EQ(0, abi->MarkdownSelectEncoding(refused, 1, order));
  EXPECT_EQ(-1, abi->MarkdownSelectEncoding(lines, 1, nullptr));

  EXPECT_EQ(3, abi->ReadSharedConfigMarkdownCodings("/unused"));
  setenv("PS_STUB_MARKDOWN_CODINGS", "-1", 1);
  EXPECT_EQ(-1, abi->ReadSharedConfigMarkdownCodings("/unused"));

  // The coded facts: the bound bit of the coding read, nothing for a miss,
  // and the not-smaller record in its place when the optimizer wrote one.
  int original = 0;
  int markdown = 0;
  int coded = 0;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  ASSERT_EQ(0, abi->HtmlStoredFactsFromReadsCoded(
                   &original, &markdown, nullptr, 0, nullptr, &coded,
                   kPsMarkdownEncodingBr, &facts));
  EXPECT_NE(0u, facts.flags & kPsHtmlFactsBrCopyBound);
  EXPECT_EQ(0u, facts.flags & kPsHtmlFactsGzipCopyBound);
  ASSERT_EQ(0, abi->HtmlStoredFactsFromReadsCoded(
                   &original, &markdown, nullptr, 0, nullptr, nullptr,
                   kPsMarkdownEncodingGzip, &facts));
  EXPECT_EQ(
      0u, facts.flags & (kPsHtmlFactsBrCopyBound | kPsHtmlFactsGzipCopyBound));
  setenv("PS_STUB_NOT_SMALLER", "1", 1);
  ASSERT_EQ(0, abi->HtmlStoredFactsFromReadsCoded(
                   &original, &markdown, nullptr, 0, nullptr, &coded,
                   kPsMarkdownEncodingGzip, &facts));
  EXPECT_NE(0u, facts.flags & kPsHtmlFactsGzipNotSmaller);
  EXPECT_EQ(0u, facts.flags & kPsHtmlFactsGzipCopyBound);
  EXPECT_EQ(-1, abi->HtmlStoredFactsFromReadsCoded(
                    &original, &markdown, nullptr, 0, nullptr, &coded,
                    kPsMarkdownEncodingIdentity, &facts));

  // Three tags of one copy, one per coding, all the same length.
  uint8_t binding[kPsHtmlHashBytes];
  uint8_t md[kPsHtmlHashBytes];
  memset(binding, 0x11, sizeof(binding));
  memset(md, 0x22, sizeof(md));
  char plain[64], gz[64], br[64], legacy[64];
  size_t a = 0, b = 0, c = 0, d = 0;
  ASSERT_EQ(0,
            abi->HtmlMarkdownEtagCoded(binding, md, kPsMarkdownEncodingIdentity,
                                       plain, sizeof(plain), &a));
  ASSERT_EQ(0, abi->HtmlMarkdownEtagCoded(binding, md, kPsMarkdownEncodingGzip,
                                          gz, sizeof(gz), &b));
  ASSERT_EQ(0, abi->HtmlMarkdownEtagCoded(binding, md, kPsMarkdownEncodingBr,
                                          br, sizeof(br), &c));
  ASSERT_EQ(0, abi->HtmlMarkdownEtag(binding, md, legacy, sizeof(legacy), &d));
  EXPECT_STREQ(legacy, plain);
  EXPECT_EQ(kPsHtmlMarkdownEtagChars, a);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
  EXPECT_STRNE(plain, gz);
  EXPECT_STRNE(plain, br);
  EXPECT_STRNE(gz, br);
}

// The verdict at its 1.16 size names the coding to send, and a stale buffer
// never leaves a coding behind.
TEST_F(HtmlServe16SetTest, TheVerdictNamesTheCoding) {
  std::unique_ptr<DaemonAbi> abi =
      Load("16", "libdaemon_stub_html_serve_16.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  PsHtmlServeVerdict v = ServeCoded(*abi, kPsMarkdownEncodingBr, 0,
                                    BoundFacts(kPsHtmlFactsBrCopyBound));
  EXPECT_EQ(kPsHtmlServeActionServeMarkdown, v.action);
  EXPECT_EQ(kPsMarkdownEncodingBr, v.markdown_encoding);
  EXPECT_EQ(0, v.reserved_1_16);
  v = ServeCoded(*abi, kPsMarkdownEncodingBr, kPsHtmlServeRequestIdentityOk,
                 BoundFacts(0));
  EXPECT_EQ(kPsHtmlServeActionServeMarkdown, v.action);
  EXPECT_EQ(kPsMarkdownEncodingIdentity, v.markdown_encoding);
  v = ServeCoded(*abi, kPsMarkdownEncodingBr, 0, BoundFacts(0));
  EXPECT_EQ(kPsHtmlServeActionFallThrough, v.action);
  EXPECT_STREQ("no_acceptable_encoding", abi->HtmlServeReasonName(v.reason));
  v = ServeCoded(*abi, kPsMarkdownEncodingIdentity,
                 kPsHtmlServeRequestEncodingNone, BoundFacts(0));
  EXPECT_EQ(kPsHtmlServeActionFallThrough, v.action);
  EXPECT_STREQ("no_acceptable_encoding", abi->HtmlServeReasonName(v.reason));
}

// The same library reporting 1.15: the 1.16 set stays unbound, the library
// is handed the 1.15 verdict size and no coding, and the caller's 1.16
// field reads identity whatever the buffer held.
TEST_F(HtmlServe16SetTest, AFifteenLibraryIsSentOnlyTheFifteenShape) {
  std::unique_ptr<DaemonAbi> abi =
      Load("15", "libdaemon_stub_html_serve_16.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServe15Set());
  const GoogleString reason = abi->HtmlServe16SetUnavailableReason();
  EXPECT_NE(GoogleString::npos, reason.find("reports C API 1.15")) << reason;
  ExpectUnbound16Answers(*abi);
  PsHtmlServeVerdict v =
      ServeCoded(*abi, kPsMarkdownEncodingBr, kPsHtmlServeRequestEncodingNone,
                 BoundFacts(kPsHtmlFactsBrCopyBound));
  // ENCODING_NONE never reached it: the plain copy, as before.
  EXPECT_EQ(kPsHtmlServeActionServeMarkdown, v.action);
  EXPECT_EQ(sizeof(PsHtmlServeVerdict), v.struct_size);
  EXPECT_EQ(kPsMarkdownEncodingIdentity, v.markdown_encoding);
  EXPECT_EQ(0, v.reserved_1_16);
}

TEST_F(HtmlServe16SetTest, OneMissingSixteenSymbolLeavesOnlyTheFifteenSet) {
  std::unique_ptr<DaemonAbi> abi =
      Load("16", "libdaemon_stub_html_serve_16_partial.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServe15Set());
  const GoogleString reason = abi->HtmlServe16SetUnavailableReason();
  EXPECT_NE(GoogleString::npos, reason.find("ps_markdown_select_encoding"))
      << reason;
  ExpectUnbound16Answers(*abi);
}

// A library that reports 1.16 but exports none of its set keeps the 1.15
// set; without the 1.15 set there is no 1.16 set either.
TEST_F(HtmlServe16SetTest, ASixteenLibraryWithoutTheSetKeepsTheFifteenSet) {
  std::unique_ptr<DaemonAbi> abi =
      Load("16", "libdaemon_stub_html_serve_15.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_TRUE(abi->HasHtmlServe15Set());
  ExpectUnbound16Answers(*abi);
  abi = Load("16", "libdaemon_stub_html_serve_15_partial.so");
  ASSERT_TRUE(abi != nullptr) << error_;
  EXPECT_FALSE(abi->HasHtmlServe15Set());
  ExpectUnbound16Answers(*abi);
}

}  // namespace

}  // namespace net_instaweb
