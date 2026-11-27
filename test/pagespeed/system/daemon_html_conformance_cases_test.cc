// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Schema check for the HTML cache engine and live conformance cases
// (test/system/daemon_html/conformance_cases.json). It proves the file parses
// and that every row has the documented shape with values from the closed
// sets, that row labels stay public-safe, and that every body reference
// resolves. It runs no row: the engine runner and the live runner do.

#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "pagespeed/kernel/base/json.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "test/pagespeed/kernel/base/gtest.h"

namespace net_instaweb {
namespace {

const char kCasesPath[] = "/test/system/daemon_html/conformance_cases.json";

using StringSet = std::set<GoogleString>;

const StringSet kRunners = {"engine", "live"};
const StringSet kPorts = {"apache", "nginx", "iis"};
const StringSet kEngineKeys = {"config", "library", "volume", "verdicts",
                               "steps"};
const StringSet kConfigKeys = {"mode",       "agent_optimize", "max_bytes",
                               "host_names", "budget_bytes",   "sites"};
const StringSet kLibraryKeys = {"minor", "set_bound", "html_mode",
                                "agent_entitled"};
const StringSet kVolumeKinds = {"original", "sidecar", "markdown", "withheld",
                                "epoch"};
const StringSet kStepKinds = {"capture", "serve", "poll_tick", "stats_tick",
                              "admin_purge"};
const StringSet kEngineStepKeys = {"kind",     "pre",    "request",
                                   "response", "during", "expect"};
const StringSet kEngineOps = {
    "purge_url",       "purge_global",    "flush_file",
    "wipe_file_cache", "create_engine",   "advance_clock",
    "make_visible",    "disable_recheck", "budget"};
const StringSet kPoints = {"before_send", "after_send", "after_commit",
                           "after_recheck"};
const StringSet kResponseKeys = {"status",      "origin", "store_delta",
                                 "final_delta", "module", "body_ref",
                                 "aborted"};
const StringSet kEngineExpectKeys = {"calls",           "calls_exclude",
                                     "port_calls",      "stored",
                                     "removed",         "notify",
                                     "counters",        "serve",
                                     "log_lines",       "admin_response",
                                     "purge_set_added", "volume_epoch_s",
                                     "budget_in_use",   "port_calls_exclude"};
const StringSet kServeKinds = {"fall_through", "final"};
const StringSet kModes = {"off", "report-only", "serve-variants"};
const StringSet kLiveKeys = {"mode",  "agent_optimize", "worker",
                             "vhost", "origin",         "steps"};
const StringSet kVhostKnobs = {"header_add",
                               "header_always",
                               "header_early",
                               "expires",
                               "auth",
                               "alias",
                               "default_vhost",
                               "in_place",
                               "pagespeed_off_location",
                               "compress_markdown",
                               "third_party_filter",
                               "merge_slashes",
                               "poll_interval_s",
                               "chained_module_origin", "named_location",
                               "proxy_hide", "trailer"};
const StringSet kOriginKeys = {"status", "headers", "body_ref",
                               "echo_conditionals", "truncate_body"};
const StringSet kLiveStepKeys = {"request",    "pre",    "process",
                                 "background", "expect", "window"};
const StringSet kLiveWindowKeys = {"stale_max_s", "margin_s", "observe_s",
                                   "every_s", "stale"};
const StringSet kLiveRequestKeys = {"method", "path", "host", "headers"};
const StringSet kLiveOps = {"purge_url",      "purge_global", "wait_markdown",
                            "sleep_s",        "origin_set",   "hold_origin",
                            "release_origin", "set_mode",     "wait_origin_held"};
const StringSet kLiveExpectKeys = {"status",         "content_type",
                                   "headers_exact",  "headers_absent",
                                   "stored",         "stored_lifetime_s",
                                   "sidecar_absent", "body_ref",
                                   "body_bytes",     "length_matches_body",
                                   "origin_saw",     "origin_saw_absent",
                                   "counter_delta",  "differential",
                                   "headers_only"};

class Checker {
 public:
  explicit Checker(const Json::Value& bodies) : bodies_(bodies) {}

  void Fail(const GoogleString& where, const GoogleString& what) {
    errors_.push_back(StrCat(where, ": ", what));
  }
  const std::vector<GoogleString>& errors() const { return errors_; }

  bool Keys(const GoogleString& where, const Json::Value& obj,
            const StringSet& allowed, const StringSet& required) {
    if (!obj.isObject()) {
      Fail(where, "not an object");
      return false;
    }
    for (const GoogleString& key : obj.getMemberNames()) {
      if (allowed.count(key) == 0)
        Fail(where, StrCat("unknown key '", key, "'"));
    }
    for (const GoogleString& key : required) {
      if (!obj.isMember(key)) Fail(where, StrCat("missing key '", key, "'"));
    }
    return true;
  }

  void OneOf(const GoogleString& where, const Json::Value& obj, const char* key,
             const StringSet& values) {
    if (!obj.isMember(key)) return;
    if (!obj[key].isString() || values.count(obj[key].asString()) == 0) {
      Fail(where, StrCat(key, " is not one of the allowed values"));
    }
  }

  void SubsetOf(const GoogleString& where, const Json::Value& obj,
                const char* key, const StringSet& values, bool non_empty) {
    if (!obj.isMember(key)) return;
    const Json::Value& list = obj[key];
    if (!list.isArray() || (non_empty && list.empty())) {
      Fail(where, StrCat(key, " is not a non-empty array"));
      return;
    }
    StringSet seen;
    for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
      if (!list[i].isString() || values.count(list[i].asString()) == 0) {
        Fail(where, StrCat(key, " has a value outside its set"));
      } else if (!seen.insert(list[i].asString()).second) {
        Fail(where, StrCat(key, " repeats a value"));
      }
    }
  }

  // Header lists are [[name, value], ...].
  void HeaderList(const GoogleString& where, const Json::Value& obj,
                  const char* key) {
    if (!obj.isMember(key)) return;
    const Json::Value& list = obj[key];
    bool ok = list.isArray();
    for (Json::ArrayIndex i = 0; ok && i < list.size(); ++i) {
      ok = list[i].isArray() && list[i].size() == 2 && list[i][0].isString() &&
           list[i][1].isString() && !list[i][0].asString().empty();
    }
    if (!ok) Fail(where, StrCat(key, " is not a list of [name, value]"));
  }

  void BodyRef(const GoogleString& where, const Json::Value& obj) {
    if (!obj.isMember("body_ref")) return;
    if (!obj["body_ref"].isString() ||
        !bodies_.isMember(obj["body_ref"].asString())) {
      Fail(where, "body_ref names no entry of bodies");
    }
  }

  // Ops are one-key objects whose key is in `ops`.
  void Ops(const GoogleString& where, const Json::Value& list,
           const StringSet& ops) {
    if (!list.isArray() || list.empty()) {
      Fail(where, "ops are not a non-empty array");
      return;
    }
    for (Json::ArrayIndex i = 0; i < list.size(); ++i) {
      const Json::Value& op = list[i];
      if (!op.isObject() || op.size() != 1 ||
          ops.count(op.getMemberNames()[0]) == 0 ||
          !op[op.getMemberNames()[0]].isObject()) {
        Fail(where, "op is not a one-key object naming a known op");
      } else if (op.isMember("origin_set")) {
        BodyRef(where, op["origin_set"]);
      }
    }
  }

  void LibraryFn(const GoogleString& where, const Json::Value& v) {
    static const std::regex kFn("ps_[a-z0-9_]+");
    if (!v.isString() || !std::regex_match(v.asString(), kFn)) {
      Fail(where, "not a library function name");
    }
  }

 private:
  const Json::Value& bodies_;
  std::vector<GoogleString> errors_;
};

void CheckEngine(Checker& c, const GoogleString& w, const Json::Value& e) {
  if (!c.Keys(w, e, kEngineKeys, {"verdicts", "steps"})) return;
  if (e.isMember("config")) c.Keys(w + " config", e["config"], kConfigKeys, {});
  if (e.isMember("library")) {
    c.Keys(w + " library", e["library"], kLibraryKeys, {});
  }
  if (e.isMember("volume")) {
    const Json::Value& vol = e["volume"];
    if (!vol.isArray()) c.Fail(w, "volume is not an array");
    for (Json::ArrayIndex i = 0; vol.isArray() && i < vol.size(); ++i) {
      c.OneOf(w + " volume", vol[i], "kind", kVolumeKinds);
      if (!vol[i].isMember("kind")) c.Fail(w + " volume", "entry has no kind");
      c.BodyRef(w + " volume", vol[i]);
    }
  }
  const Json::Value& verdicts = e["verdicts"];
  if (!verdicts.isArray()) c.Fail(w, "verdicts is not an array");
  for (Json::ArrayIndex i = 0; verdicts.isArray() && i < verdicts.size(); ++i) {
    if (!verdicts[i].isObject()) {
      c.Fail(w + " verdicts", "not an object");
      continue;
    }
    c.LibraryFn(w + " verdicts fn", verdicts[i]["fn"]);
  }
  const Json::Value& steps = e["steps"];
  if (!steps.isArray() || steps.empty()) {
    c.Fail(w, "steps is not a non-empty array");
    return;
  }
  for (Json::ArrayIndex i = 0; i < steps.size(); ++i) {
    const GoogleString sw = StrCat(w, " step ", IntegerToString(i));
    const Json::Value& s = steps[i];
    if (!c.Keys(sw, s, kEngineStepKeys, {"kind", "expect"})) continue;
    c.OneOf(sw, s, "kind", kStepKinds);
    const GoogleString kind = s["kind"].asString();
    if ((kind == "capture" || kind == "serve" || kind == "admin_purge") &&
        !s.isMember("request")) {
      c.Fail(sw, "missing request");
    }
    if (kind == "capture") {
      if (!s.isMember("response")) c.Fail(sw, "missing response");
    } else if (s.isMember("response") || s.isMember("during")) {
      c.Fail(sw, "response and during belong to capture steps");
    }
    if (kind == "admin_purge" && !s["request"]["url"].isString()) {
      c.Fail(sw, "admin_purge request has no url");
    }
    if (s.isMember("pre")) c.Ops(sw + " pre", s["pre"], kEngineOps);
    if (s.isMember("response")) {
      const Json::Value& r = s["response"];
      c.Keys(sw + " response", r, kResponseKeys, {"status", "origin"});
      for (const char* key :
           {"origin", "store_delta", "final_delta", "module"}) {
        c.HeaderList(sw + " response", r, key);
      }
      c.BodyRef(sw + " response", r);
    }
    if (s.isMember("during")) {
      const Json::Value& during = s["during"];
      if (!during.isArray() || during.empty()) c.Fail(sw, "during is empty");
      for (Json::ArrayIndex j = 0; during.isArray() && j < during.size(); ++j) {
        c.Keys(sw + " during", during[j], {"point", "do"}, {"point", "do"});
        c.OneOf(sw + " during", during[j], "point", kPoints);
        if (during[j].isMember("do")) {
          c.Ops(sw + " during do", during[j]["do"], kEngineOps);
        }
      }
    }
    const Json::Value& ex = s["expect"];
    if (!c.Keys(sw + " expect", ex, kEngineExpectKeys, {})) continue;
    if (ex.empty()) c.Fail(sw, "expect asserts nothing");
    if (ex.isMember("calls")) {
      const Json::Value& calls = ex["calls"];
      if (!calls.isArray()) c.Fail(sw, "calls is not an array");
      for (Json::ArrayIndex j = 0; calls.isArray() && j < calls.size(); ++j) {
        c.Keys(sw + " call", calls[j], {"fn", "args", "exact"}, {"fn"});
        c.LibraryFn(sw + " call fn", calls[j]["fn"]);
        if (calls[j].isMember("args") && !calls[j]["args"].isObject()) {
          c.Fail(sw + " call", "args is not an object");
        }
        if (calls[j].isMember("exact")) {
          // Struct prefixes whose every field the call's args must name.
          const Json::Value& exact = calls[j]["exact"];
          if (!exact.isArray() || exact.empty()) {
            c.Fail(sw + " call", "exact is not a list of struct prefixes");
          }
          for (Json::ArrayIndex k = 0; exact.isArray() && k < exact.size();
               ++k) {
            // Only the structs the function's record carries.
            const GoogleString fn = calls[j]["fn"].asString();
            const GoogleString prefix =
                exact[k].isString() ? exact[k].asString() : GoogleString();
            const bool ok =
                (prefix == "request" && (fn == "ps_html_serve_classify" ||
                                         fn == "ps_html_store_classify")) ||
                (prefix == "stored" && fn == "ps_html_serve_classify");
            if (!ok) {
              c.Fail(sw + " call",
                     StrCat("exact ", prefix, " is not recorded for ", fn));
            }
          }
        }
      }
    }
    if (ex.isMember("calls_exclude")) {
      const Json::Value& list = ex["calls_exclude"];
      if (!list.isArray() || list.empty()) c.Fail(sw, "calls_exclude is empty");
      for (Json::ArrayIndex j = 0; list.isArray() && j < list.size(); ++j) {
        c.LibraryFn(sw + " calls_exclude", list[j]);
      }
    }
    if (ex.isMember("serve")) {
      c.Keys(sw + " serve", ex["serve"],
             {"kind", "status", "reason", "headers_exact", "body_bytes"},
             {"kind"});
      c.OneOf(sw + " serve", ex["serve"], "kind", kServeKinds);
    }
    c.OneOf(sw + " expect", ex, "admin_response", {"ok", "error"});
    for (const char* key : {"stored", "removed", "notify", "purge_set_added"}) {
      if (ex.isMember(key) && !ex[key].isArray()) {
        c.Fail(sw, StrCat(key, " is not an array"));
      }
    }
    if (ex.isMember("counters") && !ex["counters"].isObject()) {
      c.Fail(sw, "counters is not an object");
    }
  }
}

void CheckLive(Checker& c, const GoogleString& w, const Json::Value& l) {
  if (!c.Keys(w, l, kLiveKeys, {"mode", "agent_optimize", "origin", "steps"})) {
    return;
  }
  c.OneOf(w, l, "mode", kModes);
  if (l.isMember("vhost")) c.Keys(w + " vhost", l["vhost"], kVhostKnobs, {});
  if (l.isMember("worker")) {
    c.Keys(w + " worker", l["worker"], {"html_mode", "agent_optimize"}, {});
  }
  const Json::Value& origin = l["origin"];
  if (c.Keys(w + " origin", origin, kOriginKeys, {"status", "headers"})) {
    c.HeaderList(w + " origin", origin, "headers");
    c.BodyRef(w + " origin", origin);
  }
  const Json::Value& steps = l["steps"];
  if (!steps.isArray() || steps.empty()) {
    c.Fail(w, "steps is not a non-empty array");
    return;
  }
  for (Json::ArrayIndex i = 0; i < steps.size(); ++i) {
    const GoogleString sw = StrCat(w, " step ", IntegerToString(i));
    const Json::Value& s = steps[i];
    if (!c.Keys(sw, s, kLiveStepKeys, {"request", "expect"})) continue;
    if (c.Keys(sw + " request", s["request"], kLiveRequestKeys,
               {"method", "path", "headers"})) {
      c.HeaderList(sw + " request", s["request"], "headers");
    }
    if (s.isMember("pre")) c.Ops(sw + " pre", s["pre"], kLiveOps);
    if (s.isMember("window") &&
        c.Keys(sw + " window", s["window"], kLiveWindowKeys, kLiveWindowKeys)) {
      c.Keys(sw + " window stale", s["window"]["stale"], {"content_type"},
             {"content_type"});
    }
    const Json::Value& ex = s["expect"];
    if (!c.Keys(sw + " expect", ex, kLiveExpectKeys, {})) continue;
    if (ex.empty()) c.Fail(sw, "expect asserts nothing");
    c.BodyRef(sw + " expect", ex);
    if (ex.isMember("headers_exact")) {
      const Json::Value& he = ex["headers_exact"];
      if (!he.isObject()) c.Fail(sw, "headers_exact is not an object");
      for (const GoogleString& name : he.getMemberNames()) {
        c.Keys(StrCat(sw, " headers_exact ", name), he[name],
               {"count", "value", "contains"}, {"count"});
        if (!he[name]["count"].isUInt()) {
          c.Fail(StrCat(sw, " headers_exact ", name), "count is not a count");
        }
      }
    }
    if (ex.isMember("differential")) {
      c.Keys(sw + " differential", ex["differential"],
             {"against", "allow_vary_accept"},
             {"against", "allow_vary_accept"});
      c.SubsetOf(sw + " differential", ex["differential"], "against", kModes,
                 true);
    }
  }
}

// A label names an RFC section or a short rule; it never carries a project
// tracking identifier.
bool LabelIsPublicSafe(const GoogleString& why) {
  static const std::regex kForbidden(
      "(^|[^A-Za-z0-9])([SVGDCKFO][0-9]+|[A-Z]{2,6}-[0-9]+|[A-Z]-[A-Z]|"
      "ADR|[Cc]onstraint|[Rr]uling|[Cc]ard)([^A-Za-z0-9]|$)");
  return !why.empty() && why.size() <= 120 &&
         !std::regex_search(why, kForbidden);
}

Json::Value LoadCases() {
  std::ifstream in(StrCat(GTestSrcDir(), kCasesPath).c_str(), std::ios::binary);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const GoogleString contents = buffer.str();
  EXPECT_FALSE(contents.empty()) << kCasesPath;
  Json::CharReaderBuilder builder;
  Json::CharReaderBuilder::strictMode(&builder.settings_);
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value root;
  GoogleString error;
  EXPECT_TRUE(reader->parse(contents.data(), contents.data() + contents.size(),
                            &root, &error))
      << error;
  return root;
}

TEST(DaemonHtmlConformanceCasesTest, EveryRowHasTheDocumentedShape) {
  const Json::Value doc = LoadCases();
  ASSERT_TRUE(doc.isObject());
  Checker c(doc["bodies"]);
  c.Keys("file", doc, {"format", "version", "about", "bodies", "rows"},
         {"format", "version", "about", "bodies", "rows"});
  EXPECT_EQ("daemon-html-conformance-cases", doc["format"].asString());
  EXPECT_EQ(1, doc["version"].asInt());
  ASSERT_TRUE(doc["bodies"].isObject());
  for (const GoogleString& name : doc["bodies"].getMemberNames()) {
    EXPECT_TRUE(doc["bodies"][name].isString()) << name;
  }
  const Json::Value& rows = doc["rows"];
  ASSERT_TRUE(rows.isArray());
  ASSERT_FALSE(rows.empty());

  static const std::regex kId("[a-z0-9][a-z0-9-]*");
  StringSet ids;
  int engine_rows = 0;
  int live_rows = 0;
  for (Json::ArrayIndex i = 0; i < rows.size(); ++i) {
    const Json::Value& row = rows[i];
    const GoogleString id = row["id"].isString() ? row["id"].asString() : "";
    const GoogleString w = StrCat("row '", id, "'");
    if (!c.Keys(w, row, {"id", "runners", "why", "ports", "engine", "live"},
                {"id", "runners", "why"})) {
      continue;
    }
    if (!std::regex_match(id, kId)) c.Fail(w, "id is not [a-z0-9-]");
    if (!ids.insert(id).second) c.Fail(w, "duplicate id");
    if (!row["why"].isString() || !LabelIsPublicSafe(row["why"].asString())) {
      c.Fail(w, "why is missing, too long, or not a public label");
    }
    c.SubsetOf(w, row, "runners", kRunners, true);
    c.SubsetOf(w, row, "ports", kPorts, true);
    const Json::Value& runners = row["runners"];
    StringSet named;
    for (Json::ArrayIndex j = 0; runners.isArray() && j < runners.size(); ++j) {
      named.insert(runners[j].asString());
    }
    for (const GoogleString& runner : kRunners) {
      if ((named.count(runner) != 0) != row.isMember(runner)) {
        c.Fail(w, StrCat("needs a ", runner,
                         " body exactly when runners names it"));
      }
    }
    if (row.isMember("engine")) {
      ++engine_rows;
      CheckEngine(c, w + " engine", row["engine"]);
    }
    if (row.isMember("live")) {
      ++live_rows;
      CheckLive(c, w + " live", row["live"]);
    }
  }
  for (const GoogleString& e : c.errors()) ADD_FAILURE() << e;
  EXPECT_GT(engine_rows, 0);
  EXPECT_GT(live_rows, 0);
}

TEST(DaemonHtmlConformanceCasesTest, RejectsMalformedRows) {
  Json::Value bodies(Json::objectValue);
  bodies["page"] = "<html></html>";
  Checker c(bodies);
  Json::Value live(Json::objectValue);
  live["mode"] = "serve-cached-html";  // not an M1 mode
  live["agent_optimize"] = true;
  live["origin"]["status"] = 200;
  live["origin"]["headers"] = Json::Value(Json::arrayValue);
  live["origin"]["body_ref"] = "missing";
  live["vhost"]["header_set_everything"] = true;
  live["steps"] = Json::Value(Json::arrayValue);
  CheckLive(c, "bad", live);
  // Bad mode, unknown knob, unresolved body, empty steps.
  EXPECT_EQ(4u, c.errors().size());

  EXPECT_FALSE(LabelIsPublicSafe("store: rule S99"));
  EXPECT_FALSE(LabelIsPublicSafe("per constraint 99"));
  EXPECT_FALSE(LabelIsPublicSafe("tracked as XYZ-99"));
  EXPECT_FALSE(LabelIsPublicSafe("an X-Y event"));
  EXPECT_TRUE(LabelIsPublicSafe("validator: 304 set (RFC 9110 15.4.5)"));
}

// An exact prefix the function's record does not carry would assert
// nothing.
TEST(DaemonHtmlConformanceCasesTest, RejectsAnExactStructTheCallDoesNotRecord) {
  Json::Value bodies(Json::objectValue);
  Checker c(bodies);
  Json::Value engine(Json::objectValue);
  engine["verdicts"] = Json::Value(Json::arrayValue);
  Json::Value step(Json::objectValue);
  step["kind"] = "serve";
  Json::Value call(Json::objectValue);
  call["fn"] = "ps_html_store_classify";
  call["exact"].append("stored");
  step["expect"]["calls"].append(call);
  call["fn"] = "ps_html_serve_classify";
  call["exact"][0] = "request";
  call["exact"].append("stored");
  step["expect"]["calls"].append(call);
  engine["steps"].append(step);
  CheckEngine(c, "row", engine);
  // Only the store classifier's "stored" is refused; the serve
  // classifier records both structs.
  std::vector<GoogleString> refused;
  for (const GoogleString& e : c.errors()) {
    if (e.find("is not recorded for") != GoogleString::npos) {
      refused.push_back(e);
    }
  }
  ASSERT_EQ(1u, refused.size()) << JoinCollection(c.errors(), "\n");
  EXPECT_NE(GoogleString::npos,
            refused[0].find(
                "exact stored is not recorded for ps_html_store_classify"))
      << refused[0];
}

}  // namespace
}  // namespace net_instaweb
