// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef TEST_PAGESPEED_KERNEL_PACKS_TEST_JSON_H_
#define TEST_PAGESPEED_KERNEL_PACKS_TEST_JSON_H_

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "pagespeed/kernel/base/json.h"

namespace pagespeed::packs {

// A small mutable JSON value for building and breaking packs in tests, with
// brace-initialization that reads like the document it produces:
//
//   j["rules"][0]["match"] = {{"paths", {"/a/**"}}};   // object -> array
//
// A brace list whose every element is a two-element list that starts with a
// string is an object; any other brace list is an array.
class TestJson {
 public:
  TestJson() = default;
  TestJson(bool b) : kind_(Kind::kBool), int_(b ? 1 : 0) {}
  TestJson(const char* s) : kind_(Kind::kString), str_(s) {}
  TestJson(std::string s) : kind_(Kind::kString), str_(std::move(s)) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T> &&
                                                    !std::is_same_v<T, bool>>>
  TestJson(T i) : kind_(Kind::kInt), int_(static_cast<int64_t>(i)) {}
  TestJson(std::initializer_list<TestJson> list) {
    bool all_pairs = true;
    for (const TestJson& e : list) {
      if (!(e.braced_ && e.items_.size() == 2 &&
            e.items_[0].kind_ == Kind::kString)) {
        all_pairs = false;
      }
    }
    if (all_pairs && list.size() > 0) {
      kind_ = Kind::kObject;
      for (const TestJson& e : list) members_[e.items_[0].str_] = e.items_[1];
    } else {
      kind_ = Kind::kArray;
      items_ = list;
      braced_ = true;
    }
  }

  static TestJson array() {
    TestJson j;
    j.kind_ = Kind::kArray;
    return j;
  }
  static TestJson array(std::initializer_list<TestJson> list) {
    TestJson j = array();
    j.items_ = list;
    return j;
  }
  static TestJson object() {
    TestJson j;
    j.kind_ = Kind::kObject;
    return j;
  }

  // Any JSON text, scalars included, read leniently.
  static TestJson parse(const std::string& text) {
    ::Json::CharReaderBuilder builder;
    std::unique_ptr<::Json::CharReader> reader(builder.newCharReader());
    ::Json::Value v;
    std::string errors;
    if (!reader->parse(text.data(), text.data() + text.size(), &v, &errors)) {
      ADD_FAILURE() << "TestJson::parse: " << errors;
    }
    return FromJsonCpp(v);
  }

  TestJson& operator[](const char* key) { return At(std::string(key)); }
  TestJson& operator[](const std::string& key) { return At(key); }
  TestJson& operator[](int index) {
    return items_.at(static_cast<size_t>(index));
  }
  TestJson& operator[](size_t index) { return items_.at(index); }

  void erase(const std::string& key) { members_.erase(key); }
  void push_back(TestJson j) {
    kind_ = Kind::kArray;
    braced_ = false;
    items_.push_back(std::move(j));
  }

  void pop_back() { items_.pop_back(); }

  // indent < 0: compact.
  std::string dump(int indent = -1) const {
    ::Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] =
        indent < 0 ? std::string() : std::string(indent, ' ');
    builder["emitUTF8"] = true;
    return ::Json::writeString(builder, ToJsonCpp());
  }

 private:
  enum class Kind { kNull, kBool, kInt, kString, kArray, kObject };

  TestJson& At(const std::string& key) {
    if (kind_ == Kind::kNull) kind_ = Kind::kObject;
    return members_[key];
  }

  static TestJson FromJsonCpp(const ::Json::Value& v) {
    TestJson j;
    switch (v.type()) {
      case ::Json::nullValue:
        break;
      case ::Json::booleanValue:
        j = TestJson(v.asBool());
        break;
      case ::Json::intValue:
      case ::Json::uintValue:
        j = TestJson(static_cast<int64_t>(v.asInt64()));
        break;
      case ::Json::realValue:
        j = TestJson(static_cast<int64_t>(v.asDouble()));
        break;
      case ::Json::stringValue:
        j = TestJson(v.asString());
        break;
      case ::Json::arrayValue:
        j = array();
        for (const ::Json::Value& e : v) j.items_.push_back(FromJsonCpp(e));
        break;
      case ::Json::objectValue:
        j = object();
        for (const std::string& k : v.getMemberNames()) {
          j.members_[k] = FromJsonCpp(v[k]);
        }
        break;
    }
    return j;
  }

  ::Json::Value ToJsonCpp() const {
    switch (kind_) {
      case Kind::kNull:
        return ::Json::Value();
      case Kind::kBool:
        return ::Json::Value(int_ != 0);
      case Kind::kInt:
        return ::Json::Value(static_cast<::Json::Int64>(int_));
      case Kind::kString:
        return ::Json::Value(str_.data(), str_.data() + str_.size());
      case Kind::kArray: {
        ::Json::Value a(::Json::arrayValue);
        for (const TestJson& e : items_) a.append(e.ToJsonCpp());
        return a;
      }
      case Kind::kObject: {
        ::Json::Value o(::Json::objectValue);
        for (const auto& [k, v] : members_) o[k] = v.ToJsonCpp();
        return o;
      }
    }
    return ::Json::Value();
  }

  Kind kind_ = Kind::kNull;
  bool braced_ = false;  // an unresolved brace list, maybe half an object entry
  int64_t int_ = 0;
  std::string str_;
  std::vector<TestJson> items_;
  std::map<std::string, TestJson> members_;
};

}  // namespace pagespeed::packs

#endif  // TEST_PAGESPEED_KERNEL_PACKS_TEST_JSON_H_
