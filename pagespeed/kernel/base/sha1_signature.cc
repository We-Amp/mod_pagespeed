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

#include "pagespeed/kernel/base/sha1_signature.h"

#include <algorithm>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"

// URL signatures use HMAC-SHA1. On non-Windows platforms, use OpenSSL.
// On Windows, use BCrypt API.
#if ENABLE_URL_SIGNATURES
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>  // Must precede bcrypt.h
#include <bcrypt.h>
// clang-format on
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/hmac.h>
#endif
#endif

// EVP_MAX_MD_SIZE is defined by OpenSSL, provide a fallback
#ifndef EVP_MAX_MD_SIZE
#define EVP_MAX_MD_SIZE 64
#endif

namespace net_instaweb {

namespace {

int CeilDivide(int numerator, int denominator) {
  int num = numerator + denominator - 1;
  return num / denominator;
}

}  // namespace

SHA1Signature::SHA1Signature()
    : Signature(), max_chars_(kDefaultSignatureSize) {}

SHA1Signature::SHA1Signature(int signature_size)
    : Signature(), max_chars_(signature_size) {}

SHA1Signature::~SHA1Signature() {}

GoogleString SHA1Signature::RawSign(StringPiece key, StringPiece data) const {
  unsigned char hmac[EVP_MAX_MD_SIZE];
  unsigned int signature_length;

#if ENABLE_URL_SIGNATURES
#ifdef _WIN32
  // Use BCrypt API for HMAC-SHA1 on Windows.
  BCRYPT_ALG_HANDLE hAlg = nullptr;
  BCRYPT_HASH_HANDLE hHash = nullptr;
  NTSTATUS status;

  status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA1_ALGORITHM, nullptr,
                                       BCRYPT_ALG_HANDLE_HMAC_FLAG);
  DCHECK(BCRYPT_SUCCESS(status));

  DWORD hash_length = 0;
  DWORD result_length = 0;
  status = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                             reinterpret_cast<PUCHAR>(&hash_length),
                             sizeof(hash_length), &result_length, 0);
  DCHECK(BCRYPT_SUCCESS(status));

  status = BCryptCreateHash(
      hAlg, &hHash, nullptr, 0,  // No hash object buffer (let BCrypt allocate)
      const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(key.data())),
      static_cast<ULONG>(key.size()), 0);
  DCHECK(BCRYPT_SUCCESS(status));

  status = BCryptHashData(
      hHash,
      const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(data.data())),
      static_cast<ULONG>(data.size()), 0);
  DCHECK(BCRYPT_SUCCESS(status));

  status = BCryptFinishHash(hHash, hmac, hash_length, 0);
  DCHECK(BCRYPT_SUCCESS(status));
  signature_length = hash_length;

  BCryptDestroyHash(hHash);
  BCryptCloseAlgorithmProvider(hAlg, 0);

  const unsigned char* result = hmac;
#else
  unsigned char* md = HMAC(EVP_sha1(), key.data(), key.size(),
                           reinterpret_cast<const unsigned char*>(data.data()),
                           data.size(), (uint8_t*)hmac, &signature_length);
  const unsigned char* result = const_cast<const unsigned char*>(md);
  DCHECK(md != nullptr);
#endif  // _WIN32
#else
  // URL signatures disabled
  (void)key;
  (void)data;
  (void)hmac;
  const unsigned char result[kSHA1NumBytes] = {0};
  signature_length = kSHA1NumBytes;
#endif  // ENABLE_URL_SIGNATURES
  GoogleString signature(reinterpret_cast<const char*>(result),
                         signature_length);
  return signature;
}

int SHA1Signature::RawSignatureSizeInBytes() const { return kSHA1NumBytes; }

int SHA1Signature::SignatureSizeInChars() const {
  int max_length = ComputeSizeFromNumberOfBytes(RawSignatureSizeInBytes());
  return std::min(max_length, max_chars_);
}

int SHA1Signature::ComputeSizeFromNumberOfBytes(int num_bytes) {
  return CeilDivide(num_bytes * 4, 3);
}

}  // namespace net_instaweb
