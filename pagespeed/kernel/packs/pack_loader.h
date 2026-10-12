// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_PACK_LOADER_H_
#define PAGESPEED_KERNEL_PACKS_PACK_LOADER_H_

#include <string>
#include <string_view>

#include "absl/status/statusor.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

// Parses and validates a pack file's JSON text with a strict reader (one
// object at the root, no comments, no trailing commas or data, no repeated
// key at any level, nesting bounded by kMaxJsonDepth). Every string and object
// key must be well-formed UTF-8. The whole file is rejected on the first
// problem; the error is kInvalidArgument with a message of the form
//
//   <source_name>: <json path>: <reason>
//
// (for example `edge-seo.json: rules[2].match.paths[0]: glob must start with
// '/'`; the source name is omitted when empty and the path is "$" for the
// document root). A file the JSON reader rejects reports `malformed JSON:`
// followed by the reader's line and column. Validation is strict: unknown keys, duplicate keys, wrong
// types, unknown placeholders, invalid RE2, invalid hreflang codes, relative
// URLs in tables, and every limit in pack.h are load errors.
//
// `engine_version` is the running engine's version ("1.17.0" or
// "1.17.0~dev.2": only the leading major.minor.patch is read). A pack whose
// `engine_min` is newer is refused. An empty `engine_version` skips that
// check.
absl::StatusOr<Pack> LoadPack(std::string_view json,
                              std::string_view engine_version,
                              std::string_view source_name = {});

// Reads `path` (refusing files above kMaxPackFileBytes before reading) and
// calls LoadPack() with `path` as the source name.
absl::StatusOr<Pack> LoadPackFile(const std::string& path,
                                  std::string_view engine_version);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_PACK_LOADER_H_
