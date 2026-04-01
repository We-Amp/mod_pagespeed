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

// Windows-specific MD5 implementation using BCrypt API.
// This avoids the boringssl dependency which has pthread issues
// when cross-compiling with Zig.

#include "pagespeed/kernel/base/md5_hasher.h"

#if defined(_WIN32) || defined(WIN32)

// clang-format off
#include <windows.h>  // Must precede bcrypt.h
#include <bcrypt.h>
// clang-format on

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

// Link with bcrypt.lib
#pragma comment(lib, "bcrypt.lib")

namespace net_instaweb {

namespace {
const int kMD5HashLength = 16;  // MD5 produces 128-bit (16-byte) hash
}  // namespace

MD5Hasher::~MD5Hasher() {}

GoogleString MD5Hasher::RawHash(const StringPiece& content) const {
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  unsigned char hash[kMD5HashLength];
  NTSTATUS status;

  status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    return GoogleString();
  }

  status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return GoogleString();
  }

  status = BCryptHashData(
      hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(content.data())),
      static_cast<ULONG>(content.size()), 0);
  if (!BCRYPT_SUCCESS(status)) {
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return GoogleString();
  }

  status = BCryptFinishHash(hHash, hash, kMD5HashLength, 0);
  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);

  if (!BCRYPT_SUCCESS(status)) {
    return GoogleString();
  }

  return GoogleString(reinterpret_cast<char*>(hash), kMD5HashLength);
}

int MD5Hasher::RawHashSizeInBytes() const { return kMD5HashLength; }

}  // namespace net_instaweb

#endif  // _WIN32 || WIN32
