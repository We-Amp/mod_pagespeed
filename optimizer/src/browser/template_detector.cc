// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Template Detector Implementation

#include "src/browser/template_detector.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include "src/worker/html_scanner.h"

namespace pagespeed {

namespace {

// FNV-1a constants for 64-bit hash.
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t FnvHash(uint64_t hash, const void* data, size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}

uint64_t FnvHashByte(uint64_t hash, uint8_t byte) {
  hash ^= byte;
  hash *= kFnvPrime;
  return hash;
}

}  // namespace

// static
uint64_t TemplateDetector::HashStructure(const HtmlScanResult& scan_result) {
  uint64_t hash = kFnvOffsetBasis;

  // Hash the tag hierarchy: for each element, hash (depth, tag_name).
  // This captures the DOM tree structure while ignoring content,
  // attribute values, ids, and classes.
  auto hash_element = [&hash](int elem_depth, const std::string& tag_name) {
    // Hash depth as a single byte (depths > 255 are clamped).
    uint8_t depth = static_cast<uint8_t>(elem_depth > 255 ? 255 : elem_depth);
    hash = FnvHashByte(hash, depth);

    // Hash tag name bytes.
    hash = FnvHash(hash, tag_name.data(), tag_name.size());

    // Separator byte to prevent tag name concatenation collisions.
    hash = FnvHashByte(hash, 0xFF);
  };
  // Every element in document order, the ones the scanner leaves out of
  // `elements` (inside <noscript> and the like) merged back in where they
  // stood, so the hash is the one these pages had before they were left out.
  const auto& inert = scan_result.inert_elements;
  size_t next_inert = 0;
  for (size_t i = 0; i <= scan_result.elements.size(); ++i) {
    while (next_inert < inert.size() && inert[next_inert].position <= i) {
      hash_element(inert[next_inert].depth, inert[next_inert].tag_name);
      ++next_inert;
    }
    if (i < scan_result.elements.size()) {
      hash_element(scan_result.elements[i].depth,
                   scan_result.elements[i].tag_name);
    }
  }

  // Also hash the number of stylesheet links and whether inline CSS
  // exists. These are structural features that differentiate templates.
  auto num_stylesheets = static_cast<uint32_t>(scan_result.stylesheets.size());
  hash = FnvHash(hash, &num_stylesheets, sizeof(num_stylesheets));

  uint8_t has_inline_css = scan_result.inline_css.empty() ? 0 : 1;
  hash = FnvHashByte(hash, has_inline_css);

  return hash;
}

bool TemplateDetector::HasProfile(uint64_t template_hash) const {
  std::lock_guard<std::mutex> lock(mu_);
  return profiles_.contains(template_hash);
}

void TemplateDetector::RecordProfile(uint64_t template_hash,
                                     std::string_view analyzed_url) {
  std::lock_guard<std::mutex> lock(mu_);

  // If already tracked, update the URL but don't change order.
  if (auto it = profiles_.find(template_hash); it != profiles_.end()) {
    it->second = std::string(analyzed_url);
    return;
  }

  // Evict oldest entry if at capacity.
  if (profiles_.size() >= kMaxTemplates) {
    uint64_t oldest = insertion_order_.front();
    insertion_order_.pop_front();
    profiles_.erase(oldest);
  }

  profiles_[template_hash] = std::string(analyzed_url);
  insertion_order_.push_back(template_hash);
}

bool TemplateDetector::RemoveProfile(uint64_t template_hash) {
  std::lock_guard<std::mutex> lock(mu_);
  if (profiles_.erase(template_hash) == 0) {
    return false;
  }
  // Remove from insertion order (linear scan, but rare operation).
  for (auto it = insertion_order_.begin(); it != insertion_order_.end(); ++it) {
    if (*it == template_hash) {
      insertion_order_.erase(it);
      break;
    }
  }
  return true;
}

std::string TemplateDetector::GetAnalyzedUrl(uint64_t template_hash) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (auto it = profiles_.find(template_hash); it != profiles_.end()) {
    return it->second;
  }
  return {};
}

size_t TemplateDetector::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return profiles_.size();
}

}  // namespace pagespeed
