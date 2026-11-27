// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

// Configurations written for the 1.13 and 1.14 releases and for ngx_pagespeed
// keep loading.
//
// testdata/legacy_config/nginx.conf and apache.conf hold the directives people
// wrote for those releases: the examples of the old documentation, the test
// configurations of the archived repositories, configurations quoted in their
// issues, and one line for every filter, and every option the 1.14.36.1
// sources registered for that port, that no example covers.  Each line
// carries the class it is
// expected to fall in (see the header of the corpus files):
//
//   same             accepted, no warning;
//   deprecated       accepted with a warning;
//   changed-default  accepted, no warning (the note says which default moved);
//   removed          refused: a known break, listed so it stays documented;
//   never-loaded     refused, as it was by the 1.14 release: the option has
//                    no help text, so httpd has no command for it.  Not a
//                    break.
//
// The names of the known breaks are also listed in KnownRemoved() below, so
// a new break cannot be waved through by editing the corpus alone.  As of
// this writing the list is empty on both ports: every legacy directive in the
// corpus loads, at most with a warning.
//
// Every directive is applied the way the port applies it, and the test fails
// when the outcome leaves its class.  A directive that used to load and now
// refuses fails as a regression; a known break that starts loading fails too,
// so the corpus and the upgrade notes get updated with it.
//
// The test is built twice (see BUILD): once as the nginx port, over
// SystemRewriteOptions, and once with LEGACY_CONFIG_APACHE as the Apache port,
// over ApacheConfig.  They cannot share a binary: option registration is
// process-wide, and a SystemRewriteOptions in a binary that also registered
// ApacheConfig has empty slots where the Apache options would be.
//
// How the directives are applied.  Neither nginx nor httpd can be linked into
// a unit test, so the two port parsers (NgxRewriteOptions::ParseAndSetOptions
// and mod_instaweb's ParseDirective*) are followed here step by step, over the
// real option classes and the real factory: the shared RewriteOptions and
// SystemRewriteOptions parsing, ApacheConfig, and
// SystemRewriteDriverFactory::ParseAndSetOption1/2.  What only the port
// sources know (the nginx-only option names and scopes, the scope tables, the
// Apache command names) is read from those sources, and the steps followed
// here are pinned against them at the bottom of this file, so a change to
// either parser fails here until this file is updated to match.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "net/instaweb/rewriter/public/process_context.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "pagespeed/kernel/base/google_message_handler.h"
#include "pagespeed/kernel/base/message_handler.h"
#include "pagespeed/kernel/base/null_message_handler.h"
#include "pagespeed/kernel/base/stdio_file_system.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/system/system_caches.h"
#include "pagespeed/system/system_rewrite_driver_factory.h"
#include "pagespeed/system/system_rewrite_options.h"
#include "pagespeed/system/system_thread_system.h"
#include "test/pagespeed/kernel/base/gtest.h"
#include "test/pagespeed/kernel/base/null_thread_system.h"

#ifdef LEGACY_CONFIG_APACHE
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/apache/apache_directive_table.h"
#endif

namespace net_instaweb {
namespace {

const char kCorpusDir[] = "test/pagespeed/system/testdata/legacy_config/";
#ifdef LEGACY_CONFIG_APACHE
const char kModInstawebCc[] = "pagespeed/apache/mod_instaweb.cc";
#else
const char kNgxOptionsCc[] = "pagespeed/nginx/ngx_rewrite_options.cc";
#endif

GoogleString ReadSource(const GoogleString& relative_path) {
  StdioFileSystem file_system;
  NullMessageHandler handler;
  GoogleString source;
  const GoogleString path = StrCat(GTestSrcDir(), "/", relative_path);
  EXPECT_TRUE(file_system.ReadFile(path.c_str(), &source, &handler)) << path;
  return source;
}

GoogleString Lower(StringPiece s) {
  GoogleString out = s.as_string();
  LowerString(&out);
  return out;
}

// The source without // comments and without whitespace, so a pin does not
// depend on where clang-format breaks a line.
GoogleString Condense(const GoogleString& source) {
  GoogleString out;
  bool in_string = false;
  for (size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (!in_string && c == '/' && i + 1 < source.size() &&
        source[i + 1] == '/') {
      while (i < source.size() && source[i] != '\n') {
        ++i;
      }
      continue;
    }
    if (c == '"' && (i == 0 || source[i - 1] != '\\')) {
      in_string = !in_string;
    }
    if (in_string || !isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

// The body of the function whose definition starts with `signature`, up to
// the next closing brace at column zero.
GoogleString FunctionBody(const GoogleString& source,
                          const GoogleString& signature) {
  const size_t start = source.find(signature);
  if (start == GoogleString::npos) {
    return GoogleString();
  }
  const size_t end = source.find("\n}\n", start);
  return source.substr(
      start, end == GoogleString::npos ? GoogleString::npos : end - start);
}

// ---------------------------------------------------------------------------
// The corpus.
// ---------------------------------------------------------------------------

// The directive names (without "ModPagespeed", lower case) that used to load
// and are now refused on purpose: RewriteOptions::RemovedOptions().  Every expect=removed line in a corpus must
// name one of these, and every name here must appear in the corpus as
// expect=removed.  Adding a name needs the replacement documented in the
// upgrade notes.  FileCacheInodeLimit, FileCacheCleanIntervalMs,
// MaxPrefetchJsElements, DistributeFetches and DistributedRewriteKey,
// DistributedRewriteServers and DistributedRewriteTimeoutMs were refused by
// nginx up to 2.8.2; they now load with a warning, as on Apache.
const std::set<GoogleString>& KnownRemoved() {
  static const std::set<GoogleString>* removed = [] {
    auto* names = new std::set<GoogleString>;
    for (const RewriteOptions::RemovedOption& r :
         RewriteOptions::RemovedOptions()) {
      names->insert(Lower(r.name));
    }
    return names;
  }();
  return *removed;
}

enum class Expect {
  kSame,
  kDeprecated,
  kChangedDefault,
  kRemoved,
  kNeverLoaded
};

const char* ExpectName(Expect e) {
  switch (e) {
    case Expect::kSame:
      return "same";
    case Expect::kDeprecated:
      return "deprecated";
    case Expect::kChangedDefault:
      return "changed-default";
    case Expect::kRemoved:
      return "removed";
    case Expect::kNeverLoaded:
      return "never-loaded";
  }
  return "?";
}

struct Directive {
  int line = 0;
  GoogleString text;
  StringVector args;  // args[0] is the directive name.
  GoogleString src;
  GoogleString note;
  Expect expect = Expect::kSame;
  RewriteOptions::OptionScope scope = RewriteOptions::kProcessScopeStrict;
};

// Splits a configuration line into words; double quotes group, and a
// backslash inside quotes escapes the next character, as both servers'
// tokenizers do for the quoting used in the corpus.
StringVector Words(StringPiece line) {
  StringVector out;
  GoogleString cur;
  bool in_word = false;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted) {
      if (c == '\\' && i + 1 < line.size()) {
        cur.push_back(line[++i]);
      } else if (c == '"') {
        quoted = false;
      } else {
        cur.push_back(c);
      }
    } else if (c == '"') {
      quoted = true;
      in_word = true;
    } else if (isspace(static_cast<unsigned char>(c))) {
      if (in_word) {
        out.push_back(cur);
        cur.clear();
        in_word = false;
      }
    } else {
      cur.push_back(c);
      in_word = true;
    }
  }
  if (in_word) {
    out.push_back(cur);
  }
  return out;
}

bool ParseMeta(StringPiece line, Directive* d) {
  // "#@ src=<src> expect=<class> [note=<text to end of line>]"
  StringPiece rest = line.substr(2);
  const size_t note_pos = rest.find(" note=");
  if (note_pos != StringPiece::npos) {
    d->note = rest.substr(note_pos + 6).as_string();
    rest = rest.substr(0, note_pos);
  }
  StringPieceVector fields;
  SplitStringPieceToVector(rest, " ", &fields, true);
  for (StringPiece f : fields) {
    if (StringCaseStartsWith(f, "src=")) {
      d->src = f.substr(4).as_string();
    } else if (StringCaseStartsWith(f, "expect=")) {
      StringPiece e = f.substr(7);
      if (e == "same") {
        d->expect = Expect::kSame;
      } else if (e == "deprecated") {
        d->expect = Expect::kDeprecated;
      } else if (e == "changed-default") {
        d->expect = Expect::kChangedDefault;
      } else if (e == "removed") {
        d->expect = Expect::kRemoved;
      } else if (e == "never-loaded") {
        d->expect = Expect::kNeverLoaded;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

// Reads a corpus file.  For nginx the enclosing block sets the scope a
// directive is read at (http: process, server: server, location and if:
// directory); Apache lines are all at the server-config level.
std::vector<Directive> ReadCorpus(const GoogleString& file, bool nginx) {
  std::vector<Directive> out;
  const GoogleString text = ReadSource(StrCat(kCorpusDir, file));
  StringPieceVector lines;
  SplitStringPieceToVector(text, "\n", &lines, false);
  std::vector<RewriteOptions::OptionScope> scopes;
  Directive pending;
  bool have_meta = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    StringPiece line = lines[i];
    TrimWhitespace(&line);
    if (line.empty()) {
      continue;
    }
    if (StringCaseStartsWith(line, "#@")) {
      pending = Directive();
      EXPECT_TRUE(ParseMeta(line, &pending))
          << file << ":" << i + 1 << ": bad metadata line: " << line;
      have_meta = true;
      continue;
    }
    if (line[0] == '#') {
      continue;
    }
    if (nginx && line.ends_with("{")) {
      StringVector w = Words(line);
      const GoogleString block = w.empty() ? "" : Lower(w[0]);
      scopes.push_back(block == "http"     ? RewriteOptions::kProcessScopeStrict
                       : block == "server" ? RewriteOptions::kServerScope
                                           : RewriteOptions::kDirectoryScope);
      continue;
    }
    if (nginx && line == "}") {
      EXPECT_FALSE(scopes.empty()) << file << ":" << i + 1;
      if (!scopes.empty()) {
        scopes.pop_back();
      }
      continue;
    }
    Directive d = have_meta ? pending : Directive();
    have_meta = false;
    d.line = static_cast<int>(i + 1);
    d.text = line.as_string();
    if (nginx) {
      EXPECT_TRUE(line.ends_with(";")) << file << ":" << d.line;
      line.remove_suffix(1);
      d.scope =
          scopes.empty() ? RewriteOptions::kProcessScopeStrict : scopes.back();
    }
    d.args = Words(line);
    if (nginx) {
      // Drop the "pagespeed" word; the module sees what follows it.
      EXPECT_TRUE(!d.args.empty() && StringCaseEqual(d.args[0], "pagespeed"))
          << file << ":" << d.line;
      d.args.erase(d.args.begin());
    }
    EXPECT_FALSE(d.args.empty()) << file << ":" << d.line;
    if (!d.args.empty()) {
      out.push_back(d);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// What a directive did.
// ---------------------------------------------------------------------------

enum class Outcome { kAccepted, kAcceptedWithWarning, kRefused };

class RecordingHandler : public MessageHandler {
 public:
  int warnings() const { return warnings_; }
  const GoogleString& text() const { return text_; }
  void Clear() {
    warnings_ = 0;
    text_.clear();
  }

 protected:
  void MessageSImpl(MessageType type, const GoogleString& message) override {
    if (type >= kWarning) {
      ++warnings_;
      StrAppend(&text_, text_.empty() ? "" : " | ", message);
    }
  }
  void FileMessageSImpl(MessageType type, const char* file, int line,
                        const GoogleString& message) override {
    MessageSImpl(type, message);
  }

 private:
  int warnings_ = 0;
  GoogleString text_;
};

struct Result {
  Outcome outcome = Outcome::kRefused;
  GoogleString message;
};

Result FromSettingResult(RewriteOptions::OptionSettingResult r,
                         const GoogleString& msg,
                         const RecordingHandler& handler) {
  Result result;
  switch (r) {
    case RewriteOptions::kOptionOk:
      result.outcome = handler.warnings() > 0 ? Outcome::kAcceptedWithWarning
                                              : Outcome::kAccepted;
      result.message = handler.text();
      break;
    case RewriteOptions::kOptionNameUnknown:
      result.message = "not recognized";
      break;
    case RewriteOptions::kOptionValueInvalid:
      result.message = StrCat("invalid value: ", msg);
      break;
  }
  return result;
}

// A SystemRewriteDriverFactory with just enough implemented to construct;
// only its option parsing is used.
class OptionParsingFactory : public SystemRewriteDriverFactory {
 public:
  OptionParsingFactory(const ProcessContext& process_context,
                       SystemThreadSystem* thread_system)
      : SystemRewriteDriverFactory(process_context, thread_system,
                                   nullptr /* shared_mem_runtime */,
                                   "localhost", 80) {}

  UrlAsyncFetcher* DefaultAsyncUrlFetcher() override { return nullptr; }
  UrlAsyncFetcher* AllocateFetcher(SystemRewriteOptions* config) override {
    return nullptr;
  }
  MessageHandler* DefaultHtmlParseMessageHandler() override {
    return new GoogleMessageHandler;
  }
  MessageHandler* DefaultMessageHandler() override {
    return new GoogleMessageHandler;
  }
  ServerContext* NewDecodingServerContext() override { return nullptr; }
  ~OptionParsingFactory() override {
    // SystemCaches insists on an orderly shutdown before it goes.
    if (caches() != nullptr) {
      caches()->ShutDown(&null_handler_);
    }
  }
  void NonStaticInitStats(Statistics* statistics) override {
    InitStats(statistics);
  }

 private:
  NullMessageHandler null_handler_;
};

#ifndef LEGACY_CONFIG_APACHE

// ---------------------------------------------------------------------------
// The nginx port: NgxRewriteOptions::ParseAndSetOptions.
// ---------------------------------------------------------------------------

// What only ngx_rewrite_options.cc knows: the options it registers itself
// (with their scope) and its two scope tables.
struct NgxTables {
  std::map<GoogleString, RewriteOptions::OptionScope> own_options;
  std::set<GoogleString> server_only;
  std::set<GoogleString> main_only;
};

RewriteOptions::OptionScope ScopeFromToken(StringPiece token) {
  if (token == "kQueryScope") return RewriteOptions::kQueryScope;
  if (token == "kDirectoryScope") return RewriteOptions::kDirectoryScope;
  if (token == "kServerScope") return RewriteOptions::kServerScope;
  if (token == "kLegacyProcessScope") {
    return RewriteOptions::kLegacyProcessScope;
  }
  return RewriteOptions::kProcessScopeStrict;
}

std::set<GoogleString> StringTable(const GoogleString& condensed,
                                   const GoogleString& opener) {
  std::set<GoogleString> out;
  const size_t start = condensed.find(opener);
  EXPECT_NE(GoogleString::npos, start) << opener;
  if (start == GoogleString::npos) {
    return out;
  }
  const size_t end = condensed.find("};", start);
  StringPieceVector items;
  SplitStringPieceToVector(
      StringPiece(condensed).substr(start + opener.size(),
                                    end - start - opener.size()),
      ",", &items, true);
  for (StringPiece item : items) {
    if (item.size() >= 2 && item[0] == '"' && item.ends_with("\"")) {
      out.insert(Lower(item.substr(1, item.size() - 2)));
    }
  }
  return out;
}

NgxTables ReadNgxTables() {
  NgxTables t;
  const GoogleString c = Condense(ReadSource(kNgxOptionsCc));
  // const char kStatisticsPath[] = "StatisticsPath";  ->  constant -> name.
  std::map<GoogleString, GoogleString> constants;
  for (size_t pos = c.find("constchark"); pos != GoogleString::npos;
       pos = c.find("constchark", pos + 1)) {
    const size_t bracket = c.find("[]=\"", pos);
    const size_t close = c.find("\";", bracket);
    if (bracket == GoogleString::npos || close == GoogleString::npos) {
      break;
    }
    constants[c.substr(pos + 9, bracket - pos - 9)] =
        c.substr(bracket + 4, close - bracket - 4);
  }
  // add_ngx_option(<default>,&NgxRewriteOptions::member_,"id",kName,kScope,
  for (size_t pos = c.find("add_ngx_option("); pos != GoogleString::npos;
       pos = c.find("add_ngx_option(", pos + 1)) {
    const size_t member = c.find("&NgxRewriteOptions::", pos);
    if (member == GoogleString::npos) {
      break;
    }
    StringPieceVector fields;
    SplitStringPieceToVector(StringPiece(c).substr(member, 400), ",", &fields,
                             false);
    if (fields.size() < 4) {
      continue;
    }
    auto it = constants.find(fields[2].as_string());
    if (it != constants.end()) {
      t.own_options[Lower(it->second)] = ScopeFromToken(fields[3]);
    }
  }
  t.server_only = StringTable(c, "server_only_options[]={");
  t.main_only = StringTable(c, "main_only_options[]={");
  return t;
}

// NgxRewriteOptions::GetOptionScope.
RewriteOptions::OptionScope NgxOptionScope(const NgxTables& t,
                                           const RewriteOptions& options,
                                           StringPiece name) {
  const GoogleString lower = Lower(name);
  if (t.main_only.count(lower) > 0) {
    return RewriteOptions::kProcessScopeStrict;
  }
  if (t.server_only.count(lower) > 0) {
    return RewriteOptions::kServerScope;
  }
  auto own = t.own_options.find(lower);
  if (own != t.own_options.end()) {
    return own->second == RewriteOptions::kLegacyProcessScope
               ? RewriteOptions::kProcessScopeStrict
               : own->second;
  }
  // GetOptionScope compares the name as written, without the renamed-option
  // lookup the parser itself applies.
  for (const RewriteOptions::OptionBase* option : options.all_options()) {
    if (option != nullptr && StringCaseEqual(option->option_name(), name)) {
      return option->scope() == RewriteOptions::kLegacyProcessScope
                 ? RewriteOptions::kProcessScopeStrict
                 : option->scope();
    }
  }
  return RewriteOptions::kDirectoryScope;
}

// True when `arg` holds a `$` that does not start a variable name. nginx's
// script compile refuses that ("invalid variable name"); `$ps_dollar` is the
// documented spelling for a literal trailing dollar.
bool HasBareDollar(StringPiece arg) {
  for (size_t i = 0; i < arg.size(); ++i) {
    if (arg[i] != '$') continue;
    const char next = i + 1 < arg.size() ? arg[i + 1] : '\0';
    if (!(next == '{' || next == '_' || (next >= '0' && next <= '9') ||
          (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z'))) {
      return true;
    }
  }
  return false;
}

// `script_variables` tracks whether ProcessScriptVariables on/all was seen
// earlier in the file; when set, LoadFromFile* arguments are script-compiled.
Result ApplyNginx(const Directive& d, const NgxTables& t,
                  SystemRewriteOptions* options, OptionParsingFactory* factory,
                  RecordingHandler* handler, bool script_variables = false) {
  handler->Clear();
  RewriteOptions::ResetDeprecatedFilterWarnings();
  StringPiece directive = d.args[0];
  if (StringCaseStartsWith(directive, "ModPagespeed")) {
    directive.remove_prefix(strlen("ModPagespeed"));
  }
  Result refused;
  if (script_variables && StringCaseStartsWith(directive, "LoadFromFile")) {
    for (size_t i = 1; i < d.args.size(); ++i) {
      if (HasBareDollar(d.args[i])) {
        refused.message = "invalid variable name";
        return refused;
      }
    }
  }
  if (NgxOptionScope(t, *options, directive) > d.scope) {
    refused.message = "cannot be set at this scope";
    return refused;
  }
  const int n_args = static_cast<int>(d.args.size());
  GoogleString msg;
  RewriteOptions::OptionSettingResult r = RewriteOptions::kOptionNameUnknown;
  if (n_args == 1) {
    if (StringCaseEqual(directive, "ClearInheritedScripts")) {
      return FromSettingResult(RewriteOptions::kOptionOk, msg, *handler);
    }
    RewriteOptions::EnabledEnum enabled;
    r = RewriteOptions::ParseFromString(directive, &enabled)
            ? RewriteOptions::kOptionOk
            : RewriteOptions::kOptionNameUnknown;
  } else if (n_args == 2) {
    StringPiece arg = d.args[1];
    if (StringCaseEqual(directive, "UseNativeFetcher")) {
      bool value;
      r = RewriteOptions::ParseFromString(arg, &value)
              ? RewriteOptions::kOptionOk
              : RewriteOptions::kOptionValueInvalid;
    } else if (StringCaseEqual(directive,
                               "NativeFetcherMaxKeepaliveRequests")) {
      int value;
      r = StringToInt(arg, &value) && value > 0
              ? RewriteOptions::kOptionOk
              : RewriteOptions::kOptionValueInvalid;
    } else if (StringCaseEqual(directive, "ProcessScriptVariables")) {
      r = d.scope == RewriteOptions::kProcessScopeStrict &&
                  (StringCaseEqual(arg, "all") || StringCaseEqual(arg, "on") ||
                   StringCaseEqual(arg, "off"))
              ? RewriteOptions::kOptionOk
              : RewriteOptions::kOptionValueInvalid;
    } else if (t.own_options.count(Lower(directive)) > 0) {
      // A string option NgxRewriteOptions registers itself (the handler
      // paths): any value is taken.
      r = RewriteOptions::kOptionOk;
    } else if (StringCaseEqual(directive, RewriteOptions::kFileCachePath) &&
               !StringCaseStartsWith(arg, "/")) {
      r = RewriteOptions::kOptionValueInvalid;
      msg = "must start with a slash";
    } else {
      r = options->ParseAndSetOptionFromName1(directive, arg, &msg, handler);
      if (r == RewriteOptions::kOptionNameUnknown) {
        r = factory->ParseAndSetOption1(
            directive, arg, d.scope >= RewriteOptions::kLegacyProcessScope,
            &msg, handler);
      }
    }
  } else if (n_args == 3) {
    r = options->ParseAndSetOptionFromName2(directive, d.args[1], d.args[2],
                                            &msg, handler);
    if (r == RewriteOptions::kOptionNameUnknown) {
      r = factory->ParseAndSetOption2(
          directive, d.args[1], d.args[2],
          d.scope >= RewriteOptions::kLegacyProcessScope, &msg, handler);
    }
  } else if (n_args == 4) {
    r = options->ParseAndSetOptionFromName3(directive, d.args[1], d.args[2],
                                            d.args[3], &msg, handler);
  }
  if (r == RewriteOptions::kOptionNameUnknown &&
      RewriteOptions::IsDeprecatedOptionName(directive)) {
    // "pagespeed <name> is deprecated and has no effect; please remove it
    // from your configuration", once per name per load.
    handler->Message(kWarning, "pagespeed %s is deprecated and has no effect",
                     directive.as_string().c_str());
    r = RewriteOptions::kOptionOk;
  }
  return FromSettingResult(r, msg, *handler);
}

#endif  // !LEGACY_CONFIG_APACHE

#ifdef LEGACY_CONFIG_APACHE

// ---------------------------------------------------------------------------
// The Apache port: mod_instaweb's command table and ParseDirective*.
// ---------------------------------------------------------------------------

// httpd refuses a directive that is not in the module's command table before
// the module sees it.  The table is the hand-written one in
// apache_directive_table.h (which mod_instaweb.cc builds its command_rec
// array from) plus one entry per registered option that has help text, plus
// one per deprecated name (ForEachOptionCommand, called by InstallCommands at
// the end of mod_instaweb.cc).
bool InApacheCommandTable(const ApacheConfig& config,
                          const GoogleString& mod_instaweb_source,
                          StringPiece option) {
  if (option.empty()) {
    return true;  // "ModPagespeed on"
  }
  const RewriteOptions::OptionBase* registered = nullptr;
  for (const RewriteOptions::OptionBase* o : config.all_options()) {
    if (o != nullptr && StringCaseEqual(o->option_name(), option)) {
      registered = o;
    }
  }
  if (registered != nullptr && registered->help_text() != nullptr) {
    return true;
  }
  if (RewriteOptions::IsDeprecatedOptionName(option)) {
    return true;
  }
  using namespace apache_directive;  // NOLINT(build/namespaces)
  const GoogleString wanted = StrCat("ModPagespeed", option);
  bool found = false;
#define LEGACY_CONFIG_FIND(kind, name, help) \
  found = found || StringCaseEqual(name, wanted);
  PAGESPEED_APACHE_DIRECTIVE_TABLE(LEGACY_CONFIG_FIND)
#undef LEGACY_CONFIG_FIND
  return found;
}

// Where in the httpd configuration a directive is written.
enum class ApacheWhere { kServerConfig, kVirtualHost, kDirectory };

// What httpd and mod_instaweb do with a command by scope, read from the
// module: the command table's override class (OR_ALL allows <Directory>,
// RSRC_CONF does not) and the vhost handling of process-scope options
// (CheckGlobalOption).
struct ApacheCommandScope {
  bool in_directory = true;
  bool errors_in_vhost = false;  // "used inside a <VirtualHost> ..."
};

// The hand-written entries of mod_pagespeed_filter_cmds, from the shared
// table mod_instaweb.cc builds them from: the option names (lower case,
// without "ModPagespeed") mapped to "allowed in <Directory>" (the command's
// override class includes OR_ALL or ACCESS_CONF).
std::map<GoogleString, bool> ReadHandWrittenCommands() {
  using namespace apache_directive;  // NOLINT(build/namespaces)
  std::map<GoogleString, bool> out;
#define LEGACY_CONFIG_ENTRY(kind, name, help)                               \
  {                                                                         \
    const StringPiece n(name);                                              \
    if (StringCaseStartsWith(n, "ModPagespeed")) {                          \
      out[Lower(n.substr(12))] =                                            \
          (InfoFor(Kind::kind).req_override & (kOrAll | kAccessConf)) != 0; \
    }                                                                       \
  }
  PAGESPEED_APACHE_DIRECTIVE_TABLE(LEGACY_CONFIG_ENTRY)
#undef LEGACY_CONFIG_ENTRY
  return out;
}

const char* ScopeLabel(RewriteOptions::OptionScope scope) {
  switch (scope) {
    case RewriteOptions::kDirectoryScope:
      return "kDirectoryScope";
    case RewriteOptions::kQueryScope:
      return "kQueryScope";
    case RewriteOptions::kServerScope:
      return "kServerScope";
    case RewriteOptions::kProcessScopeStrict:
      return "kProcessScopeStrict";
    case RewriteOptions::kLegacyProcessScope:
      return "kLegacyProcessScope";
  }
  return "";
}

// What FillInApacheCommand does for an option scope: the shared
// CommandForOptionScope it calls says whether the command is OR_ALL (allowed
// in <Directory>) and whether it is registered as kErrorInVHost.
ApacheCommandScope ScopeOf(RewriteOptions::OptionScope scope) {
  using namespace apache_directive;  // NOLINT(build/namespaces)
  const OptionCommand command = CommandForOptionScope(scope);
  ApacheCommandScope s;
  EXPECT_TRUE(command.req_override == kOrAll ||
              command.req_override == kRsrcConf)
      << ScopeLabel(scope) << " must set one req_override";
  s.in_directory = command.req_override == kOrAll;
  s.errors_in_vhost = command.vhost == VHostHandling::kErrorInVHost;
  return s;
}

// The command httpd would find for ModPagespeed<option>: the hand-written
// table comes first, then one entry per registered option with help text,
// then one per deprecated name (InstallCommands).  Only the generated entries
// are in vhost_command_handling_map_, so only they can refuse in a vhost with
// CheckGlobalOption; the hand-written names that are global are refused by
// the factory instead (is_process_scope).
ApacheCommandScope CommandScope(
    const ApacheConfig& config,
    const std::map<GoogleString, bool>& hand_written, StringPiece option) {
  ApacheCommandScope s;
  const auto hw = hand_written.find(Lower(option));
  if (hw != hand_written.end()) {
    s.in_directory = hw->second;
    return s;
  }
  for (const RewriteOptions::OptionBase* o : config.all_options()) {
    if (o != nullptr && o->help_text() != nullptr &&
        StringCaseEqual(o->option_name(), option)) {
      return ScopeOf(o->scope());
    }
  }
  const RewriteOptions::Properties* deprecated =
      RewriteOptions::deprecated_properties();
  for (int i = 0, n = deprecated->size(); i < n; ++i) {
    if (StringCaseEqual(deprecated->property(i)->option_name(), option)) {
      return ScopeOf(deprecated->property(i)->scope());
    }
  }
  return s;
}

Result ApplyApache(const Directive& d, const GoogleString& mod_instaweb_source,
                   ApacheConfig* config, OptionParsingFactory* factory,
                   RecordingHandler* handler,
                   ApacheWhere where = ApacheWhere::kServerConfig) {
  handler->Clear();
  RewriteOptions::ResetDeprecatedFilterWarnings();
  Result refused;
  StringPiece directive = d.args[0];
  if (!StringCaseStartsWith(directive, "ModPagespeed")) {
    refused.message = "not a ModPagespeed directive";
    return refused;
  }
  StringPiece option = directive.substr(strlen("ModPagespeed"));
  if (!InApacheCommandTable(*config, mod_instaweb_source, option)) {
    refused.message = "Invalid command (not in the module's command table)";
    return refused;
  }
  if (where != ApacheWhere::kServerConfig) {
    const ApacheCommandScope scope =
        CommandScope(*config, ReadHandWrittenCommands(), option);
    if (where == ApacheWhere::kDirectory && !scope.in_directory) {
      refused.message =
          StrCat(d.args[0], " not allowed in <Directory> context");
      return refused;
    }
    if (where == ApacheWhere::kVirtualHost && scope.errors_in_vhost) {
      refused.message = StrCat("Directive ", d.args[0],
                               " used inside a <VirtualHost> but applies "
                               "globally.");
      return refused;
    }
  }
  // ParseDirective passes !cmd->server->is_virtual as is_process_scope.
  const bool process_scope = where != ApacheWhere::kVirtualHost;
  const int n_values = static_cast<int>(d.args.size()) - 1;
  GoogleString msg;
  RewriteOptions::OptionSettingResult r = RewriteOptions::kOptionNameUnknown;
  if (n_values == 1) {
    StringPiece arg = d.args[1];
    if (StringCaseEqual(option, RewriteOptions::kFileCachePath) ||
        StringCaseEqual(option, RewriteOptions::kLogDir)) {
      // ParseDirective creates the directory and sets the path; nothing
      // else is checked.
      return FromSettingResult(RewriteOptions::kOptionOk, msg, *handler);
    }
    if (option.empty()) {
      RewriteOptions::EnabledEnum enabled;
      r = RewriteOptions::ParseFromString(arg, &enabled)
              ? RewriteOptions::kOptionOk
              : RewriteOptions::kOptionValueInvalid;
      return FromSettingResult(r, msg, *handler);
    }
    // The renames ParseDirective applies before the lookup.
    if (StringCaseEqual(option, "ImgInlineMaxBytes")) {
      option = "ImageInlineMaxBytes";
    } else if (StringCaseEqual(option, "ImgMaxRewritesAtOnce")) {
      option = "ImageMaxRewritesAtOnce";
    }
    r = config->ParseAndSetOptionFromName1(option, arg, &msg, handler);
    if (r == RewriteOptions::kOptionNameUnknown) {
      r = factory->ParseAndSetOption1(option, arg, process_scope, &msg,
                                      handler);
    }
    if (r == RewriteOptions::kOptionNameUnknown &&
        RewriteOptions::IsDeprecatedOptionName(option)) {
      // warn_deprecated(): "... is deprecated.  Please remove it ..."
      handler->Message(kWarning, "%s is deprecated.", d.args[0].c_str());
      r = RewriteOptions::kOptionOk;
    }
  } else if (n_values == 2) {
    r = config->ParseAndSetOptionFromName2(option, d.args[1], d.args[2], &msg,
                                           handler);
    if (r == RewriteOptions::kOptionNameUnknown) {
      r = factory->ParseAndSetOption2(option, d.args[1], d.args[2],
                                      process_scope, &msg, handler);
    }
  } else if (n_values == 3) {
    r = config->ParseAndSetOptionFromName3(option, d.args[1], d.args[2],
                                           d.args[3], &msg, handler);
  }
  return FromSettingResult(r, msg, *handler);
}

#endif  // LEGACY_CONFIG_APACHE

// ---------------------------------------------------------------------------
// The fixture and the report.
// ---------------------------------------------------------------------------

// One ProcessContext for the whole binary.  Its destructor shuts protobuf
// down, so a second ProcessContext in the same process (one per test)
// crashes on the first enum descriptor lookup of the next test.
class ProcessContextEnvironment : public testing::Environment {
 public:
  void SetUp() override { context_ = std::make_unique<ProcessContext>(); }
  void TearDown() override { context_.reset(); }
  const ProcessContext& context() const { return *context_; }

 private:
  std::unique_ptr<ProcessContext> context_;
};

ProcessContextEnvironment* const kProcessContextEnvironment =
    static_cast<ProcessContextEnvironment*>(
        testing::AddGlobalTestEnvironment(new ProcessContextEnvironment));

const ProcessContext& ProcessContextForBinary() {
  return kProcessContextEnvironment->context();
}

// The directives of the serve-cached-html mode a later release adds, one line
// for every value and form that release accepts, and the lines just outside
// it.  Every `loads` line is accepted, with a warning, so that a
// configuration written for that release keeps loading here (a package
// rollback must not stop the server); that release accepts no form outside
// this list.  Every other line is a configuration error, as it is there.
// DaemonHtmlCacheIgnoreCookie takes one name or prefix per line.
struct LaterReleaseLine {
  const char* line;  // without the ModPagespeed prefix
  bool loads;
};
const LaterReleaseLine kLaterReleaseLines[] = {
    {"DaemonHtmlCache serve-cached-html", true},
    {"DaemonHtmlCache Serve-Cached-HTML", true},
    {"DaemonHtmlCache serve-everything", false},
    {"DaemonHtmlCacheServeScope agents", true},
    {"DaemonHtmlCacheServeScope all", true},
    {"DaemonHtmlCacheServeScope ALL", true},
    {"DaemonHtmlCacheServeScope everyone", false},
    {"DaemonHtmlCacheServeScope agents all", false},
    {"DaemonHtmlCacheMaxAge 0", true},
    {"DaemonHtmlCacheMaxAge 60", true},
    {"DaemonHtmlCacheMaxAge 3600", true},
    {"DaemonHtmlCacheMaxAge 86400", true},
    {"DaemonHtmlCacheMaxAge 86401", false},
    {"DaemonHtmlCacheMaxAge -1", false},
    {"DaemonHtmlCacheMaxAge 60s", false},
    {"DaemonHtmlCacheCredentialed anonymous-only", true},
    {"DaemonHtmlCacheCredentialed explicit-public", true},
    {"DaemonHtmlCacheCredentialed anyone", false},
    {"DaemonHtmlCacheCredentialed rfc9111", false},
    {"DaemonHtmlCacheIgnoreCookies on", true},
    {"DaemonHtmlCacheIgnoreCookies off", true},
    {"DaemonHtmlCacheIgnoreCookies maybe", false},
    {"DaemonHtmlCacheIgnoreCookie _my_ab", true},
    {"DaemonHtmlCacheIgnoreCookie _my_prefix*", true},
    {"DaemonHtmlCacheIgnoreCookie _my_ab _my_cd", false},
    {"DaemonHtmlCacheIgnoreCookie _my_ab,_my_cd", false},
    {"DaemonHtmlCacheIgnoreCookie a=b", false},
    {"DaemonHtmlCacheIgnoreCookie _my*ab", false},
};

class LegacyConfigCompatTest : public testing::Test {
 protected:
  void SetUp() override {
#ifdef LEGACY_CONFIG_APACHE
    ApacheConfig::Initialize();
#else
    SystemRewriteOptions::Initialize();
#endif
  }

  void TearDown() override {
#ifdef LEGACY_CONFIG_APACHE
    ApacheConfig::Terminate();
#else
    SystemRewriteOptions::Terminate();
#endif
  }

  std::unique_ptr<OptionParsingFactory> NewFactory() {
    // The factory takes ownership of the thread system.  Init() creates the
    // caches that CreateSharedMemoryMetadataCache registers with, as the
    // ports do before reading their configuration.
    auto factory = std::make_unique<OptionParsingFactory>(
        ProcessContextForBinary(), new SystemThreadSystem);
    factory->Init();
    // The factory's ShutDown() reads the timer, which is otherwise created
    // only when something asks for it.
    factory->timer();
    return factory;
  }

  // Compares each outcome with its class, prints the per-class counts and
  // every directive that is not plain "same", and fails each directive
  // that left its class.
  template <class Apply>
  void Check(const char* port, const std::vector<Directive>& corpus,
             Apply apply) {
    std::map<GoogleString, int> counts;
    std::set<GoogleString> names;
    std::set<GoogleString> removed_in_corpus;
    GoogleString report;
    for (const Directive& d : corpus) {
      names.insert(Lower(d.args[0]));
      if (d.expect == Expect::kRemoved) {
        StringPiece name = d.args[0];
        if (StringCaseStartsWith(name, "ModPagespeed")) {
          name.remove_prefix(strlen("ModPagespeed"));
        }
        removed_in_corpus.insert(Lower(name));
        EXPECT_EQ(1u, KnownRemoved().count(Lower(name)))
            << port << " line " << d.line << ": `" << d.text
            << "` is marked expect=removed but its name is not in "
               "KnownRemoved().  A directive that used to load must keep "
               "loading; a deliberate break is listed there with its "
               "replacement in the upgrade notes.";
      }
      const Result result = apply(d);
      bool ok = false;
      switch (d.expect) {
        case Expect::kSame:
        case Expect::kChangedDefault:
          ok = result.outcome == Outcome::kAccepted;
          break;
        case Expect::kDeprecated:
          ok = result.outcome == Outcome::kAcceptedWithWarning;
          break;
        case Expect::kRemoved:
        case Expect::kNeverLoaded:
          ok = result.outcome == Outcome::kRefused;
          break;
      }
      ++counts[ExpectName(d.expect)];
      if (d.expect != Expect::kSame) {
        StrAppend(
            &report, "  ", ExpectName(d.expect), "  line ",
            IntegerToString(d.line), "  ", d.text,
            d.note.empty() ? "" : StrCat("  (", d.note, ")"),
            result.message.empty() ? "" : StrCat("  [", result.message, "]"),
            "\n");
      }
      if (ok) {
        continue;
      }
      switch (result.outcome) {
        case Outcome::kRefused:
          ADD_FAILURE() << port << " line " << d.line << " (" << d.src << "): `"
                        << d.text << "` is refused (" << result.message
                        << ") but is expected to be " << ExpectName(d.expect)
                        << ".  A directive that used to load must keep "
                           "loading; if the break is intended, mark it "
                           "expect=removed and document the replacement in "
                           "the upgrade notes.";
          break;
        case Outcome::kAcceptedWithWarning:
          ADD_FAILURE() << port << " line " << d.line << " (" << d.src << "): `"
                        << d.text << "` now warns (" << result.message
                        << ") but is expected to be " << ExpectName(d.expect)
                        << ".  If it was deprecated on purpose, mark it "
                           "expect=deprecated.";
          break;
        case Outcome::kAccepted:
          ADD_FAILURE() << port << " line " << d.line << " (" << d.src << "): `"
                        << d.text << "` is accepted without a "
                        << "warning but is expected to be "
                        << ExpectName(d.expect)
                        << ".  If a known break was fixed or a deprecation "
                           "dropped, update the expectation and the "
                           "upgrade notes.";
          break;
      }
    }
    GoogleString summary =
        StrCat(port, ": ", IntegerToString(static_cast<int>(corpus.size())),
               " directives, ", IntegerToString(static_cast<int>(names.size())),
               " distinct names;");
    for (const auto& c : counts) {
      StrAppend(&summary, " ", c.first, "=", IntegerToString(c.second));
    }
    for (const GoogleString& name : KnownRemoved()) {
      EXPECT_EQ(1u, removed_in_corpus.count(name))
          << port << ": " << name
          << " is in KnownRemoved() but no corpus line marks it removed.";
    }
    GTEST_LOG_(INFO) << summary << "\n" << report;
  }

#ifdef LEGACY_CONFIG_APACHE
  // The same corpus written inside <VirtualHost> and inside <Directory>, as
  // httpd applies it.  The expectation is not a list: each option's scope comes
  // from the module (the hand-written command table's APACHE_CONFIG_* macros,
  // each registered option's scope as FillInApacheCommand maps it, the deprecated
  // names' scope, and the factory's own refusal of the global names in a vhost).
  // The counts are what `apachectl configtest` reported on released 2.8.2 for the
  // same 370 lines: 327 accepted and 43 refused in a vhost (the 31 never-loaded
  // names plus 12 process-scope names), 239 and 131 in a directory (the 31 plus
  // 100 names that are not allowed in <Directory> context).
  //
  void CheckApacheScopes() {
    const std::vector<Directive> corpus = ReadCorpus("apache.conf", false);
    ASSERT_GE(corpus.size(), 300u);
    const GoogleString mod_instaweb = ReadSource(kModInstawebCc);
    ASSERT_FALSE(mod_instaweb.empty());
    ASSERT_GE(ReadHandWrittenCommands().size(), 30u);
    std::unique_ptr<OptionParsingFactory> factory = NewFactory();
    RecordingHandler handler;
    struct Case {
      const char* name;
      ApacheWhere where;
      int accepted;
      int refused;
      int refused_by_scope;
    };
    for (const Case& c :
         {Case{"server config", ApacheWhere::kServerConfig, 339, 31, 0},
          Case{"<VirtualHost>", ApacheWhere::kVirtualHost, 327, 43, 12},
          Case{"<Directory>", ApacheWhere::kDirectory, 239, 131, 100}}) {
      int accepted = 0;
      int refused = 0;
      int by_scope = 0;
      GoogleString report;
      for (const Directive& d : corpus) {
        ApacheConfig config("legacy-apache", &thread_system_);
        const Result r = ApplyApache(d, mod_instaweb, &config, factory.get(),
                                     &handler, c.where);
        if (r.outcome != Outcome::kRefused) {
          ++accepted;
          EXPECT_NE(Expect::kNeverLoaded, d.expect) << c.name << ": " << d.text;
          continue;
        }
        ++refused;
        if (d.expect != Expect::kNeverLoaded) {
          ++by_scope;
          StrAppend(&report, "  line ", IntegerToString(d.line), "  ", d.text,
                    "  [", r.message, "]\n");
          // Only a scope rule may refuse a line the server scope accepts.
          EXPECT_TRUE(r.message.find("not allowed in <Directory>") !=
                          GoogleString::npos ||
                      r.message.find("<VirtualHost>") != GoogleString::npos ||
                      r.message.find("is global") != GoogleString::npos)
              << c.name << ": `" << d.text << "` refused: " << r.message;
        }
      }
      GTEST_LOG_(INFO) << c.name << ": " << accepted << " accepted, " << refused
                       << " refused\n"
                       << report;
      EXPECT_EQ(c.accepted, accepted) << c.name;
      EXPECT_EQ(c.refused, refused) << c.name;
      EXPECT_EQ(c.refused_by_scope, by_scope) << c.name;
    }
  }

#endif

  NullThreadSystem thread_system_;
};

#ifndef LEGACY_CONFIG_APACHE

TEST_F(LegacyConfigCompatTest, NginxDirectivesKeepLoading) {
  const std::vector<Directive> corpus = ReadCorpus("nginx.conf", true);
  ASSERT_GE(corpus.size(), 300u);
  const NgxTables tables = ReadNgxTables();
  ASSERT_FALSE(tables.own_options.empty());
  ASSERT_FALSE(tables.server_only.empty());
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  bool script_variables = false;
  Check("nginx", corpus, [&](const Directive& d) {
    if (d.args.size() == 2 &&
        StringCaseEqual(d.args[0], "ProcessScriptVariables")) {
      script_variables = !StringCaseEqual(d.args[1], "off");
    }
    // NgxRewriteOptions derives from SystemRewriteOptions and adds only the
    // options read from its source above.
    SystemRewriteOptions options("legacy-nginx", &thread_system_);
    return ApplyNginx(d, tables, &options, factory.get(), &handler,
                      script_variables);
  });
}

// The later release's directives load in an http and a server block, each
// with one warning; a location block refuses them, as it refuses
// DaemonHtmlCache.
TEST_F(LegacyConfigCompatTest, NginxLaterReleaseDirectivesLoadInert) {
  const NgxTables tables = ReadNgxTables();
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  SystemRewriteOptions options("later-nginx", &thread_system_);
  for (const LaterReleaseLine& c : kLaterReleaseLines) {
    Directive d;
    d.text = c.line;
    d.args = Words(c.line);
    for (RewriteOptions::OptionScope scope :
         {RewriteOptions::kProcessScopeStrict, RewriteOptions::kServerScope}) {
      d.scope = scope;
      const Result result =
          ApplyNginx(d, tables, &options, factory.get(), &handler);
      EXPECT_EQ(c.loads ? Outcome::kAcceptedWithWarning : Outcome::kRefused,
                result.outcome)
          << c.line << " [" << result.message << "]";
      if (c.loads) {
        EXPECT_EQ(1, handler.warnings()) << c.line;
      }
    }
    d.scope = RewriteOptions::kDirectoryScope;
    EXPECT_EQ(Outcome::kRefused,
              ApplyNginx(d, tables, &options, factory.get(), &handler).outcome)
        << c.line << " in a location block";
  }
  // The mode runs as serve-variants; the settings are kept and change
  // nothing.
  EXPECT_EQ(RewriteOptions::kDaemonHtmlCacheServeVariants,
            options.daemon_html_cache());
  EXPECT_EQ(86400, options.daemon_html_cache_max_age_s());
  EXPECT_EQ(RewriteOptions::kDaemonHtmlCacheServeScopeAll,
            options.daemon_html_cache_serve_scope());
  EXPECT_EQ("_my_ab _my_prefix*", options.daemon_html_cache_ignore_cookie());
}

#endif  // !LEGACY_CONFIG_APACHE

#ifdef LEGACY_CONFIG_APACHE

TEST_F(LegacyConfigCompatTest, ApacheDirectivesKeepLoading) {
  const std::vector<Directive> corpus = ReadCorpus("apache.conf", false);
  ASSERT_GE(corpus.size(), 300u);
  const GoogleString mod_instaweb = ReadSource(kModInstawebCc);
  ASSERT_FALSE(mod_instaweb.empty());
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  Check("Apache", corpus, [&](const Directive& d) {
    ApacheConfig config("legacy-apache", &thread_system_);
    return ApplyApache(d, mod_instaweb, &config, factory.get(), &handler);
  });
}

// The corpus at vhost and directory scope (see CheckApacheScopes).
TEST_F(LegacyConfigCompatTest, ApacheScopesHoldInVhostAndDirectory) {
  CheckApacheScopes();
}

// The harness refuses what httpd refuses: a name that was never a command and
// a value that does not parse.
TEST_F(LegacyConfigCompatTest, ApacheRefusalsAreSeen) {
  const GoogleString mod_instaweb = ReadSource(kModInstawebCc);
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  for (const char* line : {"ModPagespeedNoSuchOptionEver on",
                           "ModPagespeedFileCacheSizeKb lots"}) {
    Directive d;
    d.text = line;
    d.args = Words(line);
    ApacheConfig config("legacy-apache", &thread_system_);
    EXPECT_EQ(
        Outcome::kRefused,
        ApplyApache(d, mod_instaweb, &config, factory.get(), &handler).outcome)
        << line;
  }
}

// The later release's directives are commands httpd knows, and load in the
// main server configuration and in a <VirtualHost>, each with one warning;
// <Directory> refuses them, as it refuses DaemonHtmlCache.
TEST_F(LegacyConfigCompatTest, ApacheLaterReleaseDirectivesLoadInert) {
  const GoogleString mod_instaweb = ReadSource(kModInstawebCc);
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  ApacheConfig config("later-apache", &thread_system_);
  for (const LaterReleaseLine& c : kLaterReleaseLines) {
    const GoogleString line = StrCat("ModPagespeed", c.line);
    Directive d;
    d.text = line;
    d.args = Words(line);
    for (ApacheWhere where :
         {ApacheWhere::kServerConfig, ApacheWhere::kVirtualHost}) {
      const Result result =
          ApplyApache(d, mod_instaweb, &config, factory.get(), &handler, where);
      EXPECT_EQ(c.loads ? Outcome::kAcceptedWithWarning : Outcome::kRefused,
                result.outcome)
          << line << " [" << result.message << "]";
      if (c.loads) {
        EXPECT_EQ(1, handler.warnings()) << line;
      }
    }
    EXPECT_EQ(Outcome::kRefused,
              ApplyApache(d, mod_instaweb, &config, factory.get(), &handler,
                          ApacheWhere::kDirectory)
                  .outcome)
        << line << " in <Directory>";
  }
  EXPECT_EQ(RewriteOptions::kDaemonHtmlCacheServeVariants,
            config.daemon_html_cache());
  EXPECT_EQ(RewriteOptions::kDaemonHtmlCacheCredentialedExplicitPublic,
            config.daemon_html_cache_credentialed());
  EXPECT_FALSE(config.daemon_html_cache_ignore_cookies());
}

#endif  // LEGACY_CONFIG_APACHE

#ifndef LEGACY_CONFIG_APACHE

// The harness refuses what nginx refuses, so "keeps loading" above is not
// vacuous: a name that was never an option, a value that does not parse, and
// an http-only option inside a server block.
TEST_F(LegacyConfigCompatTest, NginxRefusalsAreSeen) {
  const NgxTables tables = ReadNgxTables();
  std::unique_ptr<OptionParsingFactory> factory = NewFactory();
  RecordingHandler handler;
  struct Case {
    const char* line;
    RewriteOptions::OptionScope scope;
  };
  for (const Case& c : {
           Case{"LoadFromFileRuleMatch Disallow \\.php$",
                RewriteOptions::kServerScope},
           Case{"NoSuchOptionEver on", RewriteOptions::kServerScope},
           Case{"FileCacheSizeKb lots", RewriteOptions::kServerScope},
           Case{"UseNativeFetcher on", RewriteOptions::kServerScope},
           Case{"FileCachePath relative/path", RewriteOptions::kServerScope},
       }) {
    Directive d;
    d.text = c.line;
    d.args = Words(c.line);
    d.scope = c.scope;
    SystemRewriteOptions options("legacy-nginx", &thread_system_);
    // Script variables are on, so a bare trailing `$` is refused as in nginx.
    EXPECT_EQ(Outcome::kRefused,
              ApplyNginx(d, tables, &options, factory.get(), &handler, true)
                  .outcome)
        << c.line;
  }
  // The documented spelling is accepted.
  Directive ok;
  ok.text = "LoadFromFileRuleMatch Disallow \\.php$ps_dollar";
  ok.args = Words(ok.text.c_str());
  ok.scope = RewriteOptions::kServerScope;
  SystemRewriteOptions options("legacy-nginx", &thread_system_);
  EXPECT_NE(Outcome::kRefused,
            ApplyNginx(ok, tables, &options, factory.get(), &handler, true)
                .outcome);
}

// Every filter name and compound filter name the 1.14.36.1 release knew is in
// the corpus, so a filter that drops out of the name table fails above.
TEST_F(LegacyConfigCompatTest, TheCorpusCoversEveryOldFilterName) {
  const std::vector<Directive> corpus = ReadCorpus("nginx.conf", true);
  std::set<GoogleString> enabled;
  for (const Directive& d : corpus) {
    if (d.args.size() == 2 && StringCaseEqual(d.args[0], "EnableFilters")) {
      StringPieceVector names;
      SplitStringPieceToVector(d.args[1], ",", &names, true);
      for (StringPiece n : names) {
        enabled.insert(n.as_string());
      }
    }
  }
  EXPECT_GE(enabled.size(), 90u);
  for (const char* compound : {"core", "extend_cache", "recompress_images",
                               "rewrite_images", "rewrite_javascript"}) {
    EXPECT_EQ(1u, enabled.count(compound)) << compound;
  }
}

// ---------------------------------------------------------------------------
// Pins: the port steps followed above still match the port sources.
// ---------------------------------------------------------------------------

TEST_F(LegacyConfigCompatTest, NginxParseStepsMatchTheSource) {
  const GoogleString body = Condense(FunctionBody(
      ReadSource(kNgxOptionsCc), "NgxRewriteOptions::ParseAndSetOptions("));
  ASSERT_FALSE(body.empty());
  // The order followed by ApplyNginx: scope check, one-word forms, the
  // nginx-only names, the options, then the factory.
  const size_t scope = body.find("if(GetOptionScope(directive)>scope)");
  const size_t clear = body.find("\"ClearInheritedScripts\"");
  const size_t one = body.find("ParseAndSetOptions0(directive");
  const size_t native = body.find("\"UseNativeFetcher\"");
  const size_t name1 = body.find("ParseAndSetOptionFromName1(directive,arg");
  const size_t factory1 = body.find("driver_factory->ParseAndSetOption1(");
  const size_t name2 = body.find("ParseAndSetOptionFromName2(directive,");
  const size_t factory2 = body.find("driver_factory->ParseAndSetOption2(");
  const size_t name3 = body.find("ParseAndSetOptionFromName3(directive,");
  for (size_t pos :
       {scope, clear, one, native, name1, factory1, name2, factory2, name3}) {
    ASSERT_NE(GoogleString::npos, pos) << body;
  }
  EXPECT_LT(scope, one);
  EXPECT_LT(native, name1);
  EXPECT_LT(name1, factory1);
  EXPECT_LT(name2, factory2);
  // A name that is not recognized but is in the deprecated-name table is
  // accepted with a warning, after every lookup above (as ApplyNginx does).
  // If that goes, the deprecated names in the corpus are refused again.
  const size_t unknown = body.find("caseRewriteOptions::kOptionNameUnknown:");
  const size_t deprecated = body.find("if(IsDeprecatedOptionName(directive))");
  ASSERT_NE(GoogleString::npos, unknown) << body;
  ASSERT_NE(GoogleString::npos, deprecated) << body;
  EXPECT_LT(name3, unknown);
  EXPECT_LT(unknown, deprecated);
  EXPECT_LT(deprecated, body.find("\"not recognized or too many arguments\""));
  // FileCachePath must be absolute.
  const GoogleString name1_body =
      Condense(FunctionBody(ReadSource(kNgxOptionsCc),
                            "NgxRewriteOptions::ParseAndSetOptionFromName1("));
  EXPECT_NE(GoogleString::npos,
            name1_body.find("StringCaseStartsWith(arg,\"/\")"));
  EXPECT_NE(
      GoogleString::npos,
      name1_body.find("SystemRewriteOptions::ParseAndSetOptionFromName1("));
  // ParseAndSetOptions0: "off" means unplugged, anything else goes through
  // the EnabledEnum parse.
  const GoogleString zero_body = Condense(FunctionBody(
      ReadSource(kNgxOptionsCc), "NgxRewriteOptions::ParseAndSetOptions0("));
  EXPECT_NE(GoogleString::npos, zero_body.find("ParseFromString(directive,"));
}

#endif  // !LEGACY_CONFIG_APACHE

#ifdef LEGACY_CONFIG_APACHE

TEST_F(LegacyConfigCompatTest, ApacheParseStepsMatchTheSource) {
  const GoogleString source = ReadSource(kModInstawebCc);
  const GoogleString parse1 = Condense(
      FunctionBody(source, "static const char* ParseDirective(cmd_parms*"));
  ASSERT_FALSE(parse1.empty());
  const size_t cache_path = parse1.find("kModPagespeedFileCachePath)");
  const size_t rename =
      parse1.find("directive=CanonicalDirectiveName(directive)");
  const size_t name1 = parse1.find("config->ParseAndSetOptionFromName1(option");
  const size_t factory1 = parse1.find("factory->ParseAndSetOption1(");
  const size_t deprecated =
      parse1.find("RewriteOptions::IsDeprecatedOptionName(option)");
  for (size_t pos : {cache_path, rename, name1, factory1, deprecated}) {
    ASSERT_NE(GoogleString::npos, pos) << parse1;
  }
  EXPECT_LT(cache_path, name1);
  EXPECT_LT(rename, name1);
  EXPECT_LT(name1, factory1);
  EXPECT_LT(factory1, deprecated);
  // The renames themselves, which ApplyApache follows.
  EXPECT_EQ("ModPagespeedImageInlineMaxBytes",
            apache_directive::CanonicalDirectiveName(
                "ModPagespeedImgInlineMaxBytes"));
  EXPECT_EQ("ModPagespeedImageMaxRewritesAtOnce",
            apache_directive::CanonicalDirectiveName(
                "ModPagespeedImgMaxRewritesAtOnce"));
  const GoogleString parse2 = Condense(
      FunctionBody(source, "static const char* ParseDirective2(cmd_parms*"));
  EXPECT_NE(GoogleString::npos,
            parse2.find("config->ParseAndSetOptionFromName2(option"));
  EXPECT_NE(GoogleString::npos, parse2.find("factory->ParseAndSetOption2("));
  // The command table: the hand-written entries from the shared table, then
  // the shared walk (one per registered option with help text, then one per
  // deprecated name), placed by the shared scope mapping that ScopeOf reads.
  const GoogleString all = Condense(source);
  EXPECT_NE(GoogleString::npos,
            all.find("PAGESPEED_APACHE_DIRECTIVE_TABLE(PAGESPEED_APACHE_CMD)"));
  EXPECT_NE(GoogleString::npos,
            all.find("ForEachOptionCommand(config_template,"));
  EXPECT_NE(GoogleString::npos,
            all.find("OptionCommandcommand=CommandForOptionScope(scope);"));
  EXPECT_NE(GoogleString::npos,
            all.find("vhost_command_handling_map_[cmd]=command.vhost;"));
  EXPECT_NE(GoogleString::npos, all.find("!cmd->server->is_virtual"));
}

#endif  // LEGACY_CONFIG_APACHE

}  // namespace
}  // namespace net_instaweb
