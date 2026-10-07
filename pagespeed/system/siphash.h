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

#ifndef PAGESPEED_SYSTEM_SIPHASH_H_
#define PAGESPEED_SYSTEM_SIPHASH_H_

#include <cstddef>
#include <cstdint>

namespace net_instaweb {

// SipHash-2-4 (Aumasson and Bernstein, 2012): a keyed hash with a 128-bit
// key and a 64-bit result, built for hashing short inputs that someone else
// chooses.  Without the key, inputs that collide cannot be constructed and
// the result for an input cannot be predicted -- which is what a plain hash
// with a seed mixed in does not give.
//
// Streaming: feed the input in as many pieces as is convenient, then take
// the result once.  The result depends only on the bytes, not on how they
// were split.  One object hashes one input.
//
// Self-contained and header-only; no allocation.  Checked against the
// published test vectors in daemon_serve_arm_test.cc.
class SipHash24 {
 public:
  SipHash24(uint64_t key0, uint64_t key1)
      : v0_(key0 ^ 0x736f6d6570736575ULL),
        v1_(key1 ^ 0x646f72616e646f6dULL),
        v2_(key0 ^ 0x6c7967656e657261ULL),
        v3_(key1 ^ 0x7465646279746573ULL) {}

  void Update(const char* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      UpdateByte(static_cast<unsigned char>(data[i]));
    }
  }

  void UpdateByte(unsigned char byte) {
    // Words are taken little-endian, eight bytes at a time.
    pending_ |= static_cast<uint64_t>(byte) << (8 * pending_bytes_);
    ++total_bytes_;
    if (++pending_bytes_ == 8) {
      Compress(pending_);
      pending_ = 0;
      pending_bytes_ = 0;
    }
  }

  // The 64-bit result.  Call once, after the last Update.
  uint64_t Finish() {
    // The last word: the bytes left over, and the input's length modulo 256
    // in the top byte.
    Compress(pending_ | (static_cast<uint64_t>(total_bytes_ & 0xFF) << 56));
    v2_ ^= 0xFF;
    Round();
    Round();
    Round();
    Round();
    return v0_ ^ v1_ ^ v2_ ^ v3_;
  }

 private:
  static uint64_t RotateLeft(uint64_t value, int bits) {
    return (value << bits) | (value >> (64 - bits));
  }

  void Round() {
    v0_ += v1_;
    v1_ = RotateLeft(v1_, 13);
    v1_ ^= v0_;
    v0_ = RotateLeft(v0_, 32);
    v2_ += v3_;
    v3_ = RotateLeft(v3_, 16);
    v3_ ^= v2_;
    v0_ += v3_;
    v3_ = RotateLeft(v3_, 21);
    v3_ ^= v0_;
    v2_ += v1_;
    v1_ = RotateLeft(v1_, 17);
    v1_ ^= v2_;
    v2_ = RotateLeft(v2_, 32);
  }

  void Compress(uint64_t word) {
    v3_ ^= word;
    Round();
    Round();
    v0_ ^= word;
  }

  uint64_t v0_;
  uint64_t v1_;
  uint64_t v2_;
  uint64_t v3_;
  uint64_t pending_ = 0;
  int pending_bytes_ = 0;
  size_t total_bytes_ = 0;
};

}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_SIPHASH_H_
