// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Two processes that write alternates of one key at the same moment must
// not lose one of them.
//
// The shape is the shared cache's ordinary one: a web-server process
// re-records a URL's original while the optimizer writes the optimized copy
// of that same URL, then its gzip and brotli siblings.  Each alternate write
// first resolves the key's chain and publishes its new head later; the test
// makes both writers resolve the SAME chain and then publish one after the
// other, the web server first.
//
// Two Volume objects on one file stand for the two processes: each has its
// own stripe mutex and write cursor, and both share the file's directory and
// its cross-process write lock.  The order is forced, not timed:
//
//   1. the web server's write is parked by the storage layer's write hook,
//      after it resolved the chain, while it holds the write lock;
//   2. the optimizer's write resolves the chain (seen from outside as its
//      chain-depth statistic reaching 1) and waits for the write lock;
//   3. the web server is released and publishes;
//   4. the optimizer's hook call waits until the web server's write has
//      returned, and only then does the optimizer write and publish.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/volume.hpp"
#include "cyclone/alternate.hpp"
#include "cyclone/config.hpp"
#include "cyclone/key.hpp"
#include "gtest/gtest.h"
#include "test/test_util/temp_dir.h"

namespace {

using cyclone::AlternateId;
using cyclone::CacheKey;
using cyclone::Volume;

// One stripe, so every write below meets the same write lock.
constexpr size_t kVolumeBytes = size_t{16} * 1024 * 1024;

// The ids the products use: the stored original, the optimized copy, and
// the optimized copy's gzip and brotli siblings.
constexpr auto kOriginal = static_cast<AlternateId>(0x0C);
constexpr auto kOptimized = static_cast<AlternateId>(0x08);
constexpr auto kOptimizedGzip = static_cast<AlternateId>(0x48);
constexpr auto kOptimizedBrotli = static_cast<AlternateId>(0x88);

// One process's view of the shared volume file.
struct ProcessView {
  std::shared_ptr<Volume> volume;
  std::vector<std::shared_ptr<cyclone::VolumeReadAnchor>> anchors;

  ProcessView() = default;
  ProcessView(const ProcessView&) = delete;
  ProcessView& operator=(const ProcessView&) = delete;

  bool Open(const std::string& path) {
    cyclone::VolumeConfig config;
    config.path = path;
    config.size = kVolumeBytes;
    config.verify_checksum_on_read = true;
    // The setting both products run with: every process writes every stripe.
    cyclone::MultiProcessConfig multi_process;
    multi_process.set_enabled(true).set_process_index(0).set_total_processes(1);
    volume = std::make_shared<Volume>(config, multi_process);
    if (!volume->open().has_value()) {
      volume.reset();
      return false;
    }
    anchors = volume->make_read_anchors();
    volume->set_read_anchors(anchors.data(), anchors.size());
    return true;
  }

  ~ProcessView() {
    if (volume) {
      volume->set_read_anchors(nullptr, 0);
      volume->close();
    }
  }
};

bool WriteAlternate(Volume& volume, const CacheKey& key, AlternateId id,
                    std::string_view text) {
  const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
  auto handle = volume.write_alternate_sync(key, id, bytes.size());
  if (!handle.has_value()) {
    return false;
  }
  if (!handle->write_sync(bytes).has_value()) {
    return false;
  }
  return handle->close_sync().has_value();
}

std::vector<int> ListIds(Volume& volume, const CacheKey& key) {
  std::vector<int> ids;
  auto listed = volume.list_alternates_sync(key);
  if (listed.has_value()) {
    for (const auto& alternate : *listed) {
      ids.push_back(static_cast<int>(static_cast<uint8_t>(alternate.id)));
    }
  }
  return ids;
}

std::string Printed(const std::vector<int>& ids) {
  std::string out;
  for (const int id : ids) {
    if (!out.empty()) {
      out += ", ";
    }
    out += std::to_string(id);
  }
  return out.empty() ? std::string("(none)") : out;
}

bool Contains(const std::vector<int>& ids, int id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Polls `done` for up to a minute; false when it never became true.
bool WaitFor(const std::function<bool()>& done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(AlternatePublishRaceTest,
     AWriteThatResolvedAnOlderChainDoesNotDropAnotherWritersAlternate) {
  const std::string dir = pagespeed::test::MakeTempDir();
  const std::string path = dir + "/volume";
  {
    ProcessView server;
    ProcessView optimizer;
    ASSERT_TRUE(server.Open(path));
    ASSERT_TRUE(optimizer.Open(path));
    ASSERT_EQ(1u, server.volume->stats().stripe_count);

    const CacheKey key("http://example.test/styles/site.css");
    const std::string original_first(4307, 'o');
    const std::string original_second(4307, 'p');
    const std::string optimized(3899, 'm');

    // The first request recorded the original; the optimizer sees it.
    ASSERT_TRUE(WriteAlternate(*server.volume, key, kOriginal, original_first));
    ASSERT_EQ((std::vector<int>{0x0C}), ListIds(*optimizer.volume, key))
        << "the optimizer's view does not see the web server's entry, so the "
           "two views do not share a directory and this test proves nothing";
    ASSERT_EQ(0u, optimizer.volume->stats().alternate_max_chain_depth);

    std::atomic<int> hook_calls{0};
    std::atomic<bool> server_parked{false};
    std::atomic<bool> release_server{false};
    std::atomic<bool> server_returned{false};
    Volume::s_write_tear_gate_for_test = [&](uint64_t, uint64_t) {
      const int call = hook_calls.fetch_add(1);
      if (call == 0) {
        // The web server's re-record: chain resolved, write lock held.
        server_parked.store(true);
        while (!release_server.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      } else if (call == 1) {
        // The optimizer's write: it holds the write lock now, and waits
        // until the web server's write has published and returned.
        while (!server_returned.load()) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    };

    std::atomic<bool> server_ok{false};
    std::atomic<bool> optimizer_ok{false};
    std::thread server_thread([&] {
      server_ok.store(
          WriteAlternate(*server.volume, key, kOriginal, original_second));
      server_returned.store(true);
    });
    const bool parked = WaitFor([&] { return server_parked.load(); });

    std::thread optimizer_thread;
    bool optimizer_resolved = false;
    if (parked) {
      optimizer_thread = std::thread([&] {
        optimizer_ok.store(
            WriteAlternate(*optimizer.volume, key, kOptimized, optimized));
      });
      // The chain-depth statistic is raised right after the chain walk and
      // before the write lock is asked for: once it reads 1 the optimizer
      // has resolved [original] and is waiting behind the web server.
      optimizer_resolved = WaitFor([&] {
        return optimizer.volume->stats().alternate_max_chain_depth >= 1;
      });
    }

    release_server.store(true);
    server_thread.join();
    server_returned.store(true);
    if (optimizer_thread.joinable()) {
      optimizer_thread.join();
    }
    Volume::s_write_tear_gate_for_test = {};

    ASSERT_TRUE(parked) << "the web server's write never reached the hook";
    ASSERT_TRUE(optimizer_resolved)
        << "the optimizer's write never resolved the chain";
    ASSERT_TRUE(server_ok.load()) << "the web server's write failed";
    ASSERT_TRUE(optimizer_ok.load()) << "the optimizer's write failed";

    // The optimizer goes on to write the compressed siblings, as it does
    // for every optimized stylesheet and script.
    ASSERT_TRUE(WriteAlternate(*optimizer.volume, key, kOptimizedGzip,
                               std::string(1200, 'g')));
    ASSERT_TRUE(WriteAlternate(*optimizer.volume, key, kOptimizedBrotli,
                               std::string(1000, 'b')));

    const std::vector<int> through_optimizer = ListIds(*optimizer.volume, key);
    const std::vector<int> through_server = ListIds(*server.volume, key);
    std::cout << "alternates after the race, through the optimizer's view: "
              << Printed(through_optimizer) << "\n"
              << "alternates after the race, through the web server's view: "
              << Printed(through_server) << "\n"
              << std::flush;

    EXPECT_TRUE(Contains(through_optimizer, 0x08))
        << "the optimized copy written during the race is gone; the key "
           "lists: "
        << Printed(through_optimizer);
    EXPECT_TRUE(Contains(through_server, 0x08))
        << "the optimized copy written during the race is gone; the key "
           "lists: "
        << Printed(through_server);
    EXPECT_TRUE(Contains(through_optimizer, 0x0C));
    EXPECT_TRUE(Contains(through_optimizer, 0x48));
    EXPECT_TRUE(Contains(through_optimizer, 0x88));
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
