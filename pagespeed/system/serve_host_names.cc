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

#include "pagespeed/system/serve_host_names.h"

namespace net_instaweb {

namespace {

// The longest name the optimizer keeps, and the longest value worth looking
// at: a full name plus ":" and a 5-digit port (the optimizer's own cut-off).
constexpr size_t kMaxHostNameBytes = 127;
constexpr size_t kMaxHostInputBytes = kMaxHostNameBytes + 1 + 6;

bool IsPortDigits(StringPiece port) {
  if (port.empty() || port.size() > 5) {
    return false;
  }
  for (const char c : port) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

bool IsAlnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool IsHostNameByte(char c) {
  return IsAlnum(c) || c == '.' || c == '-' || c == '_';
}

bool IsIpv6LiteralByte(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == ':' ||
         c == '.';
}

// A configured name that is an exact host name, normalised into `out`.
bool ExactConfiguredName(StringPiece name, GoogleString* out) {
  if (name.empty() || name[0] == '.') {
    out->clear();
    return false;
  }
  return NormalizeServeHostName(name, out);
}

}  // namespace

bool NormalizeServeHostName(StringPiece host, GoogleString* out) {
  out->clear();
  if (host.empty() || host.size() > kMaxHostInputBytes) {
    return false;
  }
  GoogleString v(host.data(), host.size());
  for (char& c : v) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  bool has_alnum = false;
  if (v[0] == '[') {
    const size_t close = v.find(']');
    if (close == GoogleString::npos || close == 1) {
      return false;
    }
    const StringPiece rest = StringPiece(v).substr(close + 1);
    if (!rest.empty() && (rest[0] != ':' || !IsPortDigits(rest.substr(1)))) {
      return false;
    }
    for (size_t i = 1; i < close; ++i) {
      if (!IsIpv6LiteralByte(v[i])) {
        return false;
      }
      has_alnum = has_alnum || IsAlnum(v[i]);
    }
    v.resize(close + 1);
  } else {
    const size_t colon = v.rfind(':');
    if (colon != GoogleString::npos) {
      if (!IsPortDigits(StringPiece(v).substr(colon + 1))) {
        return false;
      }
      v.resize(colon);
    }
    if (!v.empty() && v.back() == '.') {
      v.pop_back();
    }
    // Exactly one trailing "." is dropped; a name still ending in one is
    // refused, as the optimizer refuses it.
    if (v.empty() || v.back() == '.') {
      return false;
    }
    for (const char c : v) {
      if (!IsHostNameByte(c)) {
        return false;
      }
      has_alnum = has_alnum || IsAlnum(c);
    }
  }
  if (!has_alnum || v.size() > kMaxHostNameBytes) {
    return false;
  }
  out->swap(v);
  return true;
}

GoogleString VouchedServeHost(StringPiece request_host,
                              const ConfiguredHostNames& names) {
  GoogleString request;
  if (NormalizeServeHostName(request_host, &request)) {
    GoogleString configured;
    if (ExactConfiguredName(names.primary, &configured) &&
        configured == request) {
      return request;
    }
    for (const GoogleString& alias : names.aliases) {
      if (ExactConfiguredName(alias, &configured) && configured == request) {
        return request;
      }
    }
  }
  GoogleString primary;
  if (ExactConfiguredName(names.primary, &primary)) {
    return primary;
  }
  return GoogleString();
}

}  // namespace net_instaweb
