// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#include "lib/base/symbol_table.h"

#include <cstring>

namespace net_instaweb {

SymbolTableSensitive::SymbolTableSensitive() {}

void SymbolTableSensitive::Clear() {
  string_map_.clear();
  pieces_.clear();

  // Free all storage blocks
  for (char* block : storage_) {
    delete[] block;
  }
  storage_.clear();

  next_ptr_ = nullptr;
  remaining_ = 0;
  string_bytes_allocated_ = 0;
}

char* SymbolTableSensitive::AllocateString(std::string_view src) {
  size_t len = src.size();

  // For large strings, allocate a dedicated block
  if (len > kDefaultBlockSize / 4) {
    char* block = new char[len];
    // Insert at second-to-last position to keep the main block at the end
    if (storage_.empty()) {
      storage_.push_back(block);
    } else {
      storage_.insert(storage_.end() - 1, block);
    }
    std::memcpy(block, src.data(), len);
    string_bytes_allocated_ += len;
    return block;
  }

  // Check if we need a new block
  if (remaining_ < len) {
    char* block = new char[kDefaultBlockSize];
    storage_.push_back(block);
    next_ptr_ = block;
    remaining_ = kDefaultBlockSize;
  }

  char* result = next_ptr_;
  std::memcpy(result, src.data(), len);
  next_ptr_ += len;
  remaining_ -= len;
  string_bytes_allocated_ += len;

  return result;
}

Atom SymbolTableSensitive::Intern(std::string_view src) {
  // Check if already interned
  auto it = string_map_.find(src);
  if (it != string_map_.end()) {
    return Atom(it->second);
  }

  // Allocate storage for the string
  char* data = AllocateString(src);

  // Create a stable string_view in pieces_
  pieces_.emplace_back(data, src.size());
  std::string_view* piece_ptr = &pieces_.back();

  // Add to the map
  string_map_[*piece_ptr] = piece_ptr;

  return Atom(piece_ptr);
}

}  // namespace net_instaweb
