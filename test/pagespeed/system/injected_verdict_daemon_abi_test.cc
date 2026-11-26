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

// The injected-verdict fake answers every entry point of the HTML serve set
// with what a case injects, and records what it was asked.

#include "test/pagespeed/system/injected_verdict_daemon_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/system/daemon_abi.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/system/daemon_stub_test_util.h"

namespace net_instaweb {

namespace {

constexpr char kSite[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

class InjectedVerdictDaemonAbiTest : public testing::Test {
 protected:
  void SetUp() override {
    inner_.reset(LoadDaemonAbi(StubLibraryPath("libdaemon_stub.so"), &error_));
    ASSERT_TRUE(inner_ != nullptr) << error_;
    fake_ = std::make_unique<InjectedVerdictDaemonAbi>(inner_.get());
  }

  PsHtmlServeVerdict Classify(const PsHtmlServeRequest& request) {
    PsHtmlStoredFacts facts = {};
    facts.struct_size = sizeof(facts);
    facts.flags = kPsHtmlFactsPresent;
    PsHtmlServeVerdict out = {};
    out.struct_size = sizeof(out);
    EXPECT_EQ(0, fake_->HtmlServeClassify(&request, &facts, &out));
    return out;
  }

  GoogleString error_;
  std::unique_ptr<DaemonAbi> inner_;
  std::unique_ptr<InjectedVerdictDaemonAbi> fake_;
};

TEST_F(InjectedVerdictDaemonAbiTest, EveryServeVerdictCanBeInjected) {
  PsHtmlServeRequest request = {};
  request.struct_size = sizeof(request);
  request.mode = kPsHtmlCacheServeVariants;
  request.method = "GET";
  for (int action :
       {kPsHtmlServeActionFallThrough, kPsHtmlServeActionServeHtml,
        kPsHtmlServeActionServeMarkdown, kPsHtmlServeActionValidate}) {
    for (int reason = 0; reason < 19; ++reason) {
      for (int not_modified : {0, 1}) {
        PsHtmlServeVerdict* v = &fake_->mutable_injection()->verdict;
        v->action = action;
        v->reason = reason;
        v->enforce = 1;
        v->wants_markdown = 1;
        v->age_s = 7;
        v->not_modified = not_modified;
        snprintf(v->etag, sizeof(v->etag), "W/\"psm-%016x\"",
                 static_cast<unsigned>(reason));
        const PsHtmlServeVerdict out = Classify(request);
        EXPECT_EQ(sizeof(PsHtmlServeVerdict), out.struct_size);
        EXPECT_EQ(action, out.action);
        EXPECT_EQ(reason, out.reason);
        EXPECT_EQ(1, out.enforce);
        EXPECT_EQ(1, out.wants_markdown);
        EXPECT_EQ(7u, out.age_s);
        EXPECT_EQ(not_modified, out.not_modified);
        EXPECT_STREQ(v->etag, out.etag);
        EXPECT_TRUE(fake_->HtmlServeReasonName(reason) != nullptr);
      }
    }
  }
  EXPECT_STREQ("skew", fake_->HtmlServeReasonName(18));
  EXPECT_EQ(nullptr, fake_->HtmlServeReasonName(19));
  EXPECT_EQ(nullptr, fake_->HtmlServeReasonName(-1));

  // A malformed-call answer is injectable too, and then nothing is written.
  fake_->mutable_injection()->serve_rc = -1;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  PsHtmlServeVerdict out = {};
  out.struct_size = sizeof(out);
  out.action = 42;
  EXPECT_EQ(-1, fake_->HtmlServeClassify(&request, &facts, &out));
  EXPECT_EQ(42, out.action);
}

TEST_F(InjectedVerdictDaemonAbiTest, TheVerdictIsWrittenOnlyAsFarAsTheCaller) {
  fake_->mutable_injection()->verdict.action = kPsHtmlServeActionServeMarkdown;
  fake_->mutable_injection()->verdict.not_modified = 1;
  PsHtmlServeRequest request = {};
  request.struct_size = sizeof(request);
  request.method = "GET";
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  PsHtmlServeVerdict out;
  memset(&out, 0x5A, sizeof(out));
  out.struct_size = offsetof(PsHtmlServeVerdict, not_modified);
  ASSERT_EQ(0, fake_->HtmlServeClassify(&request, &facts, &out));
  EXPECT_EQ(kPsHtmlServeActionServeMarkdown, out.action);
  EXPECT_EQ(0x5A5A5A5A, out.not_modified);
}

TEST_F(InjectedVerdictDaemonAbiTest, TheRequestAndFactsAreRecordedExactly) {
  PsHtmlServeRequest request = {};
  request.struct_size = sizeof(request);
  request.mode = kPsHtmlCacheServeVariants;
  request.method = "HEAD";
  request.accept = "text/markdown";
  request.if_none_match = "";
  request.request_flags = kPsHtmlServeRequestHasCookie;
  request.now_seconds = 1234;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  facts.flags = kPsHtmlFactsPresent | kPsHtmlFactsPurged;
  facts.stored_at = 1000;
  facts.binding_hash[0] = 0xEE;
  PsHtmlServeVerdict out = {};
  out.struct_size = sizeof(out);
  ASSERT_EQ(0, fake_->HtmlServeClassify(&request, &facts, &out));

  ASSERT_EQ(1u, fake_->serve_requests().size());
  const RecordedServeRequest& r = fake_->serve_requests()[0];
  EXPECT_EQ(sizeof(PsHtmlServeRequest), r.struct_size);
  EXPECT_EQ(kPsHtmlCacheServeVariants, r.mode);
  EXPECT_EQ("HEAD", r.method);
  EXPECT_EQ("text/markdown", r.accept);
  EXPECT_FALSE(r.accept_null);
  EXPECT_TRUE(r.cache_control_null);
  EXPECT_TRUE(r.pragma_null);
  EXPECT_FALSE(r.if_none_match_null);  // present but empty is not absent
  EXPECT_EQ(kPsHtmlServeRequestHasCookie, r.request_flags);
  EXPECT_EQ(1234u, r.now_seconds);
  ASSERT_EQ(1u, fake_->serve_facts().size());
  EXPECT_EQ(0, memcmp(&facts, &fake_->serve_facts()[0], sizeof(facts)));
}

TEST_F(InjectedVerdictDaemonAbiTest, StoreWriteParamsAndFactsAreInjectable) {
  PsWriteParams params = {};
  params.struct_size = sizeof(params);
  const char* age[] = {"5", "5"};
  ASSERT_EQ(0, fake_->HtmlStoreWriteParams(kPsHtmlStoreActionStore, 60, 1, 0, 1,
                                           2000500, age, 2, 99, "text/html",
                                           &params));
  EXPECT_EQ(2000u, params.cache_inserted_at);
  EXPECT_EQ(kPsSentinelOriginal, params.alternate_id);
  EXPECT_EQ(kPsFlagHeadersClassified | kPsFlagHeadersSidecarRequired,
            params.flags);
  ASSERT_FALSE(fake_->html_calls().empty());
  const InjectedHtmlCall& call = fake_->html_calls().back();
  EXPECT_EQ("ps_html_store_write_params", call.fn);
  EXPECT_EQ("2000500", call.args[5]);
  EXPECT_EQ("age=5", call.args.back());

  fake_->mutable_injection()->write_params_inserted_at = 1900;
  ASSERT_EQ(0, fake_->HtmlStoreWriteParams(kPsHtmlStoreActionStore, 60, 1, 0, 0,
                                           2000500, nullptr, 0, 99, "text/html",
                                           &params));
  EXPECT_EQ(1900u, params.cache_inserted_at);
  // A refused Age: the call fails and the caller stores nothing.
  fake_->mutable_injection()->write_params_rc = -1;
  EXPECT_EQ(-1, fake_->HtmlStoreWriteParams(kPsHtmlStoreActionStore, 60, 1, 0,
                                            0, 2000500, nullptr, 0, 99,
                                            "text/html", &params));

  fake_->mutable_injection()->facts.flags =
      kPsHtmlFactsPresent | kPsHtmlFactsWithheld;
  fake_->mutable_injection()->facts.withheld_reason = kPsHtmlWithheldBelowFloor;
  PsHtmlStoredFacts facts = {};
  facts.struct_size = sizeof(facts);
  int original = 0;
  ASSERT_EQ(0, fake_->HtmlStoredFactsFromReads(&original, nullptr, &original, 1,
                                               &facts));
  EXPECT_EQ(kPsHtmlFactsPresent | kPsHtmlFactsWithheld, facts.flags);
  EXPECT_EQ(kPsHtmlWithheldBelowFloor, facts.withheld_reason);
  EXPECT_EQ("withheld", fake_->html_calls().back().args[2]);
  EXPECT_EQ("null", fake_->html_calls().back().args[1]);
}

TEST_F(InjectedVerdictDaemonAbiTest, EtagAndIfNoneMatchAreInjectable) {
  uint8_t binding[kPsHtmlHashBytes] = {1};
  uint8_t markdown[kPsHtmlHashBytes] = {2};
  char buf[64];
  size_t len = 0;
  ASSERT_EQ(0,
            fake_->HtmlMarkdownEtag(binding, markdown, buf, sizeof(buf), &len));
  EXPECT_STREQ("W/\"psm-0123456789abcdef\"", buf);
  EXPECT_EQ(kPsHtmlMarkdownEtagChars, len);
  EXPECT_EQ(-3, fake_->HtmlMarkdownEtag(binding, markdown, buf, 4, &len));
  fake_->mutable_injection()->etag_rc = -1;
  EXPECT_EQ(-1,
            fake_->HtmlMarkdownEtag(binding, markdown, buf, sizeof(buf), &len));

  EXPECT_EQ(0, fake_->IfNoneMatchMatchesWeak("W/\"psm-x\"", buf));
  fake_->mutable_injection()->inm_match = 1;
  EXPECT_EQ(1, fake_->IfNoneMatchMatchesWeak("W/\"psm-x\"", buf));
  EXPECT_EQ("W/\"psm-x\"", fake_->html_calls().back().args[0]);
}

TEST_F(InjectedVerdictDaemonAbiTest, SidecarWritesAndReadsAreInjectable) {
  uint8_t hash[kPsHtmlHashBytes];
  memset(hash, 0xAB, sizeof(hash));
  PsSidecarWriteParams params = {};
  params.struct_size = sizeof(params);
  params.url = "http://a.example/";
  params.hostname = "a.example";
  params.scheme = "http";
  params.site = kSite;
  params.origin_html_hash = hash;
  const char* names[] = {"Link", "Content-Language"};
  const char* values[] = {"</a.css>; rel=preload", "en"};
  int verdict = -1;
  int stored = -1;
  ASSERT_EQ(kPsOk, fake_->CacheWriteHeadersSidecarSite(
                       nullptr, &params, names, values, 2, &verdict, &stored));
  EXPECT_EQ(0, verdict);
  EXPECT_EQ(1, stored);
  ASSERT_EQ(1u, fake_->sidecar_writes().size());
  EXPECT_EQ("Content-Language", fake_->sidecar_writes()[0][1].first);
  EXPECT_EQ(kSite, fake_->html_calls().back().args[4]);

  fake_->mutable_injection()->sidecar_write_rc = 2;  // an I/O failure
  EXPECT_EQ(2, fake_->CacheWriteHeadersSidecarSite(
                   nullptr, &params, names, values, 2, &verdict, &stored));

  void* block = &verdict;
  EXPECT_EQ(kPsErrNotFound, fake_->HeadersSidecarRead(nullptr, "u", "h", "http",
                                                      kSite, &block));
  EXPECT_EQ(nullptr, block);

  HtmlServeInjection* in = fake_->mutable_injection();
  in->sidecar_read_rc = kPsOk;
  in->block.lines = {{"Link", "</a.css>"}};
  in->block.has_pairing_hash = true;
  memcpy(in->block.pairing_hash, hash, sizeof(hash));
  for (int pairing : {kPsSidecarPaired, kPsSidecarUnpaired}) {
    in->pairing = pairing;
    ASSERT_EQ(kPsOk, fake_->HeadersSidecarRead(nullptr, "u", "h", "http", kSite,
                                               &block));
    ASSERT_TRUE(block != nullptr);
    EXPECT_EQ(1, fake_->live_blocks());
    EXPECT_EQ(1u, fake_->HeaderBlockCount(block));
    const char* name = nullptr;
    size_t name_len = 0;
    ASSERT_EQ(kPsOk, fake_->HeaderBlockGet(block, 0, &name, &name_len, nullptr,
                                           nullptr));
    EXPECT_STREQ("Link", name);
    EXPECT_EQ(4u, name_len);
    EXPECT_EQ(kPsErrInvalidArg, fake_->HeaderBlockGet(block, 1, &name, nullptr,
                                                      nullptr, nullptr));
    EXPECT_EQ(0,
              memcmp(hash, fake_->HeaderBlockPairingHash(block), sizeof(hash)));
    EXPECT_EQ(pairing, fake_->HeaderBlockPairing(block, hash));
    fake_->HeaderBlockFree(block);
    EXPECT_EQ(0, fake_->live_blocks());
  }
  EXPECT_EQ(-1, fake_->HeaderBlockPairing(nullptr, hash));
}

TEST_F(InjectedVerdictDaemonAbiTest, RemovalEpochAndWorkerStateAreInjectable) {
  EXPECT_EQ(kPsOk, fake_->CacheRemoveSite(nullptr, "u", "h", "http", kSite));
  fake_->mutable_injection()->remove_rc = kPsErrNotFound;
  EXPECT_EQ(kPsErrNotFound,
            fake_->CacheRemoveSite(nullptr, "u", "h", "http", kSite));
  EXPECT_EQ("ps_cache_remove_site", fake_->html_calls().back().fn);
  EXPECT_EQ(kSite, fake_->html_calls().back().args[3]);

  EXPECT_EQ(kPsOk, fake_->CacheWriteSitePurgeEpoch(nullptr, kSite, 77));
  EXPECT_EQ("77", fake_->html_calls().back().args[1]);
  fake_->mutable_injection()->epoch_write_rc = 2;
  EXPECT_EQ(2, fake_->CacheWriteSitePurgeEpoch(nullptr, kSite, 78));

  fake_->mutable_injection()->epoch = 1500;
  uint64_t epoch = 0;
  ASSERT_EQ(kPsOk, fake_->CacheReadSitePurgeEpoch(nullptr, kSite, &epoch));
  EXPECT_EQ(1500u, epoch);
  // A failed read leaves the out-parameter alone.
  fake_->mutable_injection()->epoch_read_rc = 3;  // corrupted
  epoch = 9;
  EXPECT_EQ(3, fake_->CacheReadSitePurgeEpoch(nullptr, kSite, &epoch));
  EXPECT_EQ(9u, epoch);

  EXPECT_EQ(kPsHtmlModeMarkdownStatic, fake_->ReadSharedConfigHtmlMode("/c"));
  fake_->mutable_injection()->html_mode = kPsHtmlModeFull;
  EXPECT_EQ(kPsHtmlModeFull, fake_->ReadSharedConfigHtmlMode("/c"));
  EXPECT_EQ(1, fake_->ReadSharedConfigAgentEntitled("/c"));
  fake_->mutable_injection()->agent_entitled = 0;
  EXPECT_EQ(0, fake_->ReadSharedConfigAgentEntitled("/c"));
  EXPECT_EQ(0, fake_->WantsAgentMarkdown("text/markdown"));
  fake_->mutable_injection()->wants_markdown = 1;
  EXPECT_EQ(1, fake_->WantsAgentMarkdown("text/markdown"));

  EXPECT_TRUE(fake_->HasHtmlServeSet());
  fake_->mutable_injection()->has_set = false;
  fake_->mutable_injection()->unavailable_reason = "too old";
  EXPECT_FALSE(fake_->HasHtmlServeSet());
  EXPECT_EQ("too old", fake_->HtmlServeSetUnavailableReason());
}

TEST_F(InjectedVerdictDaemonAbiTest, OtherCallsStillReachTheRealBinding) {
  // Everything outside the HTML serve set forwards, counted.
  const int before = fake_->calls();
  EXPECT_EQ(inner_->VersionMinor(), fake_->VersionMinor());
  EXPECT_EQ(before + 1, fake_->calls());
  EXPECT_TRUE(fake_->html_calls().empty());
}

}  // namespace

}  // namespace net_instaweb
