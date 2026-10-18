// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_store.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/packs/decision.h"
#include "pagespeed/kernel/packs/kind_registry.h"
#include "pagespeed/kernel/packs/pack.h"
#include "pagespeed/kernel/packs/signature_format.h"
#include "pagespeed/kernel/packs/template.h"
#include "pagespeed/system/rule_pack_decision_log.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "test/net/instaweb/rewriter/rewrite_options_test_base.h"
#include <openssl/curve25519.h>
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"
#include "test/pagespeed/kernel/base/mock_timer.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"

namespace net_instaweb {
namespace {

namespace fs = std::filesystem;
using LoadEvent = RulePackProvider::LoadEvent;
using LogResult = RulePackProvider::LogResult;
using SignatureEvent = RulePackProvider::SignatureEvent;
using pagespeed::packs::PackTrust;
using pagespeed::packs::TrustKey;

// 2026-10-11 12:00:00 UTC.
constexpr int64_t kNoon = 1791720000000LL;

std::string PackJson(const std::string& version, const std::string& title) {
  return absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":")", version, R"("},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[)",
      R"({"id":"t","kind":"title","enforce":true,)", R"("value":{"template":")",
      title, R"("}}]})");
}

std::string PackJsonWithLicense(const std::string& version,
                                const std::string& title) {
  return absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":")", version,
      R"(","license":{"entitlement":"edge-seo","expires":"2027-10-31",)",
      R"("terms":"https://example.com/packs/terms"}},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[)",
      R"({"id":"t","kind":"title","enforce":true,)", R"("value":{"template":")",
      title, R"("}}]})");
}

std::string SignedPackJson(const std::string& version,
                           const std::string& title) {
  return absl::StrCat(
      R"({"pack":{"id":"edge-seo","publisher":"example-publisher","version":")",
      version, R"("},)",
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[)",
      R"({"id":"t","kind":"title","enforce":true,)", R"("value":{"template":")",
      title, R"("}}]})");
}

// An Ed25519 key pair for the tests.
struct TestKey {
  TestKey() {
    uint8_t pub[32];
    ED25519_keypair(pub, private_key);
    public_key.assign(reinterpret_cast<const char*>(pub), sizeof(pub));
  }
  std::string Sign(const std::string& message) const {
    uint8_t sig[64];
    EXPECT_EQ(1, ED25519_sign(sig, reinterpret_cast<const uint8_t*>(message.data()),
                              message.size(), private_key));
    return std::string(reinterpret_cast<const char*>(sig), sizeof(sig));
  }
  uint8_t private_key[64];
  std::string public_key;
};

std::string ReadAll(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class RulePackStoreTest : public RewriteOptionsTestBase<SystemRewriteOptions> {
 protected:
  RulePackStoreTest()
      : timer_(new NullMutex, kNoon),
        handler_(new NullMutex),
        store_(&timer_, "1.17.0"),
        options_("test", &thread_system_) {}

  void SetUp() override {
    const testing::TestInfo* info =
        testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::path(GTestTempDir()) / "rule_pack_store" / info->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "pack.json").string();
  }

  // Writes `contents` to the pack file and moves its modification time on,
  // so a change is visible whatever the file system's time resolution.
  void WritePack(const std::string& contents) {
    std::ofstream(path_, std::ios::binary | std::ios::trunc) << contents;
    ++writes_;
    fs::last_write_time(path_, fs::file_time_type::clock::now() +
                                   std::chrono::seconds(writes_));
  }

  LoadedRulePack Get(LoadEvent* event = nullptr) {
    LoadEvent ignored;
    SignatureEvent ignored_signature;
    return store_.Get(path_, PackTrust(), &handler_,
                      event == nullptr ? &ignored : event,
                      &ignored_signature);
  }

  LoadedRulePack GetWith(const PackTrust& trust, SignatureEvent* sig_event,
                         LoadEvent* event = nullptr) {
    LoadEvent ignored;
    return store_.Get(path_, trust, &handler_,
                      event == nullptr ? &ignored : event, sig_event);
  }

  void WriteSidecarText(const std::string& text) {
    std::ofstream(path_ + ".sig", std::ios::binary | std::ios::trunc) << text;
    ++writes_;
    fs::last_write_time(path_ + ".sig",
                        fs::file_time_type::clock::now() +
                            std::chrono::seconds(writes_));
  }

  // Signs the pack file as it is now with `key`, under `key_id`.
  void WriteSidecar(const TestKey& key,
                    const std::string& key_id = "example-publisher") {
    WriteSidecarText(absl::StrCat("ed25519 ", key_id, " ",
                                  absl::Base64Escape(key.Sign(ReadAll(path_))),
                                  "\n"));
  }

  static PackTrust Trust(bool require, const TestKey& key,
                         const std::string& key_id = "example-publisher") {
    PackTrust trust;
    trust.require_signed = require;
    trust.keys.push_back(TrustKey{key_id, key.public_key});
    return trust;
  }

  static std::string LogOf(MockMessageHandler* handler) {
    GoogleString log;
    StringWriter writer(&log);
    handler->Dump(&writer);
    return log;
  }
  std::string Log() { return LogOf(&handler_); }

  void PassRecheckInterval() {
    timer_.AdvanceMs(RulePackStore::kPackRecheckIntervalSec * 1000);
  }

  MockTimer timer_;
  MockMessageHandler handler_;
  RulePackStore store_;
  NullThreadSystem thread_system_;
  SystemRewriteOptions options_;
  fs::path dir_;
  std::string path_;
  int writes_ = 0;
};

TEST_F(RulePackStoreTest, ALicensedPackLogsItsLicenseOnce) {
  WritePack(PackJsonWithLicense("0.1.0", "A"));
  LoadedRulePack loaded = Get();
  ASSERT_NE(nullptr, loaded.pack);
  // The unsigned line is unchanged; the licence line is one kInfo message.
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  EXPECT_EQ(1, handler_.MessagesOfType(kInfo));
  const std::string expected = absl::StrCat(
      "rule pack license: edge-seo@0.1.0 entitlement=edge-seo ",
      "expires=2027-10-31 terms=https://example.com/packs/terms sha256=",
      loaded.sha256, " path=", fs::canonical(path_).string());
  GoogleString log;
  StringWriter writer(&log);
  handler_.Dump(&writer);
  EXPECT_NE(std::string::npos, log.find(expected)) << log;

  // A new version is a new sha: announced again.
  PassRecheckInterval();
  WritePack(PackJsonWithLicense("0.2.0", "B"));
  Get();
  EXPECT_EQ(2, handler_.MessagesOfType(kInfo));

  // The exact v0.1.0 bytes again are a known sha: not announced a third time.
  PassRecheckInterval();
  WritePack(PackJsonWithLicense("0.1.0", "A"));
  LoadEvent event;
  LoadedRulePack back = Get(&event);
  EXPECT_EQ(LoadEvent::kLoaded, event);
  EXPECT_EQ(loaded.sha256, back.sha256);
  EXPECT_EQ(2, handler_.MessagesOfType(kInfo));
  EXPECT_EQ(2, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, AFreePackLogsNoLicenseLine) {
  WritePack(PackJson("0.1.0", "A"));
  Get();
  EXPECT_EQ(0, handler_.MessagesOfType(kInfo));
}

TEST_F(RulePackStoreTest, LoadsOnFirstUseAndKeepsThePack) {
  WritePack(PackJson("0.1.0", "A"));
  LoadEvent event;
  LoadedRulePack first = Get(&event);
  ASSERT_NE(nullptr, first.pack);
  EXPECT_EQ(LoadEvent::kLoaded, event);
  EXPECT_EQ("0.1.0", first.pack->info.version);
  EXPECT_EQ(64u, first.sha256.size());
  // Loaded unsigned: said once, with the hash and the path.
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));

  LoadedRulePack again = Get(&event);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(first.pack, again.pack);
  PassRecheckInterval();
  again = Get(&event);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(first.pack, again.pack);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, AChangedFileIsReloadedAfterTheRecheckInterval) {
  WritePack(PackJson("0.1.0", "A"));
  LoadedRulePack first = Get();
  WritePack(PackJson("0.2.0", "Longer title"));
  // Not looked at again within the interval.
  LoadEvent event;
  EXPECT_EQ(first.pack, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
  PassRecheckInterval();
  LoadedRulePack second = Get(&event);
  EXPECT_EQ(LoadEvent::kLoaded, event);
  ASSERT_NE(nullptr, second.pack);
  EXPECT_EQ("0.2.0", second.pack->info.version);
  EXPECT_NE(first.sha256, second.sha256);
  // The filter of a running document keeps the pack it was given.
  EXPECT_EQ("0.1.0", first.pack->info.version);
}

TEST_F(RulePackStoreTest, TheRecordOfAnnouncedLoadsIsBounded) {
  // Every distinct pack version announces its load; a long-lived process
  // that sees many versions must not remember them all.
  const size_t kVersions = RulePackStore::kMaxAnnounced / 2 + 16;
  for (size_t i = 0; i < kVersions; ++i) {
    WritePack(PackJson(absl::StrCat("0.", i, ".0"), "A"));
    PassRecheckInterval();
    ASSERT_NE(nullptr, Get().pack) << i;
  }
  EXPECT_LE(store_.announced_size_for_testing(), RulePackStore::kMaxAnnounced);
}

TEST_F(RulePackStoreTest, StatusReportsTheLoadStateWithoutLoading) {
  using State = RulePackStatus::State;
  WritePack(PackJson("0.1.0", "A"));
  EXPECT_EQ(State::kNone, store_.Status(path_).state);  // not read yet
  Get();
  RulePackStatus loaded = store_.Status(path_);
  EXPECT_EQ(State::kLoaded, loaded.state);
  ASSERT_NE(nullptr, loaded.loaded.pack);
  EXPECT_EQ("edge-seo", loaded.loaded.pack->info.id);
  EXPECT_EQ(kNoon, loaded.last_event_ms);
  EXPECT_TRUE(loaded.last_error.empty());

  WritePack("{not json");
  PassRecheckInterval();
  Get();
  RulePackStatus kept = store_.Status(path_);
  EXPECT_EQ(State::kFailedKeepingEarlier, kept.state);
  EXPECT_FALSE(kept.last_error.empty());
  EXPECT_NE(nullptr, kept.loaded.pack);
  EXPECT_EQ(kNoon + RulePackStore::kPackRecheckIntervalSec * 1000,
            kept.last_event_ms);

  WritePack(PackJson("0.2.0", "B"));
  PassRecheckInterval();
  Get();
  EXPECT_EQ(State::kLoaded, store_.Status(path_).state);
  EXPECT_TRUE(store_.Status(path_).last_error.empty());
}

TEST_F(RulePackStoreTest, PackEditsLinkHeaderOnlyForACanonicalLinkHeaderRule) {
  const std::string head =
      R"({"pack":{"id":"edge-seo","version":"0.1.0"},)"
      R"("sites":[{"host":"t.test","mode":"enforce"}],"rules":[)";
  const std::string canonical =
      R"({"id":"c","kind":"canonical","enforce":true,)"
      R"("value":{"template":"https://t.test{path}"})";
  // Not looked at yet: unknown, so the answer is yes.
  WritePack(head + canonical + "}]}");
  EXPECT_TRUE(store_.PackEditsLinkHeader(path_));
  // A pack without a link_header rule.
  Get();
  EXPECT_FALSE(store_.PackEditsLinkHeader(path_));
  // Computed at load: a new version with the rule flips it.
  WritePack(head + canonical + R"(,"link_header":"match"}]})");
  PassRecheckInterval();
  Get();
  EXPECT_TRUE(store_.PackEditsLinkHeader(path_));
  // And back, once the file is changed again.
  WritePack(PackJson("0.3.0", "A"));
  PassRecheckInterval();
  Get();
  EXPECT_FALSE(store_.PackEditsLinkHeader(path_));
}

TEST_F(RulePackStoreTest, ARefusedUpdateKeepsTheEarlierPacksLinkHeaderRule) {
  TestKey key;
  const PackTrust trust = Trust(true, key);
  const std::string rule =
      R"("rules":[{"id":"c","kind":"canonical","enforce":true,)"
      R"("value":{"template":"https://t.test{path}"})";
  const std::string head =
      R"({"pack":{"id":"edge-seo","publisher":"example-publisher","version":")";
  WritePack(head + R"(0.1.0"},"sites":[{"host":"t.test"}],)" + rule +
            R"(,"link_header":"match"}]})");
  WriteSidecar(key);
  SignatureEvent sig;
  ASSERT_NE(nullptr, GetWith(trust, &sig).pack);
  EXPECT_TRUE(store_.PackEditsLinkHeader(path_));

  // An unsigned replacement without the rule is refused; the signed pack
  // stays in use and still edits the header.
  PassRecheckInterval();
  fs::remove(path_ + ".sig");
  WritePack(head + R"(0.2.0"},"sites":[{"host":"t.test"}],)" + rule + "}]}");
  GetWith(trust, &sig);
  EXPECT_EQ(SignatureEvent::kRefusedUnsigned, sig);
  EXPECT_TRUE(store_.PackEditsLinkHeader(path_));
}

TEST_F(RulePackStoreTest, PackEditsLinkHeaderWhileTheFirstLoadIsUnderWay) {
  WritePack(PackJson("0.1.0", "A"));
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  store_.set_refresh_hook_for_testing([&] {
    entered.set_value();
    released.wait();
  });
  std::thread loader([&] { Get(); });
  entered.get_future().wait();
  EXPECT_TRUE(store_.PackEditsLinkHeader(path_));
  release.set_value();
  loader.join();
  EXPECT_FALSE(store_.PackEditsLinkHeader(path_));
}

TEST_F(RulePackStoreTest, NoPackEditsNoLinkHeaderAfterAFailedFirstLoad) {
  WritePack("{not json");
  Get();
  EXPECT_FALSE(store_.PackEditsLinkHeader(path_));
}

TEST_F(RulePackStoreTest, StatusOfAFirstBadLoadIsRefused) {
  WritePack("{not json");
  Get();
  RulePackStatus status = store_.Status(path_);
  EXPECT_EQ(RulePackStatus::State::kRefused, status.state);
  EXPECT_EQ(nullptr, status.loaded.pack);
  EXPECT_FALSE(status.last_error.empty());
}

TEST_F(RulePackStoreTest, ATouchedButUnchangedFileIsNotReparsed) {
  WritePack(PackJson("0.1.0", "A"));
  LoadedRulePack first = Get();
  WritePack(PackJson("0.1.0", "A"));  // same bytes, new modification time
  PassRecheckInterval();
  LoadEvent event;
  EXPECT_EQ(first.pack, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
}

TEST_F(RulePackStoreTest, ABadReloadKeepsTheLastGoodPackAndSaysSoOnce) {
  WritePack(PackJson("0.1.0", "A"));
  LoadedRulePack good = Get();
  const int loaded_warnings = handler_.MessagesOfType(kWarning);
  WritePack("{\"pack\": not json");
  PassRecheckInterval();
  LoadEvent event;
  LoadedRulePack after = Get(&event);
  EXPECT_EQ(LoadEvent::kFailed, event);
  EXPECT_EQ(good.pack, after.pack);
  EXPECT_EQ(good.sha256, after.sha256);
  EXPECT_EQ(loaded_warnings + 1, handler_.MessagesOfType(kWarning));
  // The same bad file again (touched): not reported again.
  WritePack("{\"pack\": not json");
  PassRecheckInterval();
  EXPECT_EQ(good.pack, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
  PassRecheckInterval();
  EXPECT_EQ(good.pack, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(loaded_warnings + 1, handler_.MessagesOfType(kWarning));
  // Fixed: the new pack is loaded.
  WritePack(PackJson("0.3.0", "C"));
  PassRecheckInterval();
  EXPECT_EQ("0.3.0", Get(&event).pack->info.version);
  EXPECT_EQ(LoadEvent::kLoaded, event);
}

TEST_F(RulePackStoreTest, ABadFirstLoadMeansNoPack) {
  WritePack(PackJson("0.1.0", "A") + "trailing");
  LoadEvent event;
  EXPECT_EQ(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kFailed, event);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  PassRecheckInterval();
  EXPECT_EQ(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, AMissingFileIsReportedOnce) {
  LoadEvent event;
  EXPECT_EQ(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kFailed, event);
  PassRecheckInterval();
  EXPECT_EQ(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  // It appears.
  WritePack(PackJson("0.1.0", "A"));
  PassRecheckInterval();
  EXPECT_NE(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kLoaded, event);
}

TEST_F(RulePackStoreTest, ATooNewEngineMinIsALoadError) {
  WritePack(absl::StrCat(
      R"({"pack":{"id":"edge-seo","version":"0.1.0","engine_min":"9.0.0"},)",
      R"("sites":[{"host":"t.test"}],"rules":[)",
      R"({"id":"t","kind":"title","value":{"template":"A"}}]})"));
  LoadEvent event;
  EXPECT_EQ(nullptr, Get(&event).pack);
  EXPECT_EQ(LoadEvent::kFailed, event);
}

TEST_F(RulePackStoreTest, TwoPathsToOneFileShareOnePack) {
  WritePack(PackJson("0.1.0", "A"));
  fs::create_directories(dir_ / "sub");
  const std::string other = (dir_ / "sub" / ".." / "pack.json").string();
  LoadEvent event;
  LoadedRulePack a = Get(&event);
  EXPECT_EQ(LoadEvent::kLoaded, event);
  SignatureEvent sig_event;
  LoadedRulePack b = store_.Get(other, PackTrust(), &handler_, &event, &sig_event);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(a.pack, b.pack);
#ifndef _WIN32
  const fs::path link = dir_ / "link.json";
  fs::create_symlink(path_, link);
  LoadedRulePack c = store_.Get(link.string(), PackTrust(), &handler_, &event, &sig_event);
  EXPECT_EQ(LoadEvent::kNone, event);
  EXPECT_EQ(a.pack, c.pack);
#endif
}

// One thread refreshes a path while others ask for it: the others are served
// what the entry already has, without waiting for the file work.
TEST_F(RulePackStoreTest, ReadersAreNotHeldUpByARefresh) {
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  std::atomic<int> hooks{0};
  store_.set_refresh_hook_for_testing([&] {
    if (hooks.fetch_add(1) == 0) {
      entered.set_value();
      released.wait();
    }
  });
  auto readers_see = [this](const LoadedRulePack& want) {
    std::atomic<int> mismatches{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
      readers.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
          LoadEvent event;
          SignatureEvent sig_event;
          LoadedRulePack got = store_.Get(path_, PackTrust(), nullptr, &event, &sig_event);
          if (got.pack != want.pack || got.sha256 != want.sha256 ||
              event != LoadEvent::kNone) {
            ++mismatches;
          }
        }
      });
    }
    // These joins return while the refresh is still held open.
    for (std::thread& r : readers) r.join();
    return mismatches.load();
  };

  // Cold start: until the first load completes there is no pack to serve.
  WritePack(PackJson("0.1.0", "A"));
  LoadedRulePack first;
  LoadEvent first_event = LoadEvent::kNone;
  std::thread loader([&] { first = Get(&first_event); });
  entered.get_future().wait();
  EXPECT_EQ(0, readers_see(LoadedRulePack()));
  release.set_value();
  loader.join();
  ASSERT_NE(nullptr, first.pack);
  EXPECT_EQ(LoadEvent::kLoaded, first_event);

  // A reload: the last good pack is served meanwhile.
  std::promise<void> entered2;
  std::promise<void> release2;
  std::shared_future<void> released2 = release2.get_future().share();
  hooks = 0;
  store_.set_refresh_hook_for_testing([&] {
    if (hooks.fetch_add(1) == 0) {
      entered2.set_value();
      released2.wait();
    }
  });
  WritePack(PackJson("0.2.0", "Longer title"));
  PassRecheckInterval();
  LoadedRulePack second;
  LoadEvent second_event = LoadEvent::kNone;
  std::thread reloader([&] { second = Get(&second_event); });
  entered2.get_future().wait();
  EXPECT_EQ(0, readers_see(first));
  release2.set_value();
  reloader.join();
  EXPECT_EQ(LoadEvent::kLoaded, second_event);
  ASSERT_NE(nullptr, second.pack);
  EXPECT_EQ("0.2.0", second.pack->info.version);
  EXPECT_EQ(second.pack, Get().pack);
  // Each version announced once.
  EXPECT_EQ(2, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, ASignedPackLogsTheSignedLine) {
  TestKey key;
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(key);
  SignatureEvent sig = SignatureEvent::kNone;
  LoadedRulePack loaded = GetWith(Trust(false, key), &sig);
  ASSERT_NE(nullptr, loaded.pack);
  EXPECT_EQ("example-publisher", loaded.key_id);
  EXPECT_EQ(SignatureEvent::kVerified, sig);
  EXPECT_EQ(0, handler_.MessagesOfType(kWarning));
  EXPECT_EQ(1, handler_.MessagesOfType(kInfo));
  EXPECT_NE(std::string::npos,
            Log().find(absl::StrCat(
                "rule pack loaded signed: edge-seo@0.1.0 key=example-publisher "
                "sha256=",
                loaded.sha256, " path=", fs::canonical(path_).string())))
      << Log();
  // Once per verdict.
  PassRecheckInterval();
  GetWith(Trust(false, key), &sig);
  EXPECT_EQ(SignatureEvent::kNone, sig);
  EXPECT_EQ(1, handler_.MessagesOfType(kInfo));
}

TEST_F(RulePackStoreTest, EachRefusalReasonRefusesWhenRequired) {
  TestKey key, other;
  struct Case {
    const char* phrase;
    SignatureEvent event;
  };
  const Case kCases[] = {
      {"unsigned", SignatureEvent::kRefusedUnsigned},
      {"signature unreadable", SignatureEvent::kRefusedSignatureUnreadable},
      {"publisher mismatch", SignatureEvent::kRefusedPublisherMismatch},
      {"unknown key", SignatureEvent::kRefusedUnknownKey},
      {"bad signature", SignatureEvent::kRefusedBadSignature},
  };
  for (int i = 0; i < 5; ++i) {
    SCOPED_TRACE(kCases[i].phrase);
    MockMessageHandler handler(new NullMutex);
    RulePackStore store(&timer_, "1.17.0");
    path_ = (dir_ / absl::StrCat("pack", i, ".json")).string();
    WritePack(SignedPackJson("0.1.0", "A"));
    switch (i) {
      case 0:
        break;
      case 1:
        WriteSidecarText("not a signature\n");
        break;
      case 2:
        WriteSidecar(key, "another-publisher");
        break;
      case 3:
        WriteSidecar(other);
        break;
      case 4:
        WriteSidecar(key);
        WritePack(SignedPackJson("0.1.0", "B"));  // the bytes changed
        break;
    }
    SignatureEvent sig = SignatureEvent::kNone;
    LoadEvent load = LoadEvent::kNone;
    // For case 3 the sidecar's key id is not listed.
    const PackTrust trust =
        i == 3 ? Trust(true, key, "another-key") : Trust(true, key);
    LoadedRulePack got = store.Get(path_, trust, &handler, &load, &sig);
    EXPECT_EQ(nullptr, got.pack);
    EXPECT_EQ(LoadEvent::kLoaded, load);
    EXPECT_EQ(kCases[i].event, sig);
    EXPECT_EQ(1, handler.MessagesOfType(kWarning)) << LogOf(&handler);
    EXPECT_NE(std::string::npos,
              LogOf(&handler).find(absl::StrCat("rule pack refused (", kCases[i].phrase,
                                      "): edge-seo@0.1.0 sha256=")))
        << LogOf(&handler);
    EXPECT_NE(std::string::npos,
              LogOf(&handler).find("; the rule_packs filter is off for it"))
        << LogOf(&handler);
    // Once per verdict.
    timer_.AdvanceMs(RulePackStore::kPackRecheckIntervalSec * 1000);
    store.Get(path_, trust, &handler, &load, &sig);
    EXPECT_EQ(SignatureEvent::kNone, sig);
    EXPECT_EQ(1, handler.MessagesOfType(kWarning));
  }
}

TEST_F(RulePackStoreTest, ARefusedUpdateKeepsTheEarlierSignedPack) {
  TestKey key;
  const PackTrust trust = Trust(true, key);
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(key);
  SignatureEvent sig;
  LoadedRulePack v1 = GetWith(trust, &sig);
  ASSERT_NE(nullptr, v1.pack);
  EXPECT_EQ(SignatureEvent::kVerified, sig);

  PassRecheckInterval();
  fs::remove(path_ + ".sig");
  WritePack(SignedPackJson("0.2.0", "B"));  // unsigned
  LoadEvent load;
  LoadedRulePack got = GetWith(trust, &sig, &load);
  EXPECT_EQ(LoadEvent::kLoaded, load);
  EXPECT_EQ(v1.pack, got.pack);
  EXPECT_EQ("0.1.0", got.pack->info.version);
  EXPECT_EQ(SignatureEvent::kRefusedUnsigned, sig);
  EXPECT_NE(std::string::npos,
            Log().find("; keeping the pack loaded earlier"))
      << Log();
  // Later Gets add nothing.
  const int warnings = handler_.MessagesOfType(kWarning);
  PassRecheckInterval();
  got = GetWith(trust, &sig);
  EXPECT_EQ(v1.pack, got.pack);
  EXPECT_EQ(SignatureEvent::kNone, sig);
  EXPECT_EQ(warnings, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, AFailingSidecarWarnsButLoadsWhenNotRequired) {
  TestKey key, other;
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(other);  // signed by a key that is not the trusted one
  SignatureEvent sig = SignatureEvent::kNone;
  LoadedRulePack got = GetWith(Trust(false, key), &sig);
  ASSERT_NE(nullptr, got.pack);
  EXPECT_EQ("", got.key_id);
  EXPECT_EQ(SignatureEvent::kNone, sig);
  EXPECT_EQ(2, handler_.MessagesOfType(kWarning)) << Log();
  EXPECT_NE(std::string::npos,
            Log().find(absl::StrCat("rule pack loaded unsigned: edge-seo@0.1.0 "
                                    "sha256=",
                                    got.sha256)))
      << Log();
  EXPECT_NE(std::string::npos,
            Log().find(absl::StrCat(
                "rule pack signature not verified (bad signature): "
                "edge-seo@0.1.0 sha256=",
                got.sha256)))
      << Log();
  PassRecheckInterval();
  GetWith(Trust(false, key), &sig);
  EXPECT_EQ(2, handler_.MessagesOfType(kWarning));
}

TEST_F(RulePackStoreTest, ASidecarAddedLaterIsPickedUpAtTheRecheck) {
  TestKey key;
  const PackTrust trust = Trust(false, key);
  WritePack(SignedPackJson("0.1.0", "A"));
  SignatureEvent sig;
  EXPECT_EQ("", GetWith(trust, &sig).key_id);
  EXPECT_EQ(SignatureEvent::kNone, sig);
  WriteSidecar(key);
  PassRecheckInterval();
  LoadEvent load;
  LoadedRulePack got = GetWith(trust, &sig, &load);
  EXPECT_EQ(LoadEvent::kNone, load);  // the pack was not parsed again
  EXPECT_EQ(SignatureEvent::kVerified, sig);
  EXPECT_EQ("example-publisher", got.key_id);
  EXPECT_NE(std::string::npos, Log().find("rule pack loaded signed:"))
      << Log();
}

TEST_F(RulePackStoreTest, TwoTrustConfigurationsShareOneParse) {
  TestKey key, other;
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(key);
  SignatureEvent sig_a, sig_b;
  LoadEvent load_a, load_b;
  LoadedRulePack a = GetWith(Trust(true, key), &sig_a, &load_a);
  LoadedRulePack b = GetWith(Trust(true, other), &sig_b, &load_b);
  EXPECT_NE(nullptr, a.pack);
  EXPECT_EQ(SignatureEvent::kVerified, sig_a);
  EXPECT_EQ(LoadEvent::kLoaded, load_a);
  // The same key id with another key: the signature does not verify.
  EXPECT_EQ(nullptr, b.pack);
  EXPECT_EQ(SignatureEvent::kRefusedBadSignature, sig_b);
  EXPECT_EQ(LoadEvent::kNone, load_b);
  // A configuration that lists no key at all: unknown key.
  PackTrust none;
  none.require_signed = true;
  SignatureEvent sig_c;
  EXPECT_EQ(nullptr, GetWith(none, &sig_c).pack);
  EXPECT_EQ(SignatureEvent::kRefusedUnknownKey, sig_c);
}

TEST_F(RulePackStoreTest, VerificationRunsOncePerVerdict) {
  TestKey key;
  const PackTrust trust = Trust(true, key);
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(key);
  SignatureEvent sig;
  GetWith(trust, &sig);
  EXPECT_EQ(SignatureEvent::kVerified, sig);
  for (int i = 0; i < 5; ++i) {
    sig = SignatureEvent::kRefusedUnsigned;
    GetWith(trust, &sig);
    EXPECT_EQ(SignatureEvent::kNone, sig);
  }
  EXPECT_EQ(1, handler_.MessagesOfType(kInfo));
}

// A new version that does not parse comes with its own sidecar; the pack that
// stays loaded is still judged by the sidecar it was loaded with.
TEST_F(RulePackStoreTest, AFailedUpdateKeepsTheSidecarOfTheLoadedPack) {
  TestKey key;
  const PackTrust trust = Trust(true, key);
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecar(key);
  SignatureEvent sig;
  LoadedRulePack v1 = GetWith(trust, &sig);
  ASSERT_NE(nullptr, v1.pack);
  EXPECT_EQ(SignatureEvent::kVerified, sig);
  const int warnings = handler_.MessagesOfType(kWarning);

  PassRecheckInterval();
  WritePack("{ this is not a pack");
  WriteSidecar(key);  // a signature of the broken file
  LoadEvent load;
  LoadedRulePack got = GetWith(trust, &sig, &load);
  EXPECT_EQ(LoadEvent::kFailed, load);
  EXPECT_EQ(v1.pack, got.pack);
  EXPECT_EQ(SignatureEvent::kNone, sig);
  // Only the load failure is reported: no refusal line for the old pack.
  EXPECT_EQ(warnings + 1, handler_.MessagesOfType(kWarning)) << Log();
  EXPECT_EQ(std::string::npos, Log().find("rule pack refused"));

  // A trust configuration this process has not seen yet still gets v1.
  TestKey other;
  PackTrust fresh = Trust(true, key, "example-publisher");
  fresh.keys.push_back(TrustKey{"zz-extra", other.public_key});
  PassRecheckInterval();
  got = GetWith(fresh, &sig);
  EXPECT_EQ(v1.pack, got.pack);
  EXPECT_EQ(SignatureEvent::kVerified, sig);
}

#ifndef _WIN32
TEST_F(RulePackStoreTest, ASidecarThatIsNotAFileIsUnreadable) {
  TestKey key;
  WritePack(SignedPackJson("0.1.0", "A"));
  fs::create_directory(path_ + ".sig");
  SignatureEvent sig;
  EXPECT_EQ(nullptr, GetWith(Trust(true, key), &sig).pack);
  EXPECT_EQ(SignatureEvent::kRefusedSignatureUnreadable, sig);
  fs::remove(path_ + ".sig");
  fs::create_symlink(dir_ / "nowhere", path_ + ".sig");  // dangling
  PassRecheckInterval();
  EXPECT_EQ(nullptr, GetWith(Trust(true, key), &sig).pack);
  EXPECT_EQ(SignatureEvent::kNone, sig);  // the same verdict, reported once
}
#endif

TEST_F(RulePackStoreTest, AnOversizedSidecarIsUnreadable) {
  TestKey key;
  WritePack(SignedPackJson("0.1.0", "A"));
  WriteSidecarText(std::string(2048, 'a'));
  SignatureEvent sig;
  EXPECT_EQ(nullptr, GetWith(Trust(true, key), &sig).pack);
  EXPECT_EQ(SignatureEvent::kRefusedSignatureUnreadable, sig);
}


class RulePackStoreLogTest : public RulePackStoreTest {
 protected:
  pagespeed::packs::PackDecision MakeDecision() const {
    pagespeed::packs::PackDecision d;
    d.rule_id = "t";
    d.kind = pagespeed::packs::FindKind("title");
    d.mode = pagespeed::packs::Mode::kEnforce;
    d.action = pagespeed::packs::Action::kInsert;
    d.reason = pagespeed::packs::Reason::kAbsent;
    d.defect = "title-missing";
    d.after = "A";
    return d;
  }
  pagespeed::packs::PageUrl Url() const {
    pagespeed::packs::PageUrl url;
    url.scheme = "https";
    url.host = "t.test";
    url.path = "/a";
    url.query = "x=1";
    return url;
  }
};

TEST_F(RulePackStoreLogTest, TheLineNamesTheTimeAndThePack) {
  WritePack(PackJson("0.1.0", "A"));
  const LoadedRulePack pack = Get();
  EXPECT_EQ(
      absl::StrCat(
          R"({"ts":1791720000000,"pack":"edge-seo@0.1.0","sha256":")",
          pack.sha256.substr(0, 16),
          R"(","host":"t.test","path":"/a?x=1","rule":"t","kind":"title",)",
          R"("mode":"enforce","action":"insert","reason":"absent",)",
          R"("defect":"title-missing","before":"","after":"A"})"),
      RulePackStore::DecisionLine(pack, Url(), MakeDecision(), kNoon));
}

TEST_F(RulePackStoreLogTest, WritesUnderLogDirOncePerKey) {
  WritePack(PackJson("0.1.0", "A"));
  const LoadedRulePack pack = Get();
  options_.set_log_dir((dir_ / "logs").string());
  EXPECT_EQ(LogResult::kWritten,
            store_.LogDecision(options_, pack, Url(), MakeDecision(), kNoon,
                               &handler_));
  EXPECT_EQ(LogResult::kSuppressed,
            store_.LogDecision(options_, pack, Url(), MakeDecision(), kNoon + 1,
                               &handler_));
  // The second line of the same rule on the same page (another action) is a
  // different key.
  pagespeed::packs::PackDecision late = MakeDecision();
  late.action = pagespeed::packs::Action::kRemove;
  late.reason = pagespeed::packs::Reason::kLateConflict;
  EXPECT_EQ(LogResult::kWritten,
            store_.LogDecision(options_, pack, Url(), late, kNoon, &handler_));
  const fs::path file =
      dir_ / "logs" / "packs" / "edge-seo" / "decisions-2026-10-11.jsonl";
  const std::string written = ReadAll(file);
  EXPECT_EQ(
      absl::StrCat(
          RulePackStore::DecisionLine(pack, Url(), MakeDecision(), kNoon), "\n",
          RulePackStore::DecisionLine(pack, Url(), late, kNoon), "\n"),
      written);
  // A new version of the pack is a new key.
  WritePack(PackJson("0.2.0", "Longer title"));
  PassRecheckInterval();
  const LoadedRulePack newer = Get();
  EXPECT_EQ(LogResult::kWritten,
            store_.LogDecision(options_, newer, Url(), MakeDecision(), kNoon,
                               &handler_));
}

TEST_F(RulePackStoreLogTest, NoLogDirDisablesTheLogWithOneWarning) {
  WritePack(PackJson("0.1.0", "A"));
  const LoadedRulePack pack = Get();
  const int before = handler_.MessagesOfType(kWarning);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(LogResult::kDisabled,
              store_.LogDecision(options_, pack, Url(), MakeDecision(), kNoon,
                                 &handler_));
  }
  EXPECT_EQ(before + 1, handler_.MessagesOfType(kWarning));
}

}  // namespace
}  // namespace net_instaweb
