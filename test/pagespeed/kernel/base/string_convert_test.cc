/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

// Unit-test the character-accumulation helpers in string_util.h that are
// defined in string_convert.cc.

#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {

namespace {

// Largest value the accumulators let through unclamped: one below the Unicode
// maximum code point. Anything strictly above 0x110000 is clamped to it.
const uint32 kUnicodeMax = 0x110000;

class StringConvertTest : public testing::Test {};

TEST_F(StringConvertTest, AccumulateDecimalValueAccumulatesDigits) {
  uint32 value = 0;
  EXPECT_TRUE(AccumulateDecimalValue('1', &value));
  EXPECT_EQ(1u, value);
  EXPECT_TRUE(AccumulateDecimalValue('2', &value));
  EXPECT_EQ(12u, value);
  EXPECT_TRUE(AccumulateDecimalValue('3', &value));
  EXPECT_EQ(123u, value);
  EXPECT_TRUE(AccumulateDecimalValue('0', &value));
  EXPECT_EQ(1230u, value);
  EXPECT_TRUE(AccumulateDecimalValue('9', &value));
  EXPECT_EQ(12309u, value);
}

TEST_F(StringConvertTest, AccumulateDecimalValueRefusesNonDigits) {
  // The entries flanking the '0'..'9' range cover both range boundaries; the
  // rest are letters, whitespace, punctuation, and NUL.
  const char kNonDigits[] = {'/', ':',  'a',  'z', 'A', 'Z',
                             ' ', '\t', '\n', '!', '-', '\0'};
  const uint32 kAccumulated = 4242;
  for (char c : kNonDigits) {
    uint32 value = kAccumulated;
    EXPECT_FALSE(AccumulateDecimalValue(c, &value))
        << "char with value " << static_cast<int>(c);
    EXPECT_EQ(kAccumulated, value)
        << "rejected char " << static_cast<int>(c) << " changed *value";
  }
}

TEST_F(StringConvertTest, AccumulateDecimalValueAllowsUnicodeMaxMinusOne) {
  uint32 value = 0;
  for (const char* c = "1114111"; *c != '\0'; ++c) {
    EXPECT_TRUE(AccumulateDecimalValue(*c, &value));
  }
  EXPECT_EQ(kUnicodeMax - 1, value);
}

TEST_F(StringConvertTest, AccumulateDecimalValueClampsAboveUnicodeMax) {
  // Crossing the limit clamps rather than wrapping.
  uint32 value = kUnicodeMax - 1;
  EXPECT_TRUE(AccumulateDecimalValue('1', &value));
  EXPECT_EQ(kUnicodeMax, value);
  // Further digits keep the accumulator pinned at the clamp.
  EXPECT_TRUE(AccumulateDecimalValue('9', &value));
  EXPECT_EQ(kUnicodeMax, value);

  // The same clamp applies when accumulating from zero.
  uint32 from_zero = 0;
  for (const char* c = "99999999"; *c != '\0'; ++c) {
    EXPECT_TRUE(AccumulateDecimalValue(*c, &from_zero));
  }
  EXPECT_EQ(kUnicodeMax, from_zero);
}

TEST_F(StringConvertTest, AccumulateHexValueAcceptsLowercaseDigits) {
  uint32 value = 0;
  EXPECT_TRUE(AccumulateHexValue('0', &value));
  EXPECT_EQ(0u, value);
  EXPECT_TRUE(AccumulateHexValue('9', &value));
  EXPECT_EQ(9u, value);
  EXPECT_TRUE(AccumulateHexValue('a', &value));
  EXPECT_EQ(0x9au, value);
  EXPECT_TRUE(AccumulateHexValue('f', &value));
  EXPECT_EQ(0x9afu, value);
}

TEST_F(StringConvertTest, AccumulateHexValueAcceptsUppercaseDigits) {
  uint32 value = 0;
  EXPECT_TRUE(AccumulateHexValue('F', &value));
  EXPECT_EQ(0xfu, value);
  EXPECT_TRUE(AccumulateHexValue('A', &value));
  EXPECT_EQ(0xfau, value);
  EXPECT_TRUE(AccumulateHexValue('1', &value));
  EXPECT_EQ(0xfa1u, value);
}

TEST_F(StringConvertTest, AccumulateHexValueAcceptsMixedCaseDigits) {
  uint32 value = 0;
  for (const char* c = "aB9cD"; *c != '\0'; ++c) {
    EXPECT_TRUE(AccumulateHexValue(*c, &value));
  }
  EXPECT_EQ(0xab9cdu, value);
}

TEST_F(StringConvertTest, AccumulateHexValueRefusesNonHexChars) {
  // '/' and ':' flank '0'..'9'; '`'/'g' flank 'a'..'f'; '@'/'G' flank 'A'..'F'.
  const char kNonHex[] = {'/', ':', '`', 'g', '@',  'G',
                          'h', 'z', 'Z', ' ', '\t', '\0'};
  const uint32 kAccumulated = 4242;
  for (char c : kNonHex) {
    uint32 value = kAccumulated;
    EXPECT_FALSE(AccumulateHexValue(c, &value))
        << "char with value " << static_cast<int>(c);
    EXPECT_EQ(kAccumulated, value)
        << "rejected char " << static_cast<int>(c) << " changed *value";
  }
}

TEST_F(StringConvertTest, AccumulateHexValueAllowsUnicodeMaxMinusOne) {
  uint32 value = 0;
  for (const char* c = "10ffff"; *c != '\0'; ++c) {
    EXPECT_TRUE(AccumulateHexValue(*c, &value));
  }
  EXPECT_EQ(kUnicodeMax - 1, value);
}

TEST_F(StringConvertTest, AccumulateHexValueClampsAboveUnicodeMax) {
  // Crossing the limit clamps rather than wrapping.
  uint32 value = kUnicodeMax - 1;
  EXPECT_TRUE(AccumulateHexValue('1', &value));
  EXPECT_EQ(kUnicodeMax, value);
  // Further digits keep the accumulator pinned at the clamp.
  EXPECT_TRUE(AccumulateHexValue('f', &value));
  EXPECT_EQ(kUnicodeMax, value);

  // The same clamp applies when accumulating from zero.
  uint32 from_zero = 0;
  for (const char* c = "fffffff"; *c != '\0'; ++c) {
    EXPECT_TRUE(AccumulateHexValue(*c, &from_zero));
  }
  EXPECT_EQ(kUnicodeMax, from_zero);
}

}  // namespace

}  // namespace net_instaweb
