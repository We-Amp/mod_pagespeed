// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Out-of-line helpers shared by the worker_*_test.cc topic files; declared in
// worker_test_fixture.h.

// clang-format off
// Keep the fixture header first: it preserves the old worker_test.cc include
// order (worker.h before any Windows header), which Windows builds have proven.
// See lib/base/message_handler.h for the FormatMessage macro interaction.
#include "test/src/worker/worker_test_fixture.h"
// clang-format on

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>

#include "gtest/gtest.h"
#include "lib/cache/cache.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "src/worker/worker.h"
#include "test/test_util/pipe_client.h"
#include "test/test_util/tcp_client.h"
#include "test/test_util/worker_quiescence.h"

namespace pagespeed {

// Helper to read a test image file into a string.
std::string ReadTestFile(const std::string& filename) {
  std::string path = "test/lib/image/testdata/" + filename;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return "";
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

// Helper: open a cross-platform IPC pipe connection to the worker
intptr_t ConnectToSocket(const std::string& path) {
  return pagespeed::test::ConnectPipe(path);
}

// Reads a one-shot mgmt response to EOF (the server closes the connection
// after writing non-AUTH responses).  A single PipeRead can return just the
// first pipe-buffer chunk, which silently truncates the merged Prometheus
// document (#877) and makes find()-based assertions position-dependent.
//
// Defined here, above the first mgmt test, because that truncation is no
// longer a large-response edge case: the Prometheus document is past 4 KiB and
// grows by three lines with every counter the build gains, so a fixed-buffer
// single read now cuts it in the ordinary case — and the failure names
// whichever metric happened to fall past the cut rather than the real cause.
std::string ReadMgmtResponseToEof(intptr_t fd) {
  std::string response;
  char buf[16384];
  for (;;) {
    ssize_t n = pagespeed::test::PipeRead(fd, buf, sizeof(buf));
    if (n <= 0) break;
    response.append(buf, static_cast<size_t>(n));
  }
  return response;
}

// Helper: pre-populate cache with content and custom metadata.
// Unlike CacheOriginal(), this lets callers set origin_content_type_str
// and other metadata fields for testing image type detection.
// URL is normalized to path-only to match worker's NormalizeCacheUrl.
void CacheOriginalWithMeta(PageSpeedCache* cache, const std::string& url,
                           const std::string& content,
                           const AlternateMetadata& meta_in) {
  std::string effective_url = WorkerTest::StripSchemeAuthority(url);
  CapabilityMask default_mask;  // Desktop/Identity = 0x08
  AlternateId id =
      MaskToAlternateId(static_cast<uint8_t>(default_mask.Encode() & 0xFF));
  AlternateMetadata meta = meta_in;
  meta.full_mask = default_mask.Encode();
  auto wh = cache->WriteAlternate(effective_url, "", "https", id,
                                  content.size(), meta);
  ASSERT_TRUE(wh.has_value()) << "Failed to open write handle";
  auto written =
      wh->write_sync(std::as_bytes(std::span(content.data(), content.size())));
  ASSERT_TRUE(written.has_value()) << "Failed to write content";
  auto closed = wh->close_sync();
  ASSERT_TRUE(closed.has_value()) << "Failed to close write handle";
}

}  // namespace pagespeed
