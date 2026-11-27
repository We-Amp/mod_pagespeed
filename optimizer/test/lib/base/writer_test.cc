// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Tests for Writer, StringWriter, NullWriter, and InlineSList.

#include "lib/base/writer.h"

#include <string>

#include "gtest/gtest.h"
#include "lib/base/inline_slist.h"
#include "lib/base/null_writer.h"
#include "lib/base/string_writer.h"

namespace net_instaweb {
namespace {

// --- NullWriter tests (null_writer.cc lines 15-21) ---

TEST(NullWriterTest, WriteReturnsTrue) {
  NullWriter writer;
  EXPECT_TRUE(writer.Write("hello world", nullptr));
}

TEST(NullWriterTest, WriteEmptyReturnsTrue) {
  NullWriter writer;
  EXPECT_TRUE(writer.Write("", nullptr));
}

TEST(NullWriterTest, FlushReturnsTrue) {
  NullWriter writer;
  EXPECT_TRUE(writer.Flush(nullptr));
}

TEST(NullWriterTest, MultipleWritesAllSucceed) {
  NullWriter writer;
  EXPECT_TRUE(writer.Write("first", nullptr));
  EXPECT_TRUE(writer.Write("second", nullptr));
  EXPECT_TRUE(writer.Write("third", nullptr));
  EXPECT_TRUE(writer.Flush(nullptr));
}

// --- Writer::Dump default (writer.cc lines 13-14) ---

// Minimal writer that just records writes (used to test Dump).
class RecordingWriter : public Writer {
 public:
  bool Write(std::string_view str, MessageHandler* /*handler*/) override {
    data_.append(str.data(), str.size());
    return true;
  }
  bool Flush(MessageHandler* /*handler*/) override { return true; }
  [[nodiscard]] const std::string& data() const { return data_; }

 private:
  std::string data_;
};

TEST(WriterTest, DefaultDumpReturnsFalse) {
  // The base Writer::Dump default implementation returns false.
  // NullWriter does not override Dump, so it inherits the default.
  NullWriter writer;
  RecordingWriter target;
  EXPECT_FALSE(writer.Dump(&target, nullptr));
}

// --- StringWriter::Dump (string_writer.cc lines 25-26) ---

TEST(StringWriterTest, DumpWritesToTarget) {
  std::string source_str;
  StringWriter source(&source_str);
  source.Write("hello", nullptr);
  source.Write(" world", nullptr);

  RecordingWriter target;
  EXPECT_TRUE(source.Dump(&target, nullptr));
  EXPECT_EQ(target.data(), "hello world");
}

TEST(StringWriterTest, DumpEmptyString) {
  std::string source_str;
  StringWriter source(&source_str);

  RecordingWriter target;
  EXPECT_TRUE(source.Dump(&target, nullptr));
  EXPECT_EQ(target.data(), "");
}

// --- InlineSList erase_after tail removal (inline_slist.h lines 258-263) ---

// Test element type for InlineSList.
class TestNode : public InlineSListElement<TestNode> {
 public:
  explicit TestNode(int v) : value(v) {}
  int value;
};

TEST(InlineSListTest, EraseTailElement) {
  // Create a list with 3 elements [A(1), B(2), C(3)].
  // Erase C(3) which is the tail - exercises lines 258-263.
  InlineSList<TestNode> list;
  list.Append(new TestNode(1));
  list.Append(new TestNode(2));
  list.Append(new TestNode(3));

  // Iterate to find and erase the tail element (3).
  // Use the pattern from the header comments.
  auto iter = list.begin();
  // iter points at element 1
  ASSERT_NE(iter, list.end());
  EXPECT_EQ(iter->value, 1);

  ++iter;
  // iter points at element 2
  ASSERT_NE(iter, list.end());
  EXPECT_EQ(iter->value, 2);

  ++iter;
  // iter points at element 3 (the tail)
  ASSERT_NE(iter, list.end());
  EXPECT_EQ(iter->value, 3);

  // Erase element 3 (the tail). This should trigger lines 258-263.
  list.Erase(&iter);

  // After erasing the tail, iter should be at end.
  EXPECT_EQ(iter, list.end());

  // List should now have [1, 2].
  auto check = list.begin();
  ASSERT_NE(check, list.end());
  EXPECT_EQ(check->value, 1);
  ++check;
  ASSERT_NE(check, list.end());
  EXPECT_EQ(check->value, 2);
  ++check;
  EXPECT_EQ(check, list.end());

  // Last element should be 2 now.
  EXPECT_EQ(list.Last()->value, 2);
}

TEST(InlineSListTest, EraseTailOfTwoElements) {
  // Create [A(10), B(20)], erase B (the tail).
  InlineSList<TestNode> list;
  list.Append(new TestNode(10));
  list.Append(new TestNode(20));

  auto iter = list.begin();
  EXPECT_EQ(iter->value, 10);
  ++iter;
  ASSERT_NE(iter, list.end());
  EXPECT_EQ(iter->value, 20);

  // Erase 20 (the tail).
  list.Erase(&iter);
  EXPECT_EQ(iter, list.end());

  // List should have [10] only.
  EXPECT_FALSE(list.IsEmpty());
  auto check = list.begin();
  ASSERT_NE(check, list.end());
  EXPECT_EQ(check->value, 10);
  ++check;
  EXPECT_EQ(check, list.end());

  EXPECT_EQ(list.Last()->value, 10);
}

TEST(InlineSListTest, EraseSingleElement) {
  // Create [A(5)], erase it. This exercises the single-element branch.
  InlineSList<TestNode> list;
  list.Append(new TestNode(5));

  auto iter = list.begin();
  ASSERT_NE(iter, list.end());
  EXPECT_EQ(iter->value, 5);

  list.Erase(&iter);
  EXPECT_EQ(iter, list.end());
  EXPECT_TRUE(list.IsEmpty());
}

}  // namespace
}  // namespace net_instaweb
