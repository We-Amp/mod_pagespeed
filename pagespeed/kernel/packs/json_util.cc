// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/kernel/packs/json_util.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <locale.h>  // _create_locale, _locale_t
#include <stdlib.h>  // _strtod_l
#else
#include <clocale>  // LC_ALL_MASK, locale_t, newlocale
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#endif

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

// jsoncpp decodes a number through a std::istream, and the stream decoding of
// at least one C++ standard library reports a subnormal result (a value too
// small for a normal double, for example 1e-320) as a read error, so a
// document holding one fails to parse. The rescue below parses the document a
// second time with every subnormal number replaced by a placeholder string,
// which no stream decoding stumbles over, and then puts the saved values back
// into the parsed tree. Everything else - grammar, UTF-8, control characters,
// nesting, duplicate keys - is decided exactly as before, on the whole
// document.

// strtod against the "C" locale, whatever the locale of the calling thread
// is, so the decimal point of a number is always '.'. The locale object is
// made once per process and never freed: later calls still use it, and a
// destructor could race a parse on another thread.
double StrtodC(const std::string& spelling, size_t* consumed) {
  char* end = nullptr;
  double value;
#if defined(_WIN32)
  static _locale_t c_locale = _create_locale(LC_ALL, "C");
  if (c_locale != nullptr) {
    value = _strtod_l(spelling.c_str(), &end, c_locale);
  } else {
    value = std::strtod(spelling.c_str(), &end);
  }
#else
  static locale_t c_locale = newlocale(LC_ALL_MASK, "C", nullptr);
  if (c_locale != nullptr) {
    value = strtod_l(spelling.c_str(), &end, c_locale);
  } else {
    value = std::strtod(spelling.c_str(), &end);
  }
#endif
  *consumed = end != nullptr ? static_cast<size_t>(end - spelling.c_str()) : 0;
  return value;
}

// One subnormal number of the text: where it starts, how many bytes strtod
// consumed, and the value it returned.
struct SubnormalToken {
  size_t start;
  size_t length;
  double value;
};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// The length of the number token a JSON reader reads at text[pos]: digits,
// an optional fraction, an optional exponent. A trailing '.' or 'e' with no
// digits behind it is part of the token, as it is in jsoncpp's scanner.
size_t NumberTokenLength(std::string_view text, size_t pos) {
  const size_t n = text.size();
  size_t p = pos + 1;  // Past the '-' or the first digit.
  while (p < n && IsDigit(text[p])) ++p;
  if (p < n && text[p] == '.') {
    ++p;
    while (p < n && IsDigit(text[p])) ++p;
  }
  if (p < n && (text[p] == 'e' || text[p] == 'E')) {
    ++p;
    if (p < n && (text[p] == '+' || text[p] == '-')) ++p;
    while (p < n && IsDigit(text[p])) ++p;
  }
  return p - pos;
}

// Collects, in one pass over the text, every number token outside a string
// whose value is subnormal. A number written inside a string is never
// touched. A value that underflows past the subnormal range decodes to zero
// and is left where it is.
void CollectSubnormalTokens(std::string_view text,
                            std::vector<SubnormalToken>* tokens) {
  bool in_string = false;
  bool escaped = false;
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      ++i;
    } else if (c == '"') {
      in_string = true;
      ++i;
    } else if (c == '-' || IsDigit(c)) {
      const size_t length = NumberTokenLength(text, i);
      const std::string spelling(text.substr(i, length));
      size_t consumed = 0;
      const double value = StrtodC(spelling, &consumed);
      if (consumed > 0 && std::fpclassify(value) == FP_SUBNORMAL) {
        tokens->push_back({i, consumed, value});
      }
      i += length;
    } else {
      ++i;
    }
  }
}

// The rewritten form of a subnormal number is the JSON string whose decoded
// text is NUL, '#' and the token's index in decimal. NUL cannot sit in a
// decoded string of the original document: the escape that writes it makes
// the rescue give up before rewriting anything, and a raw one is refused
// before that. So no string of the document can pose as a placeholder.
constexpr char kPlaceholderNul = '\0';
constexpr char kPlaceholderHash = '#';

// Reads a placeholder's index. Accepts exactly the decimal spellings written
// for an index: a leading zero, or more digits than size_t holds, is refused.
bool PlaceholderIndex(std::string_view s, size_t* index) {
  // NUL and '#' plus the 20 digits size_t holds at most.
  if (s.size() < 3 || s.size() > 22) return false;
  if (s[0] != kPlaceholderNul || s[1] != kPlaceholderHash) return false;
  if (s.size() > 3 && s[2] == '0') return false;
  size_t value = 0;
  for (size_t i = 2; i < s.size(); ++i) {
    if (!IsDigit(s[i])) return false;
    const size_t digit = static_cast<size_t>(s[i] - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
      return false;  // Would wrap past size_t.
    }
    value = value * 10 + digit;
  }
  *index = value;
  return true;
}

// Puts the saved values back where their placeholders are. Refuses the
// document when a placeholder shows up twice, is missing (a repeated key
// collapsed the member that held it), or sits where the original had a
// number that was not a value: a numeric key, which strict JSON refuses.
bool RestoreSubnormalValues(Json::Value* value,
                            const std::vector<SubnormalToken>& tokens,
                            std::vector<char>* seen) {
  if (value->isString()) {
    const char* begin = nullptr;
    const char* end = nullptr;
    if (!value->getString(&begin, &end) || begin == end ||  // Empty string.
        begin[0] != kPlaceholderNul) {
      return true;
    }
    size_t index = 0;
    if (!PlaceholderIndex(std::string_view(begin, end - begin), &index)) {
      return false;
    }
    if (index >= tokens.size() || (*seen)[index] != 0) return false;
    (*seen)[index] = 1;
    *value = Json::Value(tokens[index].value);
    return true;
  }
  if (value->isArray()) {
    for (Json::ArrayIndex i = 0; i < value->size(); ++i) {
      if (!RestoreSubnormalValues(&(*value)[i], tokens, seen)) return false;
    }
    return true;
  }
  if (value->isObject()) {
    for (const std::string& key : value->getMemberNames()) {
      if (!key.empty() && key[0] == kPlaceholderNul) return false;
      if (!RestoreSubnormalValues(&(*value)[key], tokens, seen)) return false;
    }
  }
  return true;
}

// The strict jsoncpp parse ParseStrictJson and the rescue share.
bool ParseWithJsoncpp(std::string_view text, size_t max_depth,
                      bool reject_duplicate_keys, Json::Value* out) {
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder.settings_["rejectDupKeys"] = reject_duplicate_keys;
  builder.settings_["stackLimit"] = static_cast<int>(max_depth);
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errors;
  try {
    return reader->parse(text.data(), text.data() + text.size(), out, &errors);
  } catch (const Json::Exception&) {
    return false;
  }
}

// Parses `text` with every subnormal number replaced by a placeholder, then
// restores the values into the tree: one pass to collect the numbers, one to
// write the rewritten text, one parse of it, one walk of the tree - all
// linear in the size of the text. Returns false unless the rewritten text
// parses and every placeholder is found exactly once.
bool RescueSubnormalNumbers(std::string_view text, size_t max_depth,
                            bool reject_duplicate_keys, Json::Value* out) {
  // The escape \u0000 is the only way to put NUL in a decoded string, so a
  // document carrying it anywhere is never rewritten.
  if (text.find("\\u0000") != std::string_view::npos) return false;
  std::vector<SubnormalToken> tokens;
  CollectSubnormalTokens(text, &tokens);
  if (tokens.empty()) return false;
  // The rewritten text is the original with ASCII-only number spellings
  // swapped for ASCII placeholders, so it is well-formed UTF-8, holds no raw
  // control character, nests no deeper, and the checks already made on the
  // original text still hold for it.
  std::string rewritten;
  rewritten.reserve(text.size() + 32 * tokens.size());
  size_t copied = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    const SubnormalToken& token = tokens[i];
    rewritten.append(text.substr(copied, token.start - copied));
    rewritten += "\"\\u0000#";
    rewritten += std::to_string(i);
    rewritten += "\"";
    copied = token.start + token.length;
  }
  rewritten.append(text.substr(copied));
  Json::Value root;
  if (!ParseWithJsoncpp(rewritten, max_depth, reject_duplicate_keys, &root)) {
    return false;
  }
  std::vector<char> seen(tokens.size(), 0);
  if (!RestoreSubnormalValues(&root, tokens, &seen)) return false;
  if (std::find(seen.begin(), seen.end(), 0) != seen.end()) return false;
  *out = std::move(root);
  return true;
}

}  // namespace

bool ParseStrictJson(std::string_view text, size_t max_depth,
                     bool reject_duplicate_keys, Json::Value* out) {
  if (!IsWellFormedUtf8(text) || HasControlCharInString(text) ||
      JsonNestingExceeds(text, max_depth)) {
    return false;
  }
  Json::Value root;
  if (!ParseWithJsoncpp(text, max_depth, reject_duplicate_keys, &root)) {
    // A well-formed document can still fail here because its numbers are more
    // than the stream decoding can deliver. Try once more, with the subnormal
    // numbers held aside, and keep the old result for everything else.
    return RescueSubnormalNumbers(text, max_depth, reject_duplicate_keys, out);
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
