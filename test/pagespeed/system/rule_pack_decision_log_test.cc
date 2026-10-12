// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_decision_log.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/null_mutex.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/mock_message_handler.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace net_instaweb {
namespace {

constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;
// 2026-10-11 12:00:00 UTC.
constexpr int64_t kNoon = 1791720000000LL;

std::vector<std::string> ReadLines(const std::filesystem::path& file) {
  std::vector<std::string> lines;
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

std::string ReadAll(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class RulePackDecisionLogTest : public testing::Test {
 protected:
  RulePackDecisionLogTest() : handler_(new NullMutex) {}

  void SetUp() override {
    const testing::TestInfo* info =
        testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::path(GTestTempDir()) / "rule_pack_decision_log" /
           info->name() / "packs" / "edge-seo";
    std::filesystem::remove_all(dir_);
  }

  std::filesystem::path Day(int64_t ms) const {
    return dir_ /
           RulePackDecisionLog::FileName(RulePackDecisionLog::UtcDate(ms));
  }

  std::filesystem::path dir_;
  MockMessageHandler handler_;
};

TEST_F(RulePackDecisionLogTest, UtcDate) {
  EXPECT_EQ("1970-01-01", RulePackDecisionLog::UtcDate(0));
  EXPECT_EQ("1969-12-31", RulePackDecisionLog::UtcDate(-1));
  EXPECT_EQ("2000-02-29", RulePackDecisionLog::UtcDate(951782400000LL));
  EXPECT_EQ("2026-10-11", RulePackDecisionLog::UtcDate(kNoon));
  EXPECT_EQ("2026-10-11",
            RulePackDecisionLog::UtcDate(kNoon + 12 * 3600 * 1000 - 1));
  EXPECT_EQ("2026-10-12",
            RulePackDecisionLog::UtcDate(kNoon + 12 * 3600 * 1000));
  EXPECT_EQ("decisions-2026-10-11.jsonl",
            RulePackDecisionLog::FileName("2026-10-11"));
}

TEST_F(RulePackDecisionLogTest, WritesOneLinePerKey) {
  RulePackDecisionLog log(dir_.string());
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("k1", "{\"a\":1}", kNoon, &handler_));
  EXPECT_EQ(RulePackDecisionLog::Result::kDuplicate,
            log.Write("k1", "{\"a\":1}", kNoon + 1, &handler_));
  // A second decision of the same rule on the same page (the late line) is
  // a different key.
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("k2", "{\"a\":2}", kNoon + 2, &handler_));
  EXPECT_EQ(ReadAll(Day(kNoon)), "{\"a\":1}\n{\"a\":2}\n");
  EXPECT_EQ(0, handler_.TotalMessages());
#ifndef _WIN32
  struct stat st;
  ASSERT_EQ(0, stat(Day(kNoon).c_str(), &st));
  // 0640, less whatever the umask takes away.
  EXPECT_EQ(0u, st.st_mode & 0137);
#endif
}

TEST_F(RulePackDecisionLogTest, AFullKeySetIsClearedNotDropped) {
  RulePackDecisionLog log(dir_.string());
  for (size_t i = 0; i < RulePackDecisionLog::kMaxDedupKeys; ++i) {
    ASSERT_EQ(RulePackDecisionLog::Result::kWritten,
              log.Write(absl::StrCat("k", i), "x", kNoon, &handler_));
  }
  EXPECT_EQ(RulePackDecisionLog::Result::kDuplicate,
            log.Write("k0", "x", kNoon, &handler_));
  // One more key: the set is full, so it is cleared before the key goes in.
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("new", "x", kNoon, &handler_));
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("k0", "x", kNoon, &handler_));
  EXPECT_EQ(RulePackDecisionLog::kMaxDedupKeys + 2,
            ReadLines(Day(kNoon)).size());
}

TEST_F(RulePackDecisionLogTest, StopsAtTheDailyCap) {
  RulePackDecisionLog log(dir_.string());
  log.set_max_bytes_per_day_for_testing(1000);
  const std::string line(9, 'x');  // 10 bytes with the newline
  int written = 0, dropped = 0;
  for (int i = 0; i < 600; ++i) {
    switch (log.Write(absl::StrCat("k", i), line, kNoon, &handler_)) {
      case RulePackDecisionLog::Result::kWritten:
        ++written;
        break;
      case RulePackDecisionLog::Result::kDropped:
        ++dropped;
        break;
      case RulePackDecisionLog::Result::kDuplicate:
        break;
    }
  }
  // The size is checked every kStatEveryLines lines, so the cap is
  // approximate by up to that many lines.
  EXPECT_EQ(RulePackDecisionLog::kStatEveryLines - 1, written);
  EXPECT_EQ(600 - written, dropped);

  // Another process opening the same day's file sees the cap at once.
  RulePackDecisionLog other(dir_.string());
  other.set_max_bytes_per_day_for_testing(1000);
  EXPECT_EQ(RulePackDecisionLog::Result::kDropped,
            other.Write("fresh", line, kNoon, &handler_));
  // The next day starts empty.
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            other.Write("fresh", line, kNoon + kDayMs, &handler_));
}

TEST_F(RulePackDecisionLogTest, ANewDayOpensANewFile) {
  RulePackDecisionLog log(dir_.string());
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("a", "day1", kNoon, &handler_));
  EXPECT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("b", "day2", kNoon + kDayMs, &handler_));
  // The key set lives as long as the process, not the day.
  EXPECT_EQ(RulePackDecisionLog::Result::kDuplicate,
            log.Write("a", "day1", kNoon + kDayMs, &handler_));
  EXPECT_EQ(ReadAll(Day(kNoon)), "day1\n");
  EXPECT_EQ(ReadAll(Day(kNoon + kDayMs)), "day2\n");
}

TEST_F(RulePackDecisionLogTest, TheCreatorOfADayKeepsFourteenFiles) {
  std::filesystem::create_directories(dir_);
  for (int d = 1; d <= 20; ++d) {
    std::ofstream(Day(kNoon - d * kDayMs)) << "old\n";
  }
  std::ofstream(dir_ / "notes.txt") << "keep\n";
  std::ofstream(dir_ / "decisions-latest.jsonl") << "keep\n";

  RulePackDecisionLog log(dir_.string());
  ASSERT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("k", "today", kNoon, &handler_));
  std::set<std::string> names;
  for (const auto& e : std::filesystem::directory_iterator(dir_)) {
    names.insert(e.path().filename().string());
  }
  std::set<std::string> want = {"notes.txt", "decisions-latest.jsonl"};
  for (int d = 0; d < RulePackDecisionLog::kRetentionDays; ++d) {
    want.insert(Day(kNoon - d * kDayMs).filename().string());
  }
  EXPECT_EQ(want, names);
}

TEST_F(RulePackDecisionLogTest, OnlyTheCreatorOfADayRemovesOldFiles) {
  std::filesystem::create_directories(dir_);
  const std::filesystem::path old = Day(kNoon - 30 * kDayMs);
  std::ofstream(old) << "old\n";
  std::ofstream(Day(kNoon)) << "";  // another process created today's file
  RulePackDecisionLog log(dir_.string());
  ASSERT_EQ(RulePackDecisionLog::Result::kWritten,
            log.Write("k", "today", kNoon, &handler_));
  EXPECT_TRUE(std::filesystem::exists(old));
}

TEST_F(RulePackDecisionLogTest, AnUnwritableDirectoryDropsAndWarnsOncePerDay) {
  // A file where the directory should be.
  std::filesystem::create_directories(dir_.parent_path());
  std::ofstream(dir_) << "not a directory\n";
  RulePackDecisionLog log(dir_.string());
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(RulePackDecisionLog::Result::kDropped,
              log.Write(absl::StrCat("k", i), "x", kNoon, &handler_));
  }
  EXPECT_EQ(1, handler_.MessagesOfType(kWarning));
  EXPECT_EQ(RulePackDecisionLog::Result::kDropped,
            log.Write("k", "x", kNoon + kDayMs, &handler_));
  EXPECT_EQ(2, handler_.MessagesOfType(kWarning));
}

// Many writers append to one file at once; every line comes out whole.
// `id` names the writer; lines carry a writer id, a sequence number and
// padding of a varying length (up to about 900 bytes).
std::string MakeLine(int id, int seq) {
  return absl::StrCat("{\"writer\":", id, ",\"seq\":", seq, ",\"pad\":\"",
                      std::string(50 + (seq * 37 + id * 101) % 850, 'a' + id),
                      "\"}");
}

void CheckLines(const std::filesystem::path& file, int writers, int lines) {
  const std::vector<std::string> got = ReadLines(file);
  ASSERT_EQ(static_cast<size_t>(writers) * lines, got.size());
  std::map<int, std::set<int>> seen;
  Json::CharReaderBuilder builder;
  builder["failIfExtra"] = true;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  for (const std::string& line : got) {
    Json::Value v;
    std::string errors;
    ASSERT_TRUE(
        reader->parse(line.data(), line.data() + line.size(), &v, &errors))
        << errors << ": " << line.substr(0, 120);
    const int id = v["writer"].asInt();
    const int seq = v["seq"].asInt();
    ASSERT_EQ(MakeLine(id, seq), line);
    EXPECT_TRUE(seen[id].insert(seq).second) << id << "/" << seq;
  }
  ASSERT_EQ(static_cast<size_t>(writers), seen.size());
  for (const auto& [id, seqs] : seen) {
    EXPECT_EQ(static_cast<size_t>(lines), seqs.size()) << id;
  }
}

constexpr int kWriters = 4;
constexpr int kLinesPerWriter = 10000;

#ifndef _WIN32
TEST_F(RulePackDecisionLogTest, ProcessesAppendWholeLines) {
  std::vector<pid_t> children;
  for (int id = 0; id < kWriters; ++id) {
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      // The child: its own log, as a separate server process would have.
      int failures = 0;
      {
        RulePackDecisionLog log(dir_.string());
        for (int seq = 0; seq < kLinesPerWriter; ++seq) {
          if (log.Write(absl::StrCat(id, "/", seq), MakeLine(id, seq), kNoon,
                        nullptr) != RulePackDecisionLog::Result::kWritten) {
            ++failures;
          }
        }
      }
      _exit(failures == 0 ? 0 : 1);
    }
    children.push_back(pid);
  }
  // Wait for exactly the children started above.
  for (pid_t pid : children) {
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  }
  CheckLines(Day(kNoon), kWriters, kLinesPerWriter);
}
#endif

// The same with threads, each holding its own descriptor (or handle), which
// is what the processes above do; this is the variant that runs on Windows.
TEST_F(RulePackDecisionLogTest, SeparateDescriptorsAppendWholeLines) {
  std::vector<std::thread> threads;
  for (int id = 0; id < kWriters; ++id) {
    threads.emplace_back([this, id] {
      RulePackDecisionLog log(dir_.string());
      for (int seq = 0; seq < kLinesPerWriter; ++seq) {
        log.Write(absl::StrCat(id, "/", seq), MakeLine(id, seq), kNoon,
                  nullptr);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CheckLines(Day(kNoon), kWriters, kLinesPerWriter);
}

}  // namespace
}  // namespace net_instaweb
