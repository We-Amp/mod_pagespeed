// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_SYMBOL_TABLE_H_
#define PAGESPEED_LIB_BASE_SYMBOL_TABLE_H_

#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lib/base/atom.h"
#include "lib/base/basictypes.h"

namespace net_instaweb {

// Implements a symbol table that interns strings and returns Atoms.
// Atoms are cheap to compare (pointer comparison) and their storage
// is managed by the SymbolTable.
//
// This is a case-sensitive implementation. When the symbol table is
// destroyed, all Atoms created from it become invalid.
class SymbolTableSensitive {
 public:
  SymbolTableSensitive();
  ~SymbolTableSensitive() { Clear(); }

  // Remove all symbols in the table, invalidating any Atoms that
  // were previously interned.
  void Clear();

  // Remember a string in the table, returning it as an Atom.
  // The returned Atom's Rep() points to stable storage owned by
  // this SymbolTable.
  Atom Intern(std::string_view src);

  // Returns the number of bytes allocated on behalf of the data,
  // excluding any overhead added by the symbol table.
  size_t string_bytes_allocated() const { return string_bytes_allocated_; }

 private:
  // Hash function for string_view
  struct StringViewHash {
    size_t operator()(std::string_view sv) const {
      return std::hash<std::string_view>{}(sv);
    }
  };

  // Map from string content to the stable string_view in pieces_
  std::unordered_map<std::string_view, std::string_view*, StringViewHash>
      string_map_;

  // Storage for string_view objects. We need stable addresses, so use a list.
  std::list<std::string_view> pieces_;

  // Storage for the actual string data
  static constexpr size_t kDefaultBlockSize = 4096;
  std::vector<char*> storage_;
  char* next_ptr_ = nullptr;
  size_t remaining_ = 0;
  size_t string_bytes_allocated_ = 0;

  // Allocate space for a string of the given length and copy the data.
  char* AllocateString(std::string_view src);

  DISALLOW_COPY_AND_ASSIGN(SymbolTableSensitive);
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_SYMBOL_TABLE_H_
