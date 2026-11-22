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
// A path that resolves outside the root through a symbolic link is not read
// (PS905), a directory reached twice is expanded once (PS904), and one
// Include expands to at most kMaxIncludeFiles files and kMaxIncludeLevels
// directory levels, a whole run to kMaxInputFiles files, kMaxIncludeDirectives
// Include directives and a listing budget of directories and entries (PS906).
// A file is read through the path that was checked, at most 16 MiB of it.
//
// Findings about the file structure (PS104, PS307, PS901..PS906) go to the
// report; directive lines go to the visitor.  Nothing the reader reports
// contains a name or value that is not known to be safe.
class ApacheConfigReader {
 public:
  static constexpr int kMaxIncludeFiles = 1024;
  static constexpr int kMaxIncludeLevels = 16;
  static constexpr int kMaxInputFiles = 4096;
  // Work budgets of one run: Include directives expanded, directories
  // listed and directory entries looked at.
  static constexpr int kMaxIncludeDirectives = 1024;
  static constexpr int kMaxDirectoriesListed = 4096;
  static constexpr int kMaxEntriesListed = 65536;
  // A wildcard component longer than this matches nothing.
  static constexpr size_t kMaxWildcardLength = 255;

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
  // What one Include expansion ran into.
  struct ExpandState {
    bool case_mismatch = false;
    bool outside_root = false;  // a path resolved outside the root: skipped
    bool loop = false;          // a directory reached twice: skipped
    bool truncated = false;     // too many files or levels: stopped
    std::set<GoogleString> directories;  // resolved, already expanded
  };
  void Expand(const std::vector<GoogleString>& components, size_t index,
              const std::filesystem::path& disk, const GoogleString& logical,
              int levels, ExpandState* state, std::vector<Resolved>* out) const;
  // Where `disk` resolves to, symbolic links followed; *resolved is the
  // resolved path when it exists.
  enum class Where { kInside, kOutside, kMissing };
  Where Resolve(const std::filesystem::path& disk,
                GoogleString* resolved) const;
  // The sorted names in `dir`, within the run's listing budget; sets
  // budget_exhausted_ when the budget runs out.
  std::vector<GoogleString> SortedEntries(
      const std::filesystem::path& dir) const;
  Placement PlacementFor(bool htaccess) const;
  void AddFinding(const char* rule, Severity severity, const GoogleString& file,
                  int line, const Placement& placement,
                  const GoogleString& directive, const GoogleString& message);

  std::filesystem::path root_;
  std::filesystem::path root_resolved_;
  int files_read_ = 0;
  int includes_expanded_ = 0;
  mutable int directories_listed_ = 0;
  mutable int entries_listed_ = 0;
  mutable bool budget_exhausted_ = false;
  const ValueMask* mask_;
  Report* report_;
  ApacheLineVisitor* visitor_;
  std::vector<Frame> stack_;
  std::set<GoogleString> open_files_;
};

}  // namespace lint
}  // namespace net_instaweb

#endif  // PAGESPEED_TOOLS_CLI_APACHE_READER_H_
