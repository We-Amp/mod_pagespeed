// SPDX-License-Identifier: Apache-2.0
//
// Spike prototype of an offline mod_pagespeed config linter (Apache first).
// Throwaway code: answers "can a standalone binary check an Apache config
// through the real option parser, without httpd, deterministically, without
// echoing secrets?".
//
// usage:
//   pagespeed_lint_spike [--root DIR] [--htaccess FILE]... [CONFIG]
//   pagespeed_lint_spike --list-directives
// Paths are relative to --root (default "."); absolute paths in Include are
// re-rooted under --root.  Output is JSON on stdout.
// Exit: 0 no error findings, 1 error findings, 2 usage/unreadable, 3 internal.

#include <openssl/sha.h>

#include <algorithm>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "base/logging.h"
#include "net/instaweb/public/version.h"
#include "spdlog/spdlog.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/domain_registry.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/tools/lint_spike/apache_scope.h"

namespace net_instaweb {
namespace lint {
namespace {

#include "pagespeed/tools/lint_spike/safe_option_names.inc"

namespace fs = std::filesystem;

// UTF-8 bytes <-> path, the same on every platform (no locale code page).
std::string U8(const fs::path& p) {
  std::u8string s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}
fs::path FromU8(const std::string& s) {
  return fs::path(std::u8string(s.begin(), s.end()));
}

const char kToolVersion[] = "0.0.0-spike";
const char kMasked[] = "<masked>";

// ---------------------------------------------------------------------------
// Messages from the real parser are captured, never printed directly.
class CaptureHandler : public MessageHandler {
 public:
  std::vector<std::pair<MessageType, GoogleString>> messages;

 protected:
  void MessageSImpl(MessageType type, const GoogleString& message) override {
    messages.emplace_back(type, message);
  }
  void FileMessageSImpl(MessageType type, const char* filename, int line,
                        const GoogleString& message) override {
    messages.emplace_back(type, message);
  }
};

// The module's LOG() goes to spdlog's default logger, which writes to stdout
// with a timestamp, and some messages quote option values.  The lint turns
// that logger off and only counts what the parser logged per directive.
class CountingLogSink : public pagespeed_logging::LogSink {
 public:
  int count = 0;
  void send(int severity, const char*, const char*, int, const char*,
            size_t) override {
    if (severity >= 2 /* ERROR */) ++count;
  }
};
CountingLogSink* log_sink = nullptr;

// ---------------------------------------------------------------------------
struct Finding {
  GoogleString rule;
  GoogleString severity;  // error | warning | info
  GoogleString file;
  int line = 0;
  GoogleString scope;
  GoogleString directive;
  std::vector<GoogleString> args;  // already masked
  GoogleString message;
};

struct Input {
  GoogleString path;
  GoogleString sha256;
};

std::vector<Finding> findings;
std::vector<Input> inputs;

GoogleString HexSha256(const std::string& data) {
  unsigned char d[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), d);
  static const char kHex[] = "0123456789abcdef";
  GoogleString out;
  for (unsigned char c : d) {
    out.push_back(kHex[c >> 4]);
    out.push_back(kHex[c & 15]);
  }
  return out;
}

// Every byte outside printable ASCII is \u00XX-escaped: output is plain
// ASCII, so it carries no terminal escape sequence or control character.
void JsonString(const GoogleString& s, GoogleString* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out->push_back('\\');
      out->push_back(c);
    } else if (c < 0x20 || c >= 0x7f) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out->append(buf);
    } else {
      out->push_back(c);
    }
  }
  out->push_back('"');
}

// ---------------------------------------------------------------------------
// Masking: the console's allowlist, generated at build time.
struct CaseLess {
  bool operator()(const GoogleString& a, const GoogleString& b) const {
    return StringCaseCompare(a, b) < 0;
  }
};
std::set<GoogleString, CaseLess>* safe_names = nullptr;

bool show_values = false;  // --show-values, only when stdout is a terminal

bool ValuesAreSafe(StringPiece directive) {
  if (show_values) return true;
  StringPiece prefix("ModPagespeed");
  if (!StringCaseStartsWith(directive, prefix)) return false;
  StringPiece option = directive.substr(prefix.size());
  if (option.empty()) return false;
  return safe_names->count(option.as_string()) > 0;
}

// ---------------------------------------------------------------------------
// Apache config tokenizer (ap_getword_conf).
bool GetWord(const GoogleString& line, size_t* pos, GoogleString* word) {
  size_t i = *pos;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size()) {
    *pos = i;
    return false;
  }
  word->clear();
  char q = line[i];
  if (q == '"' || q == '\'') {
    ++i;
    while (i < line.size() && line[i] != q) {
      if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == q) ++i;
      word->push_back(line[i]);
      ++i;
    }
    if (i < line.size()) ++i;
  } else {
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
      word->push_back(line[i]);
      ++i;
    }
  }
  *pos = i;
  return true;
}

std::vector<GoogleString> Words(const GoogleString& s) {
  std::vector<GoogleString> out;
  size_t pos = 0;
  GoogleString w;
  while (GetWord(s, &pos, &w)) out.push_back(w);
  return out;
}

// ---------------------------------------------------------------------------
// Paths.  The logical path (what the config says, root-relative, '/'
// separated, case as written) is what is reported.  Resolution matches each
// component against the directory listing byte-for-byte first and falls back
// to a unique ASCII case-insensitive match, reported as PS902, so the same
// config gives the same findings on case-sensitive and case-insensitive
// filesystems.
fs::path root_dir;

bool Normalise(StringPiece raw, std::vector<GoogleString>* comps) {
  GoogleString s = raw.as_string();
  std::replace(s.begin(), s.end(), '\\', '/');
  if (s.size() >= 2 && s[1] == ':' && isalpha(static_cast<unsigned char>(s[0])))
    s = s.substr(2);  // drive letter: re-rooted like '/'
  comps->clear();
  StringPieceVector parts;
  SplitStringPieceToVector(s, "/", &parts, true);
  for (StringPiece p : parts) {
    if (p == ".") continue;
    if (p == "..") {
      if (comps->empty()) return false;  // escapes the root
      comps->pop_back();
      continue;
    }
    comps->push_back(p.as_string());
  }
  return true;
}

GoogleString JoinLogical(const std::vector<GoogleString>& comps) {
  GoogleString out;
  for (size_t i = 0; i < comps.size(); ++i) {
    if (i) out += "/";
    out += comps[i];
  }
  return out;
}

std::vector<GoogleString> SortedEntries(const fs::path& dir) {
  std::vector<GoogleString> names;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    names.push_back(U8(it->path().filename()));
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool HasWildcard(const GoogleString& s) {
  return s.find_first_of("*?[") != GoogleString::npos;
}

// fnmatch subset: * ? and [...] (case-sensitive).
bool WildMatch(const char* p, const char* s) {
  if (*p == '\0') return *s == '\0';
  if (*p == '*') {
    for (const char* t = s;; ++t) {
      if (WildMatch(p + 1, t)) return true;
      if (*t == '\0') return false;
    }
  }
  if (*s == '\0') return false;
  if (*p == '?') return WildMatch(p + 1, s + 1);
  if (*p == '[') {
    const char* q = p + 1;
    bool neg = (*q == '!' || *q == '^');
    if (neg) ++q;
    bool hit = false;
    for (; *q && *q != ']'; ++q) {
      if (q[1] == '-' && q[2] && q[2] != ']') {
        if (*s >= q[0] && *s <= q[2]) hit = true;
        q += 2;
      } else if (*q == *s) {
        hit = true;
      }
    }
    if (*q != ']') return false;
    return (hit != neg) && WildMatch(q + 1, s + 1);
  }
  return *p == *s && WildMatch(p + 1, s + 1);
}

struct Resolved {
  GoogleString logical;
  fs::path disk;
  bool case_mismatch = false;
};

// Resolve fixed components; returns false when not found.
bool ResolveComponents(const std::vector<GoogleString>& comps, size_t n,
                       fs::path* disk, bool* case_mismatch) {
  fs::path cur = root_dir;
  for (size_t i = 0; i < n; ++i) {
    std::vector<GoogleString> entries = SortedEntries(cur);
    const GoogleString& want = comps[i];
    if (std::binary_search(entries.begin(), entries.end(), want)) {
      cur /= FromU8(want);
      continue;
    }
    const GoogleString* match = nullptr;
    int count = 0;
    for (const GoogleString& e : entries) {
      if (StringCaseEqual(e, want)) {
        match = &e;
        ++count;
      }
    }
    if (count != 1) return false;
    *case_mismatch = true;
    cur /= FromU8(*match);
  }
  *disk = cur;
  return true;
}

void AddFinding(const char* rule, const char* severity, const GoogleString& file,
                int line, const GoogleString& scope, const GoogleString& directive,
                std::vector<GoogleString> args, const GoogleString& message) {
  Finding f;
  f.rule = rule;
  f.severity = severity;
  f.file = file;
  f.line = line;
  f.scope = scope;
  f.directive = directive;
  f.args = std::move(args);
  f.message = message;
  findings.push_back(std::move(f));
}

// Expands one Include argument into files (logical + disk), sorted.
bool ExpandInclude(StringPiece arg, const GoogleString& from_file, int line,
                   const GoogleString& scope, bool optional,
                   std::vector<Resolved>* out) {
  std::vector<GoogleString> comps;
  if (!Normalise(arg, &comps) || comps.empty()) {
    AddFinding("PS903", "error", from_file, line, scope, "Include", {},
               "Include path leaves the configuration root");
    return false;
  }
  bool wild = HasWildcard(comps.back());
  fs::path dir;
  bool cm = false;
  if (!ResolveComponents(comps, comps.size() - (wild ? 1 : 0), &dir, &cm)) {
    if (!optional) {
      AddFinding("PS901", "error", from_file, line, scope, "Include", {},
                 StrCat("Include target not found: ", JoinLogical(comps)));
    }
    return false;
  }
  std::vector<std::pair<GoogleString, fs::path>> files;
  if (wild) {
    std::vector<GoogleString> base(comps.begin(), comps.end() - 1);
    for (const GoogleString& e : SortedEntries(dir)) {
      if (!WildMatch(comps.back().c_str(), e.c_str())) continue;
      fs::path p = dir / FromU8(e);
      if (!fs::is_regular_file(p)) continue;
      std::vector<GoogleString> c = base;
      c.push_back(e);
      files.emplace_back(JoinLogical(c), p);
    }
    if (files.empty() && !optional) {
      AddFinding("PS901", "error", from_file, line, scope, "Include", {},
                 StrCat("Include wildcard matched nothing: ",
                        JoinLogical(comps)));
    }
  } else if (fs::is_directory(dir)) {
    // httpd reads every file below a directory, sorted.
    std::vector<std::pair<GoogleString, fs::path>> stack;
    std::vector<std::pair<GoogleString, fs::path>> todo = {
        {JoinLogical(comps), dir}};
    while (!todo.empty()) {
      auto [lp, dp] = todo.back();
      todo.pop_back();
      std::vector<GoogleString> entries = SortedEntries(dp);
      std::vector<std::pair<GoogleString, fs::path>> sub;
      for (const GoogleString& e : entries) {
        fs::path p = dp / FromU8(e);
        if (fs::is_directory(p)) {
          sub.emplace_back(StrCat(lp, "/", e), p);
        } else if (fs::is_regular_file(p)) {
          files.emplace_back(StrCat(lp, "/", e), p);
        }
      }
      for (auto it = sub.rbegin(); it != sub.rend(); ++it) todo.push_back(*it);
    }
  } else {
    files.emplace_back(JoinLogical(comps), dir);
  }
  if (cm) {
    AddFinding("PS902", "warning", from_file, line, scope, "Include", {},
               StrCat("Include path differs only by letter case from the file "
                      "on disk: ", JoinLogical(comps),
                      " (works on case-insensitive filesystems only)"));
  }
  for (auto& [lp, dp] : files) {
    Resolved r;
    r.logical = lp;
    r.disk = dp;
    r.case_mismatch = cm;
    out->push_back(std::move(r));
  }
  return true;
}

// ---------------------------------------------------------------------------
// The value check: what ParseDirective / ParseDirective2 / ParseDirective3 in
// mod_instaweb.cc do, against a throwaway ApacheConfig.
ThreadSystem* thread_system = nullptr;

struct ValueResult {
  enum Kind { kOk, kInvalid, kUnknown } kind = kOk;
  GoogleString msg;
  bool removed = false;     // deprecated property: accepted, does nothing
  bool renamed = false;     // deprecated spelling
  GoogleString new_name;
  std::vector<GoogleString> warnings;
};

// The handful of process-scope options SystemRewriteDriverFactory parses
// itself (system_rewrite_driver_factory.cc ParseAndSetOption1/2).  SPIKE:
// copied; the lint proper would call factored-out static validators.
RewriteOptions::OptionSettingResult FactoryOption1(StringPiece option,
                                                   StringPiece arg,
                                                   bool process_scope,
                                                   GoogleString* msg,
                                                   std::vector<GoogleString>* w) {
  static const char* const kStrict[] = {"StaticAssetPrefix",
                                        "UsePerVHostStatistics",
                                        "InstallCrashHandler",
                                        "NumRewriteThreads",
                                        "NumExpensiveRewriteThreads"};
  static const char* const kTolerated[] = {"ForceCaching",
                                           "ListOutstandingUrlsOnError",
                                           "MessageBufferSize",
                                           "TrackOriginalContentLength"};
  bool strict = false, tolerated = false;
  for (const char* n : kStrict) strict |= StringCaseEqual(option, n);
  for (const char* n : kTolerated) tolerated |= StringCaseEqual(option, n);
  if (!strict && !tolerated) return RewriteOptions::kOptionNameUnknown;
  if (!process_scope) {
    if (strict) {
      *msg = StrCat("'", option, "' is global and can't be set at this scope.");
      return RewriteOptions::kOptionValueInvalid;
    }
    w->push_back(StrCat("'", option, "' is global and is ignored at this scope"));
    return RewriteOptions::kOptionOk;
  }
  if (StringCaseEqual(option, "StaticAssetPrefix")) return RewriteOptions::kOptionOk;
  if (StringCaseEqual(option, "NumRewriteThreads") ||
      StringCaseEqual(option, "NumExpensiveRewriteThreads")) {
    int v;
    if (StringCaseEqual(arg, "auto")) return RewriteOptions::kOptionOk;
    if (!StringToInt(arg, &v) || v < 0) {
      *msg = StrCat("'", option, "' must be a non-negative integer or 'auto'");
      return RewriteOptions::kOptionValueInvalid;
    }
    return RewriteOptions::kOptionOk;
  }
  if (StringCaseEqual(option, "MessageBufferSize")) {
    int v;
    if (!RewriteOptions::ParseFromString(arg, &v) || v < 0 || v > (1 << 30)) {
      *msg = "MessageBufferSize must be between 0 and 1073741824.";
      return RewriteOptions::kOptionValueInvalid;
    }
    return RewriteOptions::kOptionOk;
  }
  bool b;
  return RewriteOptions::ParseFromString(arg, &b)
             ? RewriteOptions::kOptionOk
             : RewriteOptions::kOptionValueInvalid;
}

RewriteOptions::OptionSettingResult FactoryOption2(StringPiece option,
                                                   StringPiece arg1,
                                                   StringPiece arg2,
                                                   bool process_scope,
                                                   GoogleString* msg,
                                                   std::vector<GoogleString>* w) {
  if (!StringCaseEqual(option, "CreateSharedMemoryMetadataCache"))
    return RewriteOptions::kOptionNameUnknown;
  if (!process_scope) {
    w->push_back(StrCat("'", option, "' is global and is ignored at this scope"));
    return RewriteOptions::kOptionOk;
  }
  int64 kb = 0;
  if (!StringToInt64(arg2, &kb) || kb < 0) {
    *msg = "size_kb must be a positive 64-bit integer";
    return RewriteOptions::kOptionValueInvalid;
  }
  return RewriteOptions::kOptionOk;
}

ValueResult CheckValue(const ApacheDirective& d, StringPiece written,
                       const std::vector<GoogleString>& args,
                       const Placement& p) {
  ValueResult r;
  RewriteOptions::ResetDeprecatedFilterWarnings();  // per-line, deterministic
  ApacheConfig config("lint", thread_system);
  CaptureHandler handler;
  StringPiece prefix("ModPagespeed");
  StringPiece directive = written;
  GoogleString msg;
  RewriteOptions::OptionSettingResult result = RewriteOptions::kOptionNameUnknown;
  bool process_scope = !p.in_vhost;

  auto finish = [&](RewriteOptions::OptionSettingResult res) {
    for (auto& m : handler.messages) {
      if (m.first >= kWarning) r.warnings.push_back(m.second);
    }
    if (res == RewriteOptions::kOptionOk) {
      r.kind = ValueResult::kOk;
    } else if (res == RewriteOptions::kOptionValueInvalid) {
      r.kind = ValueResult::kInvalid;
      r.msg = msg;
    } else {
      r.kind = ValueResult::kUnknown;
    }
    return r;
  };

  if (d.parser == Parser::kParse1) {
    const GoogleString& arg = args[0];
    if (StringCaseEqual(directive, "ModPagespeedFileCachePath")) {
      config.set_file_cache_path(arg);
      return finish(RewriteOptions::kOptionOk);
    }
    if (StringCaseEqual(directive, "ModPagespeedLogDir")) {
      config.set_log_dir(arg);
      return finish(RewriteOptions::kOptionOk);
    }
    GoogleString renamed;
    if (StringCaseEqual(directive, "ModPagespeedImgInlineMaxBytes")) {
      renamed = "ModPagespeedImageInlineMaxBytes";
    } else if (StringCaseEqual(directive, "ModPagespeedImgMaxRewritesAtOnce")) {
      renamed = "ModPagespeedImageMaxRewritesAtOnce";
    }
    if (!renamed.empty()) {
      r.renamed = true;
      r.new_name = renamed;
      directive = r.new_name;
    }
    if (StringCaseStartsWith(directive, prefix)) {
      StringPiece option = directive.substr(prefix.size());
      result = (StringCaseEqual(option, RewriteOptions::kExperimentSpec) &&
                p.kind == ContextKind::kHtaccess)
                   ? config.ParseAndSetOptionFromNameWithScope(
                         option, arg, RewriteOptions::kDirectoryScope, &msg,
                         &handler)
                   : config.ParseAndSetOptionFromName1(option, arg, &msg,
                                                       &handler);
      if (result == RewriteOptions::kOptionNameUnknown) {
        result = FactoryOption1(option, arg, process_scope, &msg, &r.warnings);
      }
      if (result != RewriteOptions::kOptionNameUnknown) return finish(result);
      if (RewriteOptions::IsDeprecatedOptionName(option)) {
        r.removed = true;
        return finish(RewriteOptions::kOptionOk);
      }
    }
    if (StringCaseEqual(directive, "ModPagespeed")) {
      RewriteOptions::EnabledEnum e;
      if (RewriteOptions::ParseFromString(arg, &e)) {
        return finish(RewriteOptions::kOptionOk);
      }
      msg = "ModPagespeed must be on, off, unplugged or standby";
      return finish(RewriteOptions::kOptionValueInvalid);
    }
    return finish(RewriteOptions::kOptionNameUnknown);
  }
  if (d.parser == Parser::kParse2) {
    StringPiece option = directive.substr(prefix.size());
    result = config.ParseAndSetOptionFromName2(option, args[0], args[1], &msg,
                                               &handler);
    if (result == RewriteOptions::kOptionNameUnknown) {
      result = FactoryOption2(option, args[0], args[1], process_scope, &msg,
                              &r.warnings);
    }
    return finish(result);
  }
  // kParse3 (TAKE3 / TAKE23).
  StringPiece option = directive.substr(prefix.size());
  result = config.ParseAndSetOptionFromName3(
      option, args[0], args[1], args.size() > 2 ? StringPiece(args[2]) : "",
      &msg, &handler);
  return finish(result);
}

// ---------------------------------------------------------------------------
// The walk over sections.
struct Frame {
  GoogleString name;  // lower case
  int line;
};

const char* const kDirSections[] = {"directory", "directorymatch", "location",
                                    "locationmatch", "files", "filesmatch",
                                    "if", "elseif", "else", "proxy",
                                    "proxymatch"};

GoogleString ScopeLabel(const Placement& p) {
  GoogleString s;
  switch (p.kind) {
    case ContextKind::kServer: s = "server"; break;
    case ContextKind::kVirtualHost: s = "virtualhost"; break;
    case ContextKind::kDirectory: s = p.in_vhost ? "virtualhost/directory"
                                                  : "directory"; break;
    case ContextKind::kHtaccess: s = "htaccess"; break;
  }
  if (p.in_conditional) s += "+conditional";
  return s;
}

Placement PlacementFor(const std::vector<Frame>& stack, bool htaccess) {
  Placement p;
  p.kind = htaccess ? ContextKind::kHtaccess : ContextKind::kServer;
  for (const Frame& f : stack) {
    if (f.name == "virtualhost") {
      p.in_vhost = true;
      if (p.kind == ContextKind::kServer) p.kind = ContextKind::kVirtualHost;
    } else if (f.name == "modpagespeedif") {
      p.in_conditional = true;
    } else if (f.name == "limit" || f.name == "limitexcept") {
      p.in_limit = true;
    } else {
      for (const char* s : kDirSections) {
        if (f.name == s && !htaccess) p.kind = ContextKind::kDirectory;
      }
    }
  }
  return p;
}

std::set<GoogleString> open_files;  // cycle guard (disk paths)

void LintDirective(const GoogleString& file, int line, const Placement& p,
                   const GoogleString& name, std::vector<GoogleString> args) {
  GoogleString scope = ScopeLabel(p);
  bool safe = ValuesAreSafe(name);
  std::vector<GoogleString> shown = args;
  if (!safe) {
    for (auto& a : shown) a = kMasked;
  }
  const ApacheDirective* d = LookupApacheDirective(name);
  if (d == nullptr) {
    AddFinding("PS101", "error", file, line, scope, name, shown,
               StrCat("Invalid command '", name,
                      "': not a mod_pagespeed directive"));
    return;
  }
  GoogleString amsg;
  if (!CheckArity(*d, static_cast<int>(args.size()), &amsg)) {
    AddFinding("PS103", "error", file, line, scope, name, shown, amsg);
    return;
  }
  ScopeResult s = CheckApacheScope(*d, p);
  if (s.verdict == Verdict::kError) {
    AddFinding(s.rule, "error", file, line, scope, name, shown, s.message);
    return;
  }
  if (s.verdict == Verdict::kWarning) {
    AddFinding(s.rule, "warning", file, line, scope, name, shown, s.message);
  }
  log_sink->count = 0;
  ValueResult v = CheckValue(*d, name, args, p);
  if (log_sink->count > 0) {
    AddFinding("PS111", "warning", file, line, scope, name, shown,
               StrCat("The option parser logged ", log_sink->count,
                      " internal error(s) for this line (text not shown)"));
  }
  if (v.kind == ValueResult::kUnknown) {
    AddFinding("PS101", "error", file, line, scope, name, shown,
               StrCat("Unknown directive ", name));
    return;
  }
  if (v.kind == ValueResult::kInvalid) {
    AddFinding("PS102", "error", file, line, scope, name, shown,
               safe ? v.msg
                    : StrCat("Invalid value for ", name,
                             " (value not shown: the option may hold a "
                             "secret)"));
  }
  if (v.removed) {
    AddFinding("PS202", "warning", file, line, scope, name, shown,
               StrCat(name, " has been removed and does nothing; remove it"));
  }
  if (v.renamed) {
    AddFinding("PS201", "warning", file, line, scope, name, shown,
               StrCat(name, " is a deprecated spelling; use ", v.new_name));
  }
  if (d->from_option_table && d->vhost == VhostHandling::kNone) {
    // nothing
  }
  for (const GoogleString& w : v.warnings) {
    bool deprecated = w.find("deprecated") != GoogleString::npos ||
                      w.find("removed") != GoogleString::npos;
    bool global = w.find("is global and is ignored") != GoogleString::npos;
    const char* rule = deprecated ? "PS201" : global ? "PS303" : "PS110";
    AddFinding(rule, "warning", file, line, scope, name, shown,
               safe ? w
                    : StrCat(name, ": parser warning (text not shown: the "
                             "option may hold a secret)"));
  }
}

void LintFile(const GoogleString& logical, const fs::path& disk, bool htaccess,
              std::vector<Frame>* stack, int depth) {
  GoogleString key = U8(disk);
  if (open_files.count(key) || depth > 32) {
    AddFinding("PS903", "error", logical, 0, "server", "Include", {},
               "Include cycle");
    return;
  }
  std::ifstream in(disk, std::ios::binary);
  if (!in) {
    AddFinding("PS901", "error", logical, 0, "server", "Include", {},
               StrCat("Cannot read ", logical));
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::string data = ss.str();
  inputs.push_back({logical, HexSha256(data)});
  open_files.insert(key);
  size_t base_depth = stack->size();

  std::vector<GoogleString> raw;
  {
    std::istringstream ls(data);
    std::string l;
    while (std::getline(ls, l)) {
      if (!l.empty() && l.back() == '\r') l.pop_back();
      raw.push_back(l);
    }
  }
  for (size_t i = 0; i < raw.size(); ++i) {
    int lineno = static_cast<int>(i) + 1;
    GoogleString line = raw[i];
    while (!line.empty() && line.back() == '\\' && i + 1 < raw.size()) {
      line.pop_back();
      line += raw[++i];
    }
    StringPiece t(line);
    TrimWhitespace(&t);
    if (t.empty() || t[0] == '#') continue;
    Placement p = PlacementFor(*stack, htaccess);
    if (t.starts_with("</")) {
      GoogleString n = t.substr(2).as_string();
      if (!n.empty() && n.back() == '>') n.pop_back();
      LowerString(&n);
      if (stack->size() <= base_depth || stack->back().name != n) {
        AddFinding("PS104", "error", logical, lineno, ScopeLabel(p), "", {},
                   StrCat("Unexpected </", n, ">"));
        continue;
      }
      stack->pop_back();
      continue;
    }
    if (t[0] == '<') {
      if (t.back() != '>') {
        AddFinding("PS104", "error", logical, lineno, ScopeLabel(p), "", {},
                   "Section opener without closing '>'");
        continue;
      }
      GoogleString inner = t.substr(1, t.size() - 2).as_string();
      std::vector<GoogleString> w = Words(inner);
      if (w.empty()) continue;
      GoogleString n = w[0];
      GoogleString lower = n;
      LowerString(&lower);
      if (lower == "modpagespeedif") {
        std::vector<GoogleString> a(w.begin() + 1, w.end());
        GoogleString pname = StrCat("<", n);
        const ApacheDirective* d = LookupApacheDirective(pname);
        if (d != nullptr) {
          ScopeResult s = CheckApacheScope(*d, p);
          if (s.verdict == Verdict::kError) {
            AddFinding(s.rule, "error", logical, lineno, ScopeLabel(p),
                       pname + ">", a, s.message);
          } else if (a.size() != 1 ||
                     !(StringCaseEqual(a[0], "spdy") ||
                       StringCaseEqual(a[0], "!spdy"))) {
            AddFinding("PS102", "error", logical, lineno, ScopeLabel(p),
                       pname + ">", a, "Conditional must be spdy or !spdy.");
          }
        }
      } else if (p.in_conditional) {
        AddFinding("PS307", "error", logical, lineno, ScopeLabel(p), n, {},
                   "Only mod_pagespeed directives should be inside "
                   "<ModPagespeedIf blocks");
      }
      stack->push_back({lower, lineno});
      continue;
    }
    std::vector<GoogleString> w = Words(t.as_string());
    GoogleString name = w[0];
    std::vector<GoogleString> args(w.begin() + 1, w.end());
    if (StringCaseEqual(name, "Include") ||
        StringCaseEqual(name, "IncludeOptional")) {
      if (htaccess) continue;
      for (const GoogleString& a : args) {
        std::vector<Resolved> files;
        ExpandInclude(a, logical, lineno, ScopeLabel(p),
                      StringCaseEqual(name, "IncludeOptional"), &files);
        for (const Resolved& r : files) {
          LintFile(r.logical, r.disk, false, stack, depth + 1);
        }
      }
      continue;
    }
    if (!StringCaseStartsWith(name, "ModPagespeed")) {
      if (p.in_conditional) {
        AddFinding("PS307", "error", logical, lineno, ScopeLabel(p), name, {},
                   "Only mod_pagespeed directives should be inside "
                   "<ModPagespeedIf blocks");
      }
      continue;  // other modules' directives are not ours to check
    }
    LintDirective(logical, lineno, p, name, args);
  }
  while (stack->size() > base_depth) {
    AddFinding("PS104", "error", logical, stack->back().line, "", "", {},
               StrCat("<", stack->back().name, "> not closed in this file"));
    stack->pop_back();
  }
  open_files.erase(key);
}

GoogleString RenderJson(const std::vector<GoogleString>& argv_inputs) {
  GoogleString o;
  o += "{\n  \"schema\": \"pagespeed-lint/1\",\n  \"tool_version\": ";
  JsonString(kToolVersion, &o);
  o += ",\n  \"module_version\": ";
  JsonString(kModPagespeedVersion, &o);
  o += ",\n  \"inputs\": [";
  for (size_t i = 0; i < inputs.size(); ++i) {
    o += i ? ",\n    {" : "\n    {";
    o += "\"path\": ";
    JsonString(inputs[i].path, &o);
    o += ", \"sha256\": ";
    JsonString(inputs[i].sha256, &o);
    o += "}";
  }
  o += inputs.empty() ? "],\n" : "\n  ],\n";
  o += "  \"findings\": [";
  for (size_t i = 0; i < findings.size(); ++i) {
    const Finding& f = findings[i];
    o += i ? ",\n    {" : "\n    {";
    o += "\"rule\": ";
    JsonString(f.rule, &o);
    o += ", \"severity\": ";
    JsonString(f.severity, &o);
    o += ", \"file\": ";
    JsonString(f.file, &o);
    o += StrCat(", \"line\": ", IntegerToString(f.line), ", \"scope\": ");
    JsonString(f.scope, &o);
    o += ", \"directive\": ";
    JsonString(f.directive, &o);
    o += ", \"args\": [";
    for (size_t j = 0; j < f.args.size(); ++j) {
      if (j) o += ", ";
      JsonString(f.args[j], &o);
    }
    o += "], \"message\": ";
    JsonString(f.message, &o);
    o += "}";
  }
  o += findings.empty() ? "]\n}\n" : "\n  ]\n}\n";
  return o;
}

int Main(int argc, char** argv) {
  GoogleString root = ".";
  GoogleString config;
  std::vector<GoogleString> htaccess;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    StringPiece a(argv[i]);
    if (a == "--root" && i + 1 < argc) {
      root = argv[++i];
    } else if (a == "--htaccess" && i + 1 < argc) {
      htaccess.push_back(argv[++i]);
    } else if (a == "--show-values") {
      if (!isatty(fileno(stdout))) {
        fprintf(stderr, "--show-values is refused when stdout is not a "
                        "terminal\n");
        return 2;
      }
      show_values = true;
    } else if (a == "--list-directives") {
      list = true;
    } else if (a.starts_with("--")) {
      fprintf(stderr, "unknown flag %s\n", argv[i]);
      return 2;
    } else if (config.empty()) {
      config = argv[i];
    } else {
      fprintf(stderr, "only one config file\n");
      return 2;
    }
  }

  spdlog::default_logger()->set_level(spdlog::level::off);
  log_sink = new CountingLogSink;
  pagespeed_logging::AddLogSink(log_sink);
  ApacheConfig::Initialize();
  domain_registry::Init();
  HtmlKeywords::Init();
  thread_system = Platform::CreateThreadSystem();
  InitApacheDirectives();
  safe_names = new std::set<GoogleString, CaseLess>(
      std::begin(kSafeOptionNames), std::end(kSafeOptionNames));

  if (list) {
    // name<TAB>nargs<TAB>safe<TAB>override : the seed list for the secret
    // test, every directive httpd would accept.
    for (int i = 0; i < NumApacheDirectives(); ++i) {
      const ApacheDirective& d = ApacheDirectiveAt(i);
      int n = d.args == ApacheArgs::kTake2 || d.args == ApacheArgs::kTake23 ? 2
              : d.args == ApacheArgs::kTake3 ? 3 : 1;
      printf("%s\t%d\t%d\t%u\n", d.name.c_str(), n,
             ValuesAreSafe(d.name) ? 1 : 0, d.req_override);
    }
    printf("#directives\t%d\n#safe_ts\t%d\n#safe_secret_excluded\t%d\n",
           NumApacheDirectives(), kSafeOptionNamesTsCount,
           kSafeOptionNamesSecretExcluded);
    return 0;
  }

  std::error_code ec;
  root_dir = fs::absolute(FromU8(root), ec);
  if (ec || !fs::is_directory(root_dir)) {
    fprintf(stderr, "root is not a directory\n");
    return 2;
  }
  std::vector<GoogleString> argv_inputs;
  std::vector<Frame> stack;
  auto run = [&](const GoogleString& f, bool ht) {
    std::vector<GoogleString> comps;
    fs::path disk;
    bool cm = false;
    if (!Normalise(f, &comps) || comps.empty() ||
        !ResolveComponents(comps, comps.size(), &disk, &cm) ||
        !fs::is_regular_file(disk)) {
      fprintf(stderr, "cannot read input\n");
      return false;
    }
    stack.clear();
    LintFile(JoinLogical(comps), disk, ht, &stack, 0);
    return true;
  };
  if (!config.empty() && !run(config, false)) return 2;
  for (const GoogleString& h : htaccess) {
    if (!run(h, true)) return 2;
  }
  if (config.empty() && htaccess.empty()) {
    fprintf(stderr, "usage: pagespeed_lint_spike [--root DIR] [--htaccess F] "
                    "CONFIG\n");
    return 2;
  }
  GoogleString json = RenderJson(argv_inputs);
#ifdef _WIN32
  // Text-mode stdout would turn every \n into \r\n: same bytes everywhere.
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  fwrite(json.data(), 1, json.size(), stdout);
  fflush(stdout);
  for (const Finding& f : findings) {
    if (f.severity == "error") return 1;
  }
  return 0;
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb

int main(int argc, char** argv) {
  return net_instaweb::lint::Main(argc, argv);
}
