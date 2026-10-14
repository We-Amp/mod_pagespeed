// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_SYSTEM_RULE_PACK_ADMIN_VIEW_H_
#define PAGESPEED_SYSTEM_RULE_PACK_ADMIN_VIEW_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net_instaweb {

// The read-only "Rule packs" admin page: what the page shows and the bounded
// read of the decision log behind it. No I/O besides ReadDecisionLogTail().
namespace rule_pack_view {

constexpr size_t kDefaultLimit = 100;
constexpr size_t kMaxLimit = 1000;
// The most bytes of a decision log one request reads.
constexpr size_t kMaxTailBytes = 1 << 20;

// The `limit` query value: kDefaultLimit when absent or not a positive
// number, never more than kMaxLimit.
size_t ClampLimit(std::string_view value);

struct Tail {
  enum class Status { kOk, kMissing, kError };
  Status status = Status::kMissing;
  std::string error;  // for kError
  // The last complete lines, oldest first, at most `limit`, without the
  // newline; empty lines are dropped.
  std::vector<std::string> lines;
};

// The last `limit` lines of the file at `path`, reading at most `max_bytes`
// from its end: the file is never read whole, a line cut by the bound is
// dropped, and a missing or unreadable file is a status, not a failure.
Tail ReadDecisionLogTail(const std::string& path, size_t limit,
                         size_t max_bytes = kMaxTailBytes);

// One parsed decision-log line.
struct Row {
  int64_t ts_ms = 0;
  std::string host, path, rule, kind, mode, action, reason, defect;
  std::string before, after, removed;
  std::string claimed_by;    // the pack that owns the target; empty if none
  std::string pack, sha256;  // "id@version" and the 16-digit hash prefix
  bool header = false;
  std::string raw;  // the line as logged
};

// False for a line that is not a JSON object.
bool ParseRow(std::string_view line, Row* row);

struct Filter {
  std::string rule, action, reason;  // empty matches everything
  // Only rows of this host (case-insensitive); empty matches every host.
  std::string host;
  bool Matches(const Row& row) const;
};

// The licence fields of a pack, shown as text only: nothing reads them to
// decide anything.
struct LicenseInfo {
  bool present = false;
  std::string label, expires, terms;
};

struct PackInfo {
  bool present = false;
  std::string id, version, publisher, sha256;
  // The id of the key a verified signature was made with; empty when the
  // pack was not verified.
  std::string key_id;
  LicenseInfo license;
  std::vector<std::string> warnings;
};

// One RulePackFile of the scope, in load order.
struct PackEntry {
  std::string file;
  // loaded, failed-keeping-earlier, refused, none, or duplicate-id (the file
  // holds a pack whose id an earlier file of the scope already has, so it is
  // not used).
  std::string state;
  int64_t last_event_ms = -1;
  std::string last_error;
  PackInfo pack;
};

struct Page {
  std::string scope;  // "vhost" or "global"
  std::string host;   // the host identifier of the server context
  bool filter_enabled = false;
  std::string pack_file;
  std::string mode;
  std::string state;  // loaded, failed-keeping-earlier, refused, none
  int64_t last_event_ms = -1;
  std::string last_error;
  PackInfo pack;
  bool have_counters = false;
  std::vector<std::pair<std::string, int64_t>> counters;
  bool require_signed = false;  // RulePackRequireSigned
  // Every RulePackFile of the scope, in load order (the fields above describe
  // the first one).
  std::vector<PackEntry> packs;
  // Warnings about kinds that a later pack carries in another mode than the
  // earlier pack that owns them.
  std::vector<std::string> overlap_warnings;
  // The pack whose decision log is shown (?pack=, else the first loaded
  // pack); empty when the logs of the known packs are merged.
  std::string tail_pack;
  bool tail_pack_requested = false;  // ?pack= was given
  int64_t now_ms = 0;                // for generated_at in the JSON
  bool log_off = false;              // LogDir is not set
  bool no_pack_file = false;         // RulePackFile is not set
  std::string log_date;              // UTC day of the log shown
  Tail tail;
  size_t limit = kDefaultLimit;
  Filter filter;
};

// The rows of `page.tail` that pass `page.filter`, newest first, at most
// `page.limit` of them (the limit counts matches, not lines read).
std::vector<Row> SelectRows(const Page& page);

std::string RenderHtml(const Page& page);
// The status of the scope as one JSON document with a "schema" integer:
// snake_case keys, ISO-8601 UTC timestamps, the configuration, every pack in
// load order, the overlap warnings, the counters and the selected decisions
// (newest first). Keys are sorted, the text has no insignificant whitespace.
std::string RenderJson(const Page& page);
// The selected lines as logged, newest first, one per line.
std::string RenderRaw(const Page& page);

std::string HtmlEscape(std::string_view s);

}  // namespace rule_pack_view
}  // namespace net_instaweb

#endif  // PAGESPEED_SYSTEM_RULE_PACK_ADMIN_VIEW_H_
