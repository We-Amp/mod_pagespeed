// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_KERNEL_PACKS_JSON_UTIL_H_
#define PAGESPEED_KERNEL_PACKS_JSON_UTIL_H_

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "pagespeed/kernel/base/json.h"

// Small helpers over Json::Value shared by the loader and the kind handlers.
// Every error is kInvalidArgument with the message "<json path>: <reason>"
// (the path is "$" for the document root); the loader prefixes the source
// name.
namespace pagespeed::packs {

// "a.b" for ("a", "b"); "b" for ("", "b").
std::string JsonChild(const std::string& path, std::string_view key);
// "a[3]" for ("a", 3).
std::string JsonIndex(const std::string& path, size_t i);

// The error every checker below returns.
absl::Status JsonFail(const std::string& path, std::string_view reason);

absl::Status ExpectObject(const Json::Value& j, const std::string& path);

// Rejects keys outside `allowed` and a missing key of `required`.
absl::Status CheckKeys(const Json::Value& obj, const std::string& path,
                       std::initializer_list<std::string_view> allowed,
                       std::initializer_list<std::string_view> required);

// Each of these leaves `*out` untouched when `key` is absent.
absl::Status GetString(const Json::Value& obj, const std::string& path,
                       std::string_view key, std::string* out);
absl::Status GetBool(const Json::Value& obj, const std::string& path,
                     std::string_view key, bool* out);

// Object-member and string access that never throws or inserts. A member
// name may contain NUL bytes, so names are always taken as string_views.
bool HasMember(const Json::Value& obj, std::string_view key);
const Json::Value& Member(const Json::Value& obj, std::string_view key);
std::string AsString(const Json::Value& v);

// OK when every object key and every string in `root` is well-formed UTF-8;
// otherwise the error names the JSON path of the first offender. jsoncpp, unlike
// a validating parser, hands over whatever bytes the file held.
absl::Status CheckUtf8(const Json::Value& root);

// Parses `text` as a page's JSON (a JSON-LD block, an expanded template) the
// way a browser's JSON.parse() reads it, for objects and arrays: strict
// grammar (no comments, trailing commas, single quotes or special floats),
// nothing after the value, well-formed UTF-8, no raw control character inside
// a string, and nesting of at most `max_depth` levels. A repeated key in an
// object is an error only when `reject_duplicate_keys` is set (a browser keeps
// the last one). Never throws.
bool ParseStrictJson(std::string_view text, size_t max_depth,
                     bool reject_duplicate_keys, Json::Value* out);

// `value` on one line with no insignificant whitespace; object keys come out
// in sorted order, so two values that are equal as JSON give equal text.
std::string CompactJson(const Json::Value& value);

}  // namespace pagespeed::packs

#endif  // PAGESPEED_KERNEL_PACKS_JSON_UTIL_H_
