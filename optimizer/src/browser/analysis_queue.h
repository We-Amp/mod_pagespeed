// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Browser Analysis Queue
//
// Bounded priority queue for browser analysis work items. Deduplicates
// by template hash and orders by priority (warmup first, then by
// notification frequency). Head-drop eviction when full.
//
// Thread-safe: accessed from worker threads (enqueue) and the main
// event loop (dequeue when Chrome tab available).

#ifndef PAGESPEED_SRC_BROWSER_ANALYSIS_QUEUE_H_
#define PAGESPEED_SRC_BROWSER_ANALYSIS_QUEUE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

#include "absl/container/flat_hash_set.h"

namespace pagespeed {

class AnalysisQueue {
 public:
  struct Item {
    std::string url;
    std::string hostname;
    std::string scheme = "https";
    uint32_t mask = 0;
    uint64_t template_hash = 0;
    bool is_warmup = false;  // Warmup sentinel -> high priority
    uint32_t frequency = 1;  // Notification count for ordering
    int retry_count = 0;     // Re-analysis attempts (0 = first run)
    // Original (pre-optimization) HTML content. Passed from the worker
    // thread to avoid reading the worker-optimized variant from cache.
    // When non-empty, RunAnalysis uses this instead of cache read.
    std::string original_html;
    // SHA-256 of the raw pre-rewrite origin HTML, captured
    // at enqueue time (== the kContentHash the worker wrote for this URL in
    // P2.2). The agent render's store-time re-validation compares it to the
    // LIVE kContentHash; on mismatch the render discards (closes the v1-render-
    // overwrites-v2 race). has_origin_html_hash=false means no binding was
    // captured (e.g. a warmup enqueue) -> the agent variant is NOT stored
    // (fail-closed; never bind rendered content to the wrong origin hash).
    bool has_origin_html_hash = false;
    std::array<std::byte, 32> origin_html_hash{};
    // A forced per-URL agent render — a warm template
    // (perf profile already exists) whose THIS-url markdown was never built.
    // RunAnalysis does ONLY the agent render (no perf viewports, no profile
    // store).  For such items `template_hash` is set to a URL-derived dedup key
    // (not the structural hash) so distinct URLs sharing a template don't
    // starve each other in the queue's per-key dedup.
    bool force_agent_render = false;
    // The notification's site and sender bits (wire v6), carried with the
    // work. Every cache read and write for this item uses `site`; the agent
    // render, which fetches from `hostname`, runs only when `host_vouched`.
    std::string site;
    bool host_vouched = false;
    bool from_module = false;
  };

  explicit AnalysisQueue(size_t max_size = 1000);

  // Enqueue with priority ordering.
  // Returns false if the item was dropped (duplicate or queue full
  // with all higher-priority items) or already queued.
  bool Enqueue(Item item);

  // Dequeue highest-priority item. Non-blocking, returns nullopt
  // if empty. Called from main event loop.
  std::optional<Item> Dequeue();

  // Signal shutdown. No-op on the queue itself, but callers should
  // stop enqueuing after this.
  void Shutdown();

  bool is_shutdown() const;
  size_t size() const;
  uint64_t drops() const;

 private:
  mutable std::mutex mu_;
  std::deque<Item> queue_;
  absl::flat_hash_set<uint64_t> pending_templates_;
  bool shutdown_ = false;
  size_t max_size_;
  uint64_t drops_ = 0;
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_BROWSER_ANALYSIS_QUEUE_H_
