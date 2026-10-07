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

#include "pagespeed/system/daemon_site_filter.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "pagespeed/kernel/base/json.h"
#include "pagespeed/system/serve_host_names.h"

namespace net_instaweb {

namespace {

constexpr char kHostBlock[] = "serve_savings_by_host";

// Nesting no real answer comes near (the stats document is four deep).
constexpr int kMaxNesting = 64;

// The keys a cooldown entry carries; anything else is dropped.
// The fields a cooldown entry carries, as the optimizer writes them and the
// console reads them: text, and whole seconds.  Anything else is dropped.
constexpr const char* kCooldownTextKeys[] = {"url", "hostname", "scheme",
                                             "reason"};
constexpr const char* kCooldownSecondsKeys[] = {"remaining_seconds",
                                                "duration_seconds"};

// Reads `body` strictly into `root`: exactly one JSON object, no comments,
// no trailing data, no repeated key at any level, bounded nesting.
bool ParseObject(StringPiece body, Json::Value* root) {
  if (body.empty()) {
    return false;
  }
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  builder.settings_["stackLimit"] = kMaxNesting;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string errors;
  try {
    if (!reader->parse(body.data(), body.data() + body.size(), root, &errors)) {
      return false;
    }
  } catch (const std::exception&) {
    // The reader throws past its nesting limit.
    return false;
  }
  return root->isObject();
}

GoogleString WriteCompact(const Json::Value& root) {
  Json::StreamWriterBuilder builder;
  builder["commentStyle"] = "None";
  builder["indentation"] = "";
  // ASCII only: non-ASCII text as \uXXXX escapes, so the answer is valid
  // UTF-8 whatever bytes the optimizer sent.
  builder["emitUTF8"] = false;
  return Json::writeString(builder, root);
}

// A counter: an integer 0..2^64-1 written as one (no fraction, exponent or
// text; a number outside 64 bits reads as a fraction and is refused).
bool ReadCounter(const Json::Value& value, uint64_t* out) {
  if (value.type() == Json::uintValue) {
    *out = value.asUInt64();
    return true;
  }
  if (value.type() == Json::intValue && value.asInt64() >= 0) {
    *out = static_cast<uint64_t>(value.asInt64());
    return true;
  }
  return false;
}

struct Counters {
  uint64_t hits = 0;
  uint64_t original_bytes = 0;
  uint64_t optimized_bytes = 0;
};

bool ReadCounters(const Json::Value& entry, Counters* out) {
  return entry.isObject() && ReadCounter(entry["hits"], &out->hits) &&
         ReadCounter(entry["original_bytes"], &out->original_bytes) &&
         ReadCounter(entry["optimized_bytes"], &out->optimized_bytes);
}

bool AddCounter(uint64_t value, uint64_t* total) {
  if (*total > std::numeric_limits<uint64_t>::max() - value) {
    return false;
  }
  *total += value;
  return true;
}

bool AddCounters(const Counters& c, Counters* total) {
  return AddCounter(c.hits, &total->hits) &&
         AddCounter(c.original_bytes, &total->original_bytes) &&
         AddCounter(c.optimized_bytes, &total->optimized_bytes);
}

Json::Value CountersJson(const Counters& c) {
  Json::Value v(Json::objectValue);
  v["hits"] = Json::Value(static_cast<Json::UInt64>(c.hits));
  v["original_bytes"] =
      Json::Value(static_cast<Json::UInt64>(c.original_bytes));
  v["optimized_bytes"] =
      Json::Value(static_cast<Json::UInt64>(c.optimized_bytes));
  return v;
}

// The by-host block a per-host console may see, rebuilt from `block`;
// false when `block` is malformed in any way.
bool SiteHostBlock(const Json::Value& block, StringPiece own_host,
                   Json::Value* out) {
  if (!block.isObject()) {
    return false;
  }
  const Json::Value& hosts = block["hosts"];
  uint64_t limit = 0;
  Counters other;
  if (!hosts.isArray() || !ReadCounter(block["limit"], &limit) ||
      !ReadCounters(block["other"], &other)) {
    return false;
  }
  Counters own;
  bool has_own = false;
  for (const Json::Value& row : hosts) {
    Counters row_counters;
    if (!ReadCounters(row, &row_counters) || !row["host"].isString()) {
      return false;
    }
    // Rows carry the optimizer's own normalised names (its reader reports
    // only fixed points of the host-name rule), so an exact compare is the
    // rule; a row that is not normalised is folded, never shown.
    const std::string host = row["host"].asString();
    if (!own_host.empty() && StringPiece(host) == own_host) {
      if (!AddCounters(row_counters, &own)) {
        return false;
      }
      has_own = true;
    } else if (!AddCounters(row_counters, &other)) {
      return false;
    }
  }
  Json::Value kept(Json::arrayValue);
  if (has_own) {
    Json::Value row = CountersJson(own);
    row["host"] =
        Json::Value(own_host.data(), own_host.data() + own_host.size());
    kept.append(row);
  }
  *out = Json::Value(Json::objectValue);
  (*out)["hosts"] = kept;
  (*out)["limit"] = Json::Value(static_cast<Json::UInt64>(limit));
  (*out)["other"] = CountersJson(other);
  // The marker: whose block this is, "" when the site has no name.
  (*out)["site"] =
      Json::Value(own_host.data(), own_host.data() + own_host.size());
  return true;
}

// The cooldown entry this site may see, rebuilt from `entry` with only its
// known fields; false when it is not this site's or any known field has
// the wrong type.
bool SiteCooldownEntry(const Json::Value& entry, StringPiece own_host,
                       Json::Value* out) {
  // Without a URL the entry names nothing the console can show.
  if (!entry.isObject() || !entry["hostname"].isString() ||
      !entry["url"].isString()) {
    return false;
  }
  GoogleString host;
  if (!NormalizeServeHostName(entry["hostname"].asString(), &host) ||
      StringPiece(host) != own_host) {
    return false;
  }
  *out = Json::Value(Json::objectValue);
  for (const char* key : kCooldownTextKeys) {
    if (entry.isMember(key)) {
      if (!entry[key].isString()) {
        return false;
      }
      (*out)[key] = entry[key];
    }
  }
  for (const char* key : kCooldownSecondsKeys) {
    if (entry.isMember(key)) {
      uint64_t seconds = 0;
      if (!ReadCounter(entry[key], &seconds)) {
        return false;
      }
      (*out)[key] = Json::Value(static_cast<Json::UInt64>(seconds));
    }
  }
  return true;
}

}  // namespace

bool FilterStatsForSite(StringPiece upstream_body, StringPiece own_host,
                        GoogleString* out) {
  out->clear();
  // Fails closed on anything thrown while reading, copying or writing --
  // an allocation failure included -- rather than ending the process.
  try {
    Json::Value root;
    if (!ParseObject(upstream_body, &root)) {
      return false;
    }
    if (root.isMember(kHostBlock)) {
      Json::Value block;
      if (SiteHostBlock(root[kHostBlock], own_host, &block)) {
        root[kHostBlock] = std::move(block);
      } else {
        root.removeMember(kHostBlock);
      }
    }
    *out = WriteCompact(root);
    return true;
  } catch (const std::exception&) {
    out->clear();
    return false;
  }
}

bool FilterCooldownsForSite(StringPiece upstream_body, StringPiece own_host,
                            GoogleString* out) {
  out->clear();
  // Fails closed on anything thrown while reading, copying or writing.
  try {
    Json::Value root;
    if (!ParseObject(upstream_body, &root)) {
      return false;
    }
    Json::Value kept(Json::arrayValue);
    const Json::Value& entries = root["cooldowns"];
    if (entries.isArray() && !own_host.empty()) {
      for (const Json::Value& entry : entries) {
        Json::Value site_entry;
        if (SiteCooldownEntry(entry, own_host, &site_entry)) {
          kept.append(std::move(site_entry));
        }
      }
    }
    // Only the keys the console reads; anything else the optimizer may add
    // could name other sites.
    Json::Value site(Json::objectValue);
    site["count"] = Json::Value(static_cast<Json::UInt64>(kept.size()));
    site["cooldowns"] = std::move(kept);
    if (root["enabled"].isBool()) {
      site["enabled"] = root["enabled"];
    }
    *out = WriteCompact(site);
    return true;
  } catch (const std::exception&) {
    out->clear();
    return false;
  }
}

}  // namespace net_instaweb
