// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// ConfigPersister unit tests: ordering, coalescing, failure reporting and the
// shutdown drain, with fake writers (no disk) and a gate that holds the
// persister thread "in flight" deterministically.

#include "src/worker/config_persister.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "uv.h"

namespace pagespeed {
namespace {

// Blocks the persister thread inside before_write until Open().
class Gate {
 public:
  void Enter() {
    std::unique_lock<std::mutex> lock(mu_);
    ++entered_;
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
  }
  void Open() {
    std::lock_guard<std::mutex> lock(mu_);
    open_ = true;
    cv_.notify_all();
  }
  bool WaitEntered(int n) {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, std::chrono::seconds(5),
                        [&] { return entered_ >= n; });
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool open_ = false;
  int entered_ = 0;
};

class RecordingHandler : public MessageHandler {
 public:
  void Message(MessageType /*type*/, const char* /*format*/, ...) override {}
  std::vector<std::string> Warnings() {
    std::lock_guard<std::mutex> lock(mu_);
    return warnings_;
  }

 protected:
  void MessageV(MessageType type, const char* format, va_list args) override {
    if (type != MessageType::kWarning) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), format, args);
    std::lock_guard<std::mutex> lock(mu_);
    warnings_.push_back(buf);
  }

 private:
  std::mutex mu_;
  std::vector<std::string> warnings_;
};

class ConfigPersisterTest : public ::testing::Test {
 protected:
  void SetUp() override { uv_loop_init(&loop_); }

  void TearDown() override {
    if (persister_) persister_->Shutdown();
    persister_.reset();
    // Drains the close callback of the async handle; a leaked or still-open
    // handle would keep the loop alive and the close below would fail.
    uv_run(&loop_, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop_), 0) << "a handle leaked past Shutdown()";
  }

  void Make(std::function<void()> before_write = nullptr,
            MessageHandler* handler = nullptr) {
    persister_ = std::make_unique<ConfigPersister>(&loop_, handler,
                                                   std::move(before_write));
  }

  // A job whose writes append `id` to the shared logs.
  ConfigPersister::Job MakeJob(int id, bool shared_dirty) {
    ConfigPersister::Job job;
    job.write_json = [this, id] {
      std::lock_guard<std::mutex> lock(mu_);
      json_log_.push_back(id);
      return 0;
    };
    job.write_shared = [this, id] {
      std::lock_guard<std::mutex> lock(mu_);
      shared_log_.push_back(id);
      return 0;
    };
    job.shared_dirty = shared_dirty;
    return job;
  }

  ConfigPersister::Done Recorder(int id) {
    return [this, id](const ConfigPersister::Result& r) {
      done_order_.push_back(id);
      results_[id] = r;
    };
  }

  bool PumpUntil(const std::function<bool()>& pred, int timeout_ms = 5000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      uv_run(&loop_, UV_RUN_NOWAIT);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  std::vector<int> JsonLog() {
    std::lock_guard<std::mutex> lock(mu_);
    return json_log_;
  }
  std::vector<int> SharedLog() {
    std::lock_guard<std::mutex> lock(mu_);
    return shared_log_;
  }

  uv_loop_t loop_;
  std::unique_ptr<ConfigPersister> persister_;
  std::mutex mu_;
  std::vector<int> json_log_;
  std::vector<int> shared_log_;
  // Touched on the loop (test) thread only.
  std::vector<int> done_order_;
  std::map<int, ConfigPersister::Result> results_;
};

TEST_F(ConfigPersisterTest, WritesInApplyOrderAndMergesTheBacklog) {
  Gate gate;
  Make([&gate] { gate.Enter(); });

  persister_->Submit(MakeJob(1, false), Recorder(1));
  ASSERT_TRUE(gate.WaitEntered(1));  // Job 1 is in flight.
  persister_->Submit(MakeJob(2, false), Recorder(2));
  persister_->Submit(MakeJob(3, false), Recorder(3));
  gate.Open();

  ASSERT_TRUE(PumpUntil([this] { return done_order_.size() == 3; }));
  // Job 2 was superseded by job 3: two writes, the newest last.
  EXPECT_EQ(JsonLog(), (std::vector<int>{1, 3}));
  // Responses go out in submission order, every one with a success.
  EXPECT_EQ(done_order_, (std::vector<int>{1, 2, 3}));
  for (int id : {1, 2, 3}) {
    EXPECT_TRUE(results_[id].json_ok) << id;
  }
}

TEST_F(ConfigPersisterTest, CoalescingKeepsASupersededSharedChange) {
  Gate gate;
  Make([&gate] { gate.Enter(); });

  persister_->Submit(MakeJob(1, false), Recorder(1));
  ASSERT_TRUE(gate.WaitEntered(1));
  persister_->Submit(MakeJob(2, true), Recorder(2));   // touches shared fields
  persister_->Submit(MakeJob(3, false), Recorder(3));  // supersedes job 2
  gate.Open();

  ASSERT_TRUE(PumpUntil([this] { return done_order_.size() == 3; }));
  // The shared file is written once, from the newest state (job 3's writer).
  EXPECT_EQ(SharedLog(), (std::vector<int>{3}));
  EXPECT_TRUE(results_[1].shared_attempted == false);
  EXPECT_TRUE(results_[3].shared_attempted);
}

TEST_F(ConfigPersisterTest, ReportsWriteFailureWithReason) {
  RecordingHandler handler;
  Make(nullptr, &handler);

  ConfigPersister::Job job;
  job.write_json = [] { return ENOSPC; };
  persister_->Submit(std::move(job), Recorder(1));

  ASSERT_TRUE(PumpUntil([this] { return done_order_.size() == 1; }));
  const std::string cause =
      std::error_code(ENOSPC, std::generic_category()).message();
  EXPECT_FALSE(results_[1].json_ok);
  EXPECT_EQ(results_[1].json_error, cause);
  ASSERT_EQ(handler.Warnings().size(), 1u);
  EXPECT_NE(handler.Warnings()[0].find("pagespeed.json"), std::string::npos);
  EXPECT_NE(handler.Warnings()[0].find(cause), std::string::npos);
}

TEST_F(ConfigPersisterTest, ShutdownDrainsPendingWrite) {
  Gate gate;
  Make([&gate] { gate.Enter(); });

  persister_->Submit(MakeJob(1, false), Recorder(1));
  ASSERT_TRUE(gate.WaitEntered(1));
  persister_->Submit(MakeJob(2, false), Recorder(2));
  gate.Open();
  persister_->Shutdown();

  // Both waiters are completed by the time Shutdown() returns, and the newest
  // state is on "disk".
  EXPECT_EQ(done_order_, (std::vector<int>{1, 2}));
  ASSERT_FALSE(JsonLog().empty());
  EXPECT_EQ(JsonLog().back(), 2);
  EXPECT_TRUE(results_[1].json_ok);
  EXPECT_TRUE(results_[2].json_ok);

  // A submit after shutdown fails fast instead of hanging.
  persister_->Submit(MakeJob(3, false), Recorder(3));
  EXPECT_EQ(done_order_.back(), 3);
  EXPECT_FALSE(results_[3].json_ok);
  EXPECT_EQ(JsonLog().back(), 2);
}

TEST_F(ConfigPersisterTest, ShutdownDoesNotWaitForeverOnHungDisk) {
  RecordingHandler handler;
  auto gate = std::make_shared<Gate>();
  persister_ = std::make_unique<ConfigPersister>(
      &loop_, &handler, [gate] { gate->Enter(); },
      std::chrono::milliseconds(100));

  // The job captures nothing from the fixture: the abandoned thread outlives
  // this test body.
  ConfigPersister::Job job;
  job.write_json = [] { return 0; };
  persister_->Submit(std::move(job), Recorder(1));
  ASSERT_TRUE(gate->WaitEntered(1));

  persister_->Shutdown();  // Must return although the write is stuck.

  EXPECT_EQ(done_order_, (std::vector<int>{1}));
  EXPECT_FALSE(results_[1].json_ok);
  bool warned = false;
  for (const auto& w : handler.Warnings()) {
    if (w.find("shutdown") != std::string::npos) warned = true;
  }
  EXPECT_TRUE(warned);

  gate->Open();  // Let the abandoned thread finish.
}

}  // namespace
}  // namespace pagespeed
