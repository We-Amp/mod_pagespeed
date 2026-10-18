// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Choosing the inflater for a compressed origin response from its
// Content-Encoding header, and doing so without holding on to memory: the
// decision runs on every compressed response the module rewrites.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "pagespeed/iis/iis_misc.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/util/gzip_inflater.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace {

// Net count of array allocations made through operator new[] and not yet
// released through operator delete[], across the whole test binary.
std::atomic<long> g_outstanding_arrays{0};

}  // namespace

void* operator new[](size_t size) {
  void* p = malloc(size == 0 ? 1 : size);
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  ++g_outstanding_arrays;
  return p;
}

void operator delete[](void* p) noexcept {
  if (p != nullptr) {
    --g_outstanding_arrays;
    free(p);
  }
}

void operator delete[](void* p, size_t) noexcept { operator delete[](p); }

class ConfigFactory;

namespace net_instaweb {

// The module's DLL entry point defines this; a test binary has none.
ConfigFactory* cf = nullptr;

namespace {

HTTP_KNOWN_HEADER Header(const char* value, size_t length) {
  HTTP_KNOWN_HEADER header;
  header.pRawValue = value;
  header.RawValueLength = static_cast<USHORT>(length);
  return header;
}

// Returns the inflater for |header| and checks that choosing it left no
// array allocation behind.
std::unique_ptr<GzipInflater> Choose(const HTTP_KNOWN_HEADER& header,
                                     GoogleString* name) {
  long before = g_outstanding_arrays.load();
  std::unique_ptr<GzipInflater> inflater(
      NewContentEncodingInflater(header, name));
  EXPECT_EQ(before, g_outstanding_arrays.load())
      << "choosing an inflater for '" << *name << "' kept memory";
  return inflater;
}

TEST(IisContentEncodingInflaterTest, GzipAndDeflateGetAnInflater) {
  GoogleString name;
  EXPECT_NE(nullptr, Choose(Header("gzip", 4), &name));
  EXPECT_EQ("gzip", name);
  EXPECT_NE(nullptr, Choose(Header("deflate", 7), &name));
  EXPECT_EQ("deflate", name);
  EXPECT_NE(nullptr, Choose(Header("GZip", 4), &name));
  EXPECT_EQ("GZip", name);
}

TEST(IisContentEncodingInflaterTest, OtherCodingsGetNone) {
  GoogleString name;
  EXPECT_EQ(nullptr, Choose(Header("br", 2), &name));
  EXPECT_EQ("br", name);
  EXPECT_EQ(nullptr, Choose(Header("gzip, br", 8), &name));
  EXPECT_EQ(nullptr, Choose(Header("x-gzip", 6), &name));
}

TEST(IisContentEncodingInflaterTest, ValueIsNotNulTerminated) {
  // IIS hands over a pointer and a length; the bytes after the value
  // belong to something else.
  const char raw[] = "gzipdeflate";
  GoogleString name;
  EXPECT_NE(nullptr, Choose(Header(raw, 4), &name));
  EXPECT_EQ("gzip", name);
  EXPECT_NE(nullptr, Choose(Header(raw + 4, 7), &name));
  EXPECT_EQ("deflate", name);
  EXPECT_EQ(nullptr, Choose(Header(raw, 3), &name));
  EXPECT_EQ("gzi", name);
}

TEST(IisContentEncodingInflaterTest, RepeatedChoicesKeepNoMemory) {
  long before = g_outstanding_arrays.load();
  for (int i = 0; i < 1000; ++i) {
    GoogleString name;
    std::unique_ptr<GzipInflater> inflater(
        NewContentEncodingInflater(Header("gzip", 4), &name));
    ASSERT_NE(nullptr, inflater);
  }
  EXPECT_EQ(before, g_outstanding_arrays.load());
}

}  // namespace
}  // namespace net_instaweb
