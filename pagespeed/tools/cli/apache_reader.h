// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef PAGESPEED_TOOLS_CLI_APACHE_READER_H_
#define PAGESPEED_TOOLS_CLI_APACHE_READER_H_

#include <filesystem>
#include <set>
#include <vector>

#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/tools/cli/lint_report.h"
#include "pagespeed/tools/cli/value_mask.h"

namespace net_instaweb {
namespace lint {

// Where in an Apache configuration a directive is written.
enum class ContextKind {
  kServer,       // the main server configuration
  kVirtualHost,  // inside <VirtualHost>
  kDirectory,    // inside <Directory>, <Location>, <Files>, <If>, <Proxy>
  kHtaccess,     // a .htaccess file
};

struct Placement {
  ContextKind kind = ContextKind::kServer;
  bool in_virtual_host = false;  // inside <VirtualHost>, at any depth
  bool in_conditional = false;   // inside <ModPagespeedIf>
  bool in_limit = false;         // inside <Limit> or <LimitExcept>
};

// "server", "virtualhost", "directory", "virtualhost/directory" or
// "htaccess", with "+conditional" inside <ModPagespeedIf>.
GoogleString ScopeLabel(const Placement& placement);

// Receives the mod_pagespeed lines of a configuration, in file order with
// includes expanded in place.
class ApacheLineVisitor {
 public:
  virtual ~ApacheLineVisitor() = default;
  // A "ModPagespeed..." directive line.  `name` is as written.
  virtual void Directive(const GoogleString& file, int line,
                         const Placement& placement, const GoogleString& name,
                         const std::vector<GoogleString>& args) = 0;
  // A <ModPagespeedIf ...> opener; `placement` is where the section is.
  // `args` are the words after the name, the closing '>' removed.
  virtual void Conditional(const GoogleString& file, int line,
                           const Placement& placement,
                           const std::vector<GoogleString>& args) = 0;
};

// Reads an Apache configuration the way httpd does for what matters here:
// line continuations, comments, quoted words, sections, and Include /
// IncludeOptional (with wildcards and directories) under a root directory.
//
// Paths are reported root-relative with '/' separators and case as written.
// Each path component is matched against the sorted directory listing byte
// for byte first, then by a unique ASCII case-insensitive match, which is
// reported (PS902): the same configuration gives the same findings on
// case-sensitive and case-insensitive file systems.  An absolute path or a
// drive letter is taken as relative to the root; a path that leaves the root
// is refused (PS903).
//
// Findings about the file structure (PS104, PS307, PS901..PS904) go to the
// report; directive lines go to the visitor.  Nothing the reader reports
// contains a name or value that is not known to be safe.
class ApacheConfigReader {
 public:
  ApacheConfigReader(const std::filesystem::path& root, const ValueMask* mask,
                     Report* report, ApacheLineVisitor* visitor);

  // Reads the main configuration file or a .htaccess file named relative to
  // the root.  False when the file itself cannot be read (a usage error).
  bool ReadConfig(StringPiece path);
  bool ReadHtaccess(StringPiece path);

  // For tests: splits one logical line into words as ap_getword_conf does.
  static std::vector<GoogleString> Words(StringPiece line);

 private:
  struct Frame {
    GoogleString name;  // lower case
    GoogleString shown_name;
    int line;
  };
  struct Resolved {
    GoogleString logical;
    std::filesystem::path disk;
  };

  bool ReadTop(StringPiece path, bool htaccess);
  void ReadFile(const GoogleString& logical, const std::filesystem::path& disk,
                const char* kind, bool htaccess, int depth);
  void Include(StringPiece arg, bool optional, const GoogleString& file,
               int line, const Placement& placement, int depth);
  bool Normalise(StringPiece raw, std::vector<GoogleString>* components) const;
  bool ResolveFixed(const std::vector<GoogleString>& components,
                    std::filesystem::path* disk, bool* case_mismatch) const;
  void Expand(const std::vector<GoogleString>& components, size_t index,
              const std::filesystem::path& disk, const GoogleString& logical,
              bool* case_mismatch, std::vector<Resolved>* out) const;
  Placement PlacementFor(bool htaccess) const;
  void AddFinding(const char* rule, Severity severity, const GoogleString& file,
                  int line, const Placement& placement,
                  const GoogleString& directive, const GoogleString& message);

  std::filesystem::path root_;
  const ValueMask* mask_;
  Report* report_;
  ApacheLineVisitor* visitor_;
  std::vector<Frame> stack_;
  std::set<GoogleString> open_files_;
};

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_APACHE_READER_H_
