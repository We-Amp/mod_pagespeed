// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include "pagespeed/system/rule_pack_admin_view.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include "pagespeed/kernel/base/json.h"

namespace net_instaweb {
namespace rule_pack_view {

namespace {

std::filesystem::path Utf8Path(const std::string& s) {
  return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string JsonString(const Json::Value& object, const char* key) {
  const Json::Value& v = object[key];
  return v.isString() ? v.asString() : std::string();
}

// "YYYY-MM-DD HH:MM:SS" of `ms`, in local time or UTC.
std::string FormatTime(int64_t ms, bool utc) {
  const std::time_t secs = static_cast<std::time_t>(ms / 1000);
  std::tm tm_buf{};
#ifdef _WIN32
  if (utc) {
    gmtime_s(&tm_buf, &secs);
  } else {
    localtime_s(&tm_buf, &secs);
  }
#else
  if (utc) {
    gmtime_r(&secs, &tm_buf);
  } else {
    localtime_r(&secs, &tm_buf);
  }
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
  return buf;
}

// "YYYY-MM-DDTHH:MM:SSZ" of `ms`.
std::string FormatIso(int64_t ms) {
  const std::string t = FormatTime(ms, true);
  return t.substr(0, 10) + "T" + t.substr(11) + "Z";
}

std::string LicenseText(const LicenseInfo& license) {
  std::string text = license.label;
  if (!license.expires.empty()) {
    if (!text.empty()) text += ", ";
    text += "expires " + license.expires;
  }
  if (!license.terms.empty()) {
    if (!text.empty()) text += ", ";
    text += license.terms;
  }
  return text;
}

Json::Value StringOrNull(const std::string& s) {
  return s.empty() ? Json::Value() : Json::Value(s);
}

std::string UrlEncode(std::string_view s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

void Cell(std::string* out, std::string_view value, std::string_view cls = "") {
  out->append("<td");
  if (!cls.empty()) {
    out->append(" class=\"");
    out->append(cls);
    out->append("\"");
  }
  out->append(">");
  out->append(HtmlEscape(value));
  out->append("</td>");
}

void KeyValue(std::string* out, std::string_view key, std::string_view value,
              std::string_view title = "") {
  out->append("<tr><th>");
  out->append(HtmlEscape(key));
  out->append("</th><td");
  if (!title.empty()) {
    out->append(" title=\"");
    out->append(HtmlEscape(title));
    out->append("\"");
  }
  out->append(">");
  out->append(HtmlEscape(value));
  out->append("</td></tr>\n");
}

// The query string that keeps the current filters.
std::string FilterQuery(const Page& page) {
  std::string q = "limit=" + std::to_string(page.limit);
  if (!page.filter.rule.empty()) q += "&rule=" + UrlEncode(page.filter.rule);
  if (!page.filter.action.empty()) {
    q += "&action=" + UrlEncode(page.filter.action);
  }
  if (!page.filter.reason.empty()) {
    q += "&reason=" + UrlEncode(page.filter.reason);
  }
  if (page.tail_pack_requested) q += "&pack=" + UrlEncode(page.tail_pack);
  return q;
}

void AppendLoadedPack(const Page& page, std::string* out) {
  std::string& o = *out;
  o.append("<h2>Loaded pack</h2>\n<table>\n");
  KeyValue(&o, "State",
           page.state == "none" && !page.pack_file.empty()
               ? "not yet loaded in this process (the log below is shared by "
                 "all processes)"
               : page.state);
  if (page.pack.present) {
    KeyValue(&o, "Id", page.pack.id);
    KeyValue(&o, "Version", page.pack.version);
    KeyValue(&o, "Publisher", page.pack.publisher);
    KeyValue(&o, "SHA-256", page.pack.sha256.substr(0, 16), page.pack.sha256);
    if (!page.pack.key_id.empty()) {
      KeyValue(&o, "Verified with key", page.pack.key_id);
    }
    if (page.pack.license.present) {
      KeyValue(&o, "License", LicenseText(page.pack.license));
    }
  }
  if (page.last_event_ms >= 0) {
    KeyValue(&o, "Last load event", FormatTime(page.last_event_ms, false),
             FormatTime(page.last_event_ms, true) + " UTC");
  }
  if (!page.last_error.empty())
    KeyValue(&o, "Last load error", page.last_error);
  for (const std::string& w : page.pack.warnings) {
    KeyValue(&o, "Warning", w);
  }
  o.append("</table>\n");
}

// The packs of the scope in load order, with the overlap warnings.
void AppendPackTable(const Page& page, std::string* out) {
  std::string& o = *out;
  o.append(
      "<h2>Packs (load order)</h2>\n<table>\n<tr><th>#</th><th>id</th>"
      "<th>version</th><th>publisher</th><th>key</th><th>sha256</th>"
      "<th>file</th><th>state</th><th>last load event</th><th>last error</th>"
      "<th>license</th><th>warnings</th><th>decisions</th></tr>\n");
  size_t position = 0;
  for (const PackEntry& entry : page.packs) {
    o.append("<tr>");
    Cell(&o, std::to_string(++position));
    Cell(&o, entry.pack.present ? entry.pack.id : "");
    Cell(&o, entry.pack.version);
    Cell(&o, entry.pack.publisher);
    Cell(&o, entry.pack.key_id);
    o.append("<td class=\"m\" title=\"");
    o.append(HtmlEscape(entry.pack.sha256));
    o.append("\">");
    o.append(HtmlEscape(entry.pack.sha256.substr(0, 16)));
    o.append("</td>");
    Cell(&o, entry.file, "m");
    Cell(&o, entry.state);
    Cell(&o, entry.last_event_ms >= 0
                 ? FormatTime(entry.last_event_ms, true) + " UTC"
                 : std::string());
    Cell(&o, entry.last_error);
    Cell(&o, LicenseText(entry.pack.license));
    std::string warnings;
    for (const std::string& w : entry.pack.warnings) {
      if (!warnings.empty()) warnings += "; ";
      warnings += w;
    }
    Cell(&o, warnings);
    o.append("<td>");
    if (entry.pack.present) {
      o.append("<a href=\"?pack=");
      o.append(HtmlEscape(UrlEncode(entry.pack.id)));
      o.append("\">log</a>");
    }
    o.append("</td></tr>\n");
  }
  o.append("</table>\n");
  if (!page.overlap_warnings.empty()) {
    o.append("<h3>Overlaps</h3>\n<table>\n");
    for (const std::string& w : page.overlap_warnings) {
      KeyValue(&o, "Warning", w);
    }
    o.append("</table>\n");
  }
}

}  // namespace

std::string HtmlEscape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out.append("&amp;");
        break;
      case '<':
        out.append("&lt;");
        break;
      case '>':
        out.append("&gt;");
        break;
      case '"':
        out.append("&quot;");
        break;
      case '\'':
        out.append("&#39;");
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

size_t ClampLimit(std::string_view value) {
  if (value.empty()) return kDefaultLimit;
  for (char c : value) {
    if (c < '0' || c > '9') return kDefaultLimit;
  }
  if (value.size() > 9) return kMaxLimit;
  size_t n = 0;
  for (char c : value) n = n * 10 + static_cast<size_t>(c - '0');
  if (n == 0) return kDefaultLimit;
  return std::min(n, kMaxLimit);
}

Tail ReadDecisionLogTail(const std::string& path, size_t limit,
                         size_t max_bytes) {
  Tail tail;
  std::error_code ec;
  const std::filesystem::path file = Utf8Path(path);
  const std::uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec) {
    if (ec == std::errc::no_such_file_or_directory ||
        ec == std::errc::not_a_directory) {
      tail.status = Tail::Status::kMissing;
    } else {
      tail.status = Tail::Status::kError;
      tail.error = ec.message();
    }
    return tail;
  }
  tail.status = Tail::Status::kOk;
  if (size == 0 || limit == 0 || max_bytes == 0) return tail;

  // One byte before the window is read too: when it is a newline the
  // window starts on a line boundary and its first line is whole.
  const std::uintmax_t start = size > max_bytes ? size - max_bytes : 0;
  const std::uintmax_t from = start > 0 ? start - 1 : 0;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    tail.status = Tail::Status::kError;
    tail.error = "cannot be opened";
    return tail;
  }
  in.seekg(static_cast<std::streamoff>(from));
  std::string data(static_cast<size_t>(size - from), '\0');
  in.read(data.data(), static_cast<std::streamsize>(data.size()));
  if (in.bad()) {
    tail.status = Tail::Status::kError;
    tail.error = "cannot be read";
    return tail;
  }
  data.resize(static_cast<size_t>(in.gcount()));

  size_t pos = 0;
  if (start > 0) {
    // Drop the line the window starts in the middle of.
    const size_t nl = data.find('\n');
    if (nl == std::string::npos) return tail;
    pos = nl + 1;
  }
  std::vector<std::string> lines;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    if (nl > pos) lines.emplace_back(data, pos, nl - pos);
    pos = nl + 1;
  }
  if (lines.size() > limit) {
    lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(
                                                   lines.size() - limit));
  }
  tail.lines = std::move(lines);
  return tail;
}

bool ParseRow(std::string_view line, Row* row) {
  Json::CharReaderBuilder builder;
  builder["stackLimit"] = 20;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  std::string errors;
  if (!reader->parse(line.data(), line.data() + line.size(), &root, &errors) ||
      !root.isObject()) {
    return false;
  }
  Row r;
  const Json::Value& ts = root["ts"];
  r.ts_ms = ts.isIntegral() ? static_cast<int64_t>(ts.asInt64()) : 0;
  r.host = JsonString(root, "host");
  r.path = JsonString(root, "path");
  r.rule = JsonString(root, "rule");
  r.kind = JsonString(root, "kind");
  r.mode = JsonString(root, "mode");
  r.action = JsonString(root, "action");
  r.reason = JsonString(root, "reason");
  r.defect = JsonString(root, "defect");
  r.before = JsonString(root, "before");
  r.after = JsonString(root, "after");
  r.removed = JsonString(root, "removed");
  r.claimed_by = JsonString(root, "claimed_by");
  r.pack = JsonString(root, "pack");
  r.sha256 = JsonString(root, "sha256");
  r.header = root["header"].isBool() && root["header"].asBool();
  r.raw = std::string(line);
  *row = std::move(r);
  return true;
}

bool Filter::Matches(const Row& row) const {
  if (!host.empty()) {
    if (host.size() != row.host.size()) return false;
    for (size_t i = 0; i < host.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(host[i])) !=
          std::tolower(static_cast<unsigned char>(row.host[i]))) {
        return false;
      }
    }
  }
  return (rule.empty() || row.rule == rule) &&
         (action.empty() || row.action == action) &&
         (reason.empty() || row.reason == reason);
}

std::vector<Row> SelectRows(const Page& page) {
  std::vector<Row> rows;
  for (auto it = page.tail.lines.rbegin(); it != page.tail.lines.rend(); ++it) {
    Row row;
    if (ParseRow(*it, &row) && page.filter.Matches(row)) {
      rows.push_back(std::move(row));
      if (rows.size() >= page.limit) break;
    }
  }
  return rows;
}

std::string RenderRaw(const Page& page) {
  std::string out;
  for (const Row& row : SelectRows(page)) {
    out.append(row.raw);
    out.push_back('\n');
  }
  return out;
}

std::string RenderJson(const Page& page) {
  Json::Value root(Json::objectValue);
  root["schema"] = 1;
  root["scope"] = page.scope;
  root["host"] = page.host;
  root["generated_at"] = FormatIso(page.now_ms);

  Json::Value config(Json::objectValue);
  config["filter_enabled"] = page.filter_enabled;
  Json::Value files(Json::arrayValue);
  for (const PackEntry& entry : page.packs) files.append(entry.file);
  config["rule_pack_files"] = files;
  config["rule_pack_mode"] = page.mode;
  config["rule_pack_require_signed"] = page.require_signed;
  root["configuration"] = config;

  Json::Value packs(Json::arrayValue);
  Json::UInt position = 0;
  for (const PackEntry& entry : page.packs) {
    Json::Value p(Json::objectValue);
    p["position"] = position++;
    p["file"] = entry.file;
    p["state"] = entry.state;
    p["id"] = entry.pack.present ? Json::Value(entry.pack.id) : Json::Value();
    p["version"] =
        entry.pack.present ? Json::Value(entry.pack.version) : Json::Value();
    p["publisher"] =
        entry.pack.present ? Json::Value(entry.pack.publisher) : Json::Value();
    p["key_id"] = StringOrNull(entry.pack.key_id);
    p["sha256"] = StringOrNull(entry.pack.sha256);
    p["last_event_at"] = entry.last_event_ms >= 0
                             ? Json::Value(FormatIso(entry.last_event_ms))
                             : Json::Value();
    p["last_error"] = StringOrNull(entry.last_error);
    Json::Value warnings(Json::arrayValue);
    for (const std::string& w : entry.pack.warnings) warnings.append(w);
    p["warnings"] = warnings;
    if (entry.pack.license.present) {
      Json::Value license(Json::objectValue);
      license["label"] = entry.pack.license.label;
      license["expires"] = StringOrNull(entry.pack.license.expires);
      license["terms"] = StringOrNull(entry.pack.license.terms);
      p["license"] = license;
    } else {
      p["license"] = Json::Value();
    }
    packs.append(p);
  }
  root["packs"] = packs;

  Json::Value overlaps(Json::arrayValue);
  for (const std::string& w : page.overlap_warnings) overlaps.append(w);
  root["overlap_warnings"] = overlaps;

  if (page.have_counters) {
    Json::Value counters(Json::objectValue);
    for (const auto& [name, value] : page.counters) {
      counters[name] = static_cast<Json::Int64>(value);
    }
    root["counters"] = counters;
  } else {
    root["counters"] = Json::Value();
  }

  Json::Value log(Json::objectValue);
  log["date"] = page.log_date;
  log["pack"] = StringOrNull(page.tail_pack);
  log["limit"] = static_cast<Json::UInt64>(page.limit);
  const char* status = "ok";
  if (page.log_off) {
    status = "log_dir_not_set";
  } else if (page.no_pack_file) {
    status = "no_pack_file";
  } else if (page.tail.status == Tail::Status::kError) {
    status = "error";
  } else if (page.tail.status == Tail::Status::kMissing) {
    status = "missing";
  }
  log["status"] = status;
  log["error"] =
      StringOrNull(page.tail.status == Tail::Status::kError ? page.tail.error
                                                            : std::string());
  Json::Value rows(Json::arrayValue);
  if (!page.log_off && !page.no_pack_file) {
    for (const Row& row : SelectRows(page)) {
      Json::Value r(Json::objectValue);
      r["at"] = FormatIso(row.ts_ms);
      r["ts_ms"] = static_cast<Json::Int64>(row.ts_ms);
      const size_t at = row.pack.rfind('@');
      r["pack_id"] =
          at == std::string::npos ? row.pack : row.pack.substr(0, at);
      r["pack_version"] =
          at == std::string::npos ? std::string() : row.pack.substr(at + 1);
      r["sha256_prefix"] = row.sha256;
      r["host"] = row.host;
      r["path"] = row.path;
      r["rule"] = row.rule;
      r["kind"] = row.kind;
      r["mode"] = row.mode;
      r["action"] = row.action;
      r["reason"] = row.reason;
      r["defect"] = row.defect;
      r["before"] = row.before;
      r["after"] = row.after;
      r["claimed_by"] = StringOrNull(row.claimed_by);
      r["removed"] = row.removed;
      r["header"] = row.header;
      rows.append(r);
    }
  }
  log["rows"] = rows;
  root["decisions"] = log;

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  builder["precision"] = 17;
  return Json::writeString(builder, root) + "\n";
}

std::string RenderHtml(const Page& page) {
  std::string o;
  o.append(
      "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
      "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
      "<title>Rule packs</title><style>"
      "body{font:14px/1.4 system-ui,sans-serif;margin:1.5rem;color:#1b1f23}"
      "nav a{margin-right:1rem}nav b{margin-right:1rem}"
      "table{border-collapse:collapse;margin:.5rem 0 1.5rem}"
      "th,td{border:1px solid #ccd;padding:.2rem .5rem;text-align:left;"
      "vertical-align:top}th{background:#f1f3f5}"
      "td.m{font-family:ui-monospace,monospace;word-break:break-all}"
      "form label{margin-right:1rem}.note{color:#555}"
      "@media(prefers-color-scheme:dark){body{background:#15171a;"
      "color:#e6e8eb}th{background:#23272d}th,td{border-color:#454b54}"
      "a{color:#8ab4f8}.note{color:#aab}}"
      "</style></head><body>\n"
      "<nav><a href=\"./\">Console</a><a href=\"./config\">Config</a>"
      "<b>Rule packs</b></nav>\n<h1>Rule packs</h1>\n");
  o.append("<p class=\"note\">Read-only. Scope: ");
  o.append(HtmlEscape(page.scope));
  if (!page.host.empty()) {
    o.append(", ");
    o.append(HtmlEscape(page.host));
  }
  o.append(
      ". The pack state is this server process's; the decision log is "
      "shared by all processes.");
  if (page.scope == "vhost") o.append(" Only rows of this host are shown.");
  o.append("</p>\n");

  o.append("<h2>Configuration</h2>\n<table>\n");
  KeyValue(&o, "rule_packs filter", page.filter_enabled ? "enabled" : "off");
  if (page.packs.size() > 1) {
    for (const PackEntry& entry : page.packs) {
      KeyValue(&o, "RulePackFile", entry.file);
    }
  } else {
    KeyValue(&o, "RulePackFile",
             page.pack_file.empty() ? "(not set)" : page.pack_file);
  }
  KeyValue(&o, "RulePackMode", page.mode);
  KeyValue(&o, "RulePackRequireSigned", page.require_signed ? "on" : "off");
  o.append("</table>\n");

  if (page.packs.size() > 1) {
    AppendPackTable(page, &o);
  } else {
    AppendLoadedPack(page, &o);
  }
  // One static anchor: the server makes no request and sends nothing.
  o.append(
      "<p class=\"note\">A pack is a set of rules this server "
      "evaluates. "
      "<a href=\"https://modpagespeed.com/packs/");
  o.append(
      "\">Available packs: modpagespeed.com/packs</a> | "
      "<a href=\"https://modpagespeed.com/docs/rule-packs/\">Rule packs "
      "documentation</a></p>\n");

  o.append("<h2>Counters</h2>\n");
  if (!page.have_counters) {
    o.append("<p class=\"note\">No statistics are kept.</p>\n");
  } else {
    o.append("<table>\n");
    for (const auto& [name, value] : page.counters) {
      KeyValue(&o, name, std::to_string(value));
    }
    o.append("</table>\n");
  }

  o.append("<h2>Decisions today (UTC ");
  o.append(HtmlEscape(page.log_date));
  o.append(")</h2>\n");
  if (page.packs.size() > 1 && !page.tail_pack.empty()) {
    o.append("<p class=\"note\">Decisions of pack ");
    o.append(HtmlEscape(page.tail_pack));
    o.append(" (choose another with ?pack=&lt;id&gt;).</p>\n");
  }
  if (page.log_off) {
    o.append(
        "<p class=\"note\">The decision log is off: LogDir is not set.</p>\n");
  } else if (page.no_pack_file) {
    o.append(
        "<p class=\"note\">RulePackFile is not set, so there is no decision "
        "log.</p>\n");
  } else if (page.tail.status == Tail::Status::kError) {
    o.append("<p class=\"note\">The decision log cannot be read: ");
    o.append(HtmlEscape(page.tail.error));
    o.append("</p>\n");
  } else if (page.tail.status == Tail::Status::kMissing ||
             page.tail.lines.empty()) {
    o.append("<p class=\"note\">No decisions logged today.</p>\n");
  } else {
    const std::vector<Row> rows = SelectRows(page);
    o.append(
        "<form method=\"get\" action=\"\"><label>rule <input name=\"rule\" "
        "value=\"");
    o.append(HtmlEscape(page.filter.rule));
    o.append("\"></label><label>action <input name=\"action\" value=\"");
    o.append(HtmlEscape(page.filter.action));
    o.append("\"></label><label>reason <input name=\"reason\" value=\"");
    o.append(HtmlEscape(page.filter.reason));
    o.append(
        "\"></label><label>limit <input name=\"limit\" size=\"5\" "
        "value=\"");
    o.append(std::to_string(page.limit));
    o.append("\"></label>");
    if (page.tail_pack_requested) {
      o.append("<input type=\"hidden\" name=\"pack\" value=\"");
      o.append(HtmlEscape(page.tail_pack));
      o.append("\">");
    }
    o.append("<button type=\"submit\">Filter</button></form>\n");
    o.append("<p class=\"note\">");
    o.append(std::to_string(rows.size()));
    o.append(" matching, newest first (the last ");
    o.append(std::to_string(page.tail.lines.size()));
    o.append(" lines, at most 1 MiB, are read). ");
    o.append("<a href=\"?raw=1&amp;");
    o.append(HtmlEscape(FilterQuery(page)));
    o.append(
        "\">raw</a></p>\n<table>\n<tr><th>ts</th><th>pack</th><th>host</"
        "th><th>path</th>"
        "<th>rule</th><th>kind</th><th>mode</th><th>action</th>"
        "<th>reason</th><th>defect</th><th>before</th><th>after</th>"
        "<th>removed</th><th>header</th></tr>\n");
    for (const Row& r : rows) {
      o.append("<tr><td title=\"");
      o.append(HtmlEscape(FormatTime(r.ts_ms, true) + " UTC"));
      o.append("\">");
      o.append(HtmlEscape(FormatTime(r.ts_ms, false)));
      o.append("</td>");
      o.append("<td title=\"sha256 ");
      o.append(HtmlEscape(r.sha256));
      o.append("\">");
      o.append(HtmlEscape(r.pack));
      o.append("</td>");
      Cell(&o, r.host, "m");
      Cell(&o, r.path, "m");
      Cell(&o, r.rule);
      Cell(&o, r.kind);
      Cell(&o, r.mode);
      Cell(&o, r.action);
      Cell(&o, r.reason);
      Cell(&o, r.defect);
      Cell(&o, r.before, "m");
      Cell(&o, r.after, "m");
      Cell(&o, r.removed, "m");
      Cell(&o, r.header ? "yes" : "");
      o.append("</tr>\n");
    }
    o.append("</table>\n");
  }
  o.append("</body></html>\n");
  return o;
}

}  // namespace rule_pack_view
}  // namespace net_instaweb
