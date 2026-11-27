// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// The known-sites file: every site id (wire v6) this worker has stored work
// for, so that a per-URL purge without a site can reach every site's entry
// after a restart, and a purge naming a site can tell a site it has served
// from one it never has.
//
// Format, one entry per line:
//   # pagespeed-sites v1
//   <64 lowercase hex>
//   ...
// Sorted. A line that is not a site id is skipped and counted.

#ifndef PAGESPEED_SRC_WORKER_KNOWN_SITES_H_
#define PAGESPEED_SRC_WORKER_KNOWN_SITES_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pagespeed {

// At most this many sites are remembered and persisted. Past it a site's
// notifications are still processed, but the site is not remembered.
inline constexpr size_t kMaxKnownSites = 65536;

// The first line of the file.
inline constexpr char kKnownSitesHeader[] = "# pagespeed-sites v1";

// `pagespeed-sites.conf` in the parent directory of `cache_path`, beside
// the host aliases file. Empty when `cache_path` is empty.
std::string KnownSitesFilePath(const std::string& cache_path);

// Writes `sites` (sorted here) atomically: temporary file, fsync, rename,
// mode 0644. Returns false on any failure, leaving the previous file intact.
bool WriteKnownSitesFile(const std::string& path,
                         const std::vector<std::string>& sites);

// Reads the file. `*readable` is false when the file exists but cannot be
// read, is too large, or does not start with the v1 header line; the result
// is then empty. A missing file is readable and empty. `*bad_lines`, when
// given, counts the lines that were skipped because they are not site ids.
std::vector<std::string> ReadKnownSitesFile(const std::string& path,
                                            bool* readable,
                                            size_t* bad_lines = nullptr);

// Writes the known-sites file off the notification receive thread, at most
// once per `interval`. `take` returns the current sites when they changed
// since it last returned them, or nothing.
class KnownSitesWriter {
 public:
  using TakeIfDirty = std::function<std::optional<std::vector<std::string>>()>;

  KnownSitesWriter(
      std::string path, TakeIfDirty take,
      std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
  ~KnownSitesWriter();

  KnownSitesWriter(const KnownSitesWriter&) = delete;
  KnownSitesWriter& operator=(const KnownSitesWriter&) = delete;

  // Starts the writer thread. Stop() writes any pending change, then joins.
  void Start();
  void Stop();

  uint64_t writes() const { return writes_.load(std::memory_order_relaxed); }
  uint64_t write_failures() const {
    return write_failures_.load(std::memory_order_relaxed);
  }

 private:
  void Run();
  void FlushOnce();

  const std::string path_;
  const TakeIfDirty take_;
  const std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::thread thread_;
  // A snapshot whose write failed; touched only by the writer thread, and by
  // Stop() after the join.
  std::optional<std::vector<std::string>> pending_;
  std::atomic<uint64_t> writes_{0};
  std::atomic<uint64_t> write_failures_{0};
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_KNOWN_SITES_H_
