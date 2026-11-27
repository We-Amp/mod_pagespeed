// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Ported from mod_pagespeed's test/pagespeed/kernel/base/symbol_table_test.cc

#include "lib/base/symbol_table.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/base/atom.h"

namespace net_instaweb {
namespace {

TEST(SymbolTableTest, InternSensitive) {
  SymbolTableSensitive symbol_table;
  std::string s1("hello");
  std::string s2("hello");
  std::string s3("goodbye");
  std::string s4("Goodbye");

  EXPECT_NE(s1.data(), s2.data());

  Atom a1 = symbol_table.Intern(s1);
  Atom a2 = symbol_table.Intern(s2);
  Atom a3 = symbol_table.Intern(s3);
  Atom a4 = symbol_table.Intern(s4);

  // Same content → same atom (pointer equality).
  EXPECT_TRUE(a1 == a2);
  EXPECT_EQ(a1.Rep()->data(), a2.Rep()->data());

  // Different content → different atom.
  EXPECT_FALSE(a1 == a3);
  EXPECT_NE(a1.Rep()->data(), a3.Rep()->data());

  // Case-sensitive: "goodbye" != "Goodbye".
  EXPECT_FALSE(a3 == a4);

  // Value round-trips.
  EXPECT_EQ(s1, a1.value());
  EXPECT_EQ(s2, a2.value());
  EXPECT_EQ(s3, a3.value());
  EXPECT_EQ(s4, a4.value());
}

TEST(SymbolTableTest, Clear) {
  SymbolTableSensitive symbol_table;
  Atom a = symbol_table.Intern("a");
  EXPECT_EQ(1u, symbol_table.string_bytes_allocated());

  a = symbol_table.Intern("a");
  EXPECT_EQ(1u, symbol_table.string_bytes_allocated());

  symbol_table.Clear();
  EXPECT_EQ(0u, symbol_table.string_bytes_allocated());

  a = symbol_table.Intern("a");
  EXPECT_EQ(1u, symbol_table.string_bytes_allocated());
}

TEST(SymbolTableTest, BigInsert) {
  SymbolTableSensitive symbol_table;
  // Large strings (>kDefaultBlockSize/4 = 1024) get dedicated blocks.
  Atom a = symbol_table.Intern(std::string(100000, 'a'));
  Atom b = symbol_table.Intern("b");
  Atom c = symbol_table.Intern(std::string(100000, 'c'));
  Atom d = symbol_table.Intern("d");

  EXPECT_TRUE(a == symbol_table.Intern(std::string(100000, 'a')));
  EXPECT_TRUE(b == symbol_table.Intern("b"));
  EXPECT_TRUE(c == symbol_table.Intern(std::string(100000, 'c')));
  EXPECT_TRUE(d == symbol_table.Intern("d"));
}

TEST(SymbolTableTest, OverflowFirstChunk) {
  SymbolTableSensitive symbol_table;
  for (int i = 0; i < 10000; ++i) {
    symbol_table.Intern(std::to_string(i));
  }
  // Must have spilled past the initial 4KB block.
  EXPECT_LT(4096u, symbol_table.string_bytes_allocated());
}

TEST(SymbolTableTest, InternEmbeddedNull) {
  const char kBytes[] = {'A', '\0', 'B'};
  SymbolTableSensitive symbol_table;
  Atom a1 = symbol_table.Intern(std::string_view(kBytes, 1));
  Atom a2 = symbol_table.Intern(std::string_view(kBytes, 3));
  EXPECT_NE(a1, a2);
}

}  // namespace
}  // namespace net_instaweb
