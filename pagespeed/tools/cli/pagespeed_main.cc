// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// `pagespeed`: offline tools for the mod_pagespeed module.
//
//   pagespeed lint [--root DIR] [--htaccess FILE]... [--json|--text]
//                  [--fail-on error|warning|info] [--show-values] [CONFIG]
//   pagespeed version [--json]
//
// `lint` checks an Apache configuration (with its Include files, under
// --root) and .htaccess files with the module's own option parser and
// directive rules, without Apache, without network access and without
// touching anything outside the files it reads.  Output is JSON (schema
// pagespeed-lint/1) when --json is given or stdout is not a terminal, text
// otherwise.  Option values are masked unless the option is on the console's
// list of options safe to show; --show-values lifts that on a terminal only.
//
// Exit status: 0 nothing at or above --fail-on (default error), 1 findings at
// or above it, 2 usage error or unreadable input, 3 internal error.

#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "net/instaweb/public/version.h"
#include "pagespeed/apache/apache_config.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/thread_system.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/domain_registry.h"
#include "pagespeed/kernel/util/platform.h"
#include "pagespeed/tools/cli/apache_lint.h"
#include "pagespeed/tools/cli/apache_reader.h"
#include "pagespeed/tools/cli/lint_report.h"
#include "pagespeed/tools/cli/log_capture.h"
#include "pagespeed/tools/cli/value_mask.h"

namespace net_instaweb {
namespace lint {
namespace {

constexpr char kUsage[] =
    "usage: pagespeed lint [--root DIR] [--htaccess FILE]... [--json|--text]\n"
    "                      [--fail-on error|warning|info] [--show-values]\n"
    "                      [CONFIG]\n"
    "       pagespeed version [--json]\n";

bool StdoutIsTerminal() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return isatty(fileno(stdout)) != 0;
#endif
}

// The same bytes on every platform: no text-mode newline translation.
void Write(const GoogleString& text) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  fwrite(text.data(), 1, text.size(), stdout);
  fflush(stdout);
}

int Usage(const char* problem) {
  if (problem != nullptr) {
    fprintf(stderr, "pagespeed: %s\n", problem);
  }
  fputs(kUsage, stderr);
  return kExitUsage;
}

const char* BuildType() {
#ifdef NDEBUG
  return "release";
#else
  return "debug";
#endif
}

int Version(int argc, char** argv) {
  bool json = false;
  for (int i = 2; i < argc; ++i) {
    if (StringPiece(argv[i]) == "--json") {
      json = true;
    } else {
      return Usage("unknown argument to version");
    }
  }
  if (json) {
    GoogleString o = "{\"tool_version\": ";
    AppendJsonString(kToolVersion, &o);
    o += ", \"module_version\": ";
    AppendJsonString(kModPagespeedVersion, &o);
    o += ", \"schema\": ";
    AppendJsonString(kSchema, &o);
    o += ", \"build\": ";
    AppendJsonString(BuildType(), &o);
    o += "}\n";
    Write(o);
  } else {
    Write(StrCat("pagespeed ", kToolVersion, " (mod_pagespeed ",
                 kModPagespeedVersion, ", ", kSchema, ", ", BuildType(),
                 ")\n"));
  }
  return kExitClean;
}

// Every command httpd would accept for the module, one per line:
// name, argument count, values shown (1/0), req_override bits.  For the
// secret-masking tests, which seed every directive with a marker value.
void ListDirectives(const ApacheCommandTable& table, const ValueMask& mask) {
  GoogleString o;
  for (const ApacheCommand& c : table.commands()) {
    if (c.handler == apache_directive::Handler::kParseScope) {
      continue;
    }
    int nargs = 1;
    if (c.args == apache_directive::Args::kTake2 ||
        c.args == apache_directive::Args::kTake23) {
      nargs = 2;
    } else if (c.args == apache_directive::Args::kTake3) {
      nargs = 3;
    }
    o += StrCat(c.name, "\t", nargs, "\t", mask.ValuesAreSafe(c.name) ? 1 : 0,
                "\t", c.req_override, "\n");
  }
  Write(o);
}

int Lint(int argc, char** argv) {
  GoogleString root = ".";
  GoogleString config;
  std::vector<GoogleString> htaccess;
  bool json = !StdoutIsTerminal();
  bool show_values = false;
  bool list = false;
  Severity fail_on = Severity::kError;
  for (int i = 2; i < argc; ++i) {
    const StringPiece a(argv[i]);
    if (a == "--root" && i + 1 < argc) {
      root = argv[++i];
    } else if (a == "--htaccess" && i + 1 < argc) {
      htaccess.push_back(argv[++i]);
    } else if (a == "--json") {
      json = true;
    } else if (a == "--text") {
      json = false;
    } else if (a == "--fail-on" && i + 1 < argc) {
      if (!ParseSeverity(argv[++i], &fail_on)) {
        return Usage("--fail-on takes error, warning or info");
      }
    } else if (a == "--show-values") {
      show_values = true;
    } else if (a == "--list-directives") {
      list = true;
    } else if (a == "--help" || a == "-h") {
      fputs(kUsage, stdout);
      return kExitClean;
    } else if (a.starts_with("-")) {
      return Usage("unknown option");
    } else if (config.empty()) {
      config = argv[i];
    } else {
      return Usage("only one configuration file may be given");
    }
  }
  // Values are only ever shown to a person at a terminal: never into a file,
  // a pipe or another program.
  if (show_values && !StdoutIsTerminal()) {
    return Usage("--show-values is refused when stdout is not a terminal");
  }
  if (!list && config.empty() && htaccess.empty()) {
    return Usage("nothing to check");
  }

  SilenceModuleLogging();
  ApacheConfig::Initialize();
  domain_registry::Init();
  HtmlKeywords::Init();
  std::unique_ptr<ThreadSystem> thread_system(Platform::CreateThreadSystem());
  int status = kExitClean;
  {
    ValueMask mask;
    if (show_values) {
      mask.ShowAll();
    }
    ApacheCommandTable table;
    if (list) {
      ListDirectives(table, mask);
    } else {
      std::error_code ec;
      const std::filesystem::path root_dir = std::filesystem::absolute(
          std::filesystem::path(std::u8string(root.begin(), root.end())), ec);
      if (ec || !std::filesystem::is_directory(root_dir, ec)) {
        status = Usage("--root is not a directory");
      } else {
        Report report;
        if (!LintApacheFiles(root_dir, config, htaccess, table, mask,
                             thread_system.get(), &report)) {
          status = Usage("cannot read an input file");
        }
        if (status == kExitClean) {
          Write(json ? report.ToJson(kModPagespeedVersion) : report.ToText());
          status = report.ExitCode(fail_on);
        }
      }
    }
  }
  ApacheConfig::Terminate();
  return status;
}

int Main(int argc, char** argv) {
  if (argc < 2) {
    return Usage(nullptr);
  }
  const StringPiece command(argv[1]);
  if (command == "lint") {
    return Lint(argc, argv);
  }
  if (command == "version" || command == "--version") {
    return Version(argc, argv);
  }
  if (command == "help" || command == "--help" || command == "-h") {
    fputs(kUsage, stdout);
    return kExitClean;
  }
  return Usage("unknown command");
}

}  // namespace
}  // namespace lint
}  // namespace net_instaweb

int main(int argc, char** argv) {
  try {
    return net_instaweb::lint::Main(argc, argv);
  } catch (const std::exception&) {
    fputs("pagespeed: internal error\n", stderr);
    return net_instaweb::lint::kExitInternal;
  } catch (...) {
    fputs("pagespeed: internal error\n", stderr);
    return net_instaweb::lint::kExitInternal;
  }
}
