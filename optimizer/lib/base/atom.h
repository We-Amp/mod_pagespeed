// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// This file is derived from mod_pagespeed and has been substantially modified.
// Originally licensed under Apache License, Version 2.0.
// Copyright (c) 2010-2017 Google Inc.
// Copyright (c) 2018 The Apache Software Foundation.

// Ported from mod_pagespeed for PageSpeed 2.0

#ifndef PAGESPEED_LIB_BASE_ATOM_H_
#define PAGESPEED_LIB_BASE_ATOM_H_

#include <string_view>

namespace net_instaweb {

// An Atom is a cheap way to represent a string in memory.  Atoms are
// created by interning strings into a SymbolTable.  Atoms can be
// compared to one another for equality using ==.  A std::string_view*
// can be extracted from an Atom.
//
// Atoms are memory-managed by the symbol table from which they came.
// When the symbol table is destroyed, so are all the Atoms that
// were interned in it.
//
// Note that Atom is a thin wrapper around std::string_view*, and can
// be passed around by value (8 bytes on 64-bit systems).
class Atom {
 public:
  Atom() : rep_(nullptr) {}

  // Return the underlying std::string_view pointer.
  const std::string_view* Rep() const { return rep_; }

  // Comparison for equality.  Atoms from different SymbolTables should
  // not be compared.
  bool operator==(const Atom& other) const { return rep_ == other.rep_; }
  bool operator!=(const Atom& other) const { return rep_ != other.rep_; }

  // Returns the string_view value of this atom.
  std::string_view value() const {
    return (rep_ != nullptr) ? *rep_ : std::string_view();
  }

 private:
  friend class SymbolTableSensitive;

  explicit Atom(const std::string_view* rep) : rep_(rep) {}

  const std::string_view* rep_;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_LIB_BASE_ATOM_H_
