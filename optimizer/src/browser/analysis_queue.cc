// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Analysis Queue Implementation

#include "src/browser/analysis_queue.h"

#include <algorithm>
#include <mutex>
#include <optional>

namespace pagespeed {

AnalysisQueue::AnalysisQueue(size_t max_size) : max_size_(max_size) {}

bool AnalysisQueue::Enqueue(Item item) {
  std::lock_guard<std::mutex> lock(mu_);

  if (shutdown_) return false;

  // Dedup by template hash.
  if (pending_templates_.contains(item.template_hash)) {
    return false;
  }

  // If at capacity, evict the lowest-priority item (tail).
  if (queue_.size() >= max_size_) {
    // Only evict if new item has higher priority than tail.
    // Priority: warmup > non-warmup, higher frequency > lower.
    const auto& tail = queue_.back();
    bool new_higher =
        (item.is_warmup && !tail.is_warmup) ||
        (item.is_warmup == tail.is_warmup && item.frequency > tail.frequency);
    if (!new_higher) {
      drops_++;
      return false;
    }

    // Evict tail.
    pending_templates_.erase(queue_.back().template_hash);
    queue_.pop_back();
    drops_++;
  }

  // Insert in priority order (warmup first, then by frequency desc).
  // Use insertion sort since the queue is bounded (max 1000).
  auto it = queue_.begin();
  while (it != queue_.end()) {
    bool current_higher =
        (it->is_warmup && !item.is_warmup) ||
        (it->is_warmup == item.is_warmup && it->frequency > item.frequency);
    if (!current_higher) break;
    ++it;
  }

  pending_templates_.insert(item.template_hash);
  queue_.insert(it, std::move(item));
  return true;
}

std::optional<AnalysisQueue::Item> AnalysisQueue::Dequeue() {
  std::lock_guard<std::mutex> lock(mu_);

  if (queue_.empty()) return std::nullopt;

  Item item = std::move(queue_.front());
  queue_.pop_front();
  pending_templates_.erase(item.template_hash);
  return item;
}

void AnalysisQueue::Shutdown() {
  std::lock_guard<std::mutex> lock(mu_);
  shutdown_ = true;
}

bool AnalysisQueue::is_shutdown() const {
  std::lock_guard<std::mutex> lock(mu_);
  return shutdown_;
}

size_t AnalysisQueue::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return queue_.size();
}

uint64_t AnalysisQueue::drops() const {
  std::lock_guard<std::mutex> lock(mu_);
  return drops_;
}

}  // namespace pagespeed
