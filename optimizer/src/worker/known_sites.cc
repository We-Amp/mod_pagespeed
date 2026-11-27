// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "src/worker/known_sites.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "lib/base/atomic_file_writer.h"
#include "src/proto/worker_ipc.h"

namespace pagespeed {

namespace {

// Header line plus kMaxKnownSites lines of 64 hex and a newline, with room.
constexpr size_t kMaxKnownSitesFileSize =
    (kMaxKnownSites + 1) * (kSiteIdChars + 2);

}  // namespace

std::string KnownSitesFilePath(const std::string& cache_path) {
  if (cache_path.empty()) return {};
  std::filesystem::path p(cache_path);
  return (p.parent_path() / "pagespeed-sites.conf").string();
}

bool WriteKnownSitesFile(const std::string& path,
                         const std::vector<std::string>& sites) {
  if (path.empty()) return false;
  std::vector<std::string> sorted = sites;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::string content;
  content.reserve(sizeof(kKnownSitesHeader) +
                  sorted.size() * (kSiteIdChars + 1));
  content += kKnownSitesHeader;
  content += '\n';
  for (const std::string& site : sorted) {
    if (!IsSiteId(site)) return false;  // never write what cannot be read
    content += site;
    content += '\n';
  }
  return AtomicWriteFile(path, content, 0644);
}

std::vector<std::string> ReadKnownSitesFile(const std::string& path,
                                            bool* readable, size_t* bad_lines) {
  *readable = true;
  if (bad_lines != nullptr) *bad_lines = 0;
  std::vector<std::string> sites;
  if (path.empty()) return sites;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // A missing file is a worker that has not recorded a site yet.
    if (ec) *readable = false;
    return sites;
  }
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    *readable = false;
    return sites;
  }
  const auto size = f.tellg();
  if (size < 0 || static_cast<size_t>(size) > kMaxKnownSitesFileSize) {
    *readable = false;
    return sites;
  }
  f.seekg(0, std::ios::beg);
  std::string content(static_cast<size_t>(size), '\0');
  if (size > 0 && !f.read(content.data(), size)) {
    *readable = false;
    return sites;
  }

  std::string_view rest(content);
  bool first = true;
  while (!rest.empty()) {
    const size_t eol = rest.find('\n');
    std::string_view line = rest.substr(0, eol);
    rest = (eol == std::string_view::npos) ? std::string_view()
                                           : rest.substr(eol + 1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (first) {
      first = false;
      if (line != kKnownSitesHeader) {
        // Another version, or not this file at all.
        *readable = false;
        return {};
      }
      continue;
    }
    if (line.empty()) continue;
    if (!IsSiteId(line) || sites.size() >= kMaxKnownSites) {
      if (bad_lines != nullptr) ++*bad_lines;
      continue;
    }
    sites.emplace_back(line);
  }
  if (first) {
    // An empty file has no header line.
    *readable = false;
    return {};
  }
  std::sort(sites.begin(), sites.end());
  sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
  return sites;
}

KnownSitesWriter::KnownSitesWriter(std::string path, TakeIfDirty take,
                                   std::chrono::milliseconds interval)
    : path_(std::move(path)), take_(std::move(take)), interval_(interval) {}

KnownSitesWriter::~KnownSitesWriter() { Stop(); }

void KnownSitesWriter::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (thread_.joinable() || path_.empty()) return;
  stop_ = false;
  thread_ = std::thread([this] { Run(); });
}

void KnownSitesWriter::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable()) return;
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
  FlushOnce();  // a change made just before the stop still lands
}

void KnownSitesWriter::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    cv_.wait_for(lock, interval_, [this] { return stop_; });
    if (stop_) break;
    lock.unlock();
    FlushOnce();
    lock.lock();
  }
}

void KnownSitesWriter::FlushOnce() {
  std::optional<std::vector<std::string>> sites = take_();
  if (sites.has_value()) pending_ = std::move(sites);
  if (!pending_.has_value()) return;
  if (WriteKnownSitesFile(path_, *pending_)) {
    pending_.reset();
    writes_.fetch_add(1, std::memory_order_relaxed);
  } else {
    // Kept, and tried again on the next interval.
    write_failures_.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace pagespeed
