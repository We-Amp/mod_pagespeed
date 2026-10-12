// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/json_util.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "absl/strings/str_cat.h"
#include "pagespeed/kernel/packs/entity_decode.h"
#include "pagespeed/kernel/packs/pack.h"

namespace pagespeed::packs {

std::string JsonChild(const std::string& path, std::string_view key) {
  return path.empty() ? std::string(key) : absl::StrCat(path, ".", key);
}

std::string JsonIndex(const std::string& path, size_t i) {
  return absl::StrCat(path, "[", i, "]");
}

absl::Status JsonFail(const std::string& path, std::string_view reason) {
  return absl::InvalidArgumentError(
      absl::StrCat(path.empty() ? "$" : path, ": ", reason));
}

absl::Status ExpectObject(const Json::Value& j, const std::string& path) {
  if (!j.isObject()) return JsonFail(path, "must be an object");
  return absl::OkStatus();
}

bool HasMember(const Json::Value& obj, std::string_view key) {
  return obj.isObject() &&
         obj.find(key.data(), key.data() + key.size()) != nullptr;
}

const Json::Value& Member(const Json::Value& obj, std::string_view key) {
  const Json::Value* v =
      obj.isObject() ? obj.find(key.data(), key.data() + key.size()) : nullptr;
  return v != nullptr ? *v : Json::Value::nullSingleton();
}

std::string AsString(const Json::Value& v) {
  const char* begin = nullptr;
  const char* end = nullptr;
  if (!v.getString(&begin, &end)) return std::string();
  return std::string(begin, end);
}

absl::Status CheckKeys(const Json::Value& obj, const std::string& path,
                       std::initializer_list<std::string_view> allowed,
                       std::initializer_list<std::string_view> required) {
  for (const std::string& key : obj.getMemberNames()) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
      return JsonFail(path, absl::StrCat("unknown key \"", key, "\""));
    }
  }
  for (std::string_view key : required) {
    if (!HasMember(obj, key)) {
      return JsonFail(path, absl::StrCat("missing required key \"", key, "\""));
    }
  }
  return absl::OkStatus();
}

absl::Status GetString(const Json::Value& obj, const std::string& path,
                       std::string_view key, std::string* out) {
  if (!HasMember(obj, key)) return absl::OkStatus();
  const Json::Value& v = Member(obj, key);
  if (!v.isString()) return JsonFail(JsonChild(path, key), "must be a string");
  *out = AsString(v);
  return absl::OkStatus();
}

absl::Status GetBool(const Json::Value& obj, const std::string& path,
                     std::string_view key, bool* out) {
  if (!HasMember(obj, key)) return absl::OkStatus();
  const Json::Value& v = Member(obj, key);
  if (!v.isBool())
    return JsonFail(JsonChild(path, key), "must be true or false");
  *out = v.asBool();
  return absl::OkStatus();
}

namespace {

absl::Status CheckUtf8At(const Json::Value& v, const std::string& path) {
  if (v.isString()) {
    if (!IsWellFormedUtf8(AsString(v))) {
      return JsonFail(path, "string is not valid UTF-8");
    }
    return absl::OkStatus();
  }
  if (v.isArray()) {
    for (Json::ArrayIndex i = 0; i < v.size(); ++i) {
      absl::Status st = CheckUtf8At(v[i], JsonIndex(path, i));
      if (!st.ok()) return st;
    }
    return absl::OkStatus();
  }
  if (v.isObject()) {
    for (const std::string& key : v.getMemberNames()) {
      if (!IsWellFormedUtf8(key)) {
        return JsonFail(path, "object key is not valid UTF-8");
      }
      absl::Status st = CheckUtf8At(Member(v, key), JsonChild(path, key));
      if (!st.ok()) return st;
    }
  }
  return absl::OkStatus();
}

}  // namespace

absl::Status CheckUtf8(const Json::Value& root) {
  return CheckUtf8At(root, "");
}

namespace {

// True when a raw control character (below 0x20) sits inside a JSON string.
// jsoncpp accepts one; JSON.parse() does not.
bool HasControlCharInString(std::string_view text) {
  bool in_string = false;
  bool escaped = false;
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!in_string) {
      in_string = c == '"';
    } else if (escaped) {
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == '"') {
      in_string = false;
    } else if (u < 0x20) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool ParseStrictJson(std::string_view text, size_t max_depth,
                     bool reject_duplicate_keys, Json::Value* out) {
  if (!IsWellFormedUtf8(text) || HasControlCharInString(text) ||
      JsonNestingExceeds(text, max_depth)) {
    return false;
  }
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder.settings_["rejectDupKeys"] = reject_duplicate_keys;
  builder.settings_["stackLimit"] = static_cast<int>(max_depth);
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  try {
    if (!reader->parse(text.data(), text.data() + text.size(), &root,
                       &errors)) {
      return false;
    }
  } catch (const Json::Exception&) {
    return false;
  }
  *out = std::move(root);
  return true;
}

std::string CompactJson(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder.settings_["indentation"] = "";
  builder.settings_["commentStyle"] = "None";
  builder.settings_["emitUTF8"] = true;
  return Json::writeString(builder, value);
}

}  // namespace pagespeed::packs
