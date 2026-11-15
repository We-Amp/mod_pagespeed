// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Cache-seeding tool for test-nginx HIT-path tests.
//
// Usage:
//   seed_test_cache --cache /tmp/test.vol --url /style.css --host localhost \
//     --content "h1{color:red}" --content-type css --mask 0x08
//   seed_test_cache --cache /tmp/test.vol --create_only
//   seed_test_cache --cache /shared/cache.vol --url /style.css \
//     --host localhost --scheme http --remove_alternate 0x6c
//
// Creates a Cyclone cache volume and writes a single alternate with the given
// content, content type, and capability mask. Used by t/seed-*.sh scripts
// before running test-nginx HIT-path tests. --create_only creates the volume
// and writes nothing: the nginx module opens its cache resolve-only
// (volume_size = 0) and cannot cold-create a volume, so MISS-path tests run
// against pre-created empty volumes.  --remove_alternate removes ONE
// alternate of an existing volume (opened resolve-only, at the size it was
// created with, like the nginx module): the compliance suite uses it to take
// a URL's stored header block away behind a running server's back.
//
// The nginx module reads and records in the keyspace of the server block
// that received the request (src/nginx/site_identity.h).  --site_listen and
// --site_names name that block, so the entry lands where it reads:
//   seed_test_cache ... --site_listen 0.0.0.0:1984 --site_names localhost
// Without --site_listen the entry goes to the unsited keyspace, which no
// server block of the nginx module reads.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_split.h"
#include "lib/cache/cache.h"
#include "lib/cache/origin_cache_control.h"
#include "lib/classify/alternate_id.h"
#include "lib/classify/alternate_metadata.h"
#include "lib/classify/capability_mask.h"
#include "lib/classify/content_type.h"
#include "src/nginx/site_identity.h"

ABSL_FLAG(std::string, cache, "", "Path to cache volume file");
ABSL_FLAG(std::string, url, "", "URL path (e.g. /style.css)");
ABSL_FLAG(std::string, host, "localhost", "Hostname for cache key");
ABSL_FLAG(std::string, scheme, "https",
          "Scheme for cache key (must match the request's scheme: the key is "
          "scheme://host/url — seeding plain-http test requests needs "
          "--scheme http)");
ABSL_FLAG(std::string, content, "", "Content string to store");
ABSL_FLAG(std::string, content_file, "",
          "File to read content from (overrides --content)");
ABSL_FLAG(std::string, content_type, "css",
          "Content type: html, css, js, image, other");
ABSL_FLAG(std::string, mask, "0x08", "Capability mask as hex (e.g. 0x08)");
ABSL_FLAG(std::string, origin_ct, "",
          "Origin content-type string (e.g. text/css)");
ABSL_FLAG(std::string, origin_html_hash, "",
          "64 hex chars: v7 origin_html_hash content-identity stamp");
ABSL_FLAG(std::string, origin_etag, "",
          "Origin ETag stored verbatim (v5, e.g. '\"abc\"')");
ABSL_FLAG(uint32_t, origin_last_modified, 0,
          "Origin Last-Modified as unix timestamp (v5, 0 = absent)");
ABSL_FLAG(std::string, origin_cc, "",
          "Origin Cache-Control value stored with the entry (e.g. "
          "'public, max-age=600'); empty = no Cache-Control");
ABSL_FLAG(std::string, content_hex, "",
          "Content as hex bytes (overrides --content; e.g. a 32-byte "
          "content-hash sentinel)");
ABSL_FLAG(std::string, remove_alternate, "",
          "Alternate id as hex (e.g. 0x6c): remove that one alternate of "
          "--url from an EXISTING volume and exit.  The volume is opened at "
          "the size it was created with; nothing is written.");
ABSL_FLAG(std::string, site_listen, "",
          "Comma-separated listening addresses of the nginx server block the "
          "entry is for, as nginx prints them (e.g. 0.0.0.0:1984).  Empty = "
          "the unsited keyspace.");
ABSL_FLAG(std::string, site_names, "",
          "Comma-separated server names of that block, regular expressions "
          "with their leading ~ (e.g. localhost,site.test).  Empty = the one "
          "empty name nginx gives a block without server_name.");
ABSL_FLAG(bool, create_only, false,
          "Create the volume and exit without writing an entry. Used to "
          "pre-create empty volumes for the resolve-only nginx module "
          "(volume_size = 0 cannot cold-create a volume).");

namespace {

pagespeed::ContentType ParseContentType(const std::string& s) {
  if (s == "html") return pagespeed::ContentType::kHtml;
  if (s == "css") return pagespeed::ContentType::kCss;
  if (s == "js") return pagespeed::ContentType::kJs;
  if (s == "image") return pagespeed::ContentType::kImage;
  return pagespeed::ContentType::kOther;
}

std::string DefaultOriginCt(pagespeed::ContentType ct) {
  switch (ct) {
    case pagespeed::ContentType::kHtml:
      return "text/html";
    case pagespeed::ContentType::kCss:
      return "text/css";
    case pagespeed::ContentType::kJs:
      return "application/javascript";
    case pagespeed::ContentType::kImage:
      return "image/jpeg";
    default:
      return "application/octet-stream";
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);

  const std::string cache_path = absl::GetFlag(FLAGS_cache);
  const std::string url = absl::GetFlag(FLAGS_url);
  const std::string host = absl::GetFlag(FLAGS_host);
  const std::string mask_str = absl::GetFlag(FLAGS_mask);
  const bool create_only = absl::GetFlag(FLAGS_create_only);

  if (cache_path.empty() || (!create_only && url.empty())) {
    std::cerr << "Usage: seed_test_cache --cache <path> --url <url> "
              << "[--host <host>] [--content <str>] [--content-file <file>] "
              << "[--content-type css|html|js|image|other] [--mask 0x08]\n"
              << "   or: seed_test_cache --cache <path> --create_only\n";
    return 1;
  }

  // Parse mask.
  char* endptr = nullptr;
  errno = 0;
  unsigned long parsed = std::strtoul(mask_str.c_str(), &endptr, 0);
  if (errno == ERANGE || endptr == mask_str.c_str() || *endptr != '\0') {
    std::cerr << "Error: invalid mask value: " << mask_str << "\n";
    return 1;
  }
  uint32_t mask_val = static_cast<uint32_t>(parsed);

  // The server block's keyspace, when one is named.
  std::string site;
  const std::string site_listen = absl::GetFlag(FLAGS_site_listen);
  if (!site_listen.empty()) {
    std::vector<std::string> names =
        absl::StrSplit(absl::GetFlag(FLAGS_site_names), ',');
    std::vector<std::string> listen = absl::StrSplit(site_listen, ',');
    site =
        pagespeed::DeriveServerBlockSite(std::move(names), std::move(listen));
  }

  // Read content from file or flag.
  std::string content;
  const std::string content_file = absl::GetFlag(FLAGS_content_file);
  if (!content_file.empty()) {
    std::ifstream ifs(content_file, std::ios::binary);
    if (!ifs) {
      std::cerr << "Error: cannot open content file: " << content_file << "\n";
      return 1;
    }
    content.assign(std::istreambuf_iterator<char>(ifs),
                   std::istreambuf_iterator<char>());
  } else if (!absl::GetFlag(FLAGS_content_hex).empty()) {
    const std::string hex = absl::GetFlag(FLAGS_content_hex);
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (hex.size() % 2 != 0) {
      std::cerr << "Error: --content_hex needs an even number of digits\n";
      return 1;
    }
    for (size_t i = 0; i < hex.size(); i += 2) {
      const int hi = nib(hex[i]);
      const int lo = nib(hex[i + 1]);
      if (hi < 0 || lo < 0) {
        std::cerr << "Error: --content_hex contains a non-hex char\n";
        return 1;
      }
      content.push_back(static_cast<char>((hi << 4) | lo));
    }
  } else {
    content = absl::GetFlag(FLAGS_content);
  }

  if (!create_only && absl::GetFlag(FLAGS_remove_alternate).empty() &&
      content.empty()) {
    std::cerr
        << "Error: no content specified (use --content or --content-file)\n";
    return 1;
  }

  const std::string remove_str = absl::GetFlag(FLAGS_remove_alternate);
  if (!remove_str.empty()) {
    char* rend = nullptr;
    errno = 0;
    unsigned long rid = std::strtoul(remove_str.c_str(), &rend, 0);
    if (errno == ERANGE || rend == remove_str.c_str() || *rend != '\0' ||
        rid > 0xFF) {
      std::cerr << "Error: invalid --remove_alternate value: " << remove_str
                << "\n";
      return 1;
    }
    pagespeed::PageSpeedCacheConfig existing;
    existing.volume_path = cache_path;
    existing.enable_checksum = true;
    existing.volume_size = 0;  // resolve-only: never re-stripe a live volume
    auto opened = pagespeed::PageSpeedCache::Create(existing);
    if (!opened.has_value()) {
      std::cerr << "Error: failed to open cache at " << cache_path
                << " err=" << static_cast<int>(opened.error()) << "\n";
      return 1;
    }
    auto removed = (*opened)->RemoveAlternate(
        url, host, absl::GetFlag(FLAGS_scheme),
        static_cast<pagespeed::AlternateId>(rid), site);
    if (!removed.has_value()) {
      std::cerr << "Error: RemoveAlternate failed err="
                << static_cast<int>(removed.error()) << "\n";
      return 1;
    }
    std::cout << "Removed alternate 0x" << std::hex << rid << " of " << url
              << " in " << cache_path << "\n";
    return 0;
  }

  // Create cache. Use default 1GB volume size to match nginx module's default
  // PageSpeedCacheConfig. Cyclone re-stripes on open if sizes differ, making
  // seeded data invisible.
  pagespeed::PageSpeedCacheConfig config;
  config.volume_path = cache_path;
  config.enable_checksum = true;

  auto cache_result = pagespeed::PageSpeedCache::Create(config);
  if (!cache_result.has_value()) {
    std::cerr << "Error: failed to create cache at " << cache_path
              << " err=" << static_cast<int>(cache_result.error()) << "\n";
    return 1;
  }
  auto& cache = *cache_result;

  if (create_only) {
    std::cout << "Created empty volume at " << cache_path << "\n";
    return 0;
  }

  // Build metadata.
  auto ct = ParseContentType(absl::GetFlag(FLAGS_content_type));
  std::string origin_ct = absl::GetFlag(FLAGS_origin_ct);
  if (origin_ct.empty()) {
    origin_ct = DefaultOriginCt(ct);
  }

  pagespeed::AlternateId id =
      pagespeed::MaskToAlternateId(static_cast<uint8_t>(mask_val & 0xFF));

  pagespeed::AlternateMetadata meta;
  meta.full_mask = mask_val;
  meta.content_type = ct;
  meta.origin_content_type = origin_ct;
  meta.cache_inserted_at = static_cast<uint32_t>(std::time(nullptr));
  meta.origin_etag = absl::GetFlag(FLAGS_origin_etag);
  meta.origin_last_modified = absl::GetFlag(FLAGS_origin_last_modified);
  const std::string origin_cc = absl::GetFlag(FLAGS_origin_cc);
  if (!origin_cc.empty()) {
    pagespeed::OriginCacheControl cc;
    pagespeed::AccumulateOriginCacheControl(origin_cc, &cc);
    meta.origin_cc_flags = cc.cc_flags;
    meta.origin_max_age = cc.max_age;
    meta.origin_s_maxage = cc.s_maxage;
  }

  // Optional v7 content-identity stamp (64 hex chars = 32 bytes).
  const std::string hash_hex = absl::GetFlag(FLAGS_origin_html_hash);
  if (!hash_hex.empty()) {
    if (hash_hex.size() != 2 * pagespeed::AlternateMetadata::kHashSize) {
      std::cerr << "Error: --origin_html_hash must be exactly "
                << 2 * pagespeed::AlternateMetadata::kHashSize
                << " hex chars\n";
      return 1;
    }
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (size_t i = 0; i < pagespeed::AlternateMetadata::kHashSize; ++i) {
      const int hi = nibble(hash_hex[2 * i]);
      const int lo = nibble(hash_hex[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        std::cerr << "Error: --origin_html_hash contains non-hex char\n";
        return 1;
      }
      meta.origin_html_hash[i] = static_cast<std::byte>((hi << 4) | lo);
    }
  }

  // Write alternate.  A sentinel id (e.g. 0x1C, Early Hints) is stored with
  // WriteSentinel: no metadata prefix, and WriteAlternate refuses it.  The
  // agent Markdown copy (0x7C) is a sentinel with a metadata prefix and has
  // its own writer.
  const bool agent_markdown = id == static_cast<pagespeed::AlternateId>(
                                        pagespeed::SentinelId::kAgentMarkdown);
  auto wh = agent_markdown ? cache->WriteAgentAlternate(
                                 url, host, absl::GetFlag(FLAGS_scheme),
                                 content.size(), meta, site)
            : pagespeed::IsSentinel(id)
                ? cache->WriteSentinel(url, host, absl::GetFlag(FLAGS_scheme),
                                       static_cast<pagespeed::SentinelId>(id),
                                       content.size(), site)
                : cache->WriteAlternate(url, host, absl::GetFlag(FLAGS_scheme),
                                        id, content.size(), meta, site);
  if (!wh.has_value()) {
    std::cerr << "Error: WriteAlternate failed err="
              << static_cast<int>(wh.error()) << "\n";
    return 1;
  }

  auto bytes = std::as_bytes(std::span(content));
  auto written = wh->write_sync(bytes);
  if (!written.has_value()) {
    std::cerr << "Error: write_sync failed err="
              << static_cast<int>(written.error()) << "\n";
    return 1;
  }

  auto closed = wh->close_sync();
  if (!closed.has_value()) {
    std::cerr << "Error: close_sync failed err="
              << static_cast<int>(closed.error()) << "\n";
    return 1;
  }

  std::cout << "Seeded " << content.size() << " bytes at " << url << " (mask=0x"
            << std::hex << mask_val << ") in " << cache_path << "\n";
  return 0;
}
